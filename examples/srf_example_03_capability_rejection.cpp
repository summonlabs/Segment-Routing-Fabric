// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 03 - a required capability that is missing, unsupported or bound to a
// stale generation is an explicit rejection, never an inferred success.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"03 - capability rejection"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("baseline: both required capabilities are explicitly supported");
    transcript.note("evidence.capability.generation",
                    scenario.fabric().watermark().capability.value());
    const MutationOutcome baseline = scenario.create(1, {node_segment(1)});
    CHECK(baseline.ok());
    const std::optional<SegmentList> list_one = scenario.s().get(list_id(1));
    CHECK(list_one.has_value());
    CHECK_EQ(list_one->content.capability.value(), kCapabilityGeneration);

    transcript.step("the required capability record is absent: unknown is never support");
    scenario.fabric().remove_capability(CapabilityKey{kCapabilityB});
    const MutationOutcome missing = scenario.create(2, {node_segment(1)});
    CHECK(!missing.ok());
    transcript.note("missing.status", status_code_name(missing.status));
    transcript.note("missing.primary", missing.result.primary_name());
    transcript.note("missing.reasons", static_cast<std::uint64_t>(missing.result.size()));
    CHECK_REASON(missing.result, ReasonCode::CapabilityMissing);
    CHECK_PRIMARY(missing.result, ReasonCode::CapabilityMissing);

    transcript.step("the record exists but is explicitly unsupported: a different rejection");
    scenario.fabric().add_capability(CapabilityKey{kCapabilityB},
                                     CapabilityGeneration{kCapabilityGeneration}, false);
    const MutationOutcome unsupported = scenario.create(3, {node_segment(1)});
    CHECK(!unsupported.ok());
    transcript.note("unsupported.primary", unsupported.result.primary_name());
    CHECK_REASON(unsupported.result, ReasonCode::CapabilityUnsupported);
    CHECK(!unsupported.result.contains(ReasonCode::CapabilityMissing));
    CHECK_PRIMARY(unsupported.result, ReasonCode::CapabilityUnsupported);

    transcript.step("the capability generation advances to 2; an old binding is rejected");
    scenario.fabric().add_capability(CapabilityKey{kCapabilityA}, CapabilityGeneration{2}, true);
    scenario.fabric().add_capability(CapabilityKey{kCapabilityB}, CapabilityGeneration{2}, true);
    scenario.fabric().set_capability_generation(CapabilityGeneration{2});
    ListDraft old_binding = scenario.draft(4);
    old_binding.segments = {node_segment(1)};
    old_binding.capability = CapabilityGeneration{kCapabilityGeneration};
    const MutationOutcome stale = scenario.s().create_list(scenario.caller(), old_binding);
    CHECK(!stale.ok());
    transcript.note("stale.primary", stale.result.primary_name());
    CHECK_REASON(stale.result, ReasonCode::CapabilityGenerationMismatch);
    CHECK_PRIMARY(stale.result, ReasonCode::CapabilityGenerationMismatch);

    transcript.step("a fresh binding to the new generation is accepted");
    const MutationOutcome fresh = scenario.create(5, {node_segment(2)});
    CHECK(fresh.ok());
    const std::optional<SegmentList> list_five = scenario.s().get(list_id(5));
    CHECK(list_five.has_value());
    CHECK_EQ(list_five->content.capability.value(), std::uint64_t{2});
    CHECK_EQ(scenario.s().list_count(), std::size_t{2});

    transcript.step("explain_missing_capability names the absent record by key");
    scenario.fabric().remove_capability(CapabilityKey{kCapabilityB});
    const Explanation explanation = scenario.s().explain_missing_capability(list_id(1));
    transcript.note("explanation.topic", explanation_topic_name(explanation.topic));
    transcript.note("explanation.entries", static_cast<std::uint64_t>(explanation.size()));
    CHECK(explanation.contains(ReasonCode::CapabilityMissing));
    CHECK(!explanation.entries.empty());
    transcript.note("explanation.first.detail", explanation.entries.front().detail);
    CHECK_EQ(explanation.entries.front().detail, kCapabilityB);

    transcript.finish();
    return transcript.exit_code();
}
