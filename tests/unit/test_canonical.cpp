// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

namespace {

srf::SegmentListContent content_of(std::vector<srf::Segment> segments,
                                   std::uint32_t width = 4) {
    (void)width;
    srf::SegmentListContent content{};
    content.id = srf::test::list_id(1);
    content.scope = srf::ScopeId{srf::test::kScope};
    content.profile = srf::SegmentProfileId{0x0001'0001ull};
    content.profile_generation = srf::SegmentProfileGeneration{1};
    content.segments = std::move(segments);
    return content;
}

} // namespace

SRF_TEST(canonical, equivalent_payload_spellings_canonicalize_identically) {
    srf::Segment short_payload = srf::test::node_segment(1);
    short_payload.payload = {std::byte{0x01}};
    srf::Segment padded_payload = srf::test::node_segment(1);
    padded_payload.payload = {std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    SRF_EXPECT(srf::segment_semantic_equal(short_payload, padded_payload, 4));
    const srf::ByteBuffer a = srf::canonical_segment_bytes(short_payload, 4);
    const srf::ByteBuffer b = srf::canonical_segment_bytes(padded_payload, 4);
    SRF_EXPECT(a == b);
}

SRF_TEST(canonical, implicitly_encoded_segment_canonicalizes_like_explicit_encoding) {
    srf::Segment implicit = srf::test::node_segment(1);
    implicit.encoding = srf::SegmentEncodingId::Invalid;
    srf::Segment explicit_encoding = srf::test::node_segment(1);
    explicit_encoding.encoding = srf::SegmentEncodingId::AbstractNodeV1;
    const std::vector<srf::Segment> canonical = srf::canonicalize_segments(
        std::span<const srf::Segment>(&implicit, 1));
    SRF_EXPECT_EQ(static_cast<int>(canonical[0].encoding),
                  static_cast<int>(srf::SegmentEncodingId::AbstractNodeV1));
    SRF_EXPECT(srf::sequence_digest(canonical, 4) == srf::sequence_digest(
                                                       std::span<const srf::Segment>(
                                                           &explicit_encoding, 1),
                                                       4));
}

SRF_TEST(canonical, order_is_semantic_and_never_normalized_away) {
    const std::vector<srf::Segment> forward{srf::test::node_segment(1),
                                            srf::test::node_segment(2),
                                            srf::test::node_segment(3)};
    const std::vector<srf::Segment> reversed{srf::test::node_segment(3),
                                             srf::test::node_segment(2),
                                             srf::test::node_segment(1)};
    const srf::ByteBuffer a = srf::canonical_sequence_bytes(forward, 4);
    const srf::ByteBuffer b = srf::canonical_sequence_bytes(reversed, 4);
    SRF_EXPECT(!(a == b));
    SRF_EXPECT(!(srf::sequence_digest(forward, 4) == srf::sequence_digest(reversed, 4)));
    SRF_EXPECT_EQ(a.size(), b.size());
}

SRF_TEST(canonical, different_orders_of_three_segments_all_differ) {
    const std::vector<srf::Segment> base{srf::test::node_segment(1), srf::test::node_segment(2),
                                         srf::test::node_segment(3)};
    std::vector<std::vector<srf::Segment>> permutations;
    std::vector<srf::Segment> work = base;
    std::sort(work.begin(), work.end(), [](const srf::Segment& x, const srf::Segment& y) {
        return x.id < y.id;
    });
    do {
        permutations.push_back(work);
    } while (std::next_permutation(work.begin(), work.end(),
                                   [](const srf::Segment& x, const srf::Segment& y) {
                                       return x.id < y.id;
                                   }));
    SRF_EXPECT_EQ(permutations.size(), static_cast<std::size_t>(6));
    for (std::size_t i = 0; i < permutations.size(); ++i) {
        for (std::size_t j = i + 1; j < permutations.size(); ++j) {
            SRF_EXPECT(!(srf::sequence_digest(permutations[i], 4) ==
                         srf::sequence_digest(permutations[j], 4)));
        }
    }
}

SRF_TEST(canonical, content_digest_is_order_sensitive_and_generation_independent) {
    std::vector<srf::Segment> segments{srf::test::node_segment(1), srf::test::node_segment(2)};
    const srf::SegmentListContent first = content_of(segments);
    const srf::Digest128 digest_one = srf::content_digest(first, 4);
    SRF_EXPECT(digest_one == srf::content_digest(content_of(segments), 4));
    std::reverse(segments.begin(), segments.end());
    SRF_EXPECT(!(digest_one == srf::content_digest(content_of(segments), 4)));
}

SRF_TEST(canonical, dependency_generation_change_changes_the_content_digest) {
    srf::SegmentListContent content = content_of({srf::test::node_segment(1)});
    const srf::Digest128 before = srf::content_digest(content, 4);
    content.capability = srf::CapabilityGeneration{2};
    SRF_EXPECT(!(before == srf::content_digest(content, 4)));
    content.capability = srf::CapabilityGeneration{};
    content.topology = srf::TopologyGeneration{7};
    SRF_EXPECT(!(before == srf::content_digest(content, 4)));
}

SRF_TEST(canonical, canonical_bytes_are_little_endian_field_explicit) {
    const srf::Segment segment = srf::test::node_segment(1);
    const srf::ByteBuffer bytes = srf::canonical_sequence_bytes(
        std::span<const srf::Segment>(&segment, 1), 4);
    // magic 0xC503, count 1, kind 1, encoding 1, id 0x3001
    SRF_EXPECT_EQ(bytes.size(), static_cast<std::size_t>(2 + 4 + 78 + 4));
    // magic 0xC503 little endian, count 1
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[0]), 0x03u);
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[1]), 0xC5u);
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[2]), 0x01u);
    // kind = 1
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[6]), 0x01u);
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[7]), 0x00u);
    // id = 0x3001 little endian. The raw segment carries an unset encoding; the
    // encoder writes what the segment says and canonicalization is the store's job.
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[8]), 0x00u);
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[10]), 0x01u);
    SRF_EXPECT_EQ(static_cast<unsigned>(bytes[11]), 0x30u);
    const std::vector<srf::Segment> canonical =
        srf::canonicalize_segments(std::span<const srf::Segment>(&segment, 1));
    const srf::ByteBuffer encoded =
        srf::canonical_sequence_bytes(canonical, 4);
    SRF_EXPECT_EQ(static_cast<unsigned>(encoded[8]), 0x01u);
    SRF_EXPECT(!(encoded == bytes));
}

SRF_TEST(canonical, segment_equality_ignores_accepted_padding_only) {
    srf::Segment a = srf::test::node_segment(1);
    srf::Segment b = srf::test::node_segment(1);
    b.payload = {std::byte{0x01}, std::byte{0x00}};
    SRF_EXPECT(a == b);
    b.payload = {std::byte{0x02}};
    SRF_EXPECT(a != b);
    b = srf::test::node_segment(1);
    b.node = srf::test::node_id(9);
    SRF_EXPECT(a != b);
}
