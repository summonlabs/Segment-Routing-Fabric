// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>

#include "srf/strong.hpp"

namespace srf {

#define SRF_DEFINE_ID(name)          \
    struct name##Tag;                \
    using name = StrongId<name##Tag>

#define SRF_DEFINE_GENERATION(name)  \
    struct name##Tag;                \
    using name = Generation<name##Tag>

// ---------------------------------------------------------------- identities
SRF_DEFINE_ID(SegmentListId);
SRF_DEFINE_ID(SegmentId);
SRF_DEFINE_ID(SegmentProfileId);
SRF_DEFINE_ID(SegmentPolicyId);
SRF_DEFINE_ID(NodeId);
SRF_DEFINE_ID(AdjacencyId);
SRF_DEFINE_ID(EndpointId);
SRF_DEFINE_ID(BindingId);
SRF_DEFINE_ID(PathId);
SRF_DEFINE_ID(RouteId);
SRF_DEFINE_ID(ConstraintEvaluationId);
SRF_DEFINE_ID(PublisherId);
SRF_DEFINE_ID(WorkerBootId);
SRF_DEFINE_ID(MutationAttemptId);
SRF_DEFINE_ID(SnapshotId);
SRF_DEFINE_ID(ScopeId);
SRF_DEFINE_ID(WorkerSessionId);
SRF_DEFINE_ID(CapabilityKey);
SRF_DEFINE_ID(HistoryId);

// --------------------------------------------------------------- generations
SRF_DEFINE_GENERATION(SegmentListGeneration);
SRF_DEFINE_GENERATION(SegmentGeneration);
SRF_DEFINE_GENERATION(SegmentProfileGeneration);
SRF_DEFINE_GENERATION(SegmentPolicyGeneration);
SRF_DEFINE_GENERATION(TopologyGeneration);
SRF_DEFINE_GENERATION(CapabilityGeneration);
SRF_DEFINE_GENERATION(PathAuthorityGeneration);
SRF_DEFINE_GENERATION(RouteGeneration);
SRF_DEFINE_GENERATION(ConstraintGeneration);
SRF_DEFINE_GENERATION(CoordinatorEpoch);

#undef SRF_DEFINE_ID
#undef SRF_DEFINE_GENERATION

/// Deterministic derivation of a stable semantic list identity from an explicit
/// caller-supplied semantic key (scope + name). The identity never contains a
/// mutable generation, and is non-zero for every input.
[[nodiscard]] SegmentListId derive_segment_list_id(std::string_view scope,
                                                   std::string_view name) noexcept;

} // namespace srf
