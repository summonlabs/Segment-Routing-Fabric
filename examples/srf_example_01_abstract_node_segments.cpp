// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 01 - the abstract governance profile and the order-sensitive digest of
// a node-only list. SYNTHETIC evidence; nothing here is a physical claim.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"01 - abstract node segments (abstract governance profile)"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());

    transcript.step("the shipped profile is ABSTRACT/SYNTHETIC, never physical");
    const SegmentProfile profile = abstract_governance_profile();
    transcript.note("profile.name", profile.name);
    transcript.note("profile.id", profile.id.value());
    transcript.note("profile.generation", profile.generation.value());
    transcript.note("profile.support", as_int(profile.support));
    transcript.note("profile.max_depth", static_cast<std::uint64_t>(profile.max_depth));
    transcript.note("profile.payload_width", static_cast<std::uint64_t>(profile.payload_width));
    transcript.note("profile.allowed_kinds_mask",
                    static_cast<std::uint64_t>(profile.allowed_kinds_mask));
    transcript.note("profile.required_capabilities",
                    static_cast<std::uint64_t>(profile.required_capabilities.size()));
    transcript.note("profile.digest", profile.digest());
    CHECK_EQ(as_int(profile.support), as_int(SupportClassification::AbstractSynthetic));
    CHECK(profile.allows_kind(SegmentKind::Node));
    CHECK(profile.allows_kind(SegmentKind::Adjacency));
    CHECK(!profile.allows_kind(SegmentKind::Invalid));
    CHECK(!profile.allow_empty);
    CHECK(!profile.allow_loose);

    transcript.step("create a governed list of three ordered node segments");
    std::vector<Segment> forward{node_segment(1), node_segment(2), node_segment(3)};
    const MutationOutcome outcome = scenario.create(1, forward);
    CHECK(outcome.ok());
    transcript.note("outcome.status", status_code_name(outcome.status));
    transcript.note("outcome.generation", outcome.generation.value());
    transcript.note("outcome.digest", outcome.digest);
    CHECK(outcome.durable);
    CHECK(!outcome.digest.is_zero());

    const std::optional<SegmentList> stored = scenario.s().get(list_id(1));
    CHECK(stored.has_value());
    const SegmentList& list = *stored;
    transcript.note("list.state", lifecycle_state_name(list.state));
    transcript.note("list.currentness", currentness_name(list.currentness));
    transcript.note("list.content.profile_generation", list.content.profile_generation.value());
    transcript.note("list.content.capability", list.content.capability.value());
    CHECK_EQ(as_int(list.state), as_int(LifecycleState::Active));
    CHECK_EQ(as_int(list.currentness), as_int(Currentness::Current));
    CHECK_EQ(list.content.segments.size(), std::size_t{3});
    for (const Segment& segment : list.content.segments) {
        CHECK_EQ(as_int(segment.encoding), as_int(SegmentEncodingId::AbstractNodeV1));
    }
    CHECK_EQ(list.content_digest, outcome.digest);
    CHECK_EQ(list.content_digest, content_digest(list.content, profile.payload_width));
    CHECK_EQ(list.sequence_digest, sequence_digest(list.content.segments, profile.payload_width));

    transcript.step("payload padding is canonical: one byte and four bytes spell the same segment");
    const Segment short_payload = node_segment(1, static_cast<std::uint8_t>(0x11));
    Segment padded = short_payload;
    padded.payload = {std::byte{0x11}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    CHECK(canonical_segment_bytes(short_payload, profile.payload_width) ==
          canonical_segment_bytes(padded, profile.payload_width));
    CHECK(segment_semantic_equal(short_payload, padded, profile.payload_width));

    transcript.step("the store canonicalizes the encoding before it digests");
    const std::vector<Segment> canonical = canonicalize_segments(forward);
    const std::vector<Segment> reversed_draft{node_segment(3), node_segment(2), node_segment(1)};
    const std::vector<Segment> canonical_reversed = canonicalize_segments(reversed_draft);
    transcript.note("draft[0].encoding", as_int(forward.front().encoding));
    transcript.note("canonical[0].encoding", as_int(canonical.front().encoding));
    CHECK_EQ(as_int(forward.front().encoding), as_int(SegmentEncodingId::Invalid));
    CHECK_EQ(as_int(canonical.front().encoding), as_int(SegmentEncodingId::AbstractNodeV1));

    transcript.step("order is semantic: the same three segments reversed digest differently");
    const Digest128 forward_digest = sequence_digest(canonical, profile.payload_width);
    const Digest128 reverse_digest = sequence_digest(canonical_reversed, profile.payload_width);
    transcript.note("sequence_digest(order 1,2,3)", forward_digest);
    transcript.note("sequence_digest(order 3,2,1)", reverse_digest);
    CHECK_EQ(forward_digest, list.sequence_digest);
    CHECK(!(forward_digest == reverse_digest));

    transcript.finish();
    return transcript.exit_code();
}
