// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/policy.hpp"

#include "srf/bytes.hpp"

namespace srf {

Digest128 SegmentPolicy::digest() const noexcept {
    ByteWriter w;
    w.u16(static_cast<std::uint16_t>(0xB700));
    w.u64(id.value());
    w.u64(generation.value());
    w.u64(owner_scope.value());
    w.u32(static_cast<std::uint32_t>(allowed_profiles.size()));
    for (const SegmentProfileId p : allowed_profiles) {
        w.u64(p.value());
    }
    w.u16(allowed_kinds_mask);
    w.u32(max_depth);
    w.u32(static_cast<std::uint32_t>(required_segments.size()));
    for (const SegmentId s : required_segments) {
        w.u64(s.value());
    }
    w.u32(static_cast<std::uint32_t>(forbidden_segments.size()));
    for (const SegmentId s : forbidden_segments) {
        w.u64(s.value());
    }
    w.u8(allow_replacement ? 1 : 0);
    w.u8(allow_derivation ? 1 : 0);
    w.u8(require_strict ? 1 : 0);
    return digest128(w.span());
}

} // namespace srf
