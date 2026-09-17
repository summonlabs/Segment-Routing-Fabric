// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 07 - one lineage supersedes another in the same atomic commit, and the
// superseded lineage can never reactivate. SYNTHETIC evidence.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"07 - supersession closes the old lineage permanently"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("declare lineage 1");
    const MutationOutcome first = scenario.create(1, {node_segment(1)});
    CHECK(first.ok());
    transcript.note("lineage 1 generation", first.generation.value());
    transcript.note("lineage 1 digest", first.digest);

    transcript.step("declare lineage 2 in the same commit that supersedes lineage 1");
    ListDraft second_draft = scenario.draft(2);
    second_draft.segments = {node_segment(1), node_segment(2)};
    second_draft.supersedes = list_id(1);
    second_draft.supersedes_generation = SegmentListGeneration{1};
    const MutationOutcome second = scenario.s().create_list(scenario.caller(), second_draft);
    CHECK(second.ok());
    const std::optional<SegmentList> old_list = scenario.s().get(list_id(1));
    CHECK(old_list.has_value());
    transcript.note("lineage 1 state", lifecycle_state_name(old_list->state));
    transcript.note("lineage 1 superseded_by", old_list->lineage.superseded_by.value());
    CHECK_EQ(as_int(old_list->state), as_int(LifecycleState::Superseded));
    CHECK_EQ(old_list->lineage.superseded_by, list_id(2));
    CHECK(lifecycle_is_terminal_for_activation(old_list->state));
    CHECK(!lifecycle_is_available(old_list->state));

    transcript.step("a superseded lineage can never be replaced back into ACTIVE");
    ListDraft replace_draft = scenario.draft(1);
    replace_draft.segments = {node_segment(1)};
    replace_draft.expected_generation = SegmentListGeneration{1};
    const MutationOutcome replaced = scenario.s().replace_list(scenario.caller(), replace_draft);
    CHECK(!replaced.ok());
    transcript.note("replace.primary", replaced.result.primary_name());
    CHECK_REASON(replaced.result, ReasonCode::ListSuperseded);
    CHECK_REASON(replaced.result, ReasonCode::LifecycleTransitionDenied);
    CHECK_PRIMARY(replaced.result, ReasonCode::ListSuperseded);

    transcript.step("a superseded lineage can never be revalidated back into ACTIVE");
    const MutationOutcome revalidated =
        scenario.s().revalidate(scenario.caller(), list_id(1), SegmentListGeneration{1});
    CHECK(!revalidated.ok());
    transcript.note("revalidate.primary", revalidated.result.primary_name());
    CHECK_REASON(revalidated.result, ReasonCode::ListSuperseded);
    CHECK_PRIMARY(revalidated.result, ReasonCode::ListSuperseded);

    transcript.step("a superseded lineage can never supersede anything itself");
    ListDraft third = scenario.draft(3);
    third.segments = {node_segment(3)};
    third.supersedes = list_id(1);
    third.supersedes_generation = SegmentListGeneration{1};
    const MutationOutcome denied = scenario.s().create_list(scenario.caller(), third);
    CHECK(!denied.ok());
    transcript.note("supersede-again.primary", denied.result.primary_name());
    CHECK_REASON(denied.result, ReasonCode::LifecycleTransitionDenied);
    CHECK_PRIMARY(denied.result, ReasonCode::LifecycleTransitionDenied);

    transcript.step("explain_supersession names the successor lineage");
    const Explanation explanation = scenario.s().explain_supersession(list_id(1));
    transcript.note("explanation.topic", explanation_topic_name(explanation.topic));
    CHECK(explanation.contains(ReasonCode::ListSuperseded));
    CHECK(!explanation.entries.empty());
    CHECK_EQ(explanation.entries.front().detail, list_id(2).value());

    transcript.step("the durable history retains the superseded revision");
    bool saw_superseded = false;
    for (const DurableHistoryEntry& entry : scenario.s().history()) {
        if (entry.list == list_id(1) && entry.state == LifecycleState::Superseded) {
            saw_superseded = true;
        }
    }
    CHECK(saw_superseded);

    transcript.step("the closed lineage can still be retired, never reactivated");
    const MutationOutcome retired =
        scenario.s().retire(scenario.caller(), list_id(1), SegmentListGeneration{1});
    CHECK(retired.ok());
    const std::optional<SegmentList> final_list = scenario.s().get(list_id(1));
    CHECK(final_list.has_value());
    transcript.note("lineage 1 final state", lifecycle_state_name(final_list->state));
    CHECK_EQ(as_int(final_list->state), as_int(LifecycleState::Retired));
    CHECK_EQ(scenario.s().list_count(), std::size_t{2});

    transcript.finish();
    return transcript.exit_code();
}
