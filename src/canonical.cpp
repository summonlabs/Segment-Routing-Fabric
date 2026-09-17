// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/canonical.hpp"

#include "srf/list.hpp"

namespace srf {
namespace {

constexpr std::uint16_t kSegmentMagic = 0xC501;
constexpr std::uint16_t kContentMagic = 0xC502;
constexpr std::uint16_t kSequenceMagic = 0xC503;

void write_segment(ByteWriter& w, const Segment& s, std::uint32_t payload_width) {
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
    w.u16(static_cast<std::uint16_t>(payload_width));
    // The encoder always writes exactly payload_width payload bytes: the payload is
    // truncated to the width and then zero padded. Structural validation rejects an
    // over-wide payload before any digest is committed, so this is a total and
    // deterministic contract for the public encoding API rather than a repair.
    const std::span<const std::byte> payload = as_bytes(s.payload);
    const std::size_t usable =
        payload.size() < static_cast<std::size_t>(payload_width) ? payload.size()
                                                                 : payload_width;
    w.bytes(payload.first(usable));
    for (std::size_t i = usable; i < static_cast<std::size_t>(payload_width); ++i) {
        w.u8(0);
    }
}

} // namespace

bool canonical_payload_equal(std::span<const std::byte> a, std::span<const std::byte> b,
                             std::uint32_t payload_width) noexcept {
    if (a.size() > payload_width || b.size() > payload_width) {
        return false;
    }
    for (std::uint32_t i = 0; i < payload_width; ++i) {
        const std::byte x = i < a.size() ? a[i] : std::byte{0};
        const std::byte y = i < b.size() ? b[i] : std::byte{0};
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool canonical_payload_padded_equal(const Segment& a, const Segment& b) noexcept {
    const std::size_t width = a.payload.size() > b.payload.size() ? a.payload.size()
                                                                  : b.payload.size();
    return canonical_payload_equal(as_bytes(a.payload), as_bytes(b.payload),
                                   static_cast<std::uint32_t>(width));
}

ByteBuffer canonical_segment_bytes(const Segment& segment, std::uint32_t payload_width) {
    ByteWriter w;
    w.reserve(80 + payload_width);
    w.u16(kSegmentMagic);
    write_segment(w, segment, payload_width);
    return std::move(w).take();
}

ByteBuffer canonical_sequence_bytes(std::span<const Segment> segments,
                                    std::uint32_t payload_width) {
    ByteWriter w;
    w.reserve(8 + segments.size() * (80 + payload_width));
    w.u16(kSequenceMagic);
    w.u32(static_cast<std::uint32_t>(segments.size()));
    for (const Segment& s : segments) {
        write_segment(w, s, payload_width);
    }
    return std::move(w).take();
}

ByteBuffer canonical_content_bytes(const SegmentListContent& content,
                                   std::uint32_t payload_width) {
    ByteWriter w;
    w.reserve(192 + content.segments.size() * (80 + payload_width));
    w.u16(kContentMagic);
    w.u32(payload_width);
    w.u64(content.id.value());
    w.u64(content.scope.value());
    w.u64(content.profile.value());
    w.u64(content.profile_generation.value());
    w.u64(content.policy.value());
    w.u64(content.policy_generation.value());
    w.u8(static_cast<std::uint8_t>(content.strictness));
    w.u64(content.topology.value());
    w.u64(content.capability.value());
    w.u64(content.path.value());
    w.u64(content.path_authority.value());
    w.u64(content.route.value());
    w.u64(content.route_generation.value());
    w.u64(content.constraint.value());
    w.u64(content.constraint_generation.value());
    w.u32(static_cast<std::uint32_t>(content.segments.size()));
    for (const Segment& s : content.segments) {
        write_segment(w, s, payload_width);
    }
    return std::move(w).take();
}

Digest128 content_digest(const SegmentListContent& content, std::uint32_t payload_width) {
    const ByteBuffer bytes = canonical_content_bytes(content, payload_width);
    return digest128(as_bytes(bytes));
}

Digest128 sequence_digest(std::span<const Segment> segments, std::uint32_t payload_width) {
    const ByteBuffer bytes = canonical_sequence_bytes(segments, payload_width);
    return digest128(as_bytes(bytes));
}

} // namespace srf
