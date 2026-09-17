// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/wire.hpp"

#include <cstring>

namespace srf {
namespace {

constexpr std::uint8_t kMagic[4] = {'S', 'R', 'F', 'W'};

[[nodiscard]] std::span<const std::byte> magic_bytes() noexcept {
    return {reinterpret_cast<const std::byte*>(kMagic), sizeof(kMagic)};
}

void write_segment(ByteWriter& w, const Segment& s) {
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
    w.bytes(as_bytes(s.payload));
}

[[nodiscard]] bool read_segment(ByteReader& r, const Limits& limits, Segment& s,
                                ValidationResult& result) {
    std::uint16_t kind = 0;
    std::uint16_t encoding = 0;
    std::uint16_t payload_length = 0;
    if (!r.u16(kind) || !r.u16(encoding)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (kind == 0 || kind > static_cast<std::uint16_t>(SegmentKind::Policy)) {
        result.add(ReasonCode::WireUnknownEnumValue, 0, kind);
        return false;
    }
    // A wire draft may leave the encoding unset: canonicalization fills it in at
    // commit. An explicit encoding that is not a known abstract encoding is not
    // normalizable and is rejected here.
    if (encoding > static_cast<std::uint16_t>(SegmentEncodingId::AbstractPolicyV1)) {
        result.add(ReasonCode::WireUnknownEnumValue, 0, encoding);
        return false;
    }
    s.kind = static_cast<SegmentKind>(kind);
    s.encoding = static_cast<SegmentEncodingId>(encoding);
    std::uint64_t values[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::uint64_t& value : values) {
        if (!r.u64(value)) {
            result.add(ReasonCode::WireMalformedFrame);
            return false;
        }
    }
    s.id = SegmentId{values[0]};
    s.generation = SegmentGeneration{values[1]};
    s.node = NodeId{values[2]};
    s.adjacency = AdjacencyId{values[3]};
    s.endpoint = EndpointId{values[4]};
    s.binding = BindingId{values[5]};
    s.policy = SegmentPolicyId{values[6]};
    s.policy_generation = SegmentPolicyGeneration{values[7]};
    s.topology = TopologyGeneration{values[8]};
    if (!r.u16(payload_length)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (payload_length > limits.max_segment_payload_bytes) {
        result.add(ReasonCode::WireFieldOutOfRange, 0, payload_length);
        return false;
    }
    if (r.remaining() < payload_length) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    s.payload.assign(payload_length, std::byte{0});
    if (payload_length > 0 &&
        !r.bytes(std::span<std::byte>(s.payload.data(), s.payload.size()))) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    return true;
}

[[nodiscard]] bool read_u64(ByteReader& r, std::uint64_t& out, ValidationResult& result) {
    if (!r.u64(out)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    return true;
}

} // namespace

std::string_view message_id_name(MessageId id) noexcept {
    switch (id) {
        case MessageId::Invalid: return "Invalid";
        case MessageId::Hello: return "Hello";
        case MessageId::HelloAck: return "HelloAck";
        case MessageId::Heartbeat: return "Heartbeat";
        case MessageId::Goodbye: return "Goodbye";
        case MessageId::CreateList: return "CreateList";
        case MessageId::ReplaceList: return "ReplaceList";
        case MessageId::Withdraw: return "Withdraw";
        case MessageId::WithdrawCommit: return "WithdrawCommit";
        case MessageId::Revoke: return "Revoke";
        case MessageId::Retire: return "Retire";
        case MessageId::Revalidate: return "Revalidate";
        case MessageId::Get: return "Get";
        case MessageId::ListSummaries: return "ListSummaries";
        case MessageId::ExplainCurrentness: return "ExplainCurrentness";
        case MessageId::RefreshCurrentness: return "RefreshCurrentness";
        case MessageId::Response: return "Response";
    }
    return "Unknown";
}

ByteBuffer encode_frame(const Frame& frame, const Limits& limits, ValidationResult& result) {
    result.set_limits(&limits);
    if (frame.payload.size() > limits.max_frame_bytes) {
        result.add(ReasonCode::LimitMaxFrameBytes, 0, limits.max_frame_bytes);
        return ByteBuffer{};
    }
    if (frame.message == MessageId::Invalid) {
        result.add(ReasonCode::WireUnknownMessageId);
        return ByteBuffer{};
    }
    ByteWriter header;
    header.bytes(magic_bytes());
    header.u16(frame.version);
    header.u16(static_cast<std::uint16_t>(frame.message));
    header.u16(frame.flags);
    header.u16(0);
    header.u32(static_cast<std::uint32_t>(frame.payload.size()));
    header.u64(frame.request_id);
    header.u32(crc32c(header.span()));

    ByteWriter out;
    out.bytes(header.span());
    out.bytes(as_bytes(frame.payload));
    out.u32(crc32c(as_bytes(frame.payload)));
    return std::move(out).take();
}

bool decode_frame(std::span<const std::byte> bytes, const Limits& limits, Frame& frame,
                  ValidationResult& result) {
    result.set_limits(&limits);
    if (bytes.size() < kFrameHeaderBytes + kFrameTrailerBytes) {
        result.add(ReasonCode::WireHeaderTruncated, 0, bytes.size());
        return false;
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        result.add(ReasonCode::WireMagicMismatch);
        return false;
    }
    ByteReader header(bytes.first(kFrameHeaderBytes));
    std::uint16_t version = 0;
    std::uint16_t message = 0;
    std::uint16_t flags = 0;
    std::uint16_t reserved = 0;
    std::uint32_t payload_length = 0;
    std::uint64_t request_id = 0;
    std::uint32_t header_crc = 0;
    if (!header.skip(4) || !header.u16(version) || !header.u16(message) || !header.u16(flags) ||
        !header.u16(reserved) || !header.u32(payload_length) || !header.u64(request_id) ||
        !header.u32(header_crc)) {
        result.add(ReasonCode::WireHeaderTruncated);
        return false;
    }
    if (header_crc != crc32c(bytes.first(kFrameHeaderBytes - 4))) {
        result.add(ReasonCode::WireIntegrityMismatch);
        return false;
    }
    if (version != kWireProtocolVersion) {
        result.add(ReasonCode::WireUnsupportedVersion, 0, version);
        return false;
    }
    if (reserved != 0) {
        result.add(ReasonCode::WireUnsupportedFlags, 0, reserved);
        return false;
    }
    if (flags != 0) {
        result.add(ReasonCode::WireUnsupportedFlags, 0, flags);
        return false;
    }
    if (message == 0 || message > static_cast<std::uint16_t>(MessageId::Response)) {
        result.add(ReasonCode::WireUnknownMessageId, 0, message);
        return false;
    }
    if (payload_length > limits.max_frame_bytes) {
        result.add(ReasonCode::LimitMaxFrameBytes, 0, payload_length);
        return false;
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kFrameHeaderBytes) + payload_length + kFrameTrailerBytes;
    if (expected > bytes.size()) {
        result.add(ReasonCode::WirePayloadLengthMismatch, 0, bytes.size());
        return false;
    }
    if (expected < bytes.size()) {
        result.add(ReasonCode::WireTrailingBytes, 0, bytes.size() - expected);
        return false;
    }
    const auto payload_size = static_cast<std::size_t>(payload_length);
    ByteReader trailer(bytes.subspan(kFrameHeaderBytes + payload_size, kFrameTrailerBytes));
    std::uint32_t payload_crc = 0;
    if (!trailer.u32(payload_crc)) {
        result.add(ReasonCode::WireHeaderTruncated);
        return false;
    }
    if (payload_crc != crc32c(bytes.subspan(kFrameHeaderBytes, payload_size))) {
        result.add(ReasonCode::WireIntegrityMismatch);
        return false;
    }
    frame.version = version;
    frame.message = static_cast<MessageId>(message);
    frame.flags = flags;
    frame.request_id = request_id;
    const std::span<const std::byte> payload = bytes.subspan(kFrameHeaderBytes, payload_size);
    frame.payload.assign(payload.begin(), payload.end());
    return true;
}

void FrameAssembler::feed(std::span<const std::byte> bytes) {
    if (overflowed_) {
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

bool FrameAssembler::next(const Limits& limits, Frame& frame, ValidationResult& result) {
    result.set_limits(&limits);
    if (overflowed_) {
        result.add(ReasonCode::WireAssemblyOverflow, 0, buffer_.size());
        return false;
    }
    if (buffer_.size() < kFrameHeaderBytes) {
        return false;
    }
    std::uint32_t payload_length = 0;
    {
        ByteReader header(std::span<const std::byte>(buffer_.data(), kFrameHeaderBytes));
        std::uint16_t ignored16 = 0;
        std::uint64_t ignored64 = 0;
        std::uint32_t ignored32 = 0;
        if (!header.skip(4) || !header.u16(ignored16) || !header.u16(ignored16) ||
            !header.u16(ignored16) || !header.u16(ignored16) || !header.u32(payload_length) ||
            !header.u64(ignored64) || !header.u32(ignored32)) {
            result.add(ReasonCode::WireHeaderTruncated);
            return false;
        }
    }
    if (payload_length > limits.max_frame_bytes) {
        overflowed_ = true;
        result.add(ReasonCode::WireAssemblyOverflow, 0, payload_length);
        return false;
    }
    const std::size_t total =
        kFrameHeaderBytes + static_cast<std::size_t>(payload_length) + kFrameTrailerBytes;
    if (buffer_.size() < total) {
        return false;
    }
    const bool ok =
        decode_frame(std::span<const std::byte>(buffer_.data(), total), limits, frame, result);
    if (!ok) {
        overflowed_ = true;
        return false;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    return true;
}

void FrameAssembler::clear() {
    buffer_.clear();
    overflowed_ = false;
}

ByteBuffer encode_hello(const HelloRequest& hello) {
    ByteWriter w;
    w.u64(hello.boot.value());
    w.u64(hello.publisher.value());
    w.u64(hello.scope.value());
    w.u64(hello.epoch.value());
    return std::move(w).take();
}

bool decode_hello(std::span<const std::byte> bytes, const Limits& limits, HelloRequest& hello,
                  ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    std::uint64_t boot = 0;
    std::uint64_t publisher = 0;
    std::uint64_t scope = 0;
    std::uint64_t epoch = 0;
    if (!read_u64(r, boot, result) || !read_u64(r, publisher, result) ||
        !read_u64(r, scope, result) || !read_u64(r, epoch, result)) {
        return false;
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    hello.boot = WorkerBootId{boot};
    hello.publisher = PublisherId{publisher};
    hello.scope = ScopeId{scope};
    hello.epoch = CoordinatorEpoch{epoch};
    return true;
}

ByteBuffer encode_hello_ack(const HelloAck& ack) {
    ByteWriter w;
    w.u64(ack.epoch.value());
    w.u64(ack.session.value());
    return std::move(w).take();
}

bool decode_hello_ack(std::span<const std::byte> bytes, const Limits& limits, HelloAck& ack,
                      ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    std::uint64_t epoch = 0;
    std::uint64_t session = 0;
    if (!read_u64(r, epoch, result) || !read_u64(r, session, result)) {
        return false;
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    ack.epoch = CoordinatorEpoch{epoch};
    ack.session = WorkerSessionId{session};
    return true;
}

ByteBuffer encode_heartbeat(const HeartbeatRequest& beat) {
    ByteWriter w;
    w.u64(beat.publisher.value());
    w.u64(beat.boot.value());
    w.u64(beat.epoch.value());
    return std::move(w).take();
}

bool decode_heartbeat(std::span<const std::byte> bytes, const Limits& limits,
                      HeartbeatRequest& beat, ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    std::uint64_t publisher = 0;
    std::uint64_t boot = 0;
    std::uint64_t epoch = 0;
    if (!read_u64(r, publisher, result) || !read_u64(r, boot, result) ||
        !read_u64(r, epoch, result)) {
        return false;
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    beat.publisher = PublisherId{publisher};
    beat.boot = WorkerBootId{boot};
    beat.epoch = CoordinatorEpoch{epoch};
    return true;
}

ByteBuffer encode_mutation(const MutationRequest& mutation, const Limits& limits,
                           ValidationResult& result) {
    result.set_limits(&limits);
    if (mutation.draft.segments.size() > limits.max_segments_per_list) {
        result.add(ReasonCode::LimitMaxSegmentsPerList, 0, limits.max_segments_per_list);
        return ByteBuffer{};
    }
    const ListDraft& d = mutation.draft;
    ByteWriter w;
    w.u64(mutation.attempt.value());
    w.u64(d.id.value());
    w.u64(d.scope.value());
    w.u64(d.profile.value());
    w.u64(d.profile_generation.value());
    w.u64(d.policy.value());
    w.u64(d.policy_generation.value());
    w.u8(static_cast<std::uint8_t>(d.strictness));
    w.u64(d.topology.value());
    w.u64(d.capability.value());
    w.u64(d.path.value());
    w.u64(d.path_authority.value());
    w.u64(d.route.value());
    w.u64(d.route_generation.value());
    w.u64(d.constraint.value());
    w.u64(d.constraint_generation.value());
    w.u64(d.expected_generation.value());
    w.u64(d.supersedes.value());
    w.u64(d.supersedes_generation.value());
    w.u8(d.derived ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(d.segments.size()));
    for (const Segment& s : d.segments) {
        write_segment(w, s);
    }
    return std::move(w).take();
}

bool decode_mutation(std::span<const std::byte> bytes, const Limits& limits,
                     MutationRequest& mutation, ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    ListDraft& d = mutation.draft;
    std::uint64_t values[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::uint64_t attempt = 0;
    if (!read_u64(r, attempt, result)) {
        return false;
    }
    mutation.attempt = MutationAttemptId{attempt};
    for (std::size_t i = 0; i < 6; ++i) {
        if (!read_u64(r, values[i], result)) {
            return false;
        }
    }
    d.id = SegmentListId{values[0]};
    d.scope = ScopeId{values[1]};
    d.profile = SegmentProfileId{values[2]};
    d.profile_generation = SegmentProfileGeneration{values[3]};
    d.policy = SegmentPolicyId{values[4]};
    d.policy_generation = SegmentPolicyGeneration{values[5]};
    std::uint8_t strictness = 0;
    if (!r.u8(strictness)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (strictness != static_cast<std::uint8_t>(Strictness::Strict) &&
        strictness != static_cast<std::uint8_t>(Strictness::Loose)) {
        result.add(ReasonCode::WireUnknownEnumValue, 0, strictness);
        return false;
    }
    d.strictness = static_cast<Strictness>(strictness);
    for (std::uint64_t& value : values) {
        if (!read_u64(r, value, result)) {
            return false;
        }
    }
    d.topology = TopologyGeneration{values[0]};
    d.capability = CapabilityGeneration{values[1]};
    d.path = PathId{values[2]};
    d.path_authority = PathAuthorityGeneration{values[3]};
    d.route = RouteId{values[4]};
    d.route_generation = RouteGeneration{values[5]};
    d.constraint = ConstraintEvaluationId{values[6]};
    d.constraint_generation = ConstraintGeneration{values[7]};
    d.expected_generation = SegmentListGeneration{values[8]};
    d.supersedes = SegmentListId{values[9]};
    d.supersedes_generation = SegmentListGeneration{values[10]};
    std::uint8_t derived = 0;
    std::uint32_t segment_count = 0;
    if (!r.u8(derived) || !r.u32(segment_count)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (derived > 1) {
        result.add(ReasonCode::WireFieldOutOfRange, 0, derived);
        return false;
    }
    d.derived = derived != 0;
    if (segment_count > limits.max_segments_per_list) {
        result.add(ReasonCode::LimitMaxSegmentsPerList, 0, segment_count);
        return false;
    }
    for (std::uint32_t i = 0; i < segment_count; ++i) {
        Segment s{};
        if (!read_segment(r, limits, s, result)) {
            return false;
        }
        d.segments.push_back(std::move(s));
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    return true;
}

ByteBuffer encode_lifecycle(const LifecycleRequest& request) {
    ByteWriter w;
    w.u64(request.attempt.value());
    w.u8(static_cast<std::uint8_t>(request.op));
    w.u64(request.list.value());
    w.u64(request.expected.value());
    return std::move(w).take();
}

bool decode_lifecycle(std::span<const std::byte> bytes, const Limits& limits,
                      LifecycleRequest& request, ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    std::uint64_t attempt = 0;
    std::uint8_t op = 0;
    std::uint64_t list = 0;
    std::uint64_t expected = 0;
    if (!read_u64(r, attempt, result) || !r.u8(op)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (op == 0 || op > static_cast<std::uint8_t>(LifecycleOp::Revalidate)) {
        result.add(ReasonCode::WireUnknownEnumValue, 0, op);
        return false;
    }
    if (!read_u64(r, list, result) || !read_u64(r, expected, result)) {
        return false;
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    request.attempt = MutationAttemptId{attempt};
    request.op = static_cast<LifecycleOp>(op);
    request.list = SegmentListId{list};
    request.expected = SegmentListGeneration{expected};
    return true;
}

ByteBuffer encode_get(const GetRequest& request) {
    ByteWriter w;
    w.u64(request.list.value());
    return std::move(w).take();
}

bool decode_get(std::span<const std::byte> bytes, const Limits& limits, GetRequest& request,
                ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    std::uint64_t value = 0;
    if (!read_u64(r, value, result)) {
        return false;
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    request.list = SegmentListId{value};
    return true;
}

ListSummary summarize(const SegmentList& list, CoordinatorEpoch epoch) {
    ListSummary summary{};
    summary.list = list.content.id;
    summary.generation = list.generation;
    summary.state = list.state;
    summary.currentness = list.currentness;
    summary.profile = list.content.profile;
    summary.profile_generation = list.content.profile_generation;
    summary.digest = list.content_digest;
    summary.segment_count = static_cast<std::uint32_t>(list.content.segments.size());
    summary.epoch = epoch;
    return summary;
}

ByteBuffer encode_response(const Response& response, const Limits& limits,
                           ValidationResult& result) {
    result.set_limits(&limits);
    ByteWriter w;
    w.u64(response.request_id);
    w.u8(static_cast<std::uint8_t>(response.status));
    w.u32(static_cast<std::uint32_t>(response.reasons.size()));
    for (const Reason& reason : response.reasons) {
        w.u16(static_cast<std::uint16_t>(reason.code));
        w.u32(reason.index);
        w.u64(reason.detail);
    }
    w.u8(response.truncated ? 1 : 0);
    w.u64(response.list.value());
    w.u64(response.generation.value());
    w.u64(response.digest.hi);
    w.u64(response.digest.lo);
    w.u8(response.idempotent_replay ? 1 : 0);
    w.u64(response.epoch.value());
    w.u64(response.session.value());
    if (response.summaries.size() > limits.max_lists) {
        result.add(ReasonCode::LimitMaxLists, 0, limits.max_lists);
        return ByteBuffer{};
    }
    w.u32(static_cast<std::uint32_t>(response.summaries.size()));
    for (const ListSummary& summary : response.summaries) {
        w.u64(summary.list.value());
        w.u64(summary.generation.value());
        w.u8(static_cast<std::uint8_t>(summary.state));
        w.u8(static_cast<std::uint8_t>(summary.currentness));
        w.u64(summary.profile.value());
        w.u64(summary.profile_generation.value());
        w.u64(summary.digest.hi);
        w.u64(summary.digest.lo);
        w.u32(summary.segment_count);
        w.u64(summary.epoch.value());
    }
    return std::move(w).take();
}

bool decode_response(std::span<const std::byte> bytes, const Limits& limits, Response& response,
                     ValidationResult& result) {
    result.set_limits(&limits);
    ByteReader r(bytes);
    std::uint64_t request_id = 0;
    std::uint8_t status = 0;
    std::uint32_t reason_count = 0;
    if (!read_u64(r, request_id, result) || !r.u8(status)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (status > static_cast<std::uint8_t>(StatusCode::Internal)) {
        result.add(ReasonCode::WireUnknownEnumValue, 0, status);
        return false;
    }
    if (!r.u32(reason_count)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (reason_count > limits.max_reasons) {
        result.add(ReasonCode::LimitMaxReasons, 0, reason_count);
        return false;
    }
    response.request_id = request_id;
    response.status = static_cast<StatusCode>(status);
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        std::uint16_t code = 0;
        std::uint32_t index = 0;
        std::uint64_t detail = 0;
        if (!r.u16(code) || !r.u32(index) || !read_u64(r, detail, result)) {
            result.add(ReasonCode::WireMalformedFrame);
            return false;
        }
        response.reasons.push_back(Reason{static_cast<ReasonCode>(code), index, detail});
    }
    std::uint8_t truncated = 0;
    if (!r.u8(truncated)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (truncated > 1) {
        result.add(ReasonCode::WireFieldOutOfRange, 0, truncated);
        return false;
    }
    response.truncated = truncated != 0;
    std::uint64_t list = 0;
    std::uint64_t generation = 0;
    std::uint64_t digest_hi = 0;
    std::uint64_t digest_lo = 0;
    std::uint64_t epoch = 0;
    std::uint64_t session = 0;
    std::uint8_t replay = 0;
    if (!read_u64(r, list, result) || !read_u64(r, generation, result) ||
        !read_u64(r, digest_hi, result) || !read_u64(r, digest_lo, result) || !r.u8(replay) ||
        !read_u64(r, epoch, result) || !read_u64(r, session, result)) {
        return false;
    }
    if (replay > 1) {
        result.add(ReasonCode::WireFieldOutOfRange, 0, replay);
        return false;
    }
    response.list = SegmentListId{list};
    response.generation = SegmentListGeneration{generation};
    response.digest = Digest128{digest_hi, digest_lo};
    response.idempotent_replay = replay != 0;
    response.epoch = CoordinatorEpoch{epoch};
    response.session = WorkerSessionId{session};
    std::uint32_t summary_count = 0;
    if (!r.u32(summary_count)) {
        result.add(ReasonCode::WireMalformedFrame);
        return false;
    }
    if (summary_count > limits.max_lists) {
        result.add(ReasonCode::LimitMaxLists, 0, summary_count);
        return false;
    }
    for (std::uint32_t i = 0; i < summary_count; ++i) {
        ListSummary summary{};
        std::uint64_t values[4] = {0, 0, 0, 0};
        std::uint8_t state = 0;
        std::uint8_t currentness = 0;
        std::uint32_t segment_count = 0;
        std::uint64_t summary_epoch = 0;
        if (!read_u64(r, values[0], result) || !read_u64(r, values[1], result) ||
            !r.u8(state) || !r.u8(currentness) || !read_u64(r, values[2], result) ||
            !read_u64(r, values[3], result) || !read_u64(r, summary.digest.hi, result) ||
            !read_u64(r, summary.digest.lo, result) || !r.u32(segment_count) ||
            !read_u64(r, summary_epoch, result)) {
            result.add(ReasonCode::WireMalformedFrame);
            return false;
        }
        summary.epoch = CoordinatorEpoch{summary_epoch};
        if (state == 0 || state > static_cast<std::uint8_t>(LifecycleState::Retired)) {
            result.add(ReasonCode::WireUnknownEnumValue, 0, state);
            return false;
        }
        if (currentness > static_cast<std::uint8_t>(Currentness::FencedPublisher)) {
            result.add(ReasonCode::WireUnknownEnumValue, 0, currentness);
            return false;
        }
        summary.list = SegmentListId{values[0]};
        summary.generation = SegmentListGeneration{values[1]};
        summary.state = static_cast<LifecycleState>(state);
        summary.currentness = static_cast<Currentness>(currentness);
        summary.profile = SegmentProfileId{values[2]};
        summary.profile_generation = SegmentProfileGeneration{values[3]};
        summary.segment_count = segment_count;
        response.summaries.push_back(summary);
    }
    if (!r.at_end()) {
        result.add(ReasonCode::WireTrailingBytes, 0, r.remaining());
        return false;
    }
    return true;
}

} // namespace srf
