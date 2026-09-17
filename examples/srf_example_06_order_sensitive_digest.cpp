// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 06 - order is semantic. The same segments in a different order produce
// a different sequence digest and a different content digest.
#include <algorithm>

#include "example_support.hpp"

namespace {

[[nodiscard]] std::vector<srf::Digest128> unit_digests(const std::vector<srf::Segment>& segments,
                                                       std::uint32_t width) {
    std::vector<srf::Digest128> out;
    out.reserve(segments.size());
    for (const srf::Segment& segment : segments) {
        const srf::ByteBuffer bytes = srf::canonical_segment_bytes(segment, width);
        out.push_back(srf::digest128(srf::as_bytes(bytes)));
    }
    return out;
}

} // namespace

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"06 - order-sensitive digest"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    const std::uint32_t width = abstract_governance_profile().payload_width;
    const std::vector<Segment> ascending{node_segment(1), node_segment(2), node_segment(3)};
    const std::vector<Segment> descending{node_segment(3), node_segment(2), node_segment(1)};

    transcript.step("the same three segments in two different orders");
    const Digest128 ascending_sequence = sequence_digest(ascending, width);
    const Digest128 descending_sequence = sequence_digest(descending, width);
    transcript.note("sequence_digest(1,2,3)", ascending_sequence);
    transcript.note("sequence_digest(3,2,1)", descending_sequence);
    CHECK(!(ascending_sequence == descending_sequence));
    CHECK(sequence_digest(ascending, width) == ascending_sequence);

    transcript.step("the two sequences are the same multiset of canonical segments");
    std::vector<Digest128> ascending_units = unit_digests(ascending, width);
    std::vector<Digest128> descending_units = unit_digests(descending, width);
    std::sort(ascending_units.begin(), ascending_units.end());
    std::sort(descending_units.begin(), descending_units.end());
    CHECK(ascending_units == descending_units);
    CHECK(!(canonical_sequence_bytes(ascending, width) ==
            canonical_sequence_bytes(descending, width)));

    transcript.step("the store commits both orders as distinct lineages");
    const MutationOutcome first = scenario.create(1, ascending);
    const MutationOutcome second = scenario.create(2, descending);
    CHECK(first.ok());
    CHECK(second.ok());
    CHECK(!(first.digest == second.digest));
    const std::optional<SegmentList> stored_first = scenario.s().get(list_id(1));
    const std::optional<SegmentList> stored_second = scenario.s().get(list_id(2));
    CHECK(stored_first.has_value());
    CHECK(stored_second.has_value());
    CHECK(!(stored_first->content_digest == stored_second->content_digest));
    CHECK(!(stored_first->sequence_digest == stored_second->sequence_digest));
    CHECK_EQ(stored_first->content.segments.front().id, segment_id(1));
    CHECK_EQ(stored_second->content.segments.front().id, segment_id(3));

    transcript.step("the sequence digest is identity-free, the content digest is not");
    const MutationOutcome third = scenario.create(3, ascending);
    CHECK(third.ok());
    const std::optional<SegmentList> stored_third = scenario.s().get(list_id(3));
    CHECK(stored_third.has_value());
    CHECK_EQ(stored_third->sequence_digest, stored_first->sequence_digest);
    CHECK(!(stored_third->content_digest == stored_first->content_digest));
    CHECK(!(stored_third->content_digest == stored_second->content_digest));

    transcript.finish();
    return transcript.exit_code();
}
