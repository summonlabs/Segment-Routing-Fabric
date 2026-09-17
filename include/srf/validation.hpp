// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "srf/authority.hpp"
#include "srf/evidence.hpp"
#include "srf/list.hpp"
#include "srf/policy.hpp"
#include "srf/profile.hpp"
#include "srf/reason.hpp"

namespace srf {

/// A point-in-time copy of the authority table. Validation never reads the live
/// table: it reads this copy, so a concurrent change is detected by the
/// commit watermark check rather than observed halfway through.
struct AuthorityView {
    CoordinatorEpoch epoch{};
    std::vector<PublisherState> publishers{};
    std::vector<WorkerBootId> fenced_boots{};

    [[nodiscard]] const PublisherState* find(PublisherId id) const noexcept;
    [[nodiscard]] bool boot_fenced(WorkerBootId boot) const noexcept;
};

/// Everything one validation pass may read. The store materializes exactly one of
/// these per mutation; nothing here is read twice from a live source.
struct ValidationRequest {
    const Limits* limits{nullptr};

    // phase 2 and 3
    const CallerIdentity* caller{nullptr};
    const AuthorityView* authority{nullptr};

    // phase 4
    SegmentListId list_id{};
    ScopeId scope{};
    const SegmentList* existing{nullptr};
    SegmentListGeneration expected_generation{};
    SegmentListGeneration intended_generation{};
    LifecycleEvent intent{LifecycleEvent::Invalid};

    // phase 5
    SegmentProfileId profile_id{};
    SegmentProfileGeneration profile_generation{};
    const SegmentProfile* profile{nullptr};
    bool profile_found{false};

    // phase 6
    SegmentPolicyId policy_id{};
    SegmentPolicyGeneration policy_generation{};
    const SegmentPolicy* policy{nullptr};
    bool policy_found{false};
    bool policy_required{false};

    // phase 7 to 12
    Strictness strictness{Strictness::Strict};
    TopologyGeneration topology{};
    CapabilityGeneration capability{};
    PathId path{};
    PathAuthorityGeneration path_authority{};
    RouteId route{};
    RouteGeneration route_generation{};
    ConstraintEvaluationId constraint{};
    ConstraintGeneration constraint_generation{};
    std::span<const Segment> segments{};
    bool derived{false};

    const FabricEvidence* evidence{nullptr};

    /// When false only the caller, epoch/scope and list lifecycle phases run.
    /// Administrative events (withdraw, revoke, retire, supersede) must never be
    /// blocked by content that has since gone stale.
    bool full{true};

    /// The operation requires the lineage to be absent (create) or present
    /// (replace, revalidate). Default deny on the wrong shape.
    bool require_absent{false};
    bool require_existing{false};
};

/// Fixed-precedence validation of one complete mutation request.
///
/// The result is deterministic: identical inputs produce an identical primary
/// reason and an identical, phase-ordered, deduplicated complete reason list.
[[nodiscard]] ValidationResult validate_request(const ValidationRequest& request);

/// Structural validation of an ordered segment sequence with no list, caller or
/// lifecycle context. Used by persistence recovery and by decode paths, where
/// external fabric evidence is deliberately unavailable.
struct SegmentSequenceRequest {
    const Limits* limits{nullptr};
    const SegmentProfile* profile{nullptr};
    std::span<const Segment> segments{};
    const FabricEvidence* evidence{nullptr};
    TopologyGeneration topology{};
};

[[nodiscard]] ValidationResult validate_segment_sequence(const SegmentSequenceRequest& request);

/// Replace an unset segment encoding with the encoding the kind requires, and
/// return the canonicalized copy. An explicitly wrong encoding is left untouched
/// for the structural phase to reject.
[[nodiscard]] std::vector<Segment> canonicalize_segments(std::span<const Segment> segments);

/// Compare the dependency generations bound by an ACTIVE list against the current
/// watermark. Returns Currentness::Current when everything still matches.
[[nodiscard]] Currentness evaluate_currentness(const SegmentList& list,
                                               const SegmentProfile* profile,
                                               const SegmentPolicy* policy,
                                               const EvidenceWatermark& watermark,
                                               CoordinatorEpoch epoch);

} // namespace srf
