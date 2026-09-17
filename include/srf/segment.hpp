// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>

#include "srf/bytes.hpp"
#include "srf/digest.hpp"
#include "srf/ids.hpp"

namespace srf {

/// Typed segment kinds. Each kind has explicit validation semantics; no kind is a
/// generic container for protocol behaviour that this runtime does not implement.
enum class SegmentKind : std::uint16_t {
    Invalid = 0,
    Node = 1,
    Adjacency = 2,
    Endpoint = 3,
    Binding = 4,
    Policy = 5,
};

/// Explicit encoding identity. Only abstract governance encodings exist in 1.0.0.
/// There is no SR-MPLS or SRv6 encoding in this runtime.
enum class SegmentEncodingId : std::uint16_t {
    Invalid = 0,
    AbstractNodeV1 = 1,
    AbstractAdjacencyV1 = 2,
    AbstractEndpointV1 = 3,
    AbstractBindingV1 = 4,
    AbstractPolicyV1 = 5,
};

/// Strict and loose forwarding semantics, where a profile implements them.
enum class Strictness : std::uint8_t {
    Strict = 1,
    Loose = 2,
};

[[nodiscard]] std::string_view segment_kind_name(SegmentKind kind) noexcept;
[[nodiscard]] std::string_view segment_encoding_name(SegmentEncodingId encoding) noexcept;
[[nodiscard]] std::string_view strictness_name(Strictness strictness) noexcept;

/// The single encoding that is legal for a kind, or Invalid for an unknown kind.
[[nodiscard]] SegmentEncodingId expected_encoding_for(SegmentKind kind) noexcept;

/// One ordered element of a segment list.
///
/// Exactly one entity reference field is populated, and it is the one that the
/// kind requires. Every other reference field is a reserved field for that kind
/// and must be zero; a non-zero reserved field is a rejection, never ignored.
struct Segment {
    SegmentKind kind{SegmentKind::Invalid};
    SegmentEncodingId encoding{SegmentEncodingId::Invalid};
    SegmentId id{};
    SegmentGeneration generation{};

    NodeId node{};
    AdjacencyId adjacency{};
    EndpointId endpoint{};
    BindingId binding{};

    /// POLICY kind only.
    SegmentPolicyId policy{};
    SegmentPolicyGeneration policy_generation{};

    /// ADJACENCY kind only. The exact topology generation the adjacency was drawn
    /// from. An adjacency is never inferred from a label or a string name.
    TopologyGeneration topology{};

    /// Raw payload before canonical padding.
    ByteBuffer payload{};

    friend bool operator==(const Segment& a, const Segment& b) noexcept;
    friend bool operator!=(const Segment& a, const Segment& b) noexcept { return !(a == b); }
};

/// Semantic equality: compares canonical encodings, not raw field spellings. Two
/// segments whose payloads differ only by accepted zero padding are equal.
[[nodiscard]] bool segment_semantic_equal(const Segment& a, const Segment& b,
                                          std::uint32_t payload_width) noexcept;

} // namespace srf
