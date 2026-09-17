// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/segment.hpp"

#include "srf/canonical.hpp"

namespace srf {

std::string_view segment_kind_name(SegmentKind kind) noexcept {
    switch (kind) {
        case SegmentKind::Invalid: return "Invalid";
        case SegmentKind::Node: return "Node";
        case SegmentKind::Adjacency: return "Adjacency";
        case SegmentKind::Endpoint: return "Endpoint";
        case SegmentKind::Binding: return "Binding";
        case SegmentKind::Policy: return "Policy";
    }
    return "Unknown";
}

std::string_view segment_encoding_name(SegmentEncodingId encoding) noexcept {
    switch (encoding) {
        case SegmentEncodingId::Invalid: return "Invalid";
        case SegmentEncodingId::AbstractNodeV1: return "AbstractNodeV1";
        case SegmentEncodingId::AbstractAdjacencyV1: return "AbstractAdjacencyV1";
        case SegmentEncodingId::AbstractEndpointV1: return "AbstractEndpointV1";
        case SegmentEncodingId::AbstractBindingV1: return "AbstractBindingV1";
        case SegmentEncodingId::AbstractPolicyV1: return "AbstractPolicyV1";
    }
    return "Unknown";
}

std::string_view strictness_name(Strictness strictness) noexcept {
    switch (strictness) {
        case Strictness::Strict: return "Strict";
        case Strictness::Loose: return "Loose";
    }
    return "Unknown";
}

SegmentEncodingId expected_encoding_for(SegmentKind kind) noexcept {
    switch (kind) {
        case SegmentKind::Node: return SegmentEncodingId::AbstractNodeV1;
        case SegmentKind::Adjacency: return SegmentEncodingId::AbstractAdjacencyV1;
        case SegmentKind::Endpoint: return SegmentEncodingId::AbstractEndpointV1;
        case SegmentKind::Binding: return SegmentEncodingId::AbstractBindingV1;
        case SegmentKind::Policy: return SegmentEncodingId::AbstractPolicyV1;
        case SegmentKind::Invalid: break;
    }
    return SegmentEncodingId::Invalid;
}

namespace {

/// The encoding a segment effectively carries. An unset encoding is not a distinct
/// spelling: it resolves to the encoding the kind requires, exactly as
/// canonicalization resolves it. Comparing the raw field would let two segments
/// that canonicalize to identical bytes slip past the duplicate gates.
[[nodiscard]] SegmentEncodingId effective_encoding(const Segment& segment) noexcept {
    if (segment.encoding != SegmentEncodingId::Invalid) {
        return segment.encoding;
    }
    return expected_encoding_for(segment.kind);
}

[[nodiscard]] bool same_scalar_content(const Segment& a, const Segment& b) noexcept {
    return a.kind == b.kind && effective_encoding(a) == effective_encoding(b) && a.id == b.id &&
           a.generation == b.generation && a.node == b.node && a.adjacency == b.adjacency &&
           a.endpoint == b.endpoint && a.binding == b.binding && a.policy == b.policy &&
           a.policy_generation == b.policy_generation && a.topology == b.topology;
}

} // namespace

bool operator==(const Segment& a, const Segment& b) noexcept {
    if (!same_scalar_content(a, b)) {
        return false;
    }
    // Raw payload comparison plus a canonical comparison: padded and unpadded
    // spellings of the same payload are equal under canonicalization.
    return canonical_payload_padded_equal(a, b);
}

bool segment_semantic_equal(const Segment& a, const Segment& b,
                            std::uint32_t payload_width) noexcept {
    if (!same_scalar_content(a, b)) {
        return false;
    }
    return canonical_payload_equal(a.payload, b.payload, payload_width);
}

} // namespace srf
