// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 02 - an adjacency segment is bound to an exact topology generation and
// is never inferred from a label or a name. SYNTHETIC evidence.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"02 - adjacency-like validation against an exact topology generation"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("the synthetic fabric is at topology generation 1");
    transcript.note("evidence.topology", scenario.fabric().watermark().topology.value());
    transcript.note("evidence.revision", scenario.fabric().revision());
    CHECK_EQ(scenario.fabric().watermark().topology.value(), kTopology);

    transcript.step("an adjacency bound to the current generation is accepted");
    const MutationOutcome accepted = scenario.create(1, {adjacency_segment(1)});
    CHECK(accepted.ok());
    const std::optional<SegmentList> stored = scenario.s().get(list_id(1));
    CHECK(stored.has_value());
    transcript.note("list.content.topology", stored->content.topology.value());
    transcript.note("list.content.segments[0].topology",
                    stored->content.segments[0].topology.value());
    transcript.note("list.content.segments[0].encoding",
                    segment_encoding_name(stored->content.segments[0].encoding));
    CHECK_EQ(stored->content.topology.value(), kTopology);
    CHECK_EQ(stored->content.segments[0].topology.value(), kTopology);
    CHECK_EQ(as_int(stored->content.segments[0].encoding),
             as_int(SegmentEncodingId::AbstractAdjacencyV1));

    transcript.step("declaring a generation the fabric is not at is an explicit rejection");
    ListDraft future = scenario.draft(2);
    future.segments = {adjacency_segment(1, 2)};
    future.topology = TopologyGeneration{2};
    const MutationOutcome mismatch = scenario.s().create_list(scenario.caller(), future);
    CHECK(!mismatch.ok());
    transcript.note("mismatch.status", status_code_name(mismatch.status));
    transcript.note("mismatch.primary", mismatch.result.primary_name());
    transcript.note("mismatch.reasons", static_cast<std::uint64_t>(mismatch.result.size()));
    CHECK_REASON(mismatch.result, ReasonCode::TopologyGenerationMismatch);
    CHECK_REASON(mismatch.result, ReasonCode::TopologyAdjacencyStale);
    CHECK_PRIMARY(mismatch.result, ReasonCode::TopologyGenerationMismatch);
    CHECK_EQ(scenario.s().list_count(), std::size_t{1});

    transcript.step("the fabric advances to topology generation 2");
    advance_topology(scenario.fabric(), 2);
    transcript.note("evidence.topology", scenario.fabric().watermark().topology.value());

    transcript.step("the old adjacency binding is stale even when the draft names no generation");
    const MutationOutcome stale = scenario.create(3, {adjacency_segment(1, 1)});
    CHECK(!stale.ok());
    transcript.note("stale.primary", stale.result.primary_name());
    CHECK_REASON(stale.result, ReasonCode::TopologyAdjacencyStale);
    CHECK_PRIMARY(stale.result, ReasonCode::TopologyAdjacencyStale);

    transcript.step("re-declaring against the new generation is accepted");
    const MutationOutcome refreshed = scenario.create(4, {adjacency_segment(1, 2)});
    CHECK(refreshed.ok());
    const std::optional<SegmentList> refreshed_list = scenario.s().get(list_id(4));
    CHECK(refreshed_list.has_value());
    CHECK_EQ(refreshed_list->content.topology.value(), std::uint64_t{2});
    CHECK_EQ(scenario.s().list_count(), std::size_t{2});

    transcript.finish();
    return transcript.exit_code();
}
