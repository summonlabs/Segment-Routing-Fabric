// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 05 - a registered policy forbids a segment kind and requires a segment.
// SYNTHETIC evidence: the policy is governance data, not protocol behaviour.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"05 - policy restriction: forbidden kind, required segment"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("register a policy bound to one profile and four permitted kinds");
    const SegmentPolicy policy = restrictive_policy();
    const ValidationResult registered = scenario.s().register_policy(policy);
    CHECK(registered.ok());
    transcript.note("policy.id", policy.id.value());
    transcript.note("policy.generation", policy.generation.value());
    transcript.note("policy.allowed_kinds_mask",
                    static_cast<std::uint64_t>(policy.allowed_kinds_mask));
    transcript.note("policy.required_segments",
                    static_cast<std::uint64_t>(policy.required_segments.size()));
    transcript.note("policy.forbidden_segments",
                    static_cast<std::uint64_t>(policy.forbidden_segments.size()));
    transcript.note("policy.digest", policy.digest());

    transcript.step("a compliant list carries the required segment and no denied kind");
    ListDraft compliant = scenario.draft(1);
    compliant.policy = policy.id;
    compliant.segments = {node_segment(2), endpoint_segment(1)};
    const MutationOutcome accepted = scenario.s().create_list(scenario.caller(), compliant);
    CHECK(accepted.ok());
    const std::optional<SegmentList> stored = scenario.s().get(list_id(1));
    CHECK(stored.has_value());
    transcript.note("list.content.policy_generation", stored->content.policy_generation.value());
    CHECK_EQ(stored->content.policy.value(), policy.id.value());
    CHECK_EQ(stored->content.policy_generation.value(), std::uint64_t{1});

    transcript.step("an adjacency segment is a denied kind");
    ListDraft denied_kind = scenario.draft(2);
    denied_kind.policy = policy.id;
    denied_kind.segments = {node_segment(2), adjacency_segment(1)};
    const MutationOutcome kind = scenario.s().create_list(scenario.caller(), denied_kind);
    CHECK(!kind.ok());
    transcript.note("kind.primary", kind.result.primary_name());
    transcript.note("kind.primary.index", static_cast<std::uint64_t>(kind.result.primary().index));
    CHECK_REASON(kind.result, ReasonCode::PolicyDeniedSegmentKind);
    CHECK_PRIMARY(kind.result, ReasonCode::PolicyDeniedSegmentKind);

    transcript.step("the required segment is absent");
    ListDraft missing = scenario.draft(3);
    missing.policy = policy.id;
    missing.segments = {node_segment(1), endpoint_segment(1)};
    const MutationOutcome required = scenario.s().create_list(scenario.caller(), missing);
    CHECK(!required.ok());
    transcript.note("required.primary", required.result.primary_name());
    transcript.note("required.primary.detail", required.result.primary().detail);
    CHECK_REASON(required.result, ReasonCode::PolicyRequiredEntityMissing);
    CHECK_PRIMARY(required.result, ReasonCode::PolicyRequiredEntityMissing);
    CHECK_EQ(required.result.primary().detail, segment_id(2).value());

    transcript.step("a forbidden segment is present");
    ListDraft forbidden = scenario.draft(4);
    forbidden.policy = policy.id;
    forbidden.segments = {node_segment(2), node_segment(9)};
    const MutationOutcome forbidden_outcome = scenario.s().create_list(scenario.caller(), forbidden);
    CHECK(!forbidden_outcome.ok());
    transcript.note("forbidden.primary", forbidden_outcome.result.primary_name());
    CHECK_REASON(forbidden_outcome.result, ReasonCode::PolicyForbiddenEntityPresent);
    CHECK_PRIMARY(forbidden_outcome.result, ReasonCode::PolicyForbiddenEntityPresent);

    transcript.step("the same policy also denies another profile");
    ListDraft denied_profile = scenario.draft(5, kOrderedProfile);
    denied_profile.policy = policy.id;
    denied_profile.segments = {node_segment(2)};
    const MutationOutcome profile_denied =
        scenario.s().create_list(scenario.caller(), denied_profile);
    CHECK(!profile_denied.ok());
    transcript.note("profile_denied.primary", profile_denied.result.primary_name());
    CHECK_REASON(profile_denied.result, ReasonCode::PolicyDeniedProfile);
    CHECK_PRIMARY(profile_denied.result, ReasonCode::PolicyDeniedProfile);

    transcript.step("only the compliant declaration entered the store");
    CHECK_EQ(scenario.s().list_count(), std::size_t{1});

    transcript.finish();
    return transcript.exit_code();
}
