// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial input: every case below must be rejected with an EXACT reason code,
// never merely "something failed". Three attack surfaces are covered:
//
//   * the durable persistence image (byte surgery on a real image, including a
//     complete single-byte corruption sweep),
//   * the wire codec (frame header, integrity trailers, payload codecs and the
//     streaming FrameAssembler),
//   * validation precedence (multi-defect requests proving that an earlier phase
//     dominates and that the later phase's reason is still reported).
//
// The final section proves that every bound in srf::Limits is consulted.
#include "fixtures.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

using srf::test::Harness;

/// A store with explicit limits, evidence and persistence. Used wherever the
/// Harness defaults would be too permissive to observe a bound.
[[nodiscard]] std::unique_ptr<srf::SegmentListStore> make_store(
    const srf::Limits& limits, std::shared_ptr<srf::SyntheticEvidence> evidence,
    std::shared_ptr<srf::MemoryPersistence> memory) {
    srf::StoreConfig config{};
    config.limits = limits;
    config.evidence = std::move(evidence);
    config.persistence = std::move(memory);
    return std::make_unique<srf::SegmentListStore>(std::move(config));
}

// --------------------------------------------------------------- persistence
/// Byte surgery over a real durable image. repair() recomputes both integrity
/// trailers so that a targeted structural corruption reaches the structural
/// checks instead of being stopped by the CRC.
class PersistImage {
public:
    explicit PersistImage(std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
    [[nodiscard]] std::span<const std::byte> span() const {
        return {bytes_.data(), bytes_.size()};
    }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] std::uint64_t payload_length() const {
        std::uint64_t value = 0;
        if (bytes_.size() >= 16) {
            std::memcpy(&value, bytes_.data() + 8, sizeof(value));
        }
        return value;
    }
    [[nodiscard]] bool in_range(std::size_t offset, std::size_t length) const {
        return offset <= bytes_.size() && length <= bytes_.size() - offset;
    }
    void set_u16(std::size_t offset, std::uint16_t value) {
        if (in_range(offset, sizeof(value))) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_u32(std::size_t offset, std::uint32_t value) {
        if (in_range(offset, sizeof(value))) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_u64(std::size_t offset, std::uint64_t value) {
        if (in_range(offset, sizeof(value))) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_byte(std::size_t offset, std::uint8_t value) {
        if (in_range(offset, 1)) {
            bytes_[offset] = static_cast<std::byte>(value);
        }
    }
    void flip_bit(std::size_t offset, std::uint8_t mask) {
        if (in_range(offset, 1)) {
            bytes_[offset] = static_cast<std::byte>(
                static_cast<std::uint8_t>(bytes_[offset]) ^ mask);
        }
    }
    void repair() {
        if (bytes_.size() < 24) {
            return;
        }
        set_u32(20, srf::crc32c(std::span<const std::byte>(bytes_.data(), 20)));
        const std::uint64_t length = payload_length();
        if (length > bytes_.size() - 28) {
            return;
        }
        const auto payload = static_cast<std::size_t>(length);
        const std::uint32_t crc =
            srf::crc32c(std::span<const std::byte>(bytes_.data() + 24, payload));
        set_u32(24 + payload, crc);
    }
    /// Offset of the 6-byte header of the first record of the given type.
    [[nodiscard]] std::size_t find_record_header(std::uint16_t type) const {
        if (bytes_.size() < 24) {
            return bytes_.size();
        }
        std::size_t cursor = 24;
        const auto end = static_cast<std::size_t>(24 + payload_length());
        if (end > bytes_.size()) {
            return bytes_.size();
        }
        while (cursor + 6 <= end) {
            std::uint16_t record_type = 0;
            std::uint32_t length = 0;
            std::memcpy(&record_type, bytes_.data() + cursor, sizeof(record_type));
            std::memcpy(&length, bytes_.data() + cursor + 2, sizeof(length));
            if (record_type == type) {
                return cursor;
            }
            cursor += 6 + static_cast<std::size_t>(length);
        }
        return bytes_.size();
    }

private:
    std::vector<std::byte> bytes_;
};

constexpr std::uint16_t kProfileRecord = 2;
constexpr std::uint16_t kListRecord = 4;

[[nodiscard]] srf::ByteBuffer encode_state(const srf::DurableState& state,
                                           const srf::Limits& limits) {
    srf::ValidationResult result;
    return srf::encode_durable_state(state, limits, result);
}

void decode_fails(std::span<const std::byte> bytes, const srf::Limits& limits,
                  srf::ReasonCode expected, srf::test::Context& srf_ctx) {
    srf::DurableState decoded{};
    srf::ValidationResult result;
    const bool ok = srf::decode_durable_state(bytes, limits, decoded, result);
    SRF_EXPECT(!ok);
    srf_ctx.record(result.primary().code == expected,
                   std::string("primary ") + std::string(srf::reason_code_name(expected)) +
                       " but was " + std::string(result.primary_name()),
                   __FILE__, __LINE__);
}

// ---------------------------------------------------------------------- wire
/// Byte surgery over a real encoded frame. The layout is 4 magic bytes, u16
/// version, u16 message, u16 flags, u16 reserved, u32 payload length, u64
/// request id, u32 header CRC, payload, u32 payload CRC.
class WireImage {
public:
    explicit WireImage(std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
    [[nodiscard]] std::span<const std::byte> span() const {
        return {bytes_.data(), bytes_.size()};
    }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    void set_u16(std::size_t offset, std::uint16_t value) {
        if (offset + sizeof(value) <= bytes_.size()) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_u32(std::size_t offset, std::uint32_t value) {
        if (offset + sizeof(value) <= bytes_.size()) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_byte(std::size_t offset, std::uint8_t value) {
        if (offset < bytes_.size()) {
            bytes_[offset] = static_cast<std::byte>(value);
        }
    }
    void repair_header() {
        if (bytes_.size() < 28) {
            return;
        }
        set_u32(24, srf::crc32c(std::span<const std::byte>(bytes_.data(), 24)));
    }

private:
    std::vector<std::byte> bytes_;
};

/// The mutation payload layout of srf/wire.hpp, written field by field.
[[nodiscard]] srf::ByteBuffer mutation_payload(const std::vector<srf::Segment>& segments,
                                               std::uint32_t declared_count) {
    srf::ByteWriter w;
    w.u64(0xE001);
    w.u64(0xC001);
    w.u64(0x100);
    w.u64(0x0001'0001ull);
    w.u64(1);
    w.u64(0);
    w.u64(0);
    w.u8(static_cast<std::uint8_t>(srf::Strictness::Strict));
    w.u64(1);
    w.u64(1);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.u8(0);
    w.u32(declared_count);
    for (const srf::Segment& s : segments) {
        w.u16(static_cast<std::uint16_t>(s.kind));
        w.u16(static_cast<std::uint16_t>(s.encoding));
        w.u64(s.id.value());
        w.u64(s.generation.value());
        w.u64(s.node.value());
        w.u64(s.adjacency.value());
        w.u64(s.endpoint.value());
        w.u64(s.binding.value());
        w.u64(s.policy.value());
        w.u64(s.policy_generation.value());
        w.u64(s.topology.value());
        w.u16(static_cast<std::uint16_t>(s.payload.size()));
        w.bytes(srf::as_bytes(s.payload));
    }
    return std::move(w).take();
}

void decode_mutation_fails(const srf::ByteBuffer& payload, const srf::Limits& limits,
                           srf::ReasonCode expected, srf::test::Context& srf_ctx) {
    srf::MutationRequest request{};
    srf::ValidationResult result;
    const bool ok = srf::decode_mutation(srf::as_bytes(payload), limits, request, result);
    SRF_EXPECT(!ok);
    srf_ctx.record(result.primary().code == expected,
                   std::string("primary ") + std::string(srf::reason_code_name(expected)) +
                       " but was " + std::string(result.primary_name()),
                   __FILE__, __LINE__);
}

// ------------------------------------------------------------------ profiles
/// A profile with no capability requirements, so the capability phase stays
/// silent and later phases can be isolated.
[[nodiscard]] srf::SegmentProfile free_profile() {
    srf::SegmentProfile profile{};
    profile.id = srf::SegmentProfileId{0x0001'0200ull};
    profile.generation = srf::SegmentProfileGeneration{1};
    profile.name = "adversarial-free-v1";
    profile.support = srf::SupportClassification::AbstractSynthetic;
    profile.allowed_kinds_mask = static_cast<std::uint16_t>(
        srf::kind_bit(srf::SegmentKind::Node) | srf::kind_bit(srf::SegmentKind::Adjacency) |
        srf::kind_bit(srf::SegmentKind::Endpoint) | srf::kind_bit(srf::SegmentKind::Binding) |
        srf::kind_bit(srf::SegmentKind::Policy));
    profile.max_depth = 8;
    profile.payload_width = 8;
    profile.allow_empty = false;
    profile.allow_consecutive_repeats = false;
    profile.allow_nonconsecutive_repeats = false;
    profile.allow_loose = false;
    return profile;
}

struct RequestFixture {
    srf::Limits limits{};
    srf::AuthorityView authority{};
    srf::CallerIdentity caller{};
    srf::SegmentProfile profile{};
    srf::FabricEvidence evidence{};
    srf::ValidationRequest request{};

    RequestFixture() {
        authority.epoch = srf::CoordinatorEpoch{1};
        srf::PublisherState publisher{};
        publisher.id = srf::PublisherId{0x9001};
        publisher.boot = srf::WorkerBootId{0xB001};
        publisher.scope = srf::ScopeId{0x100};
        publisher.epoch = srf::CoordinatorEpoch{1};
        publisher.alive = true;
        publisher.fenced = false;
        authority.publishers.push_back(publisher);

        caller.epoch = srf::CoordinatorEpoch{1};
        caller.publisher = srf::PublisherId{0x9001};
        caller.boot = srf::WorkerBootId{0xB001};
        caller.scope = srf::ScopeId{0x100};
        caller.attempt = srf::MutationAttemptId{0xE001};

        profile = free_profile();
        evidence.topology = srf::TopologyGeneration{1};
        evidence.capability = srf::CapabilityGeneration{1};
        evidence.path_authority = srf::PathAuthorityGeneration{1};

        request.limits = &limits;
        request.caller = &caller;
        request.authority = &authority;
        request.list_id = srf::test::list_id(1);
        request.scope = srf::ScopeId{0x100};
        request.profile_id = profile.id;
        request.profile = &profile;
        request.profile_found = true;
        request.evidence = &evidence;
    }
};

[[nodiscard]] srf::FabricEvidence evidence_with_segment(srf::SegmentId id, srf::SegmentKind kind,
                                                        srf::SegmentGeneration generation) {
    srf::FabricEvidence evidence{};
    evidence.topology = srf::TopologyGeneration{1};
    evidence.capability = srf::CapabilityGeneration{1};
    evidence.path_authority = srf::PathAuthorityGeneration{1};
    srf::SegmentRecord record{};
    record.id = id;
    record.kind = kind;
    record.generation = generation;
    record.scope = srf::ScopeId{0x100};
    evidence.segments.push_back(record);
    return evidence;
}

} // namespace
// ===========================================================================
// Persistence: the durable image
// ===========================================================================

SRF_TEST(adversarial, persistence_empty_bad_magic_bad_version_and_flags) {
    const srf::Limits limits{};
    {
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(
            !srf::decode_durable_state(std::span<const std::byte>{}, limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistEmpty);
    }
    Harness harness;
    SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
    const std::span<const std::byte> valid = harness.memory()->bytes();
    SRF_EXPECT(valid.size() > 64);
    {
        PersistImage image(valid);
        image.set_byte(0, 'X');
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadMagic, srf_ctx);
    }
    {
        PersistImage image(valid);
        image.set_u16(4, 99);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadVersion, srf_ctx);
    }
    {
        // Non-zero flags are a version-level rejection, never ignored.
        PersistImage image(valid);
        image.set_u16(6, 1);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadVersion, srf_ctx);
    }
}

SRF_TEST(adversarial, persistence_truncation_at_several_lengths) {
    const srf::Limits limits{};
    Harness harness;
    SRF_EXPECT(harness.create(1, {srf::test::node_segment(1), srf::test::node_segment(2)}).ok());
    SRF_EXPECT(harness.create(2, {srf::test::adjacency_segment(1)}).ok());
    const std::span<const std::byte> valid = harness.memory()->bytes();
    const std::size_t full = valid.size();
    PersistImage probe(valid);
    const auto payload = static_cast<std::size_t>(probe.payload_length());
    SRF_EXPECT_EQ(full, static_cast<std::size_t>(24 + payload + 4));
    const std::size_t lengths[] = {1, 3, 8, 20, 23, 24, 27, 28, 40, payload,
                                   full - 4, full - 1};
    for (const std::size_t length : lengths) {
        if (length == 0 || length >= full) {
            continue;
        }
        const std::vector<std::byte> truncated(
            valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        // Below the header the image is truncated; above it the header still
        // announces the full payload, which is then an overflow of the image.
        const srf::ReasonCode expected = (length >= 28 && payload > length)
                                             ? srf::ReasonCode::PersistOverflow
                                             : srf::ReasonCode::PersistTruncated;
        srf::DurableState decoded{};
        srf::ValidationResult result;
        const bool ok = srf::decode_durable_state(
            std::span<const std::byte>(truncated.data(), truncated.size()), limits, decoded,
            result);
        SRF_EXPECT(!ok);
    srf_ctx.record(result.primary().code == expected,
                   std::string("primary ") + std::string(srf::reason_code_name(expected)) +
                       " but was " + std::string(result.primary_name()),
                   __FILE__, __LINE__);
    }
}

SRF_TEST(adversarial, persistence_trailing_bytes_and_both_integrity_trailers) {
    const srf::Limits limits{};
    Harness harness;
    SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
    const std::span<const std::byte> valid = harness.memory()->bytes();
    {
        std::vector<std::byte> extended(valid.begin(), valid.end());
        extended.push_back(std::byte{0});
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(
            std::span<const std::byte>(extended.data(), extended.size()), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistTrailingBytes);
    }
    {
        std::vector<std::byte> extended(valid.begin(), valid.end());
        extended.insert(extended.end(), 4, std::byte{0xAB});
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(
            std::span<const std::byte>(extended.data(), extended.size()), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistTrailingBytes);
    }
    {
        // Header CRC: the flags byte is flipped without repairing the trailer.
        PersistImage image(valid);
        image.flip_bit(6, 0x01);
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadIntegrity, srf_ctx);
    }
    {
        // Payload CRC: the first trailer byte is flipped.
        PersistImage image(valid);
        const auto payload = static_cast<std::size_t>(image.payload_length());
        image.flip_bit(24 + payload, 0x01);
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadIntegrity, srf_ctx);
    }
    {
        // Payload CRC: the last trailer byte is flipped.
        PersistImage image(valid);
        image.flip_bit(image.size() - 1, 0x80);
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadIntegrity, srf_ctx);
    }
}

SRF_TEST(adversarial, persistence_structural_state_defects) {
    const srf::Limits limits{};
    Harness harness;
    SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
    const srf::DurableState valid = harness.store().export_state();
    SRF_EXPECT(!valid.lists.empty());
    if (valid.lists.empty()) {
        return;
    }
    {
        srf::DurableState state = valid;
        state.lists.front().generation = srf::SegmentListGeneration{};
        decode_fails(srf::as_bytes(encode_state(state, limits)), limits,
                     srf::ReasonCode::PersistImpossibleGeneration, srf_ctx);
    }
    {
        srf::DurableState state = valid;
        state.lists.push_back(state.lists.front());
        decode_fails(srf::as_bytes(encode_state(state, limits)), limits,
                     srf::ReasonCode::PersistDuplicateIdentity, srf_ctx);
    }
    {
        srf::DurableState state = valid;
        state.lists.front().content.profile = srf::SegmentProfileId{0xDEAD};
        decode_fails(srf::as_bytes(encode_state(state, limits)), limits,
                     srf::ReasonCode::PersistDanglingReference, srf_ctx);
    }
    {
        srf::DurableState state = valid;
        state.lists.front().content.segments.front().kind = srf::SegmentKind::Invalid;
        decode_fails(srf::as_bytes(encode_state(state, limits)), limits,
                     srf::ReasonCode::PersistKindInvalid, srf_ctx);
    }
    {
        srf::DurableState state = valid;
        state.lists.front().content.segments.front().encoding = srf::SegmentEncodingId::Invalid;
        decode_fails(srf::as_bytes(encode_state(state, limits)), limits,
                     srf::ReasonCode::PersistEncodingInvalid, srf_ctx);
    }
    {
        srf::DurableState state = valid;
        state.lists.front().content.segments.push_back(srf::test::node_segment(2));
        state.lists.front().content.segments.push_back(srf::test::node_segment(3));
        srf::Limits tight = limits;
        tight.max_segments_per_list = 2;
        decode_fails(srf::as_bytes(encode_state(state, tight)), tight,
                     srf::ReasonCode::PersistDepthInvalid, srf_ctx);
    }
    {
        srf::DurableState state = valid;
        state.lists.front().content_digest = srf::Digest128{1, 2};
        decode_fails(srf::as_bytes(encode_state(state, limits)), limits,
                     srf::ReasonCode::PersistDigestMismatch, srf_ctx);
    }
    {
        // A generation of zero on a segment is impossible, not merely unusual.
        srf::DurableState state = valid;
        state.lists.front().content.segments.front().generation = srf::SegmentGeneration{};
        srf::DurableState decoded{};
        srf::ValidationResult result;
        const srf::ByteBuffer bytes = encode_state(state, limits);
        SRF_EXPECT(!bytes.empty());
        const bool ok = srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result);
        SRF_EXPECT(!ok);
        // Every semantic field of a segment is covered by the content digest, so a
        // tampered generation can never be accepted as a valid record.
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistDigestMismatch);
    }
}

SRF_TEST(adversarial, persistence_absurd_counts_lengths_and_oversized_records) {
    const srf::Limits limits{};
    Harness harness;
    SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
    const std::span<const std::byte> valid = harness.memory()->bytes();
    {
        // An absurd record count in the header.
        PersistImage image(valid);
        image.set_u32(16, 0xFFFF);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistAbsurdCount, srf_ctx);
    }
    {
        // An absurd record length in a record header.
        PersistImage image(valid);
        const std::size_t record = image.find_record_header(kListRecord);
        SRF_EXPECT(record < image.size());
        image.set_u32(record + 2, 0x7FFF'FFFF);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistAbsurdCount, srf_ctx);
    }
    {
        // An unknown record type is a decode rejection, never a silent skip.
        PersistImage image(valid);
        const std::size_t record = image.find_record_header(kListRecord);
        SRF_EXPECT(record < image.size());
        image.set_u16(record, 0x00FF);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistBadRecord, srf_ctx);
    }
    {
        // A payload length beyond the image.
        PersistImage image(valid);
        image.set_u64(8, 0x0000'FFFF'FFFF'FFFFull);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistOverflow, srf_ctx);
    }
    {
        // A record larger than the configured record bound is refused on encode.
        srf::Limits tight = limits;
        tight.max_persistence_record_bytes = 32;
        srf::ValidationResult result;
        const srf::ByteBuffer bytes =
            srf::encode_durable_state(harness.store().export_state(), tight, result);
        SRF_EXPECT(bytes.empty());
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::LimitMaxPersistenceRecordBytes);
    }
    {
        // The profile record is reachable too: a corrupted profile length.
        PersistImage image(valid);
        const std::size_t record = image.find_record_header(kProfileRecord);
        SRF_EXPECT(record < image.size());
        image.set_u32(record + 2, 0x7FFF'FFFF);
        image.repair();
        decode_fails(image.span(), limits, srf::ReasonCode::PersistAbsurdCount, srf_ctx);
    }
}

SRF_TEST(adversarial, persistence_every_single_byte_corruption_is_detected) {
    const srf::Limits limits{};
    Harness harness;
    SRF_EXPECT(harness.create(1, {srf::test::node_segment(1), srf::test::node_segment(2)}).ok());
    SRF_EXPECT(harness.create(2, {srf::test::adjacency_segment(1)}).ok());
    const std::span<const std::byte> image = harness.memory()->bytes();
    const std::vector<std::byte> original(image.begin(), image.end());
    SRF_EXPECT(original.size() > 200);
    std::size_t undetected = 0;
    for (std::size_t offset = 0; offset < original.size(); ++offset) {
        srf::MemoryPersistence corrupted{};
        corrupted.set_bytes(std::span<const std::byte>(original.data(), original.size()));
        SRF_EXPECT(corrupted.corrupt_byte(offset, 0x01));
        srf::LoadReport report{};
        if (corrupted.load(report, limits)) {
            ++undetected;
        }
    }
    SRF_EXPECT_EQ(undetected, static_cast<std::size_t>(0));
}

// ===========================================================================
// Wire: the frame codec
// ===========================================================================

SRF_TEST(adversarial, wire_frame_header_defects) {
    const srf::Limits limits{};
    srf::Frame frame{};
    frame.message = srf::MessageId::Get;
    frame.request_id = 0x1234;
    frame.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    srf::ValidationResult encode_result;
    const srf::ByteBuffer encoded = srf::encode_frame(frame, limits, encode_result);
    SRF_EXPECT(encode_result.ok());
    SRF_EXPECT_EQ(encoded.size(), static_cast<std::size_t>(28 + 3 + 4));
    {
        srf::Frame decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(srf::decode_frame(srf::as_bytes(encoded), limits, decoded, result));
        SRF_EXPECT(decoded.message == srf::MessageId::Get);
        SRF_EXPECT_EQ(decoded.request_id, static_cast<std::uint64_t>(0x1234));
        SRF_EXPECT_EQ(decoded.payload.size(), static_cast<std::size_t>(3));
    }
    const auto expect_frame_fails = [&srf_ctx](std::span<const std::byte> bytes,
                                               const srf::Limits& use_limits,
                                               srf::ReasonCode expected) {
        srf::Frame decoded{};
        srf::ValidationResult result;
        const bool ok = srf::decode_frame(bytes, use_limits, decoded, result);
        SRF_EXPECT(!ok);
    srf_ctx.record(result.primary().code == expected,
                   std::string("primary ") + std::string(srf::reason_code_name(expected)) +
                       " but was " + std::string(result.primary_name()),
                   __FILE__, __LINE__);
    };
    {
        WireImage image(srf::as_bytes(encoded));
        image.set_byte(0, 'X');
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireMagicMismatch);
    }
    {
        WireImage image(srf::as_bytes(encoded));
        image.set_u16(4, 99);
        image.repair_header();
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireUnsupportedVersion);
    }
    {
        WireImage image(srf::as_bytes(encoded));
        image.set_u16(6, 999);
        image.repair_header();
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireUnknownMessageId);
    }
    {
        WireImage image(srf::as_bytes(encoded));
        image.set_u16(8, 4);
        image.repair_header();
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireUnsupportedFlags);
    }
    {
        WireImage image(srf::as_bytes(encoded));
        image.set_u16(10, 1);
        image.repair_header();
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireUnsupportedFlags);
    }
    {
        // Header CRC: a bit flipped in the message id, trailer left stale.
        WireImage image(srf::as_bytes(encoded));
        image.set_byte(6, static_cast<std::uint8_t>(
                              static_cast<std::uint8_t>(image.bytes()[6]) ^ 0x01u));
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireIntegrityMismatch);
    }
    {
        // Payload CRC: a bit flipped in the trailer.
        WireImage image(srf::as_bytes(encoded));
        image.set_byte(image.size() - 1,
                       static_cast<std::uint8_t>(
                           static_cast<std::uint8_t>(image.bytes().back()) ^ 0x01u));
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireIntegrityMismatch);
    }
    {
        // Payload CRC: a bit flipped in the payload itself.
        WireImage image(srf::as_bytes(encoded));
        image.set_byte(28, static_cast<std::uint8_t>(
                               static_cast<std::uint8_t>(image.bytes()[28]) ^ 0x80u));
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WireIntegrityMismatch);
    }
    {
        std::vector<std::byte> extended(encoded.begin(), encoded.end());
        extended.push_back(std::byte{0});
        expect_frame_fails(std::span<const std::byte>(extended.data(), extended.size()), limits,
                           srf::ReasonCode::WireTrailingBytes);
    }
    {
        expect_frame_fails(std::span<const std::byte>(encoded.data(), 20), limits,
                           srf::ReasonCode::WireHeaderTruncated);
        expect_frame_fails(std::span<const std::byte>(encoded.data(), 31), limits,
                           srf::ReasonCode::WireHeaderTruncated);
        expect_frame_fails(std::span<const std::byte>{}, limits,
                           srf::ReasonCode::WireHeaderTruncated);
    }
    {
        // The announced payload length is larger than the bytes present.
        WireImage image(srf::as_bytes(encoded));
        image.set_u32(12, 4);
        image.repair_header();
        expect_frame_fails(image.span(), limits, srf::ReasonCode::WirePayloadLengthMismatch);
    }
    {
        // An oversized announced payload is a limit rejection, not a buffer growth.
        srf::Limits tight = limits;
        tight.max_frame_bytes = 8;
        WireImage image(srf::as_bytes(encoded));
        image.set_u32(12, 9);
        image.repair_header();
        expect_frame_fails(image.span(), tight, srf::ReasonCode::LimitMaxFrameBytes);
    }
    {
        // The encoder refuses an oversized payload too.
        srf::Frame large{};
        large.message = srf::MessageId::ListSummaries;
        large.payload.assign(64, std::byte{0});
        srf::Limits tight = limits;
        tight.max_frame_bytes = 16;
        srf::ValidationResult result;
        SRF_EXPECT(srf::encode_frame(large, tight, result).empty());
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::LimitMaxFrameBytes);
    }
    {
        // The encoder refuses an unknown message id.
        srf::Frame unknown{};
        unknown.message = srf::MessageId::Invalid;
        srf::ValidationResult result;
        SRF_EXPECT(srf::encode_frame(unknown, limits, result).empty());
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::WireUnknownMessageId);
    }
}

SRF_TEST(adversarial, wire_mutation_payload_defects) {
    const srf::Limits limits{};
    const std::vector<srf::Segment> good{srf::test::node_segment(1)};
    {
        // An unset encoding is normalizable and therefore accepted.
        srf::MutationRequest request{};
        srf::ValidationResult result;
        const srf::ByteBuffer payload = mutation_payload(good, 1);
        SRF_EXPECT(srf::decode_mutation(srf::as_bytes(payload), limits, request, result));
        SRF_EXPECT_EQ(request.draft.segments.size(), static_cast<std::size_t>(1));
        SRF_EXPECT_EQ(static_cast<int>(request.draft.segments[0].kind),
                      static_cast<int>(srf::SegmentKind::Node));
        SRF_EXPECT_EQ(static_cast<int>(request.draft.segments[0].encoding),
                      static_cast<int>(srf::SegmentEncodingId::Invalid));
    }
    {
        // The exact required encoding is accepted as well.
        srf::Segment explicit_node = srf::test::node_segment(1);
        explicit_node.encoding = srf::SegmentEncodingId::AbstractNodeV1;
        srf::MutationRequest request{};
        srf::ValidationResult result;
        const srf::ByteBuffer payload = mutation_payload({explicit_node}, 1);
        SRF_EXPECT(srf::decode_mutation(srf::as_bytes(payload), limits, request, result));
        SRF_EXPECT_EQ(static_cast<int>(request.draft.segments[0].encoding),
                      static_cast<int>(srf::SegmentEncodingId::AbstractNodeV1));
    }
    {
        srf::Segment bad = srf::test::node_segment(1);
        bad.kind = static_cast<srf::SegmentKind>(99);
        decode_mutation_fails(mutation_payload({bad}, 1), limits,
                              srf::ReasonCode::WireUnknownEnumValue, srf_ctx);
    }
    {
        // An encoding that is neither unset nor a known abstract encoding.
        srf::Segment bad = srf::test::node_segment(1);
        bad.encoding = static_cast<srf::SegmentEncodingId>(99);
        decode_mutation_fails(mutation_payload({bad}, 1), limits,
                              srf::ReasonCode::WireUnknownEnumValue, srf_ctx);
    }
    {
        // A declared segment count over the configured bound.
        const std::uint32_t over = limits.max_segments_per_list + 1;
        decode_mutation_fails(mutation_payload(good, over), limits,
                              srf::ReasonCode::LimitMaxSegmentsPerList, srf_ctx);
    }
    {
        // A segment payload over the configured bound.
        srf::Segment big = srf::test::node_segment(1);
        big.payload.assign(limits.max_segment_payload_bytes + 1, std::byte{7});
        decode_mutation_fails(mutation_payload({big}, 1), limits,
                              srf::ReasonCode::WireFieldOutOfRange, srf_ctx);
    }
    {
        // Trailing bytes after the last segment.
        srf::ByteBuffer payload = mutation_payload(good, 1);
        payload.push_back(std::byte{0});
        decode_mutation_fails(payload, limits, srf::ReasonCode::WireTrailingBytes, srf_ctx);
    }
    {
        // A truncated segment body.
        srf::ByteBuffer payload = mutation_payload(good, 1);
        payload.resize(payload.size() - 2);
        decode_mutation_fails(payload, limits, srf::ReasonCode::WireMalformedFrame, srf_ctx);
    }
    {
        // A truncated draft header.
        srf::ByteBuffer payload = mutation_payload(good, 1);
        payload.resize(40);
        decode_mutation_fails(payload, limits, srf::ReasonCode::WireMalformedFrame, srf_ctx);
    }
    {
        // An out-of-range strictness byte (offset 8 + 6 * 8).
        srf::ByteBuffer payload = mutation_payload(good, 1);
        payload[56] = std::byte{0};
        decode_mutation_fails(payload, limits, srf::ReasonCode::WireUnknownEnumValue, srf_ctx);
    }
    {
        // An out-of-range derived flag (offset 8 + 6 * 8 + 1 + 11 * 8).
        srf::ByteBuffer payload = mutation_payload(good, 1);
        payload[145] = std::byte{2};
        decode_mutation_fails(payload, limits, srf::ReasonCode::WireFieldOutOfRange, srf_ctx);
    }
    {
        // The encoder refuses more segments than the bound permits.
        srf::MutationRequest request{};
        request.draft.segments.assign(limits.max_segments_per_list + 1,
                                      srf::test::node_segment(1));
        srf::ValidationResult result;
        SRF_EXPECT(srf::encode_mutation(request, limits, result).empty());
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::LimitMaxSegmentsPerList);
    }
}

SRF_TEST(adversarial, wire_assembler_one_byte_at_a_time_across_boundaries) {
    const srf::Limits limits{};
    const auto encode = [&limits](srf::MessageId message, std::uint64_t request_id,
                                  std::vector<std::byte> payload) {
        srf::Frame frame{};
        frame.message = message;
        frame.request_id = request_id;
        frame.payload = std::move(payload);
        srf::ValidationResult result;
        return srf::encode_frame(frame, limits, result);
    };
    const srf::ByteBuffer first = encode(srf::MessageId::Hello, 11, {std::byte{1}});
    const srf::ByteBuffer second = encode(srf::MessageId::Get, 22, {std::byte{2}, std::byte{3}});
    const srf::ByteBuffer third = encode(srf::MessageId::Goodbye, 33, {});
    std::vector<std::byte> all(first.begin(), first.end());
    all.insert(all.end(), second.begin(), second.end());
    all.insert(all.end(), third.begin(), third.end());
    SRF_EXPECT_EQ(all.size(), static_cast<std::size_t>((28 + 1 + 4) + (28 + 2 + 4) + (28 + 0 + 4)));

    const auto drain = [&limits, &srf_ctx](srf::FrameAssembler& assembler) {
        std::vector<srf::Frame> frames;
        for (;;) {
            srf::Frame frame{};
            srf::ValidationResult result;
            if (!assembler.next(limits, frame, result)) {
                SRF_EXPECT(result.ok());
                break;
            }
            frames.push_back(frame);
            if (frames.size() > 8) {
                break;
            }
        }
        return frames;
    };

    srf::FrameAssembler whole{};
    whole.feed(std::span<const std::byte>(all.data(), all.size()));
    const std::vector<srf::Frame> reference = drain(whole);
    SRF_EXPECT_EQ(reference.size(), static_cast<std::size_t>(3));

    srf::FrameAssembler drip{};
    std::vector<srf::Frame> streamed;
    for (std::size_t i = 0; i < all.size(); ++i) {
        drip.feed(std::span<const std::byte>(all.data() + i, 1));
        const std::vector<srf::Frame> produced = drain(drip);
        for (const srf::Frame& frame : produced) {
            streamed.push_back(frame);
        }
    }
    SRF_EXPECT_EQ(streamed.size(), reference.size());
    SRF_EXPECT_EQ(drip.buffered(), static_cast<std::size_t>(0));
    const std::size_t compare = std::min(streamed.size(), reference.size());
    for (std::size_t i = 0; i < compare; ++i) {
        SRF_EXPECT(streamed[i].message == reference[i].message);
        SRF_EXPECT_EQ(streamed[i].request_id, reference[i].request_id);
        SRF_EXPECT(streamed[i].payload == reference[i].payload);
    }
    {
        // A partial frame is never consumed and never reported.
        srf::FrameAssembler partial{};
        partial.feed(std::span<const std::byte>(all.data(), 5));
        srf::Frame frame{};
        srf::ValidationResult result;
        SRF_EXPECT(!partial.next(limits, frame, result));
        SRF_EXPECT(result.ok());
        SRF_EXPECT_EQ(partial.buffered(), static_cast<std::size_t>(5));
        SRF_EXPECT(!partial.overflowed());
    }
    {
        // An announced payload beyond the bound overflows the assembler.
        WireImage image(srf::as_bytes(first));
        image.set_u32(12, limits.max_frame_bytes + 1);
        image.repair_header();
        srf::FrameAssembler assembler{};
        assembler.feed(image.span());
        srf::Frame frame{};
        srf::ValidationResult result;
        SRF_EXPECT(!assembler.next(limits, frame, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::WireAssemblyOverflow);
        SRF_EXPECT(assembler.overflowed());
        const std::size_t buffered = assembler.buffered();
        assembler.feed(std::span<const std::byte>(all.data(), all.size()));
        SRF_EXPECT_EQ(assembler.buffered(), buffered);
        srf::ValidationResult again;
        SRF_EXPECT(!assembler.next(limits, frame, again));
        SRF_EXPECT_PRIMARY(again, srf::ReasonCode::WireAssemblyOverflow);
    }
    {
        // A frame whose trailer is corrupt is rejected and poisons the stream.
        WireImage image(srf::as_bytes(first));
        image.set_byte(image.size() - 1,
                       static_cast<std::uint8_t>(
                           static_cast<std::uint8_t>(image.bytes().back()) ^ 0x01u));
        srf::FrameAssembler assembler{};
        assembler.feed(image.span());
        srf::Frame frame{};
        srf::ValidationResult result;
        SRF_EXPECT(!assembler.next(limits, frame, result));
        SRF_EXPECT_REASON(result, srf::ReasonCode::WireIntegrityMismatch);
        SRF_EXPECT(assembler.overflowed());
    }
}

// ===========================================================================
// Validation precedence
// ===========================================================================

SRF_TEST(adversarial, validation_precedence_caller_through_policy) {
    {
        // Phase 2 caller identity dominates phase 7 structural segment.
        RequestFixture fixture;
        fixture.caller.publisher = srf::PublisherId{};
        std::vector<srf::Segment> segments{srf::test::node_segment(1), srf::test::node_segment(2)};
        segments[1].id = srf::SegmentId{};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::CallerMissingPublisher);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::CallerIdentity);
        SRF_EXPECT_REASON(result, srf::ReasonCode::SegmentIdInvalid);
    }
    {
        // Phase 3 epoch dominates phase 5 profile.
        RequestFixture fixture;
        fixture.caller.epoch = srf::CoordinatorEpoch{2};
        fixture.request.profile = nullptr;
        fixture.request.profile_found = false;
        fixture.request.profile_id = srf::SegmentProfileId{0xDEAD};
        const std::vector<srf::Segment> segments{srf::test::node_segment(1)};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::EpochStale);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::EpochBootScope);
        SRF_EXPECT_REASON(result, srf::ReasonCode::ProfileUnknown);
    }
    {
        // Phase 4 list lifecycle dominates phase 5 profile.
        RequestFixture fixture;
        fixture.request.require_existing = true;
        fixture.request.profile = nullptr;
        fixture.request.profile_found = false;
        fixture.request.profile_id = srf::SegmentProfileId{0xDEAD};
        const std::vector<srf::Segment> segments{srf::test::node_segment(1)};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::ListNotFound);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::ListLifecycleGeneration);
        SRF_EXPECT_REASON(result, srf::ReasonCode::ProfileUnknown);
    }
    {
        // Phase 5 profile dominates phase 6 policy.
        RequestFixture fixture;
        fixture.profile.allowed_kinds_mask = srf::kind_bit(srf::SegmentKind::Node);
        fixture.request.profile = &fixture.profile;
        fixture.request.policy_required = true;
        fixture.request.policy_id = srf::test::policy_id(7);
        fixture.request.policy = nullptr;
        fixture.request.policy_found = false;
        const std::vector<srf::Segment> segments{srf::test::endpoint_segment(1)};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::ProfileKindNotAllowed);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::Profile);
        SRF_EXPECT_REASON(result, srf::ReasonCode::PolicyUnknown);
    }
    {
        // Phase 6 policy dominates phase 7 structural segment.
        RequestFixture fixture;
        srf::SegmentPolicy policy{};
        policy.id = srf::test::policy_id(9);
        policy.generation = srf::SegmentPolicyGeneration{1};
        policy.owner_scope = srf::ScopeId{0x100};
        policy.max_depth = 1;
        policy.allow_replacement = true;
        fixture.request.policy = &policy;
        fixture.request.policy_found = true;
        fixture.request.policy_id = policy.id;
        fixture.request.policy_generation = policy.generation;
        std::vector<srf::Segment> segments{srf::test::node_segment(1), srf::test::node_segment(2)};
        segments[0].id = srf::SegmentId{};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PolicyMaxDepthExceeded);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::Policy);
        SRF_EXPECT_REASON(result, srf::ReasonCode::SegmentIdInvalid);
    }
}

SRF_TEST(adversarial, validation_precedence_structural_through_limits) {
    {
        // Phase 7 structural segment dominates phase 8 capability evidence.
        RequestFixture fixture;
        fixture.profile = srf::abstract_governance_profile();
        fixture.request.profile = &fixture.profile;
        fixture.request.profile_id = fixture.profile.id;
        fixture.request.capability = srf::CapabilityGeneration{};
        std::vector<srf::Segment> segments{srf::test::node_segment(1)};
        segments[0].id = srf::SegmentId{};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::SegmentIdInvalid);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::StructuralSegment);
        SRF_EXPECT_REASON(result, srf::ReasonCode::CapabilityMissing);
    }
    {
        // Phase 8 capability evidence dominates phase 9 topology evidence.
        RequestFixture fixture;
        fixture.profile = srf::abstract_governance_profile();
        fixture.request.profile = &fixture.profile;
        fixture.request.profile_id = fixture.profile.id;
        fixture.request.capability = srf::CapabilityGeneration{};
        fixture.evidence = evidence_with_segment(srf::test::segment_id(101),
                                                 srf::SegmentKind::Adjacency,
                                                 srf::SegmentGeneration{1});
        fixture.request.evidence = &fixture.evidence;
        fixture.request.topology = srf::TopologyGeneration{1};
        const std::vector<srf::Segment> segments{
            srf::test::adjacency_segment(1, srf::TopologyGeneration{1})};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::CapabilityMissing);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::CapabilityEvidence);
        SRF_EXPECT_REASON(result, srf::ReasonCode::TopologyAdjacencyUnknown);
    }
    {
        // Phase 9 topology evidence dominates phase 10 Path Authority.
        RequestFixture fixture;
        fixture.request.path = srf::PathId{0x4001};
        fixture.request.path_authority = srf::PathAuthorityGeneration{1};
        fixture.evidence = evidence_with_segment(srf::test::segment_id(101),
                                                 srf::SegmentKind::Adjacency,
                                                 srf::SegmentGeneration{1});
        fixture.request.evidence = &fixture.evidence;
        fixture.request.topology = srf::TopologyGeneration{1};
        const std::vector<srf::Segment> segments{
            srf::test::adjacency_segment(1, srf::TopologyGeneration{1})};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::TopologyAdjacencyUnknown);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::TopologyEvidence);
        SRF_EXPECT_REASON(result, srf::ReasonCode::PathUnknown);
    }
    {
        // Phase 10 Path Authority dominates phase 13 resource limits.
        RequestFixture fixture;
        fixture.limits.max_segment_payload_bytes = 4;
        fixture.request.path = srf::PathId{0x4001};
        fixture.request.path_authority = srf::PathAuthorityGeneration{1};
        fixture.evidence = evidence_with_segment(srf::test::segment_id(401),
                                                 srf::SegmentKind::Policy,
                                                 srf::SegmentGeneration{1});
        fixture.request.evidence = &fixture.evidence;
        srf::Segment heavy = srf::test::policy_segment(1);
        heavy.payload.assign(5, std::byte{7});
        SRF_EXPECT(heavy.payload.size() <= fixture.profile.payload_width);
        const std::vector<srf::Segment> segments{heavy};
        fixture.request.segments = segments;
        const srf::ValidationResult result = srf::validate_request(fixture.request);
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PathUnknown);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::PathAuthority);
        SRF_EXPECT_REASON(result, srf::ReasonCode::LimitMaxSegmentPayloadBytes);
    }
    {
        // Wire decode precedes every validation phase: a frame defect can never be
        // reported as a validation reason.
        const srf::Limits limits{};
        srf::Frame frame{};
        frame.message = srf::MessageId::CreateList;
        frame.payload = {std::byte{1}};
        srf::ValidationResult encode_result;
        const srf::ByteBuffer encoded = srf::encode_frame(frame, limits, encode_result);
        SRF_EXPECT(encode_result.ok());
        WireImage image(srf::as_bytes(encoded));
        image.set_byte(0, 'X');
        srf::Frame decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_frame(image.span(), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::WireMagicMismatch);
        SRF_EXPECT(result.primary_phase() == srf::ValidationPhase::WireDecode);
    }
}

// ===========================================================================
// Limits: every bound is consulted.
//
// Twelve bounds are observed through their exact LimitMax* reason code. Four
// bounds (max_history, max_explanation_entries, max_diff_entries,
// max_attempt_records) are enforced by bounded, silent truncation: the product
// emits no reason code for them, so consultation is proven by the exact retained
// size together with the truncation flag.
// ===========================================================================

SRF_TEST(adversarial, limits_every_bound_is_consulted) {
    {
        srf::Limits limits{};
        limits.max_lists = 2;
        Harness harness(true, limits);
        SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
        SRF_EXPECT(harness.create(2, {srf::test::node_segment(2)}).ok());
        const srf::MutationOutcome third = harness.create(3, {srf::test::node_segment(3)});
        SRF_EXPECT_EQ(static_cast<int>(third.status),
                      static_cast<int>(srf::StatusCode::LimitExceeded));
        SRF_EXPECT_REASON(third.result, srf::ReasonCode::LimitMaxLists);
        SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(2));
    }
    {
        srf::Limits limits{};
        limits.max_segments_per_list = 2;
        Harness harness(true, limits);
        const srf::MutationOutcome outcome =
            harness.create(1, {srf::test::node_segment(1), srf::test::node_segment(2),
                               srf::test::node_segment(3)});
        SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxSegmentsPerList);
        SRF_EXPECT(outcome.result.primary_phase() == srf::ValidationPhase::ResourceLimits);
    }
    {
        srf::Limits limits{};
        limits.max_segment_payload_bytes = 2;
        Harness harness(true, limits);
        srf::Segment oversized = srf::test::node_segment(1);
        oversized.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
        const srf::MutationOutcome outcome = harness.create(1, {oversized});
        SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxSegmentPayloadBytes);
    }
    {
        srf::Limits limits{};
        limits.max_profiles = 1;
        auto store = make_store(limits, std::make_shared<srf::SyntheticEvidence>(), nullptr);
        SRF_EXPECT(store->register_profile(srf::abstract_governance_profile()).ok());
        const srf::ValidationResult second =
            store->register_profile(srf::abstract_ordered_profile());
        SRF_EXPECT_PRIMARY(second, srf::ReasonCode::LimitMaxProfiles);
    }
    {
        srf::Limits limits{};
        limits.max_policies = 1;
        auto store = make_store(limits, std::make_shared<srf::SyntheticEvidence>(), nullptr);
        srf::SegmentPolicy first{};
        first.id = srf::test::policy_id(1);
        first.generation = srf::SegmentPolicyGeneration{1};
        SRF_EXPECT(store->register_policy(first).ok());
        srf::SegmentPolicy second{};
        second.id = srf::test::policy_id(2);
        second.generation = srf::SegmentPolicyGeneration{1};
        SRF_EXPECT_PRIMARY(store->register_policy(second), srf::ReasonCode::LimitMaxPolicies);
    }
    {
        srf::Limits limits{};
        limits.max_batch_size = 1;
        Harness harness(true, limits);
        std::vector<srf::ListDraft> drafts;
        for (std::uint64_t n = 1; n <= 2; ++n) {
            srf::ListDraft draft = harness.draft(n);
            draft.segments = {srf::test::node_segment(n)};
            drafts.push_back(draft);
        }
        const srf::MutationOutcome outcome =
            harness.store().batch(harness.caller(), std::span<const srf::ListDraft>(drafts));
        SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxBatchSize);
        SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));
    }
    {
        srf::Limits limits{};
        limits.max_publishers = 1;
        auto store = make_store(limits, std::make_shared<srf::SyntheticEvidence>(), nullptr);
        SRF_EXPECT(store->authority()
                       .register_publisher(srf::PublisherId{1}, srf::WorkerBootId{1},
                                           srf::ScopeId{srf::test::kScope},
                                           store->authority().epoch(), limits)
                       .ok());
        const srf::ValidationResult second = store->authority().register_publisher(
            srf::PublisherId{2}, srf::WorkerBootId{2}, srf::ScopeId{srf::test::kScope},
            store->authority().epoch(), limits);
        SRF_EXPECT_PRIMARY(second, srf::ReasonCode::LimitMaxPublishers);
    }
    {
        srf::Limits limits{};
        limits.max_frame_bytes = 16;
        srf::Frame frame{};
        frame.message = srf::MessageId::ListSummaries;
        frame.payload.assign(64, std::byte{0});
        srf::ValidationResult result;
        SRF_EXPECT(srf::encode_frame(frame, limits, result).empty());
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::LimitMaxFrameBytes);
    }
    {
        // Silent truncation: the retained history is exactly the bound.
        srf::Limits limits{};
        limits.max_history = 3;
        Harness harness(true, limits);
        SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
        std::uint64_t generation = 1;
        for (int i = 0; i < 6; ++i) {
            const srf::MutationOutcome outcome =
                harness.replace(1, {srf::test::node_segment(1)}, generation);
            SRF_EXPECT_OK(outcome);
            if (!outcome.ok()) {
                break;
            }
            generation = outcome.generation.value();
        }
        SRF_EXPECT_EQ(harness.store().history().size(), static_cast<std::size_t>(3));
    }
    {
        // Silent truncation: the explanation is exactly the bound.
        srf::Limits limits{};
        limits.max_explanation_entries = 2;
        Harness harness(true, limits);
        SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
        const srf::Explanation explanation =
            harness.store().explain_currentness(srf::test::list_id(1));
        SRF_EXPECT(explanation.truncated);
        SRF_EXPECT(explanation.size() <= static_cast<std::size_t>(2));
    }
    {
        srf::Limits limits{};
        limits.max_persistence_record_bytes = 150;
        Harness harness(true, limits);
        const srf::MutationOutcome outcome = harness.create(1, {srf::test::node_segment(1)});
        SRF_EXPECT_EQ(static_cast<int>(outcome.status),
                      static_cast<int>(srf::StatusCode::PersistenceFailed));
        SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxPersistenceRecordBytes);
        SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));
    }
    {
        srf::Limits limits{};
        limits.max_reasons = 3;
        srf::ValidationResult result;
        result.set_limits(&limits);
        for (std::uint32_t i = 0; i < 10; ++i) {
            result.add(srf::ReasonCode::SegmentIdInvalid, i);
        }
        SRF_EXPECT(result.truncated());
        SRF_EXPECT(result.size() <= static_cast<std::size_t>(3));
        SRF_EXPECT_REASON(result, srf::ReasonCode::LimitMaxReasons);
    }
    {
        // Silent truncation: the diff is exactly the bound.
        srf::Limits limits{};
        limits.max_diff_entries = 1;
        Harness harness(true, limits);
        SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
        const srf::SegmentListSnapshot before = harness.store().snapshot(srf::test::list_id(1));
        SRF_EXPECT(harness.replace(1, {srf::test::node_segment(2), srf::test::node_segment(3)}, 1)
                       .ok());
        const srf::SegmentListSnapshot after = harness.store().snapshot(srf::test::list_id(1));
        const srf::SnapshotDiff diff = harness.store().diff(before, after);
        SRF_EXPECT(diff.truncated);
        SRF_EXPECT(diff.entries.size() <= static_cast<std::size_t>(1));
    }
    {
        srf::Limits limits{};
        limits.max_lists = 8;
        limits.max_total_segments = 2;
        Harness harness(true, limits);
        SRF_EXPECT(harness.create(1, {srf::test::node_segment(1)}).ok());
        SRF_EXPECT(harness.create(2, {srf::test::node_segment(2)}).ok());
        const srf::MutationOutcome third = harness.create(3, {srf::test::node_segment(3)});
        SRF_EXPECT_REASON(third.result, srf::ReasonCode::LimitMaxTotalSegments);
        SRF_EXPECT_EQ(harness.store().total_segments(), static_cast<std::size_t>(2));
    }
    {
        // Silent truncation: the attempt log is exactly the bound.
        srf::Limits limits{};
        limits.max_attempt_records = 2;
        Harness harness(true, limits);
        for (std::uint64_t n = 1; n <= 5; ++n) {
            SRF_EXPECT(harness.create(n, {srf::test::node_segment(n)}).ok());
        }
        SRF_EXPECT_EQ(harness.store().export_state().attempts.size(),
                      static_cast<std::size_t>(2));
    }
    {
        srf::Limits limits{};
        limits.max_evidence_records = 4;
        Harness harness(true, limits);
        const srf::MutationOutcome outcome = harness.create(1, {srf::test::node_segment(1)});
        SRF_EXPECT_EQ(static_cast<int>(outcome.status),
                      static_cast<int>(srf::StatusCode::LimitExceeded));
        SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxEvidenceRecords);
    }
}
