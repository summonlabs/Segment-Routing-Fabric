// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

#include <cstring>

namespace {

std::vector<std::byte> encode(const srf::Frame& frame, const srf::Limits& limits,
                              srf::ValidationResult& result) {
    const srf::ByteBuffer buffer = srf::encode_frame(frame, limits, result);
    return std::vector<std::byte>(buffer.begin(), buffer.end());
}

void set_u16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void set_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// Recompute both integrity trailers over the image as it currently is. A
/// corrupted length field simply means the payload trailer cannot be located;
/// the header trailer is still repaired so the structural check is reached.
void repair_crcs(std::vector<std::byte>& bytes) {
    if (bytes.size() < srf::kFrameHeaderBytes + srf::kFrameTrailerBytes) {
        return;
    }
    const std::uint32_t header =
        srf::crc32c(std::span<const std::byte>(bytes.data(), srf::kFrameHeaderBytes - 4));
    set_u32(bytes, srf::kFrameHeaderBytes - 4, header);
    std::uint32_t payload_length = 0;
    std::memcpy(&payload_length, bytes.data() + 12, sizeof(payload_length));
    if (payload_length > bytes.size() - srf::kFrameHeaderBytes - srf::kFrameTrailerBytes) {
        return;
    }
    const std::uint32_t payload =
        srf::crc32c(std::span<const std::byte>(bytes.data() + srf::kFrameHeaderBytes,
                                               payload_length));
    set_u32(bytes, srf::kFrameHeaderBytes + payload_length, payload);
}

srf::Frame sample_frame() {
    srf::Frame frame{};
    frame.message = srf::MessageId::Get;
    frame.request_id = 42;
    frame.payload = srf::encode_get(srf::GetRequest{srf::test::list_id(1)});
    return frame;
}

} // namespace

SRF_TEST(wire, frame_round_trips_exactly) {
    const srf::Limits limits{};
    srf::ValidationResult result;
    const std::vector<std::byte> bytes = encode(sample_frame(), limits, result);
    SRF_EXPECT(result.ok());
    SRF_EXPECT_EQ(bytes.size(), srf::kFrameHeaderBytes + 8 + srf::kFrameTrailerBytes);
    srf::Frame decoded{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(srf::decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()), limits,
                                 decoded, decode_result));
    SRF_EXPECT(decode_result.ok());
    SRF_EXPECT_EQ(static_cast<int>(decoded.message), static_cast<int>(srf::MessageId::Get));
    SRF_EXPECT_EQ(decoded.request_id, 42u);
    SRF_EXPECT_EQ(decoded.payload.size(), static_cast<std::size_t>(8));
}

SRF_TEST(wire, every_structural_defect_is_rejected) {
    const srf::Limits limits{};
    srf::ValidationResult result;
    const std::vector<std::byte> good = encode(sample_frame(), limits, result);

    auto expect_rejected = [&](std::vector<std::byte> bytes, srf::ReasonCode code,
                               const char* label) {
        srf::Frame frame{};
        srf::ValidationResult decode_result;
        const bool ok = srf::decode_frame(
            std::span<const std::byte>(bytes.data(), bytes.size()), limits, frame, decode_result);
        SRF_EXPECT(!ok);
        SRF_EXPECT(decode_result.contains(code));
        (void)label;
    };

    expect_rejected({}, srf::ReasonCode::WireHeaderTruncated, "empty");
    expect_rejected(std::vector<std::byte>(good.begin(), good.begin() + 4),
                    srf::ReasonCode::WireHeaderTruncated, "partial header");

    std::vector<std::byte> bad_magic = good;
    bad_magic[0] = std::byte{'X'};
    repair_crcs(bad_magic);
    expect_rejected(std::move(bad_magic), srf::ReasonCode::WireMagicMismatch, "magic");

    std::vector<std::byte> bad_version = good;
    set_u16(bad_version, 4, 99);
    repair_crcs(bad_version);
    expect_rejected(std::move(bad_version), srf::ReasonCode::WireUnsupportedVersion, "version");

    std::vector<std::byte> bad_message = good;
    set_u16(bad_message, 6, 0xBEEF);
    repair_crcs(bad_message);
    expect_rejected(std::move(bad_message), srf::ReasonCode::WireUnknownMessageId, "message");

    std::vector<std::byte> bad_flags = good;
    set_u16(bad_flags, 8, 1);
    repair_crcs(bad_flags);
    expect_rejected(std::move(bad_flags), srf::ReasonCode::WireUnsupportedFlags, "flags");

    std::vector<std::byte> bad_reserved = good;
    set_u16(bad_reserved, 10, 7);
    repair_crcs(bad_reserved);
    expect_rejected(std::move(bad_reserved), srf::ReasonCode::WireUnsupportedFlags, "reserved");

    std::vector<std::byte> bad_header_crc = good;
    bad_header_crc[10] = std::byte{9};
    expect_rejected(std::move(bad_header_crc), srf::ReasonCode::WireIntegrityMismatch, "header crc");

    std::vector<std::byte> bad_payload_crc = good;
    bad_payload_crc[srf::kFrameHeaderBytes] = std::byte{0xFF};
    expect_rejected(std::move(bad_payload_crc), srf::ReasonCode::WireIntegrityMismatch,
                    "payload crc");

    std::vector<std::byte> trailing = good;
    trailing.push_back(std::byte{0});
    expect_rejected(std::move(trailing), srf::ReasonCode::WireTrailingBytes, "trailing");

    std::vector<std::byte> truncated(good.begin(), good.end() - 1);
    expect_rejected(std::move(truncated), srf::ReasonCode::WirePayloadLengthMismatch, "short");
}

SRF_TEST(wire, oversized_frame_length_is_rejected_at_decode) {
    srf::Limits limits{};
    limits.max_frame_bytes = 1024;
    srf::ValidationResult result;
    std::vector<std::byte> bytes = encode(sample_frame(), limits, result);
    set_u32(bytes, 12, 0xFFFF);
    repair_crcs(bytes);
    // The trailer no longer sits where the header claims, so this is rejected.
    srf::Frame frame{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(!srf::decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()), limits,
                                  frame, decode_result));
}

SRF_TEST(wire, assembler_handles_partial_and_coalesced_frames) {
    const srf::Limits limits{};
    srf::ValidationResult result;
    const std::vector<std::byte> first = encode(sample_frame(), limits, result);
    srf::Frame second_frame = sample_frame();
    second_frame.request_id = 43;
    const std::vector<std::byte> second = encode(second_frame, limits, result);

    std::vector<std::byte> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());

    srf::FrameAssembler assembler{};
    std::vector<std::uint64_t> seen;
    for (const std::byte value : stream) {
        assembler.feed(std::span<const std::byte>(&value, 1));
        for (;;) {
            srf::Frame frame{};
            srf::ValidationResult decode_result;
            if (!assembler.next(limits, frame, decode_result)) {
                break;
            }
            seen.push_back(frame.request_id);
        }
    }
    SRF_EXPECT_EQ(seen.size(), static_cast<std::size_t>(2));
    SRF_EXPECT_EQ(seen[0], 42u);
    SRF_EXPECT_EQ(seen[1], 43u);
    SRF_EXPECT_EQ(assembler.buffered(), static_cast<std::size_t>(0));
}

SRF_TEST(wire, assembler_rejects_an_oversized_announced_length) {
    srf::Limits limits{};
    limits.max_frame_bytes = 64;
    srf::ValidationResult result;
    std::vector<std::byte> bytes = encode(sample_frame(), limits, result);
    set_u32(bytes, 12, 4096);
    repair_crcs(bytes);
    srf::FrameAssembler assembler{};
    assembler.feed(std::span<const std::byte>(bytes.data(), bytes.size()));
    srf::Frame frame{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(!assembler.next(limits, frame, decode_result));
    SRF_EXPECT_REASON(decode_result, srf::ReasonCode::WireAssemblyOverflow);
    SRF_EXPECT(assembler.overflowed());
}

SRF_TEST(wire, payload_codecs_round_trip_and_reject_trailing_bytes) {
    const srf::Limits limits{};
    srf::ValidationResult result;

    const srf::HelloRequest hello{srf::WorkerBootId{7}, srf::PublisherId{8}, srf::ScopeId{9},
                                  srf::CoordinatorEpoch{3}};
    srf::HelloRequest decoded_hello{};
    SRF_EXPECT(srf::decode_hello(srf::as_bytes(srf::encode_hello(hello)), limits, decoded_hello,
                                 result));
    SRF_EXPECT(decoded_hello.boot == hello.boot && decoded_hello.publisher == hello.publisher &&
               decoded_hello.scope == hello.scope && decoded_hello.epoch == hello.epoch);

    srf::ByteBuffer extended = srf::encode_hello(hello);
    extended.push_back(std::byte{0});
    srf::HelloRequest trailing{};
    srf::ValidationResult trailing_result;
    SRF_EXPECT(!srf::decode_hello(srf::as_bytes(extended), limits, trailing, trailing_result));
    SRF_EXPECT_REASON(trailing_result, srf::ReasonCode::WireTrailingBytes);

    srf::MutationRequest mutation{};
    mutation.attempt = srf::MutationAttemptId{5};
    mutation.draft.id = srf::test::list_id(1);
    mutation.draft.scope = srf::ScopeId{srf::test::kScope};
    mutation.draft.profile = srf::SegmentProfileId{0x0001'0001ull};
    mutation.draft.segments = {srf::test::node_segment(1)};
    srf::ValidationResult encode_result;
    const srf::ByteBuffer encoded = srf::encode_mutation(mutation, limits, encode_result);
    SRF_EXPECT(encode_result.ok());
    srf::MutationRequest decoded_mutation{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(srf::decode_mutation(srf::as_bytes(encoded), limits, decoded_mutation,
                                    decode_result));
    SRF_EXPECT_EQ(decoded_mutation.attempt.value(), 5u);
    SRF_EXPECT_EQ(decoded_mutation.draft.id, mutation.draft.id);
    SRF_EXPECT_EQ(decoded_mutation.draft.segments.size(), static_cast<std::size_t>(1));
    SRF_EXPECT_EQ(decoded_mutation.draft.segments[0].id, mutation.draft.segments[0].id);
    SRF_EXPECT(decoded_mutation.draft.segments[0].node == mutation.draft.segments[0].node);

    // An unset encoding is legal input and survives the round trip unchanged.
    SRF_EXPECT_EQ(static_cast<int>(decoded_mutation.draft.segments[0].encoding),
                  static_cast<int>(srf::SegmentEncodingId::Invalid));

    // Unknown enum values are rejected, never defaulted.
    srf::MutationRequest bad{};
    bad.attempt = srf::MutationAttemptId{6};
    bad.draft.id = srf::test::list_id(1);
    srf::Segment bad_segment = srf::test::node_segment(1);
    bad_segment.kind = srf::SegmentKind::Invalid;
    bad.draft.segments = {bad_segment};
    srf::ValidationResult bad_encode;
    const srf::ByteBuffer bad_bytes = srf::encode_mutation(bad, limits, bad_encode);
    SRF_EXPECT(bad_encode.ok());
    srf::MutationRequest ignored{};
    srf::ValidationResult bad_result;
    SRF_EXPECT(!srf::decode_mutation(srf::as_bytes(bad_bytes), limits, ignored, bad_result));
    SRF_EXPECT_REASON(bad_result, srf::ReasonCode::WireUnknownEnumValue);

    // A declared segment count above the configured depth is rejected at decode.
    srf::Limits small{};
    small.max_segments_per_list = 0;
    srf::MutationRequest depth_request{};
    srf::ValidationResult depth_result;
    SRF_EXPECT(!srf::decode_mutation(srf::as_bytes(encoded), small, depth_request, depth_result));
    SRF_EXPECT_REASON(depth_result, srf::ReasonCode::LimitMaxSegmentsPerList);
}

SRF_TEST(wire, response_round_trips_with_reasons_and_summaries) {
    const srf::Limits limits{};
    srf::Response response{};
    response.request_id = 11;
    response.status = srf::StatusCode::Rejected;
    response.reasons.push_back(srf::Reason{srf::ReasonCode::ProfileRepeatNotPermitted, 1, 0x42});
    response.truncated = true;
    response.list = srf::test::list_id(2);
    response.generation = srf::SegmentListGeneration{4};
    response.digest = srf::Digest128{0xAA, 0xBB};
    response.idempotent_replay = true;
    response.epoch = srf::CoordinatorEpoch{6};
    response.session = srf::WorkerSessionId{0x77};
    srf::ListSummary summary{};
    summary.list = srf::test::list_id(2);
    summary.generation = srf::SegmentListGeneration{4};
    summary.state = srf::LifecycleState::Active;
    summary.currentness = srf::Currentness::Current;
    summary.profile = srf::SegmentProfileId{0x0001'0001ull};
    summary.profile_generation = srf::SegmentProfileGeneration{1};
    summary.digest = srf::Digest128{1, 2};
    summary.segment_count = 3;
    summary.epoch = srf::CoordinatorEpoch{6};
    response.summaries.push_back(summary);

    srf::ValidationResult encode_result;
    const srf::ByteBuffer encoded = srf::encode_response(response, limits, encode_result);
    SRF_EXPECT(encode_result.ok());
    srf::Response decoded{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(srf::decode_response(srf::as_bytes(encoded), limits, decoded, decode_result));
    SRF_EXPECT(decoded.request_id == response.request_id);
    SRF_EXPECT(decoded.status == response.status);
    SRF_EXPECT_EQ(decoded.reasons.size(), static_cast<std::size_t>(1));
    SRF_EXPECT(decoded.reasons[0].code == srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT(decoded.truncated);
    SRF_EXPECT(decoded.idempotent_replay);
    SRF_EXPECT(decoded.session == response.session);
    SRF_EXPECT_EQ(decoded.summaries.size(), static_cast<std::size_t>(1));
    SRF_EXPECT(decoded.summaries[0].state == srf::LifecycleState::Active);
    SRF_EXPECT(decoded.summaries[0].segment_count == 3u);

    srf::ByteBuffer extended = encoded;
    extended.push_back(std::byte{1});
    srf::Response trailing{};
    srf::ValidationResult trailing_result;
    SRF_EXPECT(!srf::decode_response(srf::as_bytes(extended), limits, trailing, trailing_result));
    SRF_EXPECT_REASON(trailing_result, srf::ReasonCode::WireTrailingBytes);
}

SRF_TEST(wire, message_names_are_total_for_the_known_range) {
    for (std::uint16_t raw = 0; raw <= static_cast<std::uint16_t>(srf::MessageId::Response);
         ++raw) {
        const auto id = static_cast<srf::MessageId>(raw);
        SRF_EXPECT(!srf::message_id_name(id).empty());
    }
    SRF_EXPECT_EQ(srf::message_id_name(srf::MessageId::Invalid), std::string_view{"Invalid"});
}
