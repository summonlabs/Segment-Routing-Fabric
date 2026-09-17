// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "srf/digest.hpp"
#include "srf/ids.hpp"
#include "srf/lifecycle.hpp"
#include "srf/segment.hpp"

namespace srf {

/// The immutable, generation-independent semantic content of one segment-list
/// revision. Mutable lifecycle state is deliberately not part of this structure,
/// so the content digest is stable across lifecycle transitions.
struct SegmentListContent {
    SegmentListId id{};
    ScopeId scope{};

    SegmentProfileId profile{};
    SegmentProfileGeneration profile_generation{};
    SegmentPolicyId policy{};
    SegmentPolicyGeneration policy_generation{};

    Strictness strictness{Strictness::Strict};

    /// Bound dependency generations. These are captured, never inferred.
    TopologyGeneration topology{};
    CapabilityGeneration capability{};
    PathId path{};
    PathAuthorityGeneration path_authority{};
    RouteId route{};
    RouteGeneration route_generation{};
    ConstraintEvaluationId constraint{};
    ConstraintGeneration constraint_generation{};

    /// Ordered segment sequence. Order is semantic and is never normalized away.
    std::vector<Segment> segments{};

    friend bool operator==(const SegmentListContent&, const SegmentListContent&) = default;
};

/// Provenance preserved from whatever explicit intent this list was derived from.
/// Derivation is never opaque and never performs a search.
struct SegmentListProvenance {
    PathId source_path{};
    PathAuthorityGeneration source_path_authority{};
    RouteId source_route{};
    RouteGeneration source_route_generation{};
    ConstraintEvaluationId source_constraint{};
    ConstraintGeneration source_constraint_generation{};
    SegmentPolicyGeneration derivation_policy{};
    TopologyGeneration observed_topology{};
    CapabilityGeneration observed_capability{};
    PublisherId publisher{};
    WorkerBootId boot{};
    CoordinatorEpoch epoch{};

    friend bool operator==(const SegmentListProvenance&, const SegmentListProvenance&) = default;
};

/// Which writer produced a revision. Retained so that a fenced or restarted writer
/// can never reactivate or silently re-take its old revisions.
struct SegmentListAuthority {
    PublisherId publisher{};
    WorkerBootId boot{};
    CoordinatorEpoch epoch{};
    MutationAttemptId attempt{};

    friend bool operator==(const SegmentListAuthority&, const SegmentListAuthority&) = default;
};

/// Lineage is preserved across replacement; replacement never erases history.
struct SegmentListLineage {
    /// The generation this revision replaced, or 0 for the first revision.
    SegmentListGeneration supersedes{};
    /// Set when this lineage was superseded by a different lineage.
    SegmentListId superseded_by{};
    HistoryId history{};

    friend bool operator==(const SegmentListLineage&, const SegmentListLineage&) = default;
};

/// One durable revision of a segment-list lineage.
struct SegmentList {
    SegmentListContent content{};
    SegmentListGeneration generation{};
    SegmentListLineage lineage{};
    SegmentListAuthority authority{};
    SegmentListProvenance provenance{};

    LifecycleState state{LifecycleState::Declared};
    Currentness currentness{Currentness::Unknown};

    Digest128 content_digest{};
    Digest128 sequence_digest{};
    std::uint64_t committed_revision{0};

    friend bool operator==(const SegmentList&, const SegmentList&) = default;
};

[[nodiscard]] std::string_view segment_list_state_name(const SegmentList& list) noexcept;

} // namespace srf
