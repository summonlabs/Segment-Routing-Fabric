// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <span>

#include "srf/bytes.hpp"
#include "srf/digest.hpp"
#include "srf/segment.hpp"

namespace srf {

struct SegmentList;
struct SegmentListContent;

/// Canonical byte encoding of one segment at the profile payload width.
///
/// The encoding is little-endian and field-explicit. It always emits every field,
/// including zero-valued reserved fields, so that the encoding is a pure function
/// of the semantic content. It never emits a raw C++ object representation.
[[nodiscard]] ByteBuffer canonical_segment_bytes(const Segment& segment,
                                                 std::uint32_t payload_width);

/// Canonical byte encoding of the ordered segment sequence only. Order is emitted
/// verbatim: canonicalization normalizes each segment and never sorts, reorders,
/// deduplicates or repairs the sequence.
[[nodiscard]] ByteBuffer canonical_sequence_bytes(std::span<const Segment> segments,
                                                  std::uint32_t payload_width);

/// Canonical byte encoding of the whole immutable content of a list: identity,
/// generation-independent semantic identity, profile and policy bindings,
/// strictness, dependency generations, authority bindings and the ordered
/// sequence. Mutable lifecycle state is deliberately excluded so that the content
/// digest is stable across lifecycle transitions.
[[nodiscard]] ByteBuffer canonical_content_bytes(const SegmentListContent& content,
                                                 std::uint32_t payload_width);

/// Order-sensitive digest of the immutable content.
[[nodiscard]] Digest128 content_digest(const SegmentListContent& content,
                                       std::uint32_t payload_width);

/// Digest of the ordered sequence alone. Two lists that differ only by segment
/// order have different sequence digests.
[[nodiscard]] Digest128 sequence_digest(std::span<const Segment> segments,
                                        std::uint32_t payload_width);

/// Payload normalization helper used by equality and by the oracle.
[[nodiscard]] bool canonical_payload_equal(std::span<const std::byte> a,
                                           std::span<const std::byte> b,
                                           std::uint32_t payload_width) noexcept;

/// Payload equality of two segments under zero-padding to the longer of the two
/// payloads. Used by Segment::operator== so that an accepted padded spelling and
/// an accepted unpadded spelling of the same payload compare equal.
[[nodiscard]] bool canonical_payload_padded_equal(const Segment& a,
                                                  const Segment& b) noexcept;

} // namespace srf
