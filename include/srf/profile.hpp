// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "srf/digest.hpp"
#include "srf/ids.hpp"
#include "srf/limits.hpp"
#include "srf/segment.hpp"

namespace srf {

/// A profile is a governed, generation-bound declaration of encoding semantics.
/// It is the gate through which this runtime decides whether an environment
/// explicitly supports segment routing at all.
///
/// 1.0.0 ships only ABSTRACT/SYNTHETIC profiles. The SupportClassification field
/// is data, not decoration: the runtime refuses to describe an abstract profile
/// as physical SR-MPLS or SRv6.
enum class SupportClassification : std::uint8_t {
    AbstractSynthetic = 1,
    /// Reserved. No profile in 1.0.0 uses this value, and no code path can reach
    /// it because physical segment routing is not implemented.
    PhysicalUnverified = 2,
};

struct SegmentProfile {
    SegmentProfileId id{};
    SegmentProfileGeneration generation{};
    std::string name{};

    SupportClassification support{SupportClassification::AbstractSynthetic};
    std::uint16_t allowed_kinds_mask{0};
    std::uint32_t max_depth{0};
    /// Canonical payload width in bytes. Raw payloads shorter than this are
    /// zero-padded during canonicalization; longer payloads are rejected.
    std::uint32_t payload_width{0};
    bool allow_empty{false};
    bool allow_consecutive_repeats{false};
    bool allow_nonconsecutive_repeats{false};
    bool allow_loose{false};
    /// Capability evidence that every list under this profile must bind.
    std::vector<CapabilityKey> required_capabilities{};

    [[nodiscard]] bool allows_kind(SegmentKind kind) const noexcept;
    [[nodiscard]] Digest128 digest() const noexcept;
};

/// Bits used in SegmentProfile::allowed_kinds_mask.
inline constexpr std::uint16_t kind_bit(SegmentKind kind) noexcept {
    return static_cast<std::uint16_t>(1u << (static_cast<std::uint16_t>(kind) - 1u));
}

// ------------------------------------------------------------------ built-ins
/// Governance profile: Node, Adjacency, Endpoint, Binding and Policy kinds,
/// payload width 4, maximum depth 8, no empty list, no repeats, strict only.
[[nodiscard]] SegmentProfile abstract_governance_profile();

/// Ordered profile that explicitly permits non-consecutive repetition.
[[nodiscard]] SegmentProfile abstract_ordered_profile();

/// Explicitly nullable profile used to prove that empty lists and consecutive
/// repeats are rejected by default and accepted only where a profile says so.
[[nodiscard]] SegmentProfile abstract_nullable_profile();

} // namespace srf
