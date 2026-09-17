// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <vector>

#include "srf/digest.hpp"
#include "srf/ids.hpp"
#include "srf/segment.hpp"

namespace srf {

/// Explicit, typed, bounded, generation-bound governance of segment lists.
struct SegmentPolicy {
    SegmentPolicyId id{};
    SegmentPolicyGeneration generation{};
    ScopeId owner_scope{};

    /// Empty means "no profile restriction".
    std::vector<SegmentProfileId> allowed_profiles{};
    /// Zero means "no kind restriction"; otherwise a kinds bitmask.
    std::uint16_t allowed_kinds_mask{0};
    /// Zero means "no additional depth restriction".
    std::uint32_t max_depth{0};

    std::vector<SegmentId> required_segments{};
    std::vector<SegmentId> forbidden_segments{};

    bool allow_replacement{true};
    bool allow_derivation{false};
    /// When true the policy only accepts lists whose strictness is Strict.
    bool require_strict{false};

    [[nodiscard]] Digest128 digest() const noexcept;
};

} // namespace srf
