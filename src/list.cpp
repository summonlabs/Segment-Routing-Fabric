// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/list.hpp"

#include "srf/bytes.hpp"

namespace srf {

std::string_view segment_list_state_name(const SegmentList& list) noexcept {
    return lifecycle_state_name(list.state);
}

SegmentListId derive_segment_list_id(std::string_view scope, std::string_view name) noexcept {
    ByteWriter w;
    w.u16(static_cast<std::uint16_t>(0x1D01));
    w.u32(static_cast<std::uint32_t>(scope.size()));
    w.bytes(as_bytes(scope));
    w.u32(static_cast<std::uint32_t>(name.size()));
    w.bytes(as_bytes(name));
    const std::uint64_t h = fnv1a64(w.span());
    return SegmentListId{h == 0 ? 1u : h};
}

} // namespace srf
