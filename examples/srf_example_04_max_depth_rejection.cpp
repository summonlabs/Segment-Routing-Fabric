// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 04 - the profile depth boundary is exact: at the limit is accepted, one
// over the limit is rejected. SYNTHETIC evidence.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"04 - profile depth boundary: exactly at the limit, one over"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    const SegmentProfile governance = abstract_governance_profile();
    transcript.note("governance.max_depth", static_cast<std::uint64_t>(governance.max_depth));

    transcript.step("eight segments is exactly the governance limit");
    const MutationOutcome at_limit = scenario.create(1, node_sequence(8));
    CHECK(at_limit.ok());
    const std::optional<SegmentList> stored = scenario.s().get(list_id(1));
    CHECK(stored.has_value());
    transcript.note("list.content.segments", static_cast<std::uint64_t>(stored->content.segments.size()));
    CHECK_EQ(stored->content.segments.size(), std::size_t{8});
    CHECK_EQ(stored->content.segments.size(),
             static_cast<std::size_t>(governance.max_depth));

    transcript.step("nine segments is one over the limit");
    const MutationOutcome over = scenario.create(2, node_sequence(9));
    CHECK(!over.ok());
    transcript.note("over.status", status_code_name(over.status));
    transcript.note("over.primary", over.result.primary_name());
    transcript.note("over.primary.detail", over.result.primary().detail);
    CHECK_REASON(over.result, ReasonCode::ProfileMaxDepthExceeded);
    CHECK_PRIMARY(over.result, ReasonCode::ProfileMaxDepthExceeded);
    CHECK_EQ(over.result.primary().detail, static_cast<std::uint64_t>(governance.max_depth));

    transcript.step("a second shipped profile has its own boundary: nullable allows four");
    const SegmentProfile nullable = abstract_nullable_profile();
    transcript.note("nullable.max_depth", static_cast<std::uint64_t>(nullable.max_depth));
    transcript.note("nullable.allows_empty", nullable.allow_empty);
    const MutationOutcome nullable_at_limit = scenario.create(3, node_sequence(4), kNullableProfile);
    CHECK(nullable_at_limit.ok());
    const MutationOutcome nullable_over = scenario.create(4, node_sequence(5), kNullableProfile);
    CHECK(!nullable_over.ok());
    transcript.note("nullable_over.primary", nullable_over.result.primary_name());
    CHECK_REASON(nullable_over.result, ReasonCode::ProfileMaxDepthExceeded);
    CHECK_PRIMARY(nullable_over.result, ReasonCode::ProfileMaxDepthExceeded);

    transcript.step("rejected declarations never enter the store");
    CHECK_EQ(scenario.s().list_count(), std::size_t{2});
    CHECK_EQ(scenario.s().total_segments(), std::size_t{12});

    transcript.finish();
    return transcript.exit_code();
}
