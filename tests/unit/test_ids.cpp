// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include <type_traits>

#include "fixtures.hpp"

namespace {

// Strong identity: distinct identifier domains are distinct types and nothing
// converts implicitly between them.
static_assert(!std::is_convertible_v<srf::SegmentId, srf::NodeId>);
static_assert(!std::is_convertible_v<srf::NodeId, srf::SegmentId>);
static_assert(!std::is_convertible_v<srf::SegmentListId, srf::SegmentId>);
static_assert(!std::is_convertible_v<srf::SegmentListGeneration, srf::SegmentPolicyGeneration>);
static_assert(!std::is_convertible_v<std::uint64_t, srf::SegmentListId>);
static_assert(std::is_same_v<decltype(srf::id_cast<srf::NodeId>(srf::SegmentId{1})),
                             srf::NodeId>);

} // namespace

SRF_TEST(ids, zero_is_invalid_and_nonzero_is_valid) {
    const srf::SegmentListId none{};
    const srf::SegmentListId some{7};
    SRF_EXPECT(!none.valid());
    SRF_EXPECT(some.valid());
    SRF_EXPECT(static_cast<bool>(some));
    SRF_EXPECT(!static_cast<bool>(none));
    const srf::SegmentListGeneration no_generation{};
    const srf::SegmentListGeneration one = srf::SegmentListGeneration::initial();
    SRF_EXPECT(!no_generation.valid());
    SRF_EXPECT(one.valid());
    SRF_EXPECT_EQ(one.value(), 1u);
}

SRF_TEST(ids, generations_are_monotonic_and_never_wrap) {
    srf::SegmentListGeneration generation = srf::SegmentListGeneration::initial();
    for (int i = 0; i < 1000; ++i) {
        const auto next = generation.next();
        SRF_EXPECT(next.has_value());
        SRF_EXPECT(*next > generation);
        generation = *next;
    }
    const srf::SegmentListGeneration last{srf::SegmentListGeneration::max_value};
    SRF_EXPECT(last.exhausted());
    SRF_EXPECT(!last.next().has_value());
}

SRF_TEST(ids, semantic_list_identity_is_stable_and_non_zero) {
    const srf::SegmentListId a = srf::derive_segment_list_id("core", "primary-path");
    const srf::SegmentListId b = srf::derive_segment_list_id("core", "primary-path");
    const srf::SegmentListId c = srf::derive_segment_list_id("core", "secondary-path");
    SRF_EXPECT(a.valid());
    SRF_EXPECT_EQ(a, b);
    SRF_EXPECT_NE(a, c);
    SRF_EXPECT_EQ(srf::derive_segment_list_id("", ""), srf::derive_segment_list_id("", ""));
}

SRF_TEST(ids, explicit_conversion_is_spelled_out) {
    const srf::SegmentId segment{42};
    const srf::NodeId node = srf::id_cast<srf::NodeId>(segment);
    SRF_EXPECT_EQ(node.value(), 42u);
    const srf::SegmentPolicyGeneration policy = srf::generation_cast<srf::SegmentPolicyGeneration>(
        srf::SegmentProfileGeneration{9});
    SRF_EXPECT_EQ(policy.value(), 9u);
}

SRF_TEST(ids, digest_helpers_are_deterministic_and_not_zero) {
    const srf::ByteBuffer empty{};
    const srf::Digest128 a = srf::digest128(srf::as_bytes(empty));
    const srf::Digest128 b = srf::digest128(srf::as_bytes(empty));
    SRF_EXPECT(a == b);
    SRF_EXPECT(!a.is_zero());
    const srf::ByteBuffer one{std::byte{1}};
    SRF_EXPECT(!(srf::digest128(srf::as_bytes(one)) == a));
    SRF_EXPECT_EQ(srf::crc32c(srf::as_bytes(one)), srf::crc32c(srf::as_bytes(one)));
    SRF_EXPECT_NE(srf::crc32c(srf::as_bytes(one)), srf::crc32c(srf::as_bytes(empty)));
}
