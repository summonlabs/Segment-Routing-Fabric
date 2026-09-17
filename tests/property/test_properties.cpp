// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic randomized properties over the public store API.
//
// Every property draws its shape from srf::test::Random (splitmix64) with an
// explicit seed that is installed through srf::test::set_active_seed(), so a
// failing iteration prints the exact seed needed to replay it. Scenarios are
// always built as valid drafts first and then mutated one aspect at a time, so
// the runtime is always asked a question it can answer.
#include "fixtures.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using srf::test::Harness;
using srf::test::Random;

/// "Several hundred" deterministic iterations per property.
constexpr std::size_t kIterations = 320;
constexpr std::uint32_t kGovernanceWidth = 4;
constexpr std::uint64_t kGatedProfileId = 0x0001'0100ull;
constexpr std::uint64_t kGatedCapability = 0x2001;

[[nodiscard]] std::uint64_t seed_for(std::uint64_t tag) {
    return 0x5EED'0000'0000'0000ull ^ (tag * 0x9E37'79B9'7F4A'7C15ull);
}

void shuffle(Random& rng, std::vector<std::uint64_t>& values) {
    for (std::size_t i = values.size(); i > 1; --i) {
        const std::size_t j = rng.below(static_cast<std::uint32_t>(i));
        std::swap(values[i - 1], values[j]);
    }
}

/// A sequence of distinct NODE segments drawn from the synthetic fabric.
[[nodiscard]] std::vector<srf::Segment> random_node_sequence(Random& rng, std::uint32_t count) {
    std::vector<std::uint64_t> pool;
    pool.reserve(60);
    for (std::uint64_t n = 1; n <= 60; ++n) {
        pool.push_back(n);
    }
    shuffle(rng, pool);
    std::vector<srf::Segment> out;
    for (std::uint32_t i = 0; i < count && i < pool.size(); ++i) {
        out.push_back(srf::test::node_segment(pool[i], 1 + rng.below(4)));
    }
    return out;
}

/// A distinct segment of a random kind, valid for the governance profile.
[[nodiscard]] srf::Segment random_kind_segment(Random& rng, std::uint64_t n, std::size_t index) {
    switch (index % 4u) {
        case 0: return srf::test::node_segment(n, 1 + rng.below(4));
        case 1: return srf::test::endpoint_segment(n);
        case 2: return srf::test::binding_segment(n);
        default: return srf::test::policy_segment(n);
    }
}

/// The seven deterministic watermark races are exercised in tests/race; here the
/// properties only need a store whose dependencies can be moved on purpose.
[[nodiscard]] bool list_is_active(const std::optional<srf::SegmentList>& list) {
    return list.has_value() && list->state == srf::LifecycleState::Active;
}

void expect_never_active(srf::test::Context& srf_ctx, srf::SegmentListStore& store,
                         srf::SegmentListId id) {
    const auto stored = store.get(id);
    SRF_EXPECT(!list_is_active(stored));
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Order is semantic: the stored sequence equals the submitted sequence,
//    element by element, in the submitted order.
// ---------------------------------------------------------------------------
SRF_TEST(property, list_order_is_preserved) {
    const std::uint64_t seed = seed_for(1);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::uint32_t count = 1 + rng.below(5);
        const std::vector<srf::Segment> submitted = random_node_sequence(rng, count);
        const std::vector<srf::Segment> canonical = srf::canonicalize_segments(submitted);
        const srf::MutationOutcome outcome = harness.create(idn, submitted);
        SRF_EXPECT(outcome.ok());
        if (!outcome.ok()) {
            continue;
        }
        const auto stored = harness.store().get(srf::test::list_id(idn));
        SRF_EXPECT(stored.has_value());
        if (!stored.has_value()) {
            continue;
        }
        SRF_EXPECT_EQ(stored->content.segments.size(), canonical.size());
        const std::size_t compare =
            std::min(stored->content.segments.size(), canonical.size());
        for (std::size_t i = 0; i < compare; ++i) {
            SRF_EXPECT_EQ(static_cast<int>(stored->content.segments[i].kind),
                          static_cast<int>(canonical[i].kind));
            SRF_EXPECT_EQ(stored->content.segments[i].id, canonical[i].id);
            SRF_EXPECT_EQ(stored->content.segments[i].node, canonical[i].node);
            SRF_EXPECT_EQ(static_cast<int>(stored->content.segments[i].encoding),
                          static_cast<int>(canonical[i].encoding));
            SRF_EXPECT(stored->content.segments[i].payload == canonical[i].payload);
        }
        SRF_EXPECT(srf::canonical_sequence_bytes(stored->content.segments, kGovernanceWidth) ==
                   srf::canonical_sequence_bytes(canonical, kGovernanceWidth));
        SRF_EXPECT_EQ(stored->sequence_digest,
                      srf::sequence_digest(canonical, kGovernanceWidth));
    }
}

// ---------------------------------------------------------------------------
// 2. A structurally invalid list never becomes ACTIVE, and the mutation reports
//    the exact defect it found.
// ---------------------------------------------------------------------------
SRF_TEST(property, structurally_invalid_list_never_becomes_active) {
    const std::uint64_t seed = seed_for(2);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::uint32_t count = 1 + rng.below(4);
        std::vector<srf::Segment> defective = random_node_sequence(rng, count);
        const std::uint32_t index = rng.below(static_cast<std::uint32_t>(defective.size()));
        srf::ReasonCode expected = srf::ReasonCode::Ok;
        switch (iteration % 10u) {
            case 0:
                defective[index].kind = srf::SegmentKind::Invalid;
                expected = srf::ReasonCode::SegmentKindInvalid;
                break;
            case 1:
                defective[index].id = srf::SegmentId{};
                expected = srf::ReasonCode::SegmentIdInvalid;
                break;
            case 2:
                defective[index].generation = srf::SegmentGeneration{};
                expected = srf::ReasonCode::SegmentGenerationInvalid;
                break;
            case 3:
                defective[index].encoding = srf::SegmentEncodingId::AbstractAdjacencyV1;
                expected = srf::ReasonCode::SegmentEncodingMismatch;
                break;
            case 4:
                defective[index].endpoint = srf::test::endpoint_id(1);
                expected = srf::ReasonCode::SegmentReservedFieldSet;
                break;
            case 5:
                defective[index].node = srf::NodeId{};
                expected = srf::ReasonCode::SegmentBindingIncomplete;
                break;
            case 6:
                defective[index].payload = {std::byte{1}, std::byte{2}, std::byte{3},
                                            std::byte{4}, std::byte{5}};
                expected = srf::ReasonCode::ProfilePayloadWidthExceeded;
                break;
            case 7: {
                const srf::Segment duplicate = defective[index];
                defective.insert(defective.begin() + static_cast<std::ptrdiff_t>(index),
                                 duplicate);
                expected = srf::ReasonCode::SegmentDuplicateConsecutive;
                break;
            }
            case 8:
                defective.clear();
                expected = srf::ReasonCode::ProfileEmptyNotPermitted;
                break;
            default: {
                defective = random_node_sequence(rng, 9);
                expected = srf::ReasonCode::ProfileMaxDepthExceeded;
                break;
            }
        }
        const srf::MutationOutcome outcome = harness.create(idn, defective);
        SRF_EXPECT(!outcome.ok());
        SRF_EXPECT(outcome.result.failed());
        SRF_EXPECT_REASON(outcome.result, expected);
        expect_never_active(srf_ctx, harness.store(), srf::test::list_id(idn));
    }
}

// ---------------------------------------------------------------------------
// 3. A list whose profile requires a missing or unsupported capability never
//    becomes ACTIVE.
// ---------------------------------------------------------------------------
SRF_TEST(property, missing_or_unsupported_capability_never_becomes_active) {
    const std::uint64_t seed = seed_for(3);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    srf::SegmentProfile gated = srf::abstract_governance_profile();
    gated.id = srf::SegmentProfileId{kGatedProfileId};
    gated.generation = srf::SegmentProfileGeneration{1};
    gated.name = "property-gated-v1";
    gated.required_capabilities = {srf::CapabilityKey{kGatedCapability}};
    SRF_EXPECT(harness.store().register_profile(gated).ok());

    std::size_t activated = 0;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::vector<srf::Segment> segments = random_node_sequence(rng, 1 + rng.below(3));
        srf::ListDraft draft = harness.draft(idn, kGatedProfileId);
        draft.segments = segments;
        const std::uint32_t variant = rng.below(3);
        srf::ReasonCode expected = srf::ReasonCode::Ok;
        if (variant == 0) {
            harness.fabric().remove_capability(kGatedCapability);
            expected = srf::ReasonCode::CapabilityMissing;
        } else if (variant == 1) {
            harness.fabric().set_capability_supported(kGatedCapability, false);
            expected = srf::ReasonCode::CapabilityUnsupported;
        } else {
            harness.fabric().set_capability_supported(kGatedCapability, true);
        }
        const srf::MutationOutcome outcome =
            harness.store().create_list(harness.caller(), draft);
        if (expected == srf::ReasonCode::Ok) {
            SRF_EXPECT(outcome.ok());
            const auto stored = harness.store().get(srf::test::list_id(idn));
            SRF_EXPECT(list_is_active(stored));
            if (list_is_active(stored)) {
                ++activated;
            }
        } else {
            SRF_EXPECT(!outcome.ok());
            SRF_EXPECT_REASON(outcome.result, expected);
            expect_never_active(srf_ctx, harness.store(), srf::test::list_id(idn));
        }
    }
    // The positive control must actually have been reachable, otherwise the
    // property would be vacuously true.
    SRF_EXPECT(activated > 0);
}

// ---------------------------------------------------------------------------
// 4. A stale adjacency never remains current.
// ---------------------------------------------------------------------------
SRF_TEST(property, stale_adjacency_never_remains_current) {
    const std::uint64_t seed = seed_for(4);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::uint64_t adjacency = 1 + (iteration % 60);
        const auto topology = srf::TopologyGeneration{
            harness.fabric().evidence()->watermark().topology.value()};
        srf::ListDraft draft = harness.draft(idn);
        draft.topology = topology;
        draft.segments = {srf::test::adjacency_segment(adjacency, topology)};
        const srf::MutationOutcome outcome =
            harness.store().create_list(harness.caller(), draft);
        SRF_EXPECT(outcome.ok());
        if (!outcome.ok()) {
            continue;
        }
        SRF_EXPECT(list_is_active(harness.store().get(srf::test::list_id(idn))));

        const bool removed = (iteration % 2u) == 1u;
        if (removed) {
            harness.fabric().remove_adjacency(adjacency);
        } else {
            harness.fabric().advance_topology();
        }
        (void)harness.store().refresh_currentness();
        const auto stored = harness.store().get(srf::test::list_id(idn));
        SRF_EXPECT(stored.has_value());
        if (!stored.has_value()) {
            continue;
        }
        SRF_EXPECT(stored->currentness != srf::Currentness::Current);
        SRF_EXPECT_EQ(static_cast<int>(stored->currentness),
                      static_cast<int>(srf::Currentness::StaleTopology));
        SRF_EXPECT_EQ(static_cast<int>(stored->state),
                      static_cast<int>(srf::LifecycleState::RevalidationRequired));
        if (removed) {
            // Restore the removed adjacency so later iterations draw a live one.
            harness.fabric().advance_topology();
        }
    }
}

// ---------------------------------------------------------------------------
// 5. Stale Path Authority never remains current.
// ---------------------------------------------------------------------------
SRF_TEST(property, stale_path_authority_never_remains_current) {
    const std::uint64_t seed = seed_for(5);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::uint64_t authority =
            harness.fabric().evidence()->watermark().path_authority.value();
        srf::ListDraft draft = harness.draft(idn);
        draft.segments = random_node_sequence(rng, 1 + rng.below(3));
        draft.path = srf::PathId{srf::test::kPath};
        draft.path_authority = srf::PathAuthorityGeneration{authority};
        const srf::MutationOutcome outcome =
            harness.store().create_list(harness.caller(), draft);
        SRF_EXPECT(outcome.ok());
        if (!outcome.ok()) {
            continue;
        }
        const auto before = harness.store().get(srf::test::list_id(idn));
        SRF_EXPECT(list_is_active(before));
        if (!before.has_value()) {
            continue;
        }
        SRF_EXPECT_EQ(before->content.path_authority.value(), authority);

        harness.fabric().advance_path_authority();
        (void)harness.store().refresh_currentness();
        const auto stored = harness.store().get(srf::test::list_id(idn));
        SRF_EXPECT(stored.has_value());
        if (!stored.has_value()) {
            continue;
        }
        SRF_EXPECT(stored->currentness != srf::Currentness::Current);
        SRF_EXPECT_EQ(static_cast<int>(stored->currentness),
                      static_cast<int>(srf::Currentness::StalePathAuthority));
        SRF_EXPECT_EQ(static_cast<int>(stored->state),
                      static_cast<int>(srf::LifecycleState::RevalidationRequired));
        SRF_EXPECT_EQ(stored->content.path_authority.value(), authority);
    }
}

// ---------------------------------------------------------------------------
// 6. Canonically equivalent spellings hash identically: payload padding and an
//    unset encoding are normalization, not identity.
// ---------------------------------------------------------------------------
SRF_TEST(property, canonically_equivalent_spellings_hash_identically) {
    const std::uint64_t seed = seed_for(6);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t n = 1 + rng.below(60);
        srf::Segment first = random_kind_segment(rng, n, iteration);
        const srf::SegmentEncodingId required = srf::expected_encoding_for(first.kind);
        SRF_EXPECT(required != srf::SegmentEncodingId::Invalid);
        first.encoding = required;
        const std::uint32_t raw = rng.below(kGovernanceWidth + 1);
        first.payload.clear();
        for (std::uint32_t b = 0; b < raw; ++b) {
            first.payload.push_back(std::byte{static_cast<unsigned char>(1 + rng.below(200))});
        }

        // Same segment, spelled with an unset encoding and a zero padded payload.
        srf::Segment second = first;
        second.encoding = srf::SegmentEncodingId::Invalid;
        while (second.payload.size() < kGovernanceWidth) {
            second.payload.push_back(std::byte{0});
        }

        // Canonicalization is the documented normalization step: an unset encoding
        // becomes the encoding the kind requires, and the payload is zero padded to
        // the profile width by the encoder. Two equivalent spellings must then hash
        // identically.
        const std::vector<srf::Segment> first_one{first};
        const std::vector<srf::Segment> second_one{second};
        const std::vector<srf::Segment> a = srf::canonicalize_segments(first_one);
        const std::vector<srf::Segment> b = srf::canonicalize_segments(second_one);
        SRF_EXPECT_EQ(a.size(), static_cast<std::size_t>(1));
        SRF_EXPECT_EQ(b.size(), static_cast<std::size_t>(1));
        if (a.size() != 1 || b.size() != 1) {
            continue;
        }
        SRF_EXPECT_EQ(static_cast<int>(a.front().encoding), static_cast<int>(required));
        SRF_EXPECT(srf::segment_semantic_equal(a.front(), b.front(), kGovernanceWidth));
        SRF_EXPECT(srf::canonical_segment_bytes(a.front(), kGovernanceWidth) ==
                   srf::canonical_segment_bytes(b.front(), kGovernanceWidth));
        SRF_EXPECT_EQ(srf::sequence_digest(a, kGovernanceWidth),
                      srf::sequence_digest(b, kGovernanceWidth));
        SRF_EXPECT(srf::canonical_sequence_bytes(a, kGovernanceWidth) ==
                   srf::canonical_sequence_bytes(b, kGovernanceWidth));
    }

    // End to end: replacing a list with the equivalent unpadded spelling leaves
    // the content digest unchanged, because padding is not semantics.
    Harness harness;
    for (std::size_t iteration = 0; iteration < 32; ++iteration) {
        srf::Segment padded = srf::test::node_segment(1 + iteration, 7);
        padded.payload = {std::byte{7}, std::byte{0}, std::byte{0}, std::byte{0}};
        srf::Segment unpadded = padded;
        unpadded.payload = {std::byte{7}};
        const srf::MutationOutcome created = harness.create(1 + iteration, {padded});
        SRF_EXPECT(created.ok());
        if (!created.ok()) {
            continue;
        }
        srf::ListDraft draft = harness.draft(1 + iteration);
        draft.segments = {unpadded};
        draft.expected_generation = srf::SegmentListGeneration{1};
        const srf::MutationOutcome replaced =
            harness.store().replace_list(harness.caller(), draft);
        SRF_EXPECT(replaced.ok());
        if (!replaced.ok()) {
            continue;
        }
        SRF_EXPECT_EQ(replaced.digest, created.digest);
        SRF_EXPECT_EQ(replaced.generation.value(), 2u);
    }
}

// ---------------------------------------------------------------------------
// 7. Different semantic order hashes differently.
// ---------------------------------------------------------------------------
SRF_TEST(property, different_semantic_order_hashes_differently) {
    const std::uint64_t seed = seed_for(7);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        std::vector<srf::Segment> forward = random_node_sequence(rng, 2 + rng.below(4));
        std::vector<srf::Segment> reversed(forward.rbegin(), forward.rend());
        SRF_EXPECT(srf::canonical_sequence_bytes(forward, kGovernanceWidth) !=
                   srf::canonical_sequence_bytes(reversed, kGovernanceWidth));
        SRF_EXPECT(srf::sequence_digest(forward, kGovernanceWidth) !=
                   srf::sequence_digest(reversed, kGovernanceWidth));

        const srf::MutationOutcome created = harness.create(idn, forward);
        SRF_EXPECT(created.ok());
        if (!created.ok()) {
            continue;
        }
        srf::ListDraft draft = harness.draft(idn);
        draft.segments = reversed;
        draft.expected_generation = srf::SegmentListGeneration{1};
        const srf::MutationOutcome replaced =
            harness.store().replace_list(harness.caller(), draft);
        SRF_EXPECT(replaced.ok());
        if (!replaced.ok()) {
            continue;
        }
        SRF_EXPECT(replaced.digest != created.digest);
        const auto stored = harness.store().get(srf::test::list_id(idn));
        SRF_EXPECT(stored.has_value());
        if (!stored.has_value()) {
            continue;
        }
        SRF_EXPECT_EQ(stored->content.segments.size(), reversed.size());
        for (std::size_t i = 0;
             i < std::min(stored->content.segments.size(), reversed.size()); ++i) {
            SRF_EXPECT_EQ(stored->content.segments[i].id, reversed[i].id);
        }
    }
}

// ---------------------------------------------------------------------------
// 8. A retired list never reactivates through any path.
// ---------------------------------------------------------------------------
SRF_TEST(property, retired_list_never_reactivates) {
    const std::uint64_t seed = seed_for(8);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::vector<srf::Segment> segments = random_node_sequence(rng, 1 + rng.below(3));
        SRF_EXPECT(harness.create(idn, segments).ok());
        const srf::SegmentListId id = srf::test::list_id(idn);
        const srf::SegmentListGeneration generation{1};
        SRF_EXPECT(harness.store().retire(harness.caller(), id, generation).ok());

        const srf::MutationOutcome revalidate =
            harness.store().revalidate(harness.caller(), id, generation);
        SRF_EXPECT(!revalidate.ok());
        SRF_EXPECT(revalidate.result.failed());
        SRF_EXPECT_REASON(revalidate.result, srf::ReasonCode::ListRetired);

        const srf::MutationOutcome withdraw =
            harness.store().withdraw(harness.caller(), id, generation);
        SRF_EXPECT(!withdraw.ok());
        SRF_EXPECT(withdraw.result.contains(srf::ReasonCode::ListRetired) ||
                   withdraw.result.contains(srf::ReasonCode::LifecycleTransitionDenied));

        const srf::MutationOutcome withdraw_commit =
            harness.store().withdraw_commit(harness.caller(), id, generation);
        SRF_EXPECT(!withdraw_commit.ok());
        SRF_EXPECT(withdraw_commit.result.contains(srf::ReasonCode::ListRetired) ||
                   withdraw_commit.result.contains(srf::ReasonCode::LifecycleTransitionDenied));

        const srf::MutationOutcome revoke =
            harness.store().revoke(harness.caller(), id, generation);
        SRF_EXPECT(!revoke.ok());

        srf::ListDraft draft = harness.draft(idn);
        draft.segments = random_node_sequence(rng, 1);
        draft.expected_generation = generation;
        const srf::MutationOutcome replace =
            harness.store().replace_list(harness.caller(), draft);
        SRF_EXPECT(!replace.ok());
        SRF_EXPECT_REASON(replace.result, srf::ReasonCode::ListRetired);

        const auto stored = harness.store().get(id);
        SRF_EXPECT(stored.has_value());
        if (stored.has_value()) {
            SRF_EXPECT_EQ(static_cast<int>(stored->state),
                          static_cast<int>(srf::LifecycleState::Retired));
            SRF_EXPECT_EQ(stored->generation.value(), 1u);
        }
    }
}

// ---------------------------------------------------------------------------
// 9. A stale worker or a stale epoch can never mutate.
// ---------------------------------------------------------------------------
SRF_TEST(property, stale_worker_or_epoch_can_never_mutate) {
    const std::uint64_t seed = seed_for(9);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    // A second publisher is registered so that it can be fenced without
    // disturbing the publisher the other properties mutate with.
    const srf::PublisherId fenced_publisher{0x9100};
    const srf::WorkerBootId live_boot{0xB100};
    SRF_EXPECT(harness.store()
                   .authority()
                   .register_publisher(fenced_publisher, live_boot, srf::ScopeId{srf::test::kScope},
                                       harness.store().authority().epoch(),
                                       harness.store().limits())
                   .ok());
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::vector<srf::Segment> segments = random_node_sequence(rng, 1 + rng.below(3));
        const std::uint64_t revision = harness.store().revision();
        const std::size_t lists = harness.store().list_count();

        srf::ListDraft draft = harness.draft(idn);
        draft.segments = segments;
        srf::CallerIdentity caller = harness.caller();
        srf::ReasonCode expected = srf::ReasonCode::Ok;
        if (iteration % 3u == 0u) {
            caller.epoch = srf::CoordinatorEpoch{caller.epoch.value() + 1};
            expected = srf::ReasonCode::EpochStale;
        } else if (iteration % 3u == 1u) {
            caller.publisher = fenced_publisher;
            caller.boot = srf::WorkerBootId{live_boot.value() + 1};
            expected = srf::ReasonCode::CallerStaleWorkerBoot;
        } else {
            SRF_EXPECT(harness.store().authority().mark_dead(fenced_publisher).ok());
            caller.publisher = fenced_publisher;
            caller.boot = live_boot;
            expected = srf::ReasonCode::CallerFencedPublisher;
        }
        const srf::MutationOutcome outcome =
            harness.store().create_list(caller, draft);
        SRF_EXPECT(!outcome.ok());
        SRF_EXPECT_REASON(outcome.result, expected);
        SRF_EXPECT_EQ(harness.store().revision(), revision);
        SRF_EXPECT_EQ(harness.store().list_count(), lists);
        expect_never_active(srf_ctx, harness.store(), srf::test::list_id(idn));
    }
}

// ---------------------------------------------------------------------------
// 10. Exact replay advances nothing: revision, generation and the persistence
//     save count are all unchanged, while a reuse mismatch is a conflict.
// ---------------------------------------------------------------------------
SRF_TEST(property, exact_replay_advances_nothing) {
    const std::uint64_t seed = seed_for(10);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    SRF_EXPECT(harness.memory() != nullptr);
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const srf::CallerIdentity caller = harness.caller();
        srf::ListDraft draft = harness.draft(idn);
        draft.segments = random_node_sequence(rng, 1 + rng.below(3));
        const srf::MutationOutcome first = harness.store().create_list(caller, draft);
        SRF_EXPECT(first.ok());
        if (!first.ok()) {
            continue;
        }
        const std::uint64_t revision = harness.store().revision();
        const std::size_t saves = harness.memory()->save_count();

        const srf::MutationOutcome replay = harness.store().create_list(caller, draft);
        SRF_EXPECT(replay.ok());
        SRF_EXPECT(replay.idempotent_replay);
        SRF_EXPECT_EQ(replay.generation.value(), first.generation.value());
        SRF_EXPECT_EQ(replay.digest, first.digest);
        SRF_EXPECT_EQ(harness.store().revision(), revision);
        const auto replayed = harness.store().get(srf::test::list_id(idn));
        SRF_EXPECT(replayed.has_value());
        if (replayed.has_value()) {
            SRF_EXPECT_EQ(replayed->generation.value(), first.generation.value());
            SRF_EXPECT_EQ(replayed->content_digest, first.digest);
        }
        SRF_EXPECT_EQ(harness.memory()->save_count(), saves);

        srf::ListDraft different = draft;
        different.segments = random_node_sequence(rng, 1 + rng.below(3));
        const srf::MutationOutcome mismatch =
            harness.store().create_list(caller, different);
        SRF_EXPECT(!mismatch.ok());
        SRF_EXPECT_REASON(mismatch.result, srf::ReasonCode::CommitReplayPayloadMismatch);
        SRF_EXPECT_EQ(harness.store().revision(), revision);
        SRF_EXPECT_EQ(harness.memory()->save_count(), saves);
    }
}

// ---------------------------------------------------------------------------
// 11. Generations never decrease and never wrap.
// ---------------------------------------------------------------------------
SRF_TEST(property, generations_never_decrease_and_never_wrap) {
    const std::uint64_t seed = seed_for(11);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        SRF_EXPECT(harness.create(idn, random_node_sequence(rng, 1)).ok());
        std::uint64_t previous = 1;
        const std::uint32_t rounds = 1 + rng.below(4);
        for (std::uint32_t round = 0; round < rounds; ++round) {
            const srf::MutationOutcome outcome =
                harness.replace(idn, random_node_sequence(rng, 1), previous);
            SRF_EXPECT(outcome.ok());
            if (!outcome.ok()) {
                break;
            }
            SRF_EXPECT(outcome.generation.valid());
            SRF_EXPECT(outcome.generation.value() > previous);
            SRF_EXPECT(!outcome.generation.exhausted());
            const auto stored = harness.store().get(srf::test::list_id(idn));
            SRF_EXPECT(stored.has_value());
            if (stored.has_value()) {
                SRF_EXPECT_EQ(stored->generation.value(), outcome.generation.value());
            }
            previous = outcome.generation.value();
        }
    }

    // Exhaustion is an explicit rejection, never a wrap.
    const srf::SegmentListGeneration at_max{srf::SegmentListGeneration::max_value};
    SRF_EXPECT(at_max.exhausted());
    SRF_EXPECT(!at_max.next().has_value());

    // The same rule through the durable path: a recovered lineage already at the
    // maximum generation cannot be replaced.
    {
        Harness source;
        SRF_EXPECT(source.create(1, {srf::test::node_segment(1)}).ok());
        srf::DurableState state = source.store().export_state();
        SRF_EXPECT(!state.lists.empty());
        if (!state.lists.empty()) {
            state.lists.front().generation = at_max;
            auto memory = std::make_shared<srf::MemoryPersistence>();
            srf::ValidationResult saved;
            SRF_EXPECT(memory->save(state, source.store().limits(), saved));
            srf::StoreConfig config{};
            config.limits = source.store().limits();
            config.evidence = source.fabric().evidence();
            config.persistence = memory;
            srf::SegmentListStore restarted(std::move(config));
            const srf::LoadReport report = restarted.load();
            SRF_EXPECT(report.loaded);
            SRF_EXPECT_EQ(restarted.list_count(), static_cast<std::size_t>(1));
            SRF_EXPECT(restarted.authority()
                           .register_publisher(srf::PublisherId{0x9200},
                                               srf::WorkerBootId{0xB200},
                                               srf::ScopeId{srf::test::kScope},
                                               restarted.authority().epoch(),
                                               restarted.limits())
                           .ok());
            srf::CallerIdentity caller{};
            caller.epoch = restarted.authority().epoch();
            caller.publisher = srf::PublisherId{0x9200};
            caller.boot = srf::WorkerBootId{0xB200};
            caller.scope = srf::ScopeId{srf::test::kScope};
            caller.attempt = srf::MutationAttemptId{0xEA00};
            srf::ListDraft draft{};
            draft.id = srf::test::list_id(1);
            draft.scope = srf::ScopeId{srf::test::kScope};
            draft.profile = srf::SegmentProfileId{0x0001'0001ull};
            draft.segments = {srf::test::node_segment(2)};
            draft.expected_generation = at_max;
            const srf::MutationOutcome outcome = restarted.replace_list(caller, draft);
            SRF_EXPECT(!outcome.ok());
            SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::ListGenerationExhausted);
            const auto stored = restarted.get(srf::test::list_id(1));
            SRF_EXPECT(stored.has_value());
            if (stored.has_value()) {
                SRF_EXPECT_EQ(stored->generation.value(), srf::SegmentListGeneration::max_value);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 12. Persistence round-trips: encode/decode of the exported state reproduces
//     every list, digest and segment.
// ---------------------------------------------------------------------------
SRF_TEST(property, persistence_round_trip_reproduces_everything) {
    const std::uint64_t seed = seed_for(12);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        Harness harness;
        const std::uint32_t list_count = 1 + rng.below(4);
        for (std::uint32_t n = 1; n <= list_count; ++n) {
            const std::uint32_t count = 1 + rng.below(4);
            std::vector<srf::Segment> segments;
            for (std::uint32_t i = 0; i < count; ++i) {
                segments.push_back(random_kind_segment(rng, 1 + n * 4 + i, i));
            }
            const srf::MutationOutcome outcome = harness.create(n, segments);
            SRF_EXPECT(outcome.ok());
        }
        const srf::DurableState original = harness.store().export_state();
        SRF_EXPECT_EQ(original.lists.size(), static_cast<std::size_t>(list_count));
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes =
            srf::encode_durable_state(original, harness.store().limits(), encode_result);
        SRF_EXPECT(encode_result.ok());
        SRF_EXPECT(!bytes.empty());
        srf::ValidationResult second_result;
        const srf::ByteBuffer again =
            srf::encode_durable_state(original, harness.store().limits(), second_result);
        SRF_EXPECT(again == bytes);

        srf::DurableState decoded{};
        srf::ValidationResult decode_result;
        SRF_EXPECT(srf::decode_durable_state(srf::as_bytes(bytes), harness.store().limits(),
                                             decoded, decode_result));
        SRF_EXPECT(decode_result.ok());
        SRF_EXPECT_EQ(decoded.lists.size(), original.lists.size());
        for (std::size_t li = 0; li < std::min(decoded.lists.size(), original.lists.size());
             ++li) {
            const srf::SegmentList& expected = original.lists[li];
            const srf::SegmentList& actual = decoded.lists[li];
            SRF_EXPECT_EQ(actual.content.id, expected.content.id);
            SRF_EXPECT_EQ(actual.generation.value(), expected.generation.value());
            SRF_EXPECT_EQ(actual.content_digest, expected.content_digest);
            SRF_EXPECT_EQ(actual.sequence_digest, expected.sequence_digest);
            SRF_EXPECT_EQ(static_cast<int>(actual.state), static_cast<int>(expected.state));
            SRF_EXPECT_EQ(actual.content.segments.size(), expected.content.segments.size());
            const std::size_t segments =
                std::min(actual.content.segments.size(), expected.content.segments.size());
            for (std::size_t si = 0; si < segments; ++si) {
                const srf::Segment& a = actual.content.segments[si];
                const srf::Segment& b = expected.content.segments[si];
                SRF_EXPECT_EQ(static_cast<int>(a.kind), static_cast<int>(b.kind));
                SRF_EXPECT_EQ(static_cast<int>(a.encoding), static_cast<int>(b.encoding));
                SRF_EXPECT_EQ(a.id, b.id);
                SRF_EXPECT_EQ(a.generation.value(), b.generation.value());
                SRF_EXPECT_EQ(a.node, b.node);
                SRF_EXPECT_EQ(a.adjacency, b.adjacency);
                SRF_EXPECT_EQ(a.endpoint, b.endpoint);
                SRF_EXPECT_EQ(a.binding, b.binding);
                SRF_EXPECT_EQ(a.policy, b.policy);
                SRF_EXPECT_EQ(a.topology.value(), b.topology.value());
                SRF_EXPECT(a.payload == b.payload);
            }
        }
        SRF_EXPECT_EQ(decoded.history.size(), original.history.size());
        SRF_EXPECT_EQ(decoded.attempts.size(), original.attempts.size());
    }
}

// ---------------------------------------------------------------------------
// 13. The index stays consistent: get() agrees with lists() for every stored id,
//     and disagrees with the store for every absent id.
// ---------------------------------------------------------------------------
SRF_TEST(property, indexes_remain_consistent) {
    const std::uint64_t seed = seed_for(13);
    srf::test::set_active_seed(seed);
    Random rng(seed);
    Harness harness;
    std::vector<std::uint64_t> known;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t idn = 1 + iteration;
        const std::vector<srf::Segment> segments = random_node_sequence(rng, 1 + rng.below(3));
        srf::MutationOutcome outcome = harness.create(idn, segments);
        SRF_EXPECT(outcome.ok());
        if (!outcome.ok()) {
            continue;
        }
        known.push_back(idn);
        if (rng.chance(35)) {
            const std::uint64_t victim = known[rng.below(static_cast<std::uint32_t>(known.size()))];
            const auto current = harness.store().get(srf::test::list_id(victim));
            SRF_EXPECT(current.has_value());
            if (current.has_value()) {
                srf::ListDraft draft = harness.draft(victim);
                draft.segments = random_node_sequence(rng, 1 + rng.below(3));
                draft.expected_generation = current->generation;
                const srf::MutationOutcome replaced =
                    harness.store().replace_list(harness.caller(), draft);
                SRF_EXPECT(replaced.ok());
            }
        }
        if (iteration % 16u == 0u) {
            const std::vector<srf::SegmentList> lists = harness.store().lists();
            SRF_EXPECT_EQ(lists.size(), harness.store().list_count());
            std::uint64_t segments_in_lists = 0;
            for (const srf::SegmentList& list : lists) {
                const auto by_id = harness.store().get(list.content.id);
                SRF_EXPECT(by_id.has_value());
                if (by_id.has_value()) {
                    SRF_EXPECT_EQ(by_id->generation.value(), list.generation.value());
                    SRF_EXPECT_EQ(by_id->content_digest, list.content_digest);
                    SRF_EXPECT_EQ(by_id->content.segments.size(),
                                  list.content.segments.size());
                }
                segments_in_lists += list.content.segments.size();
            }
            SRF_EXPECT_EQ(segments_in_lists, static_cast<std::uint64_t>(
                                                 harness.store().total_segments()));
            for (std::uint32_t probe = 0; probe < 8; ++probe) {
                const std::uint64_t absent = 100000 + rng.below(100000);
                SRF_EXPECT(!harness.store().get(srf::test::list_id(absent)).has_value());
            }
        }
    }
}
