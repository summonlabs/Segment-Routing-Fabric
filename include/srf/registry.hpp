// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "srf/policy.hpp"
#include "srf/profile.hpp"
#include "srf/reason.hpp"

namespace srf {

/// Generation-bound registries for the two kinds of governance data that Segment
/// Routing Fabric owns: segment profiles and segment policies.
///
/// Registering a new revision of an existing identifier advances that
/// identifier's generation. The generation counter is monotonic and non-wrapping:
/// exhaustion is an explicit rejection, never a wrap.
class Registry {
public:
    Registry() = default;

    [[nodiscard]] ValidationResult register_profile(const SegmentProfile& profile,
                                                    const Limits& limits);
    [[nodiscard]] ValidationResult register_policy(const SegmentPolicy& policy,
                                                   const Limits& limits);

    /// Advance the generation of an existing profile in place. Used by the race
    /// proofs and by an administrator re-declaring encoding semantics.
    [[nodiscard]] ValidationResult advance_profile_generation(SegmentProfileId id);
    [[nodiscard]] ValidationResult advance_policy_generation(SegmentPolicyId id);

    [[nodiscard]] bool find_profile(SegmentProfileId id, SegmentProfile& out) const;
    [[nodiscard]] bool find_profile_exact(SegmentProfileId id, SegmentProfileGeneration generation,
                                          SegmentProfile& out) const;
    [[nodiscard]] bool find_policy(SegmentPolicyId id, SegmentPolicy& out) const;
    [[nodiscard]] bool find_policy_exact(SegmentPolicyId id, SegmentPolicyGeneration generation,
                                         SegmentPolicy& out) const;

    [[nodiscard]] std::vector<SegmentProfile> profiles() const;
    [[nodiscard]] std::vector<SegmentPolicy> policies() const;
    [[nodiscard]] std::size_t profile_count() const;
    [[nodiscard]] std::size_t policy_count() const;
    [[nodiscard]] std::uint64_t revision() const;

    void restore_profile(const SegmentProfile& profile);
    void restore_policy(const SegmentPolicy& policy);

private:
    mutable std::mutex mutex_{};
    std::vector<SegmentProfile> profiles_{};
    std::vector<SegmentPolicy> policies_{};
    std::uint64_t revision_{0};
};

} // namespace srf
