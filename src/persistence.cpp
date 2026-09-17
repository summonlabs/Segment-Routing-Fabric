// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/persistence.hpp"

#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include "srf/canonical.hpp"

namespace srf {
namespace {

constexpr std::uint8_t kMagic[4] = {'S', 'R', 'F', 'L'};
constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kTrailerBytes = 4;

enum class RecordType : std::uint16_t {
    Invalid = 0,
    Meta = 1,
    Profile = 2,
    Policy = 3,
    List = 4,
    History = 5,
    Attempt = 6,
    Publisher = 7,
    FencedBoot = 8,
};

[[nodiscard]] std::span<const std::byte> magic_bytes() noexcept {
    return {reinterpret_cast<const std::byte*>(kMagic), sizeof(kMagic)};
}

// ------------------------------------------------------------------- encoding
struct Encoder {
    ByteWriter records{};
    std::uint32_t count{0};
    const Limits* limits{nullptr};
    ValidationResult* result{nullptr};
    bool failed{false};

    void emit(RecordType type, const ByteBuffer& body) {
        if (failed) {
            return;
        }
        if (limits != nullptr && body.size() > limits->max_persistence_record_bytes) {
            result->add(ReasonCode::LimitMaxPersistenceRecordBytes, count,
                        limits->max_persistence_record_bytes);
            failed = true;
            return;
        }
        records.u16(static_cast<std::uint16_t>(type));
        records.u32(static_cast<std::uint32_t>(body.size()));
        records.bytes(as_bytes(body));
        ++count;
    }
};

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

[[nodiscard]] std::uint32_t bound_payload_width(const DurableState& state,
                                                SegmentProfileId id) noexcept {
    for (const SegmentProfile& profile : state.profiles) {
        if (profile.id == id) {
            return profile.payload_width == 0 ? 1u : profile.payload_width;
        }
    }
    return 0;
}

Digest128 payload_identity(const Segment& s) {
    return digest128(as_bytes(s.payload));
}

// ------------------------------------------------------------------- decoding
class Decoder {
public:
    Decoder(std::span<const std::byte> body, ValidationResult& result, std::uint32_t index) noexcept
        : reader_(body), result_(result), index_(index) {}

    [[nodiscard]] bool fail(ReasonCode code, std::uint64_t detail = 0) {
        result_.add(code, index_, detail);
        return false;
    }

    [[nodiscard]] bool done() {
        if (!reader_.at_end()) {
            return fail(ReasonCode::PersistTrailingBytes, reader_.remaining());
        }
        return true;
    }

    [[nodiscard]] bool u8(std::uint8_t& v) {
        return reader_.u8(v) ? true : fail(ReasonCode::PersistTruncated);
    }
    [[nodiscard]] bool u16(std::uint16_t& v) {
        return reader_.u16(v) ? true : fail(ReasonCode::PersistTruncated);
    }
    [[nodiscard]] bool u32(std::uint32_t& v) {
        return reader_.u32(v) ? true : fail(ReasonCode::PersistTruncated);
    }
    [[nodiscard]] bool u64(std::uint64_t& v) {
        return reader_.u64(v) ? true : fail(ReasonCode::PersistTruncated);
    }
    [[nodiscard]] bool flag(bool& v) {
        std::uint8_t raw = 0;
        if (!u8(raw)) {
            return false;
        }
        if (raw > 1) {
            return fail(ReasonCode::PersistCorrupt, raw);
        }
        v = raw != 0;
        return true;
    }
    [[nodiscard]] bool blob(std::size_t length, std::vector<std::byte>& out) {
        if (reader_.remaining() < length) {
            return fail(ReasonCode::PersistTruncated, length);
        }
        out.assign(length, std::byte{0});
        if (length == 0) {
            return true;
        }
        return reader_.bytes(std::span<std::byte>(out.data(), out.size()))
                   ? true
                   : fail(ReasonCode::PersistTruncated);
    }
    [[nodiscard]] bool text(std::size_t length, std::string& out) {
        if (reader_.remaining() < length) {
            return fail(ReasonCode::PersistTruncated, length);
        }
        out.assign(length, '\0');
        if (length == 0) {
            return true;
        }
        return reader_.bytes(
                   std::span<std::byte>(reinterpret_cast<std::byte*>(out.data()), length))
                   ? true
                   : fail(ReasonCode::PersistTruncated);
    }
    [[nodiscard]] bool at_end() const noexcept { return reader_.at_end(); }

private:
    ByteReader reader_;
    ValidationResult& result_;
    std::uint32_t index_;
};

[[nodiscard]] bool decode_segment(Decoder& d, Segment& s, const Limits& limits) {
    std::uint16_t kind = 0;
    std::uint16_t encoding = 0;
    std::uint16_t payload_length = 0;
    if (!d.u16(kind) || !d.u16(encoding)) {
        return false;
    }
    if (kind == 0 || kind > static_cast<std::uint16_t>(SegmentKind::Policy)) {
        return d.fail(ReasonCode::PersistKindInvalid, kind);
    }
    if (encoding == 0 ||
        encoding > static_cast<std::uint16_t>(SegmentEncodingId::AbstractPolicyV1)) {
        return d.fail(ReasonCode::PersistEncodingInvalid, encoding);
    }
    s.kind = static_cast<SegmentKind>(kind);
    s.encoding = static_cast<SegmentEncodingId>(encoding);
    std::uint64_t values[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::uint64_t& value : values) {
        if (!d.u64(value)) {
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
    if (!d.u16(payload_length)) {
        return false;
    }
    if (payload_length > limits.max_segment_payload_bytes) {
        return d.fail(ReasonCode::PersistAbsurdCount, payload_length);
    }
    return d.blob(payload_length, s.payload);
}

[[nodiscard]] bool decode_profile(Decoder& d, SegmentProfile& profile) {
    std::uint8_t support = 0;
    std::uint16_t name_length = 0;
    std::uint16_t capability_count = 0;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    if (!d.u64(id) || !d.u64(generation) || !d.u8(support) ||
        !d.u16(profile.allowed_kinds_mask) || !d.u32(profile.max_depth) ||
        !d.u32(profile.payload_width) || !d.flag(profile.allow_empty) ||
        !d.flag(profile.allow_consecutive_repeats) ||
        !d.flag(profile.allow_nonconsecutive_repeats) || !d.flag(profile.allow_loose) ||
        !d.u16(name_length)) {
        return false;
    }
    profile.id = SegmentProfileId{id};
    profile.generation = SegmentProfileGeneration{generation};
    profile.support = SupportClassification::AbstractSynthetic;
    if (support != static_cast<std::uint8_t>(SupportClassification::AbstractSynthetic)) {
        return d.fail(ReasonCode::PersistProfileInvalid, support);
    }
    if (name_length > 256) {
        return d.fail(ReasonCode::PersistAbsurdCount, name_length);
    }
    if (!d.text(name_length, profile.name)) {
        return false;
    }
    if (!d.u16(capability_count)) {
        return false;
    }
    if (capability_count > 64) {
        return d.fail(ReasonCode::PersistAbsurdCount, capability_count);
    }
    for (std::uint16_t i = 0; i < capability_count; ++i) {
        std::uint64_t key = 0;
        if (!d.u64(key)) {
            return false;
        }
        profile.required_capabilities.push_back(CapabilityKey{key});
    }
    if (!profile.id.valid() || !profile.generation.valid() || profile.payload_width == 0 ||
        profile.max_depth == 0) {
        return d.fail(ReasonCode::PersistProfileInvalid, profile.id.value());
    }
    return d.done();
}

[[nodiscard]] bool decode_policy(Decoder& d, SegmentPolicy& policy) {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t owner = 0;
    std::uint32_t count = 0;
    std::uint16_t kinds_mask = 0;
    std::uint32_t max_depth = 0;
    if (!d.u64(id) || !d.u64(generation) || !d.u64(owner) || !d.u32(count)) {
        return false;
    }
    if (count > 64) {
        return d.fail(ReasonCode::PersistAbsurdCount, count);
    }
    policy.id = SegmentPolicyId{id};
    policy.generation = SegmentPolicyGeneration{generation};
    policy.owner_scope = ScopeId{owner};
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t value = 0;
        if (!d.u64(value)) {
            return false;
        }
        policy.allowed_profiles.push_back(SegmentProfileId{value});
    }
    if (!d.u16(kinds_mask) || !d.u32(max_depth)) {
        return false;
    }
    policy.allowed_kinds_mask = kinds_mask;
    policy.max_depth = max_depth;
    if (!d.u32(count)) {
        return false;
    }
    if (count > 256) {
        return d.fail(ReasonCode::PersistAbsurdCount, count);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t value = 0;
        if (!d.u64(value)) {
            return false;
        }
        policy.required_segments.push_back(SegmentId{value});
    }
    if (!d.u32(count)) {
        return false;
    }
    if (count > 256) {
        return d.fail(ReasonCode::PersistAbsurdCount, count);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t value = 0;
        if (!d.u64(value)) {
            return false;
        }
        policy.forbidden_segments.push_back(SegmentId{value});
    }
    if (!d.flag(policy.allow_replacement) || !d.flag(policy.allow_derivation) ||
        !d.flag(policy.require_strict)) {
        return false;
    }
    if (!policy.id.valid() || !policy.generation.valid()) {
        return d.fail(ReasonCode::PersistProfileInvalid, policy.id.value());
    }
    return d.done();
}

[[nodiscard]] bool decode_list(Decoder& d, const Limits& limits,
                               const std::vector<SegmentProfile>& profiles, SegmentList& list) {
    SegmentListContent& c = list.content;
    std::uint8_t strictness = 0;
    std::uint16_t payload_width = 0;
    std::uint32_t segment_count = 0;
    std::uint64_t values[4] = {0, 0, 0, 0};
    for (std::uint64_t& value : values) {
        if (!d.u64(value)) {
            return false;
        }
    }
    c.id = SegmentListId{values[0]};
    c.scope = ScopeId{values[1]};
    c.profile = SegmentProfileId{values[2]};
    c.profile_generation = SegmentProfileGeneration{values[3]};
    if (!d.u64(values[0]) || !d.u64(values[1])) {
        return false;
    }
    c.policy = SegmentPolicyId{values[0]};
    c.policy_generation = SegmentPolicyGeneration{values[1]};
    if (!d.u8(strictness) || !d.u16(payload_width)) {
        return false;
    }
    if (strictness != static_cast<std::uint8_t>(Strictness::Strict) &&
        strictness != static_cast<std::uint8_t>(Strictness::Loose)) {
        return d.fail(ReasonCode::PersistCorrupt, strictness);
    }
    c.strictness = static_cast<Strictness>(strictness);
    if (payload_width == 0 || payload_width > limits.max_segment_payload_bytes) {
        return d.fail(ReasonCode::PersistProfileInvalid, payload_width);
    }
    for (std::uint64_t& value : values) {
        if (!d.u64(value)) {
            return false;
        }
    }
    c.topology = TopologyGeneration{values[0]};
    c.capability = CapabilityGeneration{values[1]};
    c.path = PathId{values[2]};
    c.path_authority = PathAuthorityGeneration{values[3]};
    for (std::uint64_t& value : values) {
        if (!d.u64(value)) {
            return false;
        }
    }
    c.route = RouteId{values[0]};
    c.route_generation = RouteGeneration{values[1]};
    c.constraint = ConstraintEvaluationId{values[2]};
    c.constraint_generation = ConstraintGeneration{values[3]};

    std::uint64_t generation = 0;
    if (!d.u64(generation)) {
        return false;
    }
    list.generation = SegmentListGeneration{generation};
    if (!d.u64(values[0]) || !d.u64(values[1]) || !d.u64(values[2])) {
        return false;
    }
    list.lineage.supersedes = SegmentListGeneration{values[0]};
    list.lineage.superseded_by = SegmentListId{values[1]};
    list.lineage.history = HistoryId{values[2]};
    for (std::uint64_t& value : values) {
        if (!d.u64(value)) {
            return false;
        }
    }
    list.authority.publisher = PublisherId{values[0]};
    list.authority.boot = WorkerBootId{values[1]};
    list.authority.epoch = CoordinatorEpoch{values[2]};
    list.authority.attempt = MutationAttemptId{values[3]};
    std::uint64_t provenance[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::uint64_t& field : provenance) {
        if (!d.u64(field)) {
            return false;
        }
    }
    list.provenance.source_path = PathId{provenance[0]};
    list.provenance.source_path_authority = PathAuthorityGeneration{provenance[1]};
    list.provenance.source_route = RouteId{provenance[2]};
    list.provenance.source_route_generation = RouteGeneration{provenance[3]};
    list.provenance.source_constraint = ConstraintEvaluationId{provenance[4]};
    list.provenance.source_constraint_generation = ConstraintGeneration{provenance[5]};
    list.provenance.derivation_policy = SegmentPolicyGeneration{provenance[6]};
    list.provenance.observed_topology = TopologyGeneration{provenance[7]};
    list.provenance.observed_capability = CapabilityGeneration{provenance[8]};
    list.provenance.publisher = PublisherId{provenance[9]};
    list.provenance.boot = WorkerBootId{provenance[10]};
    std::uint64_t provenance_epoch = 0;
    if (!d.u64(provenance_epoch)) {
        return false;
    }
    list.provenance.epoch = CoordinatorEpoch{provenance_epoch};

    std::uint8_t state = 0;
    std::uint8_t currentness = 0;
    if (!d.u8(state) || !d.u8(currentness)) {
        return false;
    }
    if (state == 0 || state > static_cast<std::uint8_t>(LifecycleState::Retired)) {
        return d.fail(ReasonCode::PersistCorrupt, state);
    }
    if (currentness > static_cast<std::uint8_t>(Currentness::FencedPublisher)) {
        return d.fail(ReasonCode::PersistCorrupt, currentness);
    }
    list.state = static_cast<LifecycleState>(state);
    list.currentness = static_cast<Currentness>(currentness);
    if (!d.u64(list.content_digest.hi) || !d.u64(list.content_digest.lo) ||
        !d.u64(list.sequence_digest.hi) || !d.u64(list.sequence_digest.lo) ||
        !d.u64(list.committed_revision) || !d.u32(segment_count)) {
        return false;
    }
    if (segment_count > limits.max_segments_per_list) {
        return d.fail(ReasonCode::PersistDepthInvalid, segment_count);
    }
    if (!list.generation.valid()) {
        return d.fail(ReasonCode::PersistImpossibleGeneration, generation);
    }
    for (std::uint32_t i = 0; i < segment_count; ++i) {
        Segment segment{};
        if (!decode_segment(d, segment, limits)) {
            return false;
        }
        c.segments.push_back(std::move(segment));
    }
    if (!d.done()) {
        return false;
    }
    if (c.profile.valid()) {
        bool found = false;
        for (const SegmentProfile& profile : profiles) {
            if (profile.id == c.profile) {
                found = true;
                break;
            }
        }
        if (!found) {
            return d.fail(ReasonCode::PersistDanglingReference, c.profile.value());
        }
    }
    const Digest128 recomputed_sequence = sequence_digest(c.segments, payload_width);
    const Digest128 recomputed_content = content_digest(c, payload_width);
    if (!(recomputed_sequence == list.sequence_digest) ||
        !(recomputed_content == list.content_digest)) {
        return d.fail(ReasonCode::PersistDigestMismatch, list.generation.value());
    }
    return true;
}

[[nodiscard]] bool decode_history(Decoder& d, DurableHistoryEntry& entry) {
    std::uint64_t id = 0;
    std::uint64_t list = 0;
    std::uint64_t generation = 0;
    std::uint8_t state = 0;
    std::uint8_t currentness = 0;
    if (!d.u64(id) || !d.u64(list) || !d.u64(generation) || !d.u8(state) ||
        !d.u8(currentness) || !d.u64(entry.digest.hi) || !d.u64(entry.digest.lo) ||
        !d.u64(entry.revision)) {
        return false;
    }
    if (state == 0 || state > static_cast<std::uint8_t>(LifecycleState::Retired)) {
        return d.fail(ReasonCode::PersistCorrupt, state);
    }
    if (currentness > static_cast<std::uint8_t>(Currentness::FencedPublisher)) {
        return d.fail(ReasonCode::PersistCorrupt, currentness);
    }
    entry.id = HistoryId{id};
    entry.list = SegmentListId{list};
    entry.generation = SegmentListGeneration{generation};
    entry.state = static_cast<LifecycleState>(state);
    entry.currentness = static_cast<Currentness>(currentness);
    if (!entry.id.valid() || !entry.generation.valid()) {
        return d.fail(ReasonCode::PersistImpossibleGeneration, generation);
    }
    return d.done();
}

[[nodiscard]] bool decode_attempt(Decoder& d, DurableAttemptRecord& entry) {
    std::uint64_t attempt = 0;
    std::uint64_t list = 0;
    std::uint64_t generation = 0;
    if (!d.u64(attempt) || !d.u64(list) || !d.u64(entry.payload.hi) || !d.u64(entry.payload.lo) ||
        !d.u64(generation) || !d.flag(entry.committed)) {
        return false;
    }
    entry.attempt = MutationAttemptId{attempt};
    entry.list = SegmentListId{list};
    entry.generation = SegmentListGeneration{generation};
    if (!entry.attempt.valid()) {
        return d.fail(ReasonCode::PersistImpossibleGeneration, attempt);
    }
    return d.done();
}

[[nodiscard]] bool decode_publisher(Decoder& d, PublisherState& publisher) {
    std::uint64_t id = 0;
    std::uint64_t boot = 0;
    std::uint64_t scope = 0;
    std::uint64_t epoch = 0;
    if (!d.u64(id) || !d.u64(boot) || !d.u64(scope) || !d.u64(epoch) ||
        !d.flag(publisher.alive) || !d.flag(publisher.fenced)) {
        return false;
    }
    publisher.id = PublisherId{id};
    publisher.boot = WorkerBootId{boot};
    publisher.scope = ScopeId{scope};
    publisher.epoch = CoordinatorEpoch{epoch};
    if (!publisher.id.valid() || !publisher.boot.valid()) {
        return d.fail(ReasonCode::PersistImpossibleGeneration, id);
    }
    return d.done();
}

} // namespace

ByteBuffer encode_durable_state(const DurableState& state, const Limits& limits,
                                ValidationResult& result) {
    result.set_limits(&limits);
    Encoder enc{};
    enc.limits = &limits;
    enc.result = &result;

    {
        ByteWriter body;
        body.u64(state.epoch.value());
        body.u64(state.revision);
        enc.emit(RecordType::Meta, body.buffer());
    }
    for (const SegmentProfile& profile : state.profiles) {
        ByteWriter body;
        body.u64(profile.id.value());
        body.u64(profile.generation.value());
        body.u8(static_cast<std::uint8_t>(SupportClassification::AbstractSynthetic));
        body.u16(profile.allowed_kinds_mask);
        body.u32(profile.max_depth);
        body.u32(profile.payload_width);
        body.u8(profile.allow_empty ? 1 : 0);
        body.u8(profile.allow_consecutive_repeats ? 1 : 0);
        body.u8(profile.allow_nonconsecutive_repeats ? 1 : 0);
        body.u8(profile.allow_loose ? 1 : 0);
        body.u16(static_cast<std::uint16_t>(profile.name.size()));
        body.bytes(as_bytes(profile.name));
        body.u16(static_cast<std::uint16_t>(profile.required_capabilities.size()));
        for (const CapabilityKey key : profile.required_capabilities) {
            body.u64(key.value());
        }
        enc.emit(RecordType::Profile, body.buffer());
    }
    for (const SegmentPolicy& policy : state.policies) {
        ByteWriter body;
        body.u64(policy.id.value());
        body.u64(policy.generation.value());
        body.u64(policy.owner_scope.value());
        body.u32(static_cast<std::uint32_t>(policy.allowed_profiles.size()));
        for (const SegmentProfileId id : policy.allowed_profiles) {
            body.u64(id.value());
        }
        body.u16(policy.allowed_kinds_mask);
        body.u32(policy.max_depth);
        body.u32(static_cast<std::uint32_t>(policy.required_segments.size()));
        for (const SegmentId id : policy.required_segments) {
            body.u64(id.value());
        }
        body.u32(static_cast<std::uint32_t>(policy.forbidden_segments.size()));
        for (const SegmentId id : policy.forbidden_segments) {
            body.u64(id.value());
        }
        body.u8(policy.allow_replacement ? 1 : 0);
        body.u8(policy.allow_derivation ? 1 : 0);
        body.u8(policy.require_strict ? 1 : 0);
        enc.emit(RecordType::Policy, body.buffer());
    }
    for (const SegmentList& list : state.lists) {
        const SegmentListContent& c = list.content;
        std::uint32_t width = bound_payload_width(state, c.profile);
        if (width == 0) {
            for (const Segment& segment : c.segments) {
                width = width > static_cast<std::uint32_t>(segment.payload.size())
                            ? width
                            : static_cast<std::uint32_t>(segment.payload.size());
            }
            if (width == 0) {
                width = 1;
            }
        }
        if (width > 0xFFFFu) {
            width = 1;
        }
        ByteWriter body;
        body.u64(c.id.value());
        body.u64(c.scope.value());
        body.u64(c.profile.value());
        body.u64(c.profile_generation.value());
        body.u64(c.policy.value());
        body.u64(c.policy_generation.value());
        body.u8(static_cast<std::uint8_t>(c.strictness));
        body.u16(static_cast<std::uint16_t>(width));
        body.u64(c.topology.value());
        body.u64(c.capability.value());
        body.u64(c.path.value());
        body.u64(c.path_authority.value());
        body.u64(c.route.value());
        body.u64(c.route_generation.value());
        body.u64(c.constraint.value());
        body.u64(c.constraint_generation.value());
        body.u64(list.generation.value());
        body.u64(list.lineage.supersedes.value());
        body.u64(list.lineage.superseded_by.value());
        body.u64(list.lineage.history.value());
        body.u64(list.authority.publisher.value());
        body.u64(list.authority.boot.value());
        body.u64(list.authority.epoch.value());
        body.u64(list.authority.attempt.value());
        body.u64(list.provenance.source_path.value());
        body.u64(list.provenance.source_path_authority.value());
        body.u64(list.provenance.source_route.value());
        body.u64(list.provenance.source_route_generation.value());
        body.u64(list.provenance.source_constraint.value());
        body.u64(list.provenance.source_constraint_generation.value());
        body.u64(list.provenance.derivation_policy.value());
        body.u64(list.provenance.observed_topology.value());
        body.u64(list.provenance.observed_capability.value());
        body.u64(list.provenance.publisher.value());
        body.u64(list.provenance.boot.value());
        body.u64(list.provenance.epoch.value());
        body.u8(static_cast<std::uint8_t>(list.state));
        body.u8(static_cast<std::uint8_t>(list.currentness));
        body.u64(list.content_digest.hi);
        body.u64(list.content_digest.lo);
        body.u64(list.sequence_digest.hi);
        body.u64(list.sequence_digest.lo);
        body.u64(list.committed_revision);
        body.u32(static_cast<std::uint32_t>(c.segments.size()));
        for (const Segment& segment : c.segments) {
            write_segment(body, segment);
        }
        enc.emit(RecordType::List, body.buffer());
    }
    for (const DurableHistoryEntry& entry : state.history) {
        ByteWriter body;
        body.u64(entry.id.value());
        body.u64(entry.list.value());
        body.u64(entry.generation.value());
        body.u8(static_cast<std::uint8_t>(entry.state));
        body.u8(static_cast<std::uint8_t>(entry.currentness));
        body.u64(entry.digest.hi);
        body.u64(entry.digest.lo);
        body.u64(entry.revision);
        enc.emit(RecordType::History, body.buffer());
    }
    for (const DurableAttemptRecord& entry : state.attempts) {
        ByteWriter body;
        body.u64(entry.attempt.value());
        body.u64(entry.list.value());
        body.u64(entry.payload.hi);
        body.u64(entry.payload.lo);
        body.u64(entry.generation.value());
        body.u8(entry.committed ? 1 : 0);
        enc.emit(RecordType::Attempt, body.buffer());
    }
    for (const PublisherState& publisher : state.publishers) {
        ByteWriter body;
        body.u64(publisher.id.value());
        body.u64(publisher.boot.value());
        body.u64(publisher.scope.value());
        body.u64(publisher.epoch.value());
        body.u8(publisher.alive ? 1 : 0);
        body.u8(publisher.fenced ? 1 : 0);
        enc.emit(RecordType::Publisher, body.buffer());
    }
    for (const WorkerBootId boot : state.fenced_boots) {
        ByteWriter body;
        body.u64(boot.value());
        enc.emit(RecordType::FencedBoot, body.buffer());
    }

    if (enc.failed) {
        return ByteBuffer{};
    }

    ByteWriter head;
    head.bytes(magic_bytes());
    head.u16(kPersistenceFormatVersion);
    head.u16(0);
    head.u64(static_cast<std::uint64_t>(enc.records.size()));
    head.u32(enc.count);
    head.u32(crc32c(head.span()));

    ByteWriter file;
    file.bytes(head.span());
    file.bytes(enc.records.span());
    file.u32(crc32c(enc.records.span()));
    return std::move(file).take();
}

bool decode_durable_state(std::span<const std::byte> bytes, const Limits& limits, DurableState& out,
                          ValidationResult& result) {
    result.set_limits(&limits);
    if (bytes.empty()) {
        result.add(ReasonCode::PersistEmpty);
        return false;
    }
    if (bytes.size() < kHeaderBytes + kTrailerBytes) {
        result.add(ReasonCode::PersistTruncated, 0, bytes.size());
        return false;
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        result.add(ReasonCode::PersistBadMagic);
        return false;
    }
    ByteReader header(bytes.first(kHeaderBytes));
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint64_t payload_length = 0;
    std::uint32_t record_count = 0;
    std::uint32_t header_crc = 0;
    if (!header.skip(4) || !header.u16(version) || !header.u16(flags) ||
        !header.u64(payload_length) || !header.u32(record_count) || !header.u32(header_crc)) {
        result.add(ReasonCode::PersistTruncated);
        return false;
    }
    if (header_crc != crc32c(bytes.first(kHeaderBytes - 4))) {
        result.add(ReasonCode::PersistBadIntegrity);
        return false;
    }
    if (version != kPersistenceFormatVersion || flags != 0) {
        result.add(ReasonCode::PersistBadVersion, 0, version);
        return false;
    }
    if (payload_length > bytes.size()) {
        result.add(ReasonCode::PersistOverflow, 0, payload_length);
        return false;
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kHeaderBytes) + payload_length + kTrailerBytes;
    if (expected > bytes.size()) {
        result.add(ReasonCode::PersistTruncated, 0, bytes.size());
        return false;
    }
    if (expected < bytes.size()) {
        result.add(ReasonCode::PersistTrailingBytes, 0, bytes.size() - expected);
        return false;
    }
    const auto payload_size = static_cast<std::size_t>(payload_length);
    const std::span<const std::byte> records = bytes.subspan(kHeaderBytes, payload_size);
    ByteReader trailer(bytes.subspan(kHeaderBytes + payload_size, kTrailerBytes));
    std::uint32_t payload_crc = 0;
    if (!trailer.u32(payload_crc)) {
        result.add(ReasonCode::PersistTruncated);
        return false;
    }
    if (payload_crc != crc32c(records)) {
        result.add(ReasonCode::PersistBadIntegrity);
        return false;
    }
    const std::uint64_t plausible = static_cast<std::uint64_t>(limits.max_lists) +
                                    limits.max_profiles + limits.max_policies + limits.max_history +
                                    limits.max_attempt_records + limits.max_publishers + 1;
    if (record_count > plausible) {
        result.add(ReasonCode::PersistAbsurdCount, 0, record_count);
        return false;
    }

    DurableState decoded{};
    decoded.format_version = version;
    std::uint32_t parsed = 0;
    ByteReader r(records);
    while (!r.at_end()) {
        std::uint16_t raw_type = 0;
        std::uint32_t length = 0;
        if (!r.u16(raw_type) || !r.u32(length)) {
            result.add(ReasonCode::PersistTruncated, parsed);
            return false;
        }
        if (raw_type < static_cast<std::uint16_t>(RecordType::Meta) ||
            raw_type > static_cast<std::uint16_t>(RecordType::FencedBoot)) {
            result.add(ReasonCode::PersistBadRecord, parsed, raw_type);
            return false;
        }
        if (length > limits.max_persistence_record_bytes) {
            result.add(ReasonCode::PersistAbsurdCount, parsed, length);
            return false;
        }
        if (r.remaining() < length) {
            result.add(ReasonCode::PersistTruncated, parsed);
            return false;
        }
        const std::span<const std::byte> body = r.rest().first(length);
        if (!r.skip(length)) {
            result.add(ReasonCode::PersistTruncated, parsed);
            return false;
        }
        Decoder d(body, result, parsed);
        switch (static_cast<RecordType>(raw_type)) {
            case RecordType::Meta: {
                std::uint64_t epoch = 0;
                std::uint64_t revision = 0;
                if (!d.u64(epoch) || !d.u64(revision) || !d.done()) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                decoded.epoch = CoordinatorEpoch{epoch};
                decoded.revision = revision;
                break;
            }
            case RecordType::Profile: {
                SegmentProfile profile{};
                if (!decode_profile(d, profile)) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                for (const SegmentProfile& existing : decoded.profiles) {
                    if (existing.id == profile.id) {
                        result.add(ReasonCode::PersistDuplicateIdentity, parsed, profile.id.value());
                        return false;
                    }
                }
                decoded.profiles.push_back(std::move(profile));
                break;
            }
            case RecordType::Policy: {
                SegmentPolicy policy{};
                if (!decode_policy(d, policy)) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                for (const SegmentPolicy& existing : decoded.policies) {
                    if (existing.id == policy.id) {
                        result.add(ReasonCode::PersistDuplicateIdentity, parsed, policy.id.value());
                        return false;
                    }
                }
                decoded.policies.push_back(std::move(policy));
                break;
            }
            case RecordType::List: {
                SegmentList list{};
                if (!decode_list(d, limits, decoded.profiles, list)) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                for (const SegmentList& existing : decoded.lists) {
                    if (existing.content.id == list.content.id) {
                        result.add(ReasonCode::PersistDuplicateIdentity, parsed,
                                   list.content.id.value());
                        return false;
                    }
                }
                decoded.lists.push_back(std::move(list));
                break;
            }
            case RecordType::History: {
                DurableHistoryEntry entry{};
                if (!decode_history(d, entry)) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                decoded.history.push_back(entry);
                break;
            }
            case RecordType::Attempt: {
                DurableAttemptRecord entry{};
                if (!decode_attempt(d, entry)) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                decoded.attempts.push_back(entry);
                break;
            }
            case RecordType::Publisher: {
                PublisherState publisher{};
                if (!decode_publisher(d, publisher)) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                decoded.publishers.push_back(publisher);
                break;
            }
            case RecordType::FencedBoot: {
                std::uint64_t boot = 0;
                if (!d.u64(boot) || !d.done()) {
                    if (result.ok()) {
                        result.add(ReasonCode::PersistBadRecord, parsed);
                    }
                    return false;
                }
                if (!WorkerBootId{boot}.valid()) {
                    result.add(ReasonCode::PersistImpossibleGeneration, parsed, boot);
                    return false;
                }
                decoded.fenced_boots.push_back(WorkerBootId{boot});
                break;
            }
            case RecordType::Invalid:
            default:
                result.add(ReasonCode::PersistBadRecord, parsed, raw_type);
                return false;
        }
        ++parsed;
    }
    if (parsed != record_count) {
        result.add(ReasonCode::PersistCountMismatch, 0, record_count);
        return false;
    }
    if (decoded.history.size() > limits.max_history) {
        result.add(ReasonCode::PersistAbsurdCount, 0, decoded.history.size());
        return false;
    }
    if (decoded.attempts.size() > limits.max_attempt_records) {
        result.add(ReasonCode::PersistAbsurdCount, 0, decoded.attempts.size());
        return false;
    }
    if (decoded.lists.size() > limits.max_lists) {
        result.add(ReasonCode::PersistAbsurdCount, 0, decoded.lists.size());
        return false;
    }
    if (decoded.profiles.size() > limits.max_profiles) {
        result.add(ReasonCode::PersistAbsurdCount, 0, decoded.profiles.size());
        return false;
    }
    if (decoded.policies.size() > limits.max_policies) {
        result.add(ReasonCode::PersistAbsurdCount, 0, decoded.policies.size());
        return false;
    }
    if (decoded.publishers.size() > limits.max_publishers) {
        result.add(ReasonCode::PersistAbsurdCount, 0, decoded.publishers.size());
        return false;
    }
    out = std::move(decoded);
    return true;
}

// --------------------------------------------------------------- file backend
FilePersistence::FilePersistence(std::filesystem::path path) : path_(std::move(path)) {}

bool FilePersistence::save(const DurableState& state, const Limits& limits,
                           ValidationResult& out) {
    out.set_limits(&limits);
    const ByteBuffer bytes = encode_durable_state(state, limits, out);
    if (out.failed() || bytes.empty()) {
        if (out.ok()) {
            out.add(ReasonCode::PersistBadRecord);
        }
        return false;
    }
    std::error_code ec;
    const std::filesystem::path parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            out.add(ReasonCode::PersistIoError, 0, static_cast<std::uint64_t>(ec.value()));
            return false;
        }
    }
    std::filesystem::path temp = path_;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            out.add(ReasonCode::PersistIoError);
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            out.add(ReasonCode::PersistIoError);
            file.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, path_, ec);
    if (ec) {
        std::error_code cleanup;
        std::filesystem::remove(temp, cleanup);
        out.add(ReasonCode::PersistIoError, 0, static_cast<std::uint64_t>(ec.value()));
        return false;
    }
    return true;
}

bool FilePersistence::load(LoadReport& report, const Limits& limits) {
    report.result.set_limits(&limits);
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        report.empty = true;
        report.loaded = false;
        return true;
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        report.result.add(ReasonCode::PersistIoError);
        return false;
    }
    std::vector<std::byte> bytes;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        report.result.add(ReasonCode::PersistIoError);
        return false;
    }
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file) {
            report.result.add(ReasonCode::PersistIoError);
            return false;
        }
    }
    DurableState state{};
    if (!decode_durable_state(std::span<const std::byte>(bytes.data(), bytes.size()), limits,
                              state, report.result)) {
        report.loaded = false;
        return false;
    }
    report.loaded = true;
    report.empty = false;
    report.state = std::move(state);
    report.lists_recovered = report.state.lists.size();
    report.history_entries = report.state.history.size();
    return true;
}

bool FilePersistence::erase() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    return !ec;
}

// ------------------------------------------------------------- memory backend
bool MemoryPersistence::save(const DurableState& state, const Limits& limits,
                             ValidationResult& out) {
    out.set_limits(&limits);
    ++save_count_;
    if (fail_next_) {
        fail_next_ = false;
        out.add(ReasonCode::CommitPersistenceFailure);
        return false;
    }
    const ByteBuffer bytes = encode_durable_state(state, limits, out);
    if (out.failed() || bytes.empty()) {
        if (out.ok()) {
            out.add(ReasonCode::PersistBadRecord);
        }
        return false;
    }
    bytes_ = bytes;
    return true;
}

bool MemoryPersistence::load(LoadReport& report, const Limits& limits) {
    report.result.set_limits(&limits);
    if (bytes_.empty()) {
        report.empty = true;
        report.loaded = false;
        return true;
    }
    DurableState state{};
    if (!decode_durable_state(std::span<const std::byte>(bytes_.data(), bytes_.size()), limits,
                              state, report.result)) {
        report.loaded = false;
        return false;
    }
    report.loaded = true;
    report.empty = false;
    report.state = std::move(state);
    report.lists_recovered = report.state.lists.size();
    report.history_entries = report.state.history.size();
    return true;
}

bool MemoryPersistence::erase() {
    bytes_.clear();
    return true;
}

void MemoryPersistence::fail_next_save() {
    fail_next_ = true;
}

bool MemoryPersistence::corrupt_byte(std::size_t offset, std::uint8_t xor_mask) {
    if (offset >= bytes_.size()) {
        return false;
    }
    bytes_[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(bytes_[offset]) ^ xor_mask);
    return true;
}

void MemoryPersistence::set_bytes(std::span<const std::byte> bytes) {
    bytes_.assign(bytes.begin(), bytes.end());
}

} // namespace srf
