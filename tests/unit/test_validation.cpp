// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

namespace {

srf::ValidationResult validate_draft(const srf::test::Fabric& fabric,
                                     const srf::SegmentProfile& profile,
                                     const srf::ListDraft& draft,
                                     const srf::SegmentPolicy* policy = nullptr,
                                     srf::LifecycleEvent intent = srf::LifecycleEvent::ValidateBegin,
                                     bool full = true) {
    const srf::FabricEvidence evidence = fabric.raw().snapshot();
    srf::ValidationRequest request{};
    srf::Limits limits{};
    request.limits = &limits;
    request.list_id = draft.id;
    request.scope = draft.scope;
    request.intent = intent;
    request.full = full;
    request.profile_id = draft.profile;
    request.profile = &profile;
    request.profile_found = true;
    request.policy_id = draft.policy;
    request.policy = policy;
    request.policy_found = policy != nullptr;
    request.policy_required = draft.policy.valid();
    request.strictness = draft.strictness;
    request.topology = draft.topology;
    request.capability = draft.capability;
    request.path = draft.path;
    request.path_authority = draft.path_authority;
    request.route = draft.route;
    request.route_generation = draft.route_generation;
    request.constraint = draft.constraint;
    request.constraint_generation = draft.constraint_generation;
    request.segments = draft.segments;
    request.derived = draft.derived;
    request.evidence = &evidence;
    return srf::validate_request(request);
}

srf::ListDraft base_draft() {
    srf::ListDraft draft{};
    draft.id = srf::test::list_id(1);
    draft.scope = srf::ScopeId{srf::test::kScope};
    draft.profile = srf::SegmentProfileId{0x0001'0001ull};
    draft.profile_generation = srf::SegmentProfileGeneration{1};
    draft.segments = {srf::test::node_segment(1)};
    return draft;
}

} // namespace

SRF_TEST(validation, valid_list_passes_every_phase) {
    const srf::SegmentProfile profile = srf::abstract_governance_profile();
    const srf::ListDraft draft = base_draft();
    const srf::ValidationResult result = validate_draft(*std::make_shared<srf::test::Fabric>(),
                                                        profile, draft);
    SRF_EXPECT(result.ok());
    SRF_EXPECT_EQ(result.primary_phase(), srf::ValidationPhase::Ok);
    SRF_EXPECT_EQ(result.size(), static_cast<std::size_t>(0));
}

SRF_TEST(validation, precedence_wire_then_caller_then_lifecycle_then_profile) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile profile = srf::abstract_governance_profile();
    srf::ListDraft draft = base_draft();
    draft.segments = {srf::test::node_segment(1), srf::test::node_segment(1)};

    // Profile phase dominates the structural duplicate phase.
    const srf::ValidationResult profile_first = validate_draft(fabric, profile, draft);
    SRF_EXPECT(profile_first.failed());
    SRF_EXPECT_PRIMARY(profile_first, srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT_REASON(profile_first, srf::ReasonCode::SegmentDuplicateConsecutive);
    SRF_EXPECT_EQ(profile_first.primary_phase(), srf::ValidationPhase::Profile);

    // A missing caller identity dominates everything after it.
    const srf::FabricEvidence evidence = fabric.raw().snapshot();
    srf::Limits limits{};
    srf::ValidationRequest request{};
    request.limits = &limits;
    request.list_id = draft.id;
    request.profile_id = draft.profile;
    request.profile = &profile;
    request.profile_found = true;
    request.segments = draft.segments;
    request.evidence = &evidence;
    const srf::CallerIdentity empty_caller{};
    request.caller = &empty_caller;
    const srf::ValidationResult caller_first = srf::validate_request(request);
    SRF_EXPECT_PRIMARY(caller_first, srf::ReasonCode::CallerMissingPublisher);
    SRF_EXPECT_REASON(caller_first, srf::ReasonCode::CallerMissingWorkerBoot);
    SRF_EXPECT_REASON(caller_first, srf::ReasonCode::CallerMissingMutationAttempt);
    SRF_EXPECT_REASON(caller_first, srf::ReasonCode::CallerMissingScope);
    SRF_EXPECT_REASON(caller_first, srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT_EQ(caller_first.primary_phase(), srf::ValidationPhase::CallerIdentity);
}

SRF_TEST(validation, every_phase_is_reachable_and_ordered) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile profile = srf::abstract_governance_profile();

    // Profile phase: unknown kind for this profile.
    srf::ListDraft policy_kind = base_draft();
    policy_kind.profile = srf::SegmentProfileId{0x0001'0003ull};
    policy_kind.segments = {srf::test::node_segment(1)};
    const srf::SegmentProfile nullable = srf::abstract_nullable_profile();
    srf::ListDraft adjacency_draft = base_draft();
    adjacency_draft.segments = {srf::test::adjacency_segment(1)};
    const srf::ValidationResult adjacency_ok = validate_draft(fabric, profile, adjacency_draft);
    SRF_EXPECT(adjacency_ok.ok());

    // Capability evidence.
    srf::test::Fabric weak;
    weak.remove_capability(srf::test::kCapabilityB);
    srf::ListDraft capability_draft = base_draft();
    const srf::ValidationResult capability_missing =
        validate_draft(weak, profile, capability_draft);
    SRF_EXPECT_PRIMARY(capability_missing, srf::ReasonCode::CapabilityMissing);
    SRF_EXPECT_EQ(capability_missing.primary_phase(), srf::ValidationPhase::CapabilityEvidence);

    srf::test::Fabric unsupported;
    unsupported.set_capability_supported(srf::test::kCapabilityA, false);
    const srf::ValidationResult capability_unsupported =
        validate_draft(unsupported, profile, capability_draft);
    SRF_EXPECT_PRIMARY(capability_unsupported, srf::ReasonCode::CapabilityUnsupported);

    // Topology evidence.
    srf::test::Fabric stale_topology;
    stale_topology.advance_topology();
    srf::ListDraft adjacency_bound = base_draft();
    adjacency_bound.topology = srf::TopologyGeneration{srf::test::kTopology};
    adjacency_bound.segments = {srf::test::adjacency_segment(1)};
    const srf::ValidationResult topology_mismatch =
        validate_draft(stale_topology, profile, adjacency_bound);
    SRF_EXPECT_PRIMARY(topology_mismatch, srf::ReasonCode::TopologyGenerationMismatch);

    srf::test::Fabric missing_adjacency;
    missing_adjacency.remove_adjacency(1);
    srf::ListDraft adjacency_draft2 = base_draft();
    adjacency_draft2.topology = srf::TopologyGeneration{srf::test::kTopology};
    adjacency_draft2.segments = {srf::test::adjacency_segment(1)};
    const srf::ValidationResult adjacency_unknown =
        validate_draft(missing_adjacency, profile, adjacency_draft2);
    SRF_EXPECT_PRIMARY(adjacency_unknown, srf::ReasonCode::TopologyAdjacencyUnknown);

    // Path Authority.
    srf::test::Fabric stale_path;
    stale_path.advance_path_authority();
    srf::ListDraft path_draft = base_draft();
    path_draft.path = srf::PathId{srf::test::kPath};
    path_draft.path_authority = srf::PathAuthorityGeneration{srf::test::kPathAuthority};
    const srf::ValidationResult path_stale = validate_draft(stale_path, profile, path_draft);
    SRF_EXPECT_PRIMARY(path_stale, srf::ReasonCode::PathAuthorityGenerationStale);
    SRF_EXPECT_EQ(path_stale.primary_phase(), srf::ValidationPhase::PathAuthority);

    srf::test::Fabric no_path;
    no_path.remove_path();
    const srf::ValidationResult path_unknown = validate_draft(no_path, profile, path_draft);
    SRF_EXPECT_PRIMARY(path_unknown, srf::ReasonCode::PathUnknown);

    // Route binding.
    srf::ListDraft route_draft = base_draft();
    route_draft.route = srf::RouteId{srf::test::kRoute};
    route_draft.route_generation = srf::RouteGeneration{srf::test::kRouteGeneration};
    srf::test::Fabric route_moved;
    route_moved.advance_route_generation();
    const srf::ValidationResult route_stale = validate_draft(route_moved, profile, route_draft);
    SRF_EXPECT_PRIMARY(route_stale, srf::ReasonCode::RouteBindingGenerationStale);

    // Constraint binding.
    srf::ListDraft constraint_draft = base_draft();
    constraint_draft.constraint = srf::ConstraintEvaluationId{srf::test::kConstraint};
    constraint_draft.constraint_generation =
        srf::ConstraintGeneration{srf::test::kConstraintGeneration};
    constraint_draft.derived = true;
    srf::test::Fabric constraint_moved;
    constraint_moved.advance_constraint_generation();
    const srf::ValidationResult constraint_stale =
        validate_draft(constraint_moved, profile, constraint_draft);
    SRF_EXPECT_PRIMARY(constraint_stale, srf::ReasonCode::ConstraintBindingGenerationStale);

    // Resource limits come after Path Authority.
    srf::Limits small{};
    small.max_segments_per_list = 1;
    srf::ListDraft over_depth = base_draft();
    over_depth.segments = {srf::test::node_segment(1), srf::test::node_segment(2)};
    srf::FabricEvidence evidence = fabric.raw().snapshot();
    srf::ValidationRequest request{};
    request.limits = &small;
    request.list_id = over_depth.id;
    request.profile_id = over_depth.profile;
    request.profile = &profile;
    request.profile_found = true;
    request.segments = over_depth.segments;
    request.evidence = &evidence;
    request.path = srf::PathId{srf::test::kPath};
    request.path_authority = srf::PathAuthorityGeneration{999};
    const srf::ValidationResult limit_last = srf::validate_request(request);
    SRF_EXPECT_PRIMARY(limit_last, srf::ReasonCode::PathAuthorityGenerationStale);
    SRF_EXPECT_REASON(limit_last, srf::ReasonCode::LimitMaxSegmentsPerList);
    SRF_EXPECT(limit_last.primary_phase() < srf::ValidationPhase::ResourceLimits);
}

SRF_TEST(validation, profile_rules_are_enforced) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile profile = srf::abstract_governance_profile();

    srf::ListDraft deep = base_draft();
    for (std::uint64_t i = 1; i <= 9; ++i) {
        deep.segments.push_back(srf::test::node_segment(i));
    }
    const srf::ValidationResult depth = validate_draft(fabric, profile, deep);
    SRF_EXPECT_PRIMARY(depth, srf::ReasonCode::ProfileMaxDepthExceeded);

    srf::ListDraft depth_exact = base_draft();
    depth_exact.segments.clear();
    for (std::uint64_t i = 1; i <= 8; ++i) {
        depth_exact.segments.push_back(srf::test::node_segment(i));
    }
    SRF_EXPECT(validate_draft(fabric, profile, depth_exact).ok());

    srf::ListDraft empty = base_draft();
    empty.segments.clear();
    const srf::ValidationResult empty_result = validate_draft(fabric, profile, empty);
    SRF_EXPECT_PRIMARY(empty_result, srf::ReasonCode::ListEmptyNotPermitted);
    SRF_EXPECT_REASON(empty_result, srf::ReasonCode::ProfileEmptyNotPermitted);

    const srf::SegmentProfile nullable = srf::abstract_nullable_profile();
    srf::ListDraft nullable_empty = base_draft();
    nullable_empty.profile = nullable.id;
    nullable_empty.segments.clear();
    SRF_EXPECT(validate_draft(fabric, nullable, nullable_empty).ok());

    srf::ListDraft loose = base_draft();
    loose.strictness = srf::Strictness::Loose;
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, loose),
                       srf::ReasonCode::ProfileStrictnessUnsupported);
    const srf::SegmentProfile ordered = srf::abstract_ordered_profile();
    srf::ListDraft loose_ok = base_draft();
    loose_ok.profile = ordered.id;
    loose_ok.strictness = srf::Strictness::Loose;
    SRF_EXPECT(validate_draft(fabric, ordered, loose_ok).ok());

    srf::ListDraft wide = base_draft();
    wide.segments[0].payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, wide),
                       srf::ReasonCode::ProfilePayloadWidthExceeded);

    srf::ListDraft wrong_kind = base_draft();
    wrong_kind.profile = nullable.id;
    wrong_kind.segments = {srf::test::endpoint_segment(1)};
    SRF_EXPECT_PRIMARY(validate_draft(fabric, nullable, wrong_kind),
                       srf::ReasonCode::ProfileKindNotAllowed);
}

SRF_TEST(validation, structural_rules_are_enforced_per_kind) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile profile = srf::abstract_governance_profile();

    srf::ListDraft no_reference = base_draft();
    no_reference.segments[0].node = srf::NodeId{};
    const srf::ValidationResult missing = validate_draft(fabric, profile, no_reference);
    SRF_EXPECT_REASON(missing, srf::ReasonCode::SegmentBindingIncomplete);
    SRF_EXPECT_REASON(missing, srf::ReasonCode::SegmentMissingBinding);

    srf::ListDraft reserved = base_draft();
    reserved.segments[0].adjacency = srf::test::adjacency_id(1);
    SRF_EXPECT_REASON(validate_draft(fabric, profile, reserved),
                      srf::ReasonCode::SegmentReservedFieldSet);

    srf::ListDraft bad_encoding = base_draft();
    bad_encoding.segments[0].encoding = srf::SegmentEncodingId::AbstractAdjacencyV1;
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, bad_encoding),
                       srf::ReasonCode::SegmentEncodingMismatch);

    srf::ListDraft no_id = base_draft();
    no_id.segments[0].id = srf::SegmentId{};
    SRF_EXPECT_REASON(validate_draft(fabric, profile, no_id), srf::ReasonCode::SegmentIdInvalid);

    srf::ListDraft no_generation = base_draft();
    no_generation.segments[0].generation = srf::SegmentGeneration{};
    SRF_EXPECT_REASON(validate_draft(fabric, profile, no_generation),
                      srf::ReasonCode::SegmentGenerationInvalid);

    srf::ListDraft unknown_entity = base_draft();
    unknown_entity.segments[0].id = srf::SegmentId{0xDEAD};
    SRF_EXPECT_REASON(validate_draft(fabric, profile, unknown_entity),
                      srf::ReasonCode::SegmentEntityUnknown);

    srf::ListDraft moved_entity = base_draft();
    srf::test::Fabric moved;
    moved.bump_segment_generation(1, 2);
    SRF_EXPECT_PRIMARY(validate_draft(moved, profile, moved_entity),
                       srf::ReasonCode::SegmentEntityGenerationMismatch);

    srf::ListDraft kind_mismatch = base_draft();
    kind_mismatch.segments[0].id = srf::test::segment_id(101);
    SRF_EXPECT_REASON(validate_draft(fabric, profile, kind_mismatch),
                      srf::ReasonCode::SegmentEntityKindMismatch);

    srf::ListDraft missing_adjacency_topology = base_draft();
    missing_adjacency_topology.segments = {srf::test::adjacency_segment(1)};
    missing_adjacency_topology.segments[0].topology = srf::TopologyGeneration{};
    SRF_EXPECT_REASON(validate_draft(fabric, profile, missing_adjacency_topology),
                      srf::ReasonCode::SegmentBindingIncomplete);
}

SRF_TEST(validation, repetition_rules_follow_the_profile) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile ordered = srf::abstract_ordered_profile();

    srf::ListDraft non_consecutive = base_draft();
    non_consecutive.profile = ordered.id;
    non_consecutive.segments = {srf::test::node_segment(1), srf::test::node_segment(2),
                                srf::test::node_segment(1)};
    const srf::ValidationResult allowed = validate_draft(fabric, ordered, non_consecutive);
    SRF_EXPECT(allowed.ok());

    srf::ListDraft consecutive = base_draft();
    consecutive.profile = ordered.id;
    consecutive.segments = {srf::test::node_segment(1), srf::test::node_segment(1)};
    const srf::ValidationResult denied = validate_draft(fabric, ordered, consecutive);
    SRF_EXPECT_PRIMARY(denied, srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT_REASON(denied, srf::ReasonCode::SegmentDuplicateConsecutive);

    // An unset encoding and the encoding the kind requires are the same segment,
    // so a repeat spelled with the two different spellings is still a repeat.
    srf::Segment unset_encoding = srf::test::node_segment(1);
    unset_encoding.encoding = srf::SegmentEncodingId::Invalid;
    srf::Segment explicit_encoding = srf::test::node_segment(1);
    explicit_encoding.encoding = srf::SegmentEncodingId::AbstractNodeV1;
    srf::ListDraft mixed_spelling = base_draft();
    mixed_spelling.segments = {unset_encoding, explicit_encoding};
    const srf::ValidationResult mixed_result = validate_draft(fabric, ordered, mixed_spelling);
    SRF_EXPECT(mixed_result.failed());
    SRF_EXPECT_REASON(mixed_result, srf::ReasonCode::ProfileRepeatNotPermitted);
    SRF_EXPECT_REASON(mixed_result, srf::ReasonCode::SegmentDuplicateConsecutive);

    const srf::SegmentProfile governance = srf::abstract_governance_profile();
    srf::ListDraft non_consecutive_denied = base_draft();
    non_consecutive_denied.segments = {srf::test::node_segment(1), srf::test::node_segment(2),
                                       srf::test::node_segment(1)};
    const srf::ValidationResult non_consecutive_result =
        validate_draft(fabric, governance, non_consecutive_denied);
    SRF_EXPECT_REASON(non_consecutive_result, srf::ReasonCode::SegmentDuplicateNonConsecutive);
}

SRF_TEST(validation, policy_restrictions_are_explicit_and_typed) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile profile = srf::abstract_governance_profile();
    srf::SegmentPolicy policy{};
    policy.id = srf::test::policy_id(1);
    policy.generation = srf::SegmentPolicyGeneration{1};

    srf::ListDraft draft = base_draft();
    SRF_EXPECT(validate_draft(fabric, profile, draft, &policy).ok());

    policy.max_depth = 0;
    policy.allowed_kinds_mask = srf::kind_bit(srf::SegmentKind::Endpoint);
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, draft, &policy),
                       srf::ReasonCode::PolicyDeniedSegmentKind);

    policy.allowed_kinds_mask = 0;
    policy.required_segments = {srf::test::segment_id(7)};
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, draft, &policy),
                       srf::ReasonCode::PolicyRequiredEntityMissing);

    policy.required_segments.clear();
    policy.forbidden_segments = {srf::test::segment_id(1)};
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, draft, &policy),
                       srf::ReasonCode::PolicyForbiddenEntityPresent);

    policy.forbidden_segments.clear();
    policy.allowed_profiles = {srf::SegmentProfileId{0x0001'0002ull}};
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, draft, &policy),
                       srf::ReasonCode::PolicyDeniedProfile);

    policy.allowed_profiles.clear();
    policy.owner_scope = srf::ScopeId{srf::test::kOtherScope};
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, draft, &policy),
                       srf::ReasonCode::PolicyOwnershipMismatch);

    policy.owner_scope = srf::ScopeId{srf::test::kScope};
    srf::ListDraft derived = base_draft();
    derived.derived = true;
    SRF_EXPECT_PRIMARY(validate_draft(fabric, profile, derived, &policy),
                       srf::ReasonCode::PolicyDerivationDenied);

    policy.allow_derivation = true;
    policy.max_depth = 0;
    policy.require_strict = true;
    srf::ListDraft loose = base_draft();
    loose.strictness = srf::Strictness::Loose;
    loose.profile = srf::SegmentProfileId{0x0001'0002ull};
    SRF_EXPECT_REASON(validate_draft(fabric, srf::abstract_ordered_profile(), loose, &policy),
                      srf::ReasonCode::PolicyStrictnessRequired);
}

SRF_TEST(validation, lifecycle_only_scope_skips_content_phases) {
    srf::test::Fabric fabric;
    const srf::SegmentProfile profile = srf::abstract_governance_profile();
    srf::ListDraft draft = base_draft();
    draft.segments = {srf::test::node_segment(1), srf::test::node_segment(1)};
    const srf::ValidationResult full = validate_draft(fabric, profile, draft, nullptr,
                                                      srf::LifecycleEvent::WithdrawBegin, true);
    SRF_EXPECT(full.failed());
    const srf::ValidationResult reduced = validate_draft(
        fabric, profile, draft, nullptr, srf::LifecycleEvent::WithdrawBegin, false);
    SRF_EXPECT(reduced.ok());
}
