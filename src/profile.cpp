// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/profile.hpp"

#include "srf/canonical.hpp"

namespace srf {

bool SegmentProfile::allows_kind(SegmentKind kind) const noexcept {
    if (kind == SegmentKind::Invalid) {
        return false;
    }
    return (allowed_kinds_mask & kind_bit(kind)) != 0;
}

Digest128 SegmentProfile::digest() const noexcept {
    ByteWriter w;
    w.u16(static_cast<std::uint16_t>(0xA510));
    w.u64(id.value());
    w.u64(generation.value());
    w.u8(static_cast<std::uint8_t>(support));
    w.u16(allowed_kinds_mask);
    w.u32(max_depth);
    w.u32(payload_width);
    w.u8(allow_empty ? 1 : 0);
    w.u8(allow_consecutive_repeats ? 1 : 0);
    w.u8(allow_nonconsecutive_repeats ? 1 : 0);
    w.u8(allow_loose ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(required_capabilities.size()));
    for (const CapabilityKey key : required_capabilities) {
        w.u64(key.value());
    }
    w.bytes(as_bytes(name));
    return digest128(w.span());
}

SegmentProfile abstract_governance_profile() {
    SegmentProfile p{};
    p.id = SegmentProfileId{0x0001'0001ull};
    p.generation = SegmentProfileGeneration{1};
    p.name = "abstract-governance-v1";
    p.support = SupportClassification::AbstractSynthetic;
    p.allowed_kinds_mask = static_cast<std::uint16_t>(
        kind_bit(SegmentKind::Node) | kind_bit(SegmentKind::Adjacency) |
        kind_bit(SegmentKind::Endpoint) | kind_bit(SegmentKind::Binding) |
        kind_bit(SegmentKind::Policy));
    p.max_depth = 8;
    p.payload_width = 4;
    p.allow_empty = false;
    p.allow_consecutive_repeats = false;
    p.allow_nonconsecutive_repeats = false;
    p.allow_loose = false;
    p.required_capabilities = {CapabilityKey{0x1001}, CapabilityKey{0x1002}};
    return p;
}

SegmentProfile abstract_ordered_profile() {
    SegmentProfile p{};
    p.id = SegmentProfileId{0x0001'0002ull};
    p.generation = SegmentProfileGeneration{1};
    p.name = "abstract-ordered-v1";
    p.support = SupportClassification::AbstractSynthetic;
    p.allowed_kinds_mask = static_cast<std::uint16_t>(
        kind_bit(SegmentKind::Node) | kind_bit(SegmentKind::Adjacency) |
        kind_bit(SegmentKind::Endpoint));
    p.max_depth = 16;
    p.payload_width = 8;
    p.allow_empty = false;
    p.allow_consecutive_repeats = false;
    p.allow_nonconsecutive_repeats = true;
    p.allow_loose = true;
    p.required_capabilities = {CapabilityKey{0x1001}};
    return p;
}

SegmentProfile abstract_nullable_profile() {
    SegmentProfile p{};
    p.id = SegmentProfileId{0x0001'0003ull};
    p.generation = SegmentProfileGeneration{1};
    p.name = "abstract-nullable-v1";
    p.support = SupportClassification::AbstractSynthetic;
    p.allowed_kinds_mask = static_cast<std::uint16_t>(
        kind_bit(SegmentKind::Node) | kind_bit(SegmentKind::Policy));
    p.max_depth = 4;
    p.payload_width = 2;
    p.allow_empty = true;
    p.allow_consecutive_repeats = true;
    p.allow_nonconsecutive_repeats = true;
    p.allow_loose = false;
    p.required_capabilities = {};
    return p;
}

} // namespace srf
