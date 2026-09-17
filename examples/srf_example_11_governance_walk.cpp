// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 11 - one complete governance walk: declare, validate, activate,
// snapshot, diff, explain, withdraw, retire. SYNTHETIC evidence throughout.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"11 - end-to-end governance walk"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("declare: create list 1 with three ordered node segments");
    const MutationOutcome declared =
        scenario.create(1, {node_segment(1), node_segment(2), node_segment(3)});
    CHECK(declared.ok());
    CHECK(declared.durable);
    transcript.note("generation", declared.generation.value());
    transcript.note("digest", declared.digest);

    transcript.step("validate + activate: the commit ran the phases and landed ACTIVE");
    const std::optional<SegmentList> active = scenario.s().get(list_id(1));
    CHECK(active.has_value());
    transcript.note("state", lifecycle_state_name(active->state));
    transcript.note("currentness", currentness_name(active->currentness));
    CHECK_EQ(as_int(active->state), as_int(LifecycleState::Active));
    CHECK_EQ(as_int(active->currentness), as_int(Currentness::Current));
    CHECK(lifecycle_is_available(active->state));
    CHECK(currentness_is_usable(active->currentness));

    // The same content can be validated again, with no side effect and no
    // lifecycle or authority context: the phases are a pure function.
    const SegmentProfile profile = abstract_governance_profile();
    const FabricEvidence validation_evidence = scenario.fabric().snapshot();
    ValidationRequest request{};
    request.limits = &scenario.s().limits();
    request.list_id = list_id(1);
    request.scope = ScopeId{kScope};
    request.profile_id = active->content.profile;
    request.profile = &profile;
    request.profile_found = true;
    request.segments = active->content.segments;
    request.topology = active->content.topology;
    request.evidence = &validation_evidence;
    request.full = true;
    const ValidationResult validation = validate_request(request);
    transcript.note("revalidated.content", validation.ok());
    CHECK(validation.ok());

    transcript.step("snapshot: capture the immutable revision 1");
    const SegmentListSnapshot before = scenario.s().snapshot(list_id(1));
    CHECK(before.valid());
    transcript.note("snapshot.id", before.id.value());
    transcript.note("snapshot.revision", before.revision);
    transcript.note("snapshot.digest", before.digest);
    CHECK_EQ(before.digest, declared.digest);
    CHECK_EQ(as_int(before.list.state), as_int(LifecycleState::Active));

    transcript.step("replace: the same three segments in the reverse order");
    ListDraft reverse_draft = scenario.draft(1);
    reverse_draft.segments = {node_segment(3), node_segment(2), node_segment(1)};
    reverse_draft.expected_generation = SegmentListGeneration{1};
    const MutationOutcome replaced = scenario.s().replace_list(scenario.caller(), reverse_draft);
    CHECK(replaced.ok());
    transcript.note("replaced generation", replaced.generation.value());
    transcript.note("replaced digest", replaced.digest);
    CHECK_EQ(replaced.generation.value(), std::uint64_t{2});
    CHECK(!(replaced.digest == before.digest));

    transcript.step("diff: compare the two snapshots");
    const SegmentListSnapshot after = scenario.s().snapshot(list_id(1));
    CHECK(after.valid());
    const SnapshotDiff diff = scenario.s().diff(before, after);
    transcript.note("diff.entries", static_cast<std::uint64_t>(diff.entries.size()));
    transcript.note("diff.order_changed", diff.order_changed);
    transcript.note("diff.truncated", diff.truncated);
    transcript.note("diff.from", diff.from.value());
    transcript.note("diff.to", diff.to.value());
    CHECK(!diff.empty());
    CHECK(!diff.truncated);
    CHECK(diff.order_changed);
    CHECK(diff.contains(DiffKind::GenerationAdvance));
    CHECK(diff.contains(DiffKind::Reorder));

    transcript.step("explain: why the list is available, and why one segment is valid");
    const Explanation explanation = scenario.s().explain(list_id(1));
    transcript.note("explanation.topic", explanation_topic_name(explanation.topic));
    CHECK_EQ(as_int(explanation.topic), as_int(ExplanationTopic::WhyActive));
    CHECK(explanation.contains(ReasonCode::CurrentnessCurrent));
    const Explanation segment = scenario.s().explain_segment(list_id(1), 0);
    transcript.note("segment.topic", explanation_topic_name(segment.topic));
    CHECK(segment.contains(ReasonCode::Ok));

    transcript.step("withdraw: availability is removed immediately, then finalised");
    const MutationOutcome withdrawing =
        scenario.s().withdraw(scenario.caller(), list_id(1), SegmentListGeneration{2});
    CHECK(withdrawing.ok());
    const std::optional<SegmentList> withdrawing_list = scenario.s().get(list_id(1));
    CHECK(withdrawing_list.has_value());
    transcript.note("state after withdraw", lifecycle_state_name(withdrawing_list->state));
    CHECK_EQ(as_int(withdrawing_list->state), as_int(LifecycleState::Withdrawing));
    CHECK(!lifecycle_is_available(withdrawing_list->state));
    const MutationOutcome withdrawn =
        scenario.s().withdraw_commit(scenario.caller(), list_id(1), SegmentListGeneration{2});
    CHECK(withdrawn.ok());

    transcript.step("retire: the lineage is closed for good");
    const MutationOutcome retired =
        scenario.s().retire(scenario.caller(), list_id(1), SegmentListGeneration{2});
    CHECK(retired.ok());
    const std::optional<SegmentList> final_list = scenario.s().get(list_id(1));
    CHECK(final_list.has_value());
    transcript.note("final state", lifecycle_state_name(final_list->state));
    transcript.note("final currentness", currentness_name(final_list->currentness));
    CHECK_EQ(as_int(final_list->state), as_int(LifecycleState::Retired));
    CHECK(lifecycle_is_terminal_for_activation(final_list->state));

    ListDraft after_retire_draft = scenario.draft(1);
    after_retire_draft.segments = {node_segment(1)};
    after_retire_draft.expected_generation = SegmentListGeneration{2};
    const MutationOutcome after_retire =
        scenario.s().replace_list(scenario.caller(), after_retire_draft);
    CHECK(!after_retire.ok());
    transcript.note("after_retire.primary", after_retire.result.primary_name());
    CHECK_REASON(after_retire.result, ReasonCode::ListRetired);
    CHECK_PRIMARY(after_retire.result, ReasonCode::ListRetired);

    transcript.step("the durable history retains every transition");
    transcript.note("history entries", static_cast<std::uint64_t>(scenario.s().history().size()));
    transcript.note("store revision", scenario.s().revision());
    transcript.note("durable image bytes",
                    static_cast<std::uint64_t>(scenario.persistence->bytes().size()));
    CHECK(scenario.s().history().size() >= std::size_t{5});

    transcript.finish();
    return transcript.exit_code();
}
