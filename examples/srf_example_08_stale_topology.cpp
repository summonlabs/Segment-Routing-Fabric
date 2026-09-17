// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 08 - the topology advances and refresh_currentness degrades an ACTIVE
// list to REVALIDATION_REQUIRED. Refresh never repairs and never reactivates.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"08 - stale topology forces REVALIDATION_REQUIRED"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("declare an adjacency list against topology generation 1");
    const MutationOutcome created = scenario.create(1, {adjacency_segment(1)});
    CHECK(created.ok());
    const std::optional<SegmentList> before = scenario.s().get(list_id(1));
    CHECK(before.has_value());
    transcript.note("state before", lifecycle_state_name(before->state));
    transcript.note("currentness before", currentness_name(before->currentness));
    transcript.note("bound topology", before->content.topology.value());
    CHECK_EQ(as_int(before->state), as_int(LifecycleState::Active));
    CHECK_EQ(as_int(before->currentness), as_int(Currentness::Current));

    transcript.step("the topology advances to generation 2");
    advance_topology(scenario.fabric(), 2);
    transcript.note("evidence.topology", scenario.fabric().watermark().topology.value());

    transcript.step("refresh_currentness degrades the list; it never repairs it");
    const CurrentnessReport report = scenario.s().refresh_currentness();
    transcript.note("report.examined", static_cast<std::uint64_t>(report.examined));
    transcript.note("report.invalidated", static_cast<std::uint64_t>(report.invalidated));
    transcript.note("report.changes", static_cast<std::uint64_t>(report.changes.size()));
    CHECK_EQ(report.examined, std::size_t{1});
    CHECK_EQ(report.invalidated, std::size_t{1});
    CHECK_EQ(report.changes.size(), std::size_t{1});
    const CurrentnessChange& change = report.changes.front();
    transcript.note("change.was", currentness_name(change.was));
    transcript.note("change.now", currentness_name(change.now));
    CHECK_EQ(as_int(change.was), as_int(Currentness::Current));
    CHECK_EQ(as_int(change.now), as_int(Currentness::StaleTopology));

    const std::optional<SegmentList> after = scenario.s().get(list_id(1));
    CHECK(after.has_value());
    transcript.note("state after", lifecycle_state_name(after->state));
    transcript.note("currentness after", currentness_name(after->currentness));
    CHECK_EQ(as_int(after->state), as_int(LifecycleState::RevalidationRequired));
    CHECK_EQ(as_int(after->currentness), as_int(Currentness::StaleTopology));
    CHECK(!lifecycle_is_available(after->state));
    CHECK(!currentness_is_usable(after->currentness));

    transcript.step("explain reports why revalidation is required");
    const Explanation explanation = scenario.s().explain(list_id(1));
    transcript.note("explanation.topic", explanation_topic_name(explanation.topic));
    CHECK_EQ(as_int(explanation.topic), as_int(ExplanationTopic::WhyRevalidationRequired));
    CHECK(explanation.contains(ReasonCode::CurrentnessStaleTopology));

    transcript.step("explain_stale_topology names the exact stale adjacency");
    const Explanation topology = scenario.s().explain_stale_topology(list_id(1));
    transcript.note("topology.entries", static_cast<std::uint64_t>(topology.size()));
    CHECK(topology.contains(ReasonCode::TopologyAdjacencyStale));
    CHECK(topology.contains(ReasonCode::TopologyGenerationMismatch));

    transcript.step("revalidating the unchanged content still fails: it is genuinely stale");
    const MutationOutcome revalidated =
        scenario.s().revalidate(scenario.caller(), list_id(1), SegmentListGeneration{1});
    CHECK(!revalidated.ok());
    transcript.note("revalidate.primary", revalidated.result.primary_name());
    CHECK_REASON(revalidated.result, ReasonCode::TopologyAdjacencyStale);

    transcript.step("a fresh declaration against generation 2 is accepted");
    const MutationOutcome fresh = scenario.create(2, {adjacency_segment(1, 2)});
    CHECK(fresh.ok());
    const std::optional<SegmentList> fresh_list = scenario.s().get(list_id(2));
    CHECK(fresh_list.has_value());
    CHECK_EQ(fresh_list->content.topology.value(), std::uint64_t{2});
    CHECK_EQ(as_int(fresh_list->state), as_int(LifecycleState::Active));
    CHECK_EQ(scenario.s().list_count(), std::size_t{2});

    transcript.finish();
    return transcript.exit_code();
}
