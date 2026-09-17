// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Race suite: deterministic watermark races plus genuinely concurrent mutation
// races. Every value here is SYNTHETIC.
//
// (a) Deterministic watermark races. The store snapshots the evidence watermark
//     and the store state, validates outside its lock, and only then re-verifies
//     every generation and watermark before it commits. Each dependency that can
//     move underneath an in-flight mutation is moved deterministically - from the
//     store precommit hook or from the evidence observation hook - and the
//     mutation must be rejected with the documented reason code while the
//     revision that was current before the injection stays current, byte for
//     byte, with no additional persistence save.
//
// (b) Genuinely concurrent races. A worker thread advances evidence generations
//     (topology, capability, path authority) or registry generations (profile,
//     policy) while the main thread creates, replaces and revalidates lists. The
//     main thread publishes a counter of completed operations and the worker
//     advances exactly one generation per completed operation, so the overlap is
//     guaranteed rather than hoped for: every advance races the next mutation.
//     Only acknowledged mutations are asserted about: each one must have left the
//     store consistent, the store revision must never decrease, an ACTIVE list
//     may never carry a staleness the live evidence does not corroborate, and
//     after the threads are joined and currentness has been refreshed every
//     stored list is either (ACTIVE, CURRENT) or (REVALIDATION_REQUIRED, a stale
//     kind) - never an ACTIVE list holding a stale currentness.
//
// Nothing here is timing dependent. The deterministic proofs use the product's
// documented injection points; the concurrent proofs assert invariants that hold
// at every interleaving. There is no sleep anywhere: the threads are released
// together by a start barrier, the worker waits only by yielding, and every
// worker loop is bounded by a fixed number of advances.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "fixtures.hpp"

namespace {

using srf::test::Context;
using srf::test::Harness;

// ---------------------------------------------------------------------------
// segment helpers
// ---------------------------------------------------------------------------

/// The encoding the kind requires, spelled out so a draft segment can be compared
/// with a stored, already canonicalized segment.
[[nodiscard]] srf::Segment canonical(srf::Segment segment) {
    if (segment.encoding == srf::SegmentEncodingId::Invalid) {
        segment.encoding = srf::expected_encoding_for(segment.kind);
    }
    return segment;
}

[[nodiscard]] srf::Segment with_payload(srf::Segment segment, unsigned char byte) {
    segment.payload = srf::ByteBuffer{std::byte{byte}};
    return segment;
}

[[nodiscard]] srf::Segment listed_node(std::uint64_t number, unsigned char byte) {
    return canonical(with_payload(srf::test::node_segment(number), byte));
}

[[nodiscard]] srf::Segment listed_adjacency(std::uint64_t number, std::uint64_t topology,
                                            unsigned char byte) {
    return canonical(with_payload(
        srf::test::adjacency_segment(number, srf::TopologyGeneration{topology}), byte));
}

// ---------------------------------------------------------------------------
// (a) deterministic watermark races
// ---------------------------------------------------------------------------

/// The revision a rejected mutation must leave untouched: generation, content,
/// both digests and the persistence save count at the instant it was captured.
struct RevisionFingerprint {
    bool present{false};
    srf::SegmentList list{};
    std::size_t save_count{0};
};

[[nodiscard]] RevisionFingerprint fingerprint(const Harness& harness, srf::SegmentListId id) {
    RevisionFingerprint out{};
    out.save_count = harness.memory()->save_count();
    const std::optional<srf::SegmentList> current = harness.store().get(id);
    if (current.has_value()) {
        out.present = true;
        out.list = *current;
    }
    return out;
}

/// Moves one dependency from a documented hook point and proves the three things
/// a race proof owes: the in-flight mutation is rejected with the expected reason,
/// the revision that was current before the injection is still current byte for
/// byte, and the rejection persisted nothing.
///
/// The injection runs with no store lock held (that is exactly what the hook
/// point establishes), so an injection may itself perform a legitimate nested
/// mutation; the guard keeps the nested call from re-entering the injection.
void prove_injected_rejection(Context& srf_ctx, Harness& harness, const srf::ListDraft& draft,
                              srf::ReasonCode expected, bool inject_at_evidence_observation,
                              std::uint64_t reference_generation,
                              const std::optional<srf::ReasonCode>& also_expected,
                              const std::function<void(Harness&)>& inject) {
    RevisionFingerprint reference{};
    bool injected = false;
    const std::function<void()> hook = [&] {
        if (injected) {
            return;
        }
        injected = true;
        inject(harness);
        reference = fingerprint(harness, draft.id);
    };

    if (inject_at_evidence_observation) {
        harness.fabric().evidence()->set_observation_hook(hook);
    } else {
        harness.store().set_precommit_hook(hook);
    }

    const srf::MutationOutcome outcome = harness.store().replace_list(harness.caller(), draft);

    harness.store().set_precommit_hook(nullptr);
    harness.fabric().evidence()->set_observation_hook(nullptr);

    SRF_EXPECT(injected);
    SRF_EXPECT(!outcome.ok());
    SRF_EXPECT_EQ(outcome.list.value(), draft.id.value());
    SRF_EXPECT_REJECTED(outcome, expected);
    if (also_expected.has_value()) {
        SRF_EXPECT_REJECTED(outcome, *also_expected);
    }

    SRF_EXPECT(reference.present);
    if (!reference.present) {
        return;
    }
    SRF_EXPECT_EQ(reference.list.generation.value(), reference_generation);

    const RevisionFingerprint after = fingerprint(harness, draft.id);
    SRF_EXPECT(after.present);
    if (!after.present) {
        return;
    }
    // The revision that was current after the injection is still current: same
    // generation, same segment content, same canonical digests.
    SRF_EXPECT(after.list.content == reference.list.content);
    SRF_EXPECT(after.list.content.segments == reference.list.content.segments);
    SRF_EXPECT(after.list.content_digest == reference.list.content_digest);
    SRF_EXPECT(after.list.sequence_digest == reference.list.sequence_digest);
    SRF_EXPECT_EQ(after.list.generation.value(), reference.list.generation.value());
    SRF_EXPECT(after.list == reference.list);
    // The revision the rejected mutation would have created never became current.
    SRF_EXPECT_NE(after.list.generation.value(), reference.list.generation.value() + 1);
    if (!draft.segments.empty() && !after.list.content.segments.empty()) {
        SRF_EXPECT(after.list.content.segments.front().payload !=
                   draft.segments.front().payload);
    }
    // A rejected mutation is never persisted.
    SRF_EXPECT_EQ(after.save_count, reference.save_count);
}

/// Topology generation advance observed through the evidence observation hook:
/// the watermark is snapshotted, the snapshot is taken (and the topology moves
/// while the copy is being handed over), validation runs against the stale
/// snapshot, and the commit must notice that the evidence watermark moved.
SRF_TEST(race, topology_advance_between_snapshot_and_commit_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 1;

    const std::vector<srf::Segment> seeded{srf::test::adjacency_segment(1),
                                           srf::test::adjacency_segment(2)};
    SRF_EXPECT_OK(harness.create(kList, seeded));

    srf::ListDraft draft = harness.draft(kList);
    draft.topology = srf::TopologyGeneration{srf::test::kTopology};
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_adjacency(1, srf::test::kTopology, 0x2A),
                      listed_adjacency(2, srf::test::kTopology, 0x2A)};

    prove_injected_rejection(srf_ctx, harness, draft, srf::ReasonCode::CommitWatermarkChanged,
                             /*inject_at_evidence_observation=*/true, /*reference_generation=*/1,
                             std::nullopt, [](Harness& h) { h.fabric().advance_topology(); });
}

/// Capability generation advance observed the same way: the capability record
/// generations and the capability watermark move after the snapshot was taken.
SRF_TEST(race, capability_advance_between_snapshot_and_commit_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 2;
    SRF_EXPECT_OK(harness.create(kList, {srf::test::node_segment(1)}));

    srf::ListDraft draft = harness.draft(kList);
    draft.capability = srf::CapabilityGeneration{srf::test::kCapabilityGeneration};
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_node(1, 0x2A)};

    prove_injected_rejection(srf_ctx, harness, draft, srf::ReasonCode::CommitWatermarkChanged,
                             /*inject_at_evidence_observation=*/true, /*reference_generation=*/1,
                             std::nullopt,
                             [](Harness& h) { h.fabric().advance_capability_generation(); });
}

/// Path Authority advance injected at the store precommit hook: the list binds an
/// explicit path authority generation and the authority moves before the commit.
SRF_TEST(race, path_authority_advance_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 3;

    srf::ListDraft seeded = harness.draft(kList);
    seeded.path = srf::PathId{srf::test::kPath};
    seeded.path_authority = srf::PathAuthorityGeneration{srf::test::kPathAuthority};
    seeded.segments = {srf::test::node_segment(1)};
    SRF_EXPECT_OK(harness.store().create_list(harness.caller(), seeded));

    srf::ListDraft draft = harness.draft(kList);
    draft.path = seeded.path;
    draft.path_authority = seeded.path_authority;
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_node(1, 0x2A)};

    prove_injected_rejection(srf_ctx, harness, draft, srf::ReasonCode::CommitWatermarkChanged,
                             /*inject_at_evidence_observation=*/false, /*reference_generation=*/1,
                             std::nullopt, [](Harness& h) { h.fabric().advance_path_authority(); });
}

/// Profile generation advance: the registry revision moves after the profile was
/// resolved for this validation.
SRF_TEST(race, profile_generation_advance_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 4;
    SRF_EXPECT_OK(harness.create(kList, {srf::test::node_segment(1)}));

    srf::ValidationResult advance{};
    srf::ListDraft draft = harness.draft(kList);
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_node(1, 0x2A)};

    prove_injected_rejection(srf_ctx, harness, draft, srf::ReasonCode::CommitWatermarkChanged,
                             /*inject_at_evidence_observation=*/false, /*reference_generation=*/1,
                             std::nullopt, [&advance](Harness& h) {
                                 advance = h.store().registry().advance_profile_generation(
                                     srf::SegmentProfileId{0x0001'0001ull});
                             });
    SRF_EXPECT(advance.ok());
}

/// Policy generation advance: the registry revision moves after the policy was
/// resolved for this validation.
SRF_TEST(race, policy_generation_advance_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 5;

    srf::SegmentPolicy policy{};
    policy.id = srf::test::policy_id(1);
    policy.generation = srf::SegmentPolicyGeneration{1};
    policy.owner_scope = srf::ScopeId{srf::test::kScope};
    policy.allow_replacement = true;
    SRF_EXPECT(harness.store().register_policy(policy).ok());

    srf::ListDraft seeded = harness.draft(kList);
    seeded.policy = policy.id;
    seeded.segments = {srf::test::node_segment(1)};
    SRF_EXPECT_OK(harness.store().create_list(harness.caller(), seeded));

    srf::ValidationResult advance{};
    srf::ListDraft draft = harness.draft(kList);
    draft.policy = policy.id;
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_node(1, 0x2A)};

    prove_injected_rejection(srf_ctx, harness, draft, srf::ReasonCode::CommitWatermarkChanged,
                             /*inject_at_evidence_observation=*/false, /*reference_generation=*/1,
                             std::nullopt, [&advance, &policy](Harness& h) {
                                 advance = h.store().registry().advance_policy_generation(policy.id);
                             });
    SRF_EXPECT(advance.ok());
}

/// Epoch advance: the coordinator epoch moves after the caller identity and the
/// commit watermark were captured for this validation.
SRF_TEST(race, epoch_advance_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 6;
    SRF_EXPECT_OK(harness.create(kList, {srf::test::node_segment(1)}));

    bool advanced = false;
    srf::ListDraft draft = harness.draft(kList);
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_node(1, 0x2A)};

    prove_injected_rejection(srf_ctx, harness, draft, srf::ReasonCode::EpochStale,
                             /*inject_at_evidence_observation=*/false, /*reference_generation=*/1,
                             std::nullopt, [&advanced](Harness& h) {
                                 advanced = h.store().authority().advance_epoch();
                             });
    SRF_EXPECT(advanced);
}

/// Target-list generation advance: the injection performs a legitimate nested
/// replacement of the same lineage while the outer replacement is in flight, so
/// the lineage generation the outer mutation prepared against has moved by the
/// time the outer commit re-verifies. The nested call re-enters run_batch and
/// calls the hook again; the guard makes the injection happen exactly once.
SRF_TEST(race, target_list_generation_advance_is_rejected) {
    Harness harness;
    constexpr std::uint64_t kList = 7;
    SRF_EXPECT_OK(harness.create(kList, {srf::test::node_segment(1)}));

    srf::MutationOutcome nested{};
    srf::ListDraft draft = harness.draft(kList);
    draft.expected_generation = srf::SegmentListGeneration{1};
    draft.segments = {listed_node(1, 0x2A)};

    prove_injected_rejection(
        srf_ctx, harness, draft, srf::ReasonCode::ListGenerationConflict,
        /*inject_at_evidence_observation=*/false, /*reference_generation=*/2,
        srf::ReasonCode::CommitWatermarkChanged, [&nested, kList](Harness& h) {
            srf::ListDraft inner = h.draft(kList);
            inner.expected_generation = srf::SegmentListGeneration{1};
            inner.segments = {listed_node(1, 0x05)};
            nested = h.store().replace_list(h.caller(), inner);
        });

    SRF_EXPECT_OK(nested);
    SRF_EXPECT_EQ(nested.generation.value(), 2);
}

// ---------------------------------------------------------------------------
// (b) genuinely concurrent races
// ---------------------------------------------------------------------------

enum class ConcurrentWorker {
    Evidence,
    Registry,
};

struct RaceTally {
    std::size_t acknowledged{0};
    std::size_t rejected{0};
    std::size_t watermark_conflicts{0};
    std::size_t profile_generation_mismatches{0};
    std::size_t list_generation_conflicts{0};
};

/// Releases the worker on every exit path and joins it, so no test can return
/// with a joinable thread and no test can wait on a worker that is still waiting
/// for main-thread progress it will never see.
class ThreadJoiner {
public:
    ThreadJoiner(std::thread& worker, std::atomic<bool>& done) noexcept
        : worker_(worker), done_(done) {}
    ~ThreadJoiner() {
        done_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::thread& worker_;
    std::atomic<bool>& done_;
};

[[nodiscard]] std::size_t evidence_record_count(const srf::FabricEvidence& evidence) {
    return evidence.capabilities.size() + evidence.segments.size() + evidence.nodes.size() +
           evidence.adjacencies.size() + evidence.endpoints.size() + evidence.bindings.size() +
           evidence.paths.size() + evidence.routes.size() + evidence.constraints.size();
}

/// An ACTIVE list may only be stale when the dependency generation it bound has
/// since moved: the store reports staleness, the live evidence must corroborate
/// it. The check is written against the reason the store would have recorded, so
/// it stays valid while the worker keeps advancing dependencies.
void check_active_staleness_explained(Context& srf_ctx, const Harness& harness,
                                      const srf::SegmentList& list,
                                      const srf::FabricEvidence& evidence,
                                      srf::CoordinatorEpoch epoch) {
    if (list.state != srf::LifecycleState::Active ||
        list.currentness == srf::Currentness::Current) {
        return;
    }
    switch (list.currentness) {
        case srf::Currentness::StaleProfile: {
            srf::SegmentProfile profile{};
            const bool found =
                harness.store().registry().find_profile(list.content.profile, profile);
            SRF_EXPECT(!found || profile.generation != list.content.profile_generation);
            return;
        }
        case srf::Currentness::StalePolicy: {
            srf::SegmentPolicy policy{};
            const bool found =
                list.content.policy.valid() &&
                harness.store().registry().find_policy(list.content.policy, policy);
            SRF_EXPECT(!found || policy.generation != list.content.policy_generation);
            return;
        }
        case srf::Currentness::StaleCapability: {
            bool moved = list.content.capability.valid() &&
                         list.content.capability != evidence.capability;
            srf::SegmentProfile profile{};
            if (harness.store().registry().find_profile(list.content.profile, profile)) {
                for (const srf::CapabilityKey key : profile.required_capabilities) {
                    const srf::CapabilityRecord* record = evidence.find_capability(key);
                    if (record == nullptr || !record->supported ||
                        record->generation != evidence.capability) {
                        moved = true;
                    }
                }
            }
            SRF_EXPECT(moved);
            return;
        }
        case srf::Currentness::StaleTopology: {
            bool moved =
                list.content.topology.valid() && list.content.topology != evidence.topology;
            for (const srf::Segment& segment : list.content.segments) {
                if (segment.kind != srf::SegmentKind::Adjacency) {
                    continue;
                }
                const srf::AdjacencyRecord* record = evidence.find_adjacency(segment.adjacency);
                if (record == nullptr || record->topology != segment.topology) {
                    moved = true;
                }
            }
            SRF_EXPECT(moved);
            return;
        }
        case srf::Currentness::StalePathAuthority: {
            const srf::PathRecord* record = evidence.find_path(list.content.path);
            SRF_EXPECT(record == nullptr || record->authority != list.content.path_authority);
            return;
        }
        case srf::Currentness::StaleRouteBinding: {
            const srf::RouteRecord* record = evidence.find_route(list.content.route);
            SRF_EXPECT(record == nullptr || record->generation != list.content.route_generation);
            return;
        }
        case srf::Currentness::StaleConstraintBinding: {
            const srf::ConstraintRecord* record = evidence.find_constraint(list.content.constraint);
            SRF_EXPECT(record == nullptr ||
                       record->generation != list.content.constraint_generation);
            return;
        }
        case srf::Currentness::StaleEpoch:
            SRF_EXPECT(list.authority.epoch.valid() && epoch.valid() &&
                       list.authority.epoch != epoch);
            return;
        case srf::Currentness::FencedPublisher:
            SRF_EXPECT(harness.store().authority().is_fenced(list.authority.boot));
            return;
        case srf::Currentness::Current:
        case srf::Currentness::Unknown:
        case srf::Currentness::RevalidationRequired:
        default:
            // ACTIVE with an unclassified currentness is never explained by a
            // dependency movement.
            SRF_EXPECT(false);
            return;
    }
}

/// Every acknowledged mutation must have left the store consistent: the index
/// agrees with get() for every stored id, list_count() agrees with lists(), and
/// the acknowledged generation and digest are the stored revision.
void check_acknowledged_mutation(Context& srf_ctx, const Harness& harness,
                                 const srf::MutationOutcome& outcome) {
    const std::optional<srf::SegmentList> stored = harness.store().get(outcome.list);
    SRF_EXPECT(stored.has_value());
    if (!stored.has_value()) {
        return;
    }
    SRF_EXPECT(stored->generation == outcome.generation);
    SRF_EXPECT_EQ(stored->generation.value(), outcome.generation.value());
    SRF_EXPECT(stored->content_digest == outcome.digest);

    const std::vector<srf::SegmentList> lists = harness.store().lists();
    SRF_EXPECT_EQ(harness.store().list_count(), lists.size());
    for (const srf::SegmentList& list : lists) {
        const std::optional<srf::SegmentList> one = harness.store().get(list.content.id);
        SRF_EXPECT(one.has_value());
        if (one.has_value()) {
            SRF_EXPECT(one->generation == list.generation);
            SRF_EXPECT(one->content == list.content);
            SRF_EXPECT(one->content_digest == list.content_digest);
        }
    }
}

/// The settled shape of the store once no writer is running: every stored list is
/// either (ACTIVE, CURRENT) or (REVALIDATION_REQUIRED, a stale kind). No ACTIVE
/// list may be stale, and no stored list may be UNKNOWN.
void check_settled_lists(Context& srf_ctx, const Harness& harness,
                         const srf::FabricEvidence& evidence, srf::CoordinatorEpoch epoch) {
    const std::vector<srf::SegmentList> lists = harness.store().lists();
    SRF_EXPECT(!lists.empty());
    SRF_EXPECT_EQ(harness.store().list_count(), lists.size());
    for (const srf::SegmentList& list : lists) {
        const std::optional<srf::SegmentList> stored = harness.store().get(list.content.id);
        SRF_EXPECT(stored.has_value());
        if (stored.has_value()) {
            SRF_EXPECT(stored->generation == list.generation);
            SRF_EXPECT(stored->content == list.content);
            SRF_EXPECT(stored->content_digest == list.content_digest);
        }
        check_active_staleness_explained(srf_ctx, harness, list, evidence, epoch);
        if (list.state == srf::LifecycleState::Active) {
            SRF_EXPECT(list.currentness == srf::Currentness::Current);
        } else {
            SRF_EXPECT(list.state == srf::LifecycleState::RevalidationRequired);
            SRF_EXPECT(list.currentness != srf::Currentness::Current);
            SRF_EXPECT(list.currentness != srf::Currentness::Unknown);
        }
    }
}

void run_concurrent_race(Context& srf_ctx, ConcurrentWorker kind) {
    constexpr std::size_t kWorkerAdvanceCap = 160;
    constexpr std::size_t kOperations = 96;
    constexpr std::size_t kSeedLists = 4;
    constexpr std::uint64_t kSeedBase = 0x40;
    constexpr std::uint64_t kCreateBase = 0x100;
    constexpr std::uint64_t kSettledList = 0x300;

    Harness harness;

    // The registry race needs a registered policy so that a policy generation
    // advance is one of the dependencies that moves underneath a mutation.
    srf::SegmentPolicy policy{};
    const bool with_policy = kind == ConcurrentWorker::Registry;
    if (with_policy) {
        policy.id = srf::test::policy_id(9);
        policy.generation = srf::SegmentPolicyGeneration{1};
        policy.owner_scope = srf::ScopeId{srf::test::kScope};
        policy.allow_replacement = true;
        SRF_EXPECT(harness.store().register_policy(policy).ok());
    }

    const std::size_t initial_records = evidence_record_count(harness.fabric().evidence()->snapshot());
    SRF_EXPECT(initial_records > 0);
    SRF_EXPECT(initial_records < static_cast<std::size_t>(srf::Limits{}.max_evidence_records));

    const auto make_draft = [&harness, &policy, with_policy](std::uint64_t number, bool adjacency,
                                                             unsigned char byte) {
        srf::ListDraft draft = harness.draft(number);
        if (with_policy) {
            draft.policy = policy.id;
            // Bind the profile generation this caller just observed. A registry
            // move between the observation and the validation is then a real
            // ProfileGenerationMismatch rather than a silently re-bound generation.
            srf::SegmentProfile observed{};
            if (harness.store().registry().find_profile(draft.profile, observed)) {
                draft.profile_generation = observed.generation;
            }
        }
        if (adjacency) {
            const std::uint64_t topology =
                harness.fabric().evidence()->watermark().topology.value();
            draft.topology = srf::TopologyGeneration{topology};
            draft.segments = {listed_adjacency(1, topology, byte),
                              listed_adjacency(2, topology, byte)};
        } else {
            draft.segments = {listed_node(1, byte)};
        }
        return draft;
    };

    // Seed revisions that the race replaces and revalidates.
    for (std::size_t i = 0; i < kSeedLists; ++i) {
        SRF_EXPECT_OK(harness.store().create_list(harness.caller(),
                                                  make_draft(kSeedBase + i, false,
                                                             static_cast<unsigned char>(0x10 + i))));
    }

    std::promise<void> gate;
    const std::shared_future<void> start = gate.get_future().share();
    std::atomic<bool> main_done{false};
    std::atomic<bool> revision_regressed{false};
    std::atomic<bool> worker_step_failed{false};
    std::atomic<std::size_t> worker_advances{0};
    std::atomic<std::uint64_t> main_ops{0};
    const srf::SegmentProfileId profile_id{0x0001'0001ull};

    std::thread worker([&] {
        start.wait();
        const auto advance_one_generation = [&](std::size_t index) {
            if (kind == ConcurrentWorker::Evidence) {
                switch (index % 3u) {
                    case 0u:
                        harness.fabric().advance_topology();
                        break;
                    case 1u:
                        harness.fabric().advance_capability_generation();
                        break;
                    default:
                        harness.fabric().advance_path_authority();
                        break;
                }
                return;
            }
            const srf::ValidationResult advanced =
                index % 2u == 0u
                    ? harness.store().registry().advance_profile_generation(profile_id)
                    : harness.store().registry().advance_policy_generation(policy.id);
            if (!advanced.ok()) {
                worker_step_failed.store(true);
            }
        };

        // Progress gate: one advance per main-thread operation that has already
        // completed, so every advance races the next mutation instead of being
        // consumed before the first mutation begins. The loop is bounded by a
        // fixed advance cap, waits only by yielding (never by sleeping), and the
        // joiner raises the stop flag on every exit path.
        std::size_t advances = 0;
        std::uint64_t last_seen = 0;
        std::uint64_t last_revision = harness.store().revision();
        while (!main_done.load(std::memory_order_relaxed) && advances < kWorkerAdvanceCap) {
            const std::uint64_t seen = main_ops.load(std::memory_order_relaxed);
            if (seen != last_seen) {
                last_seen = seen;
                advance_one_generation(advances);
                ++advances;
                const std::uint64_t now = harness.store().revision();
                if (now < last_revision) {
                    revision_regressed.store(true);
                }
                last_revision = now;
            }
            std::this_thread::yield();
        }
        worker_advances.store(advances);
    });
    const ThreadJoiner joiner(worker, main_done);

    // ---- start barrier: both threads are released together. No sleep anywhere.
    gate.set_value();
    start.wait();

    RaceTally tally{};
    std::uint64_t observed_revision = harness.store().revision();
    for (std::size_t i = 0; i < kOperations; ++i) {
        const unsigned char byte = static_cast<unsigned char>(0x20u + (i % 0x40u));
        const std::size_t lane = i % 4u;
        srf::MutationOutcome outcome{};
        bool attempted = false;
        if (lane == 0u || lane == 2u) {
            // A fresh lineage, so the create can never collide with a predecessor.
            outcome = harness.store().create_list(harness.caller(),
                                                  make_draft(kCreateBase + i, lane == 0u, byte));
            attempted = true;
        } else {
            const std::uint64_t number = kSeedBase + (i % kSeedLists);
            const srf::SegmentListId id = srf::test::list_id(number);
            const std::optional<srf::SegmentList> current = harness.store().get(id);
            SRF_EXPECT(current.has_value());
            if (current.has_value()) {
                if (lane == 1u) {
                    srf::ListDraft draft = make_draft(number, false, byte);
                    draft.expected_generation = current->generation;
                    outcome = harness.store().replace_list(harness.caller(), draft);
                } else {
                    outcome = harness.store().revalidate(harness.caller(), id, current->generation);
                }
                attempted = true;
            }
        }

        if (attempted) {
            const std::uint64_t now = harness.store().revision();
            SRF_EXPECT(now >= observed_revision);
            observed_revision = now;

            if (outcome.ok()) {
                ++tally.acknowledged;
                check_acknowledged_mutation(srf_ctx, harness, outcome);
            } else {
                ++tally.rejected;
                if (outcome.result.contains(srf::ReasonCode::CommitWatermarkChanged)) {
                    ++tally.watermark_conflicts;
                }
                if (outcome.result.contains(srf::ReasonCode::ProfileGenerationMismatch)) {
                    ++tally.profile_generation_mismatches;
                }
                if (outcome.result.contains(srf::ReasonCode::ListGenerationConflict)) {
                    ++tally.list_generation_conflicts;
                }
            }
        }

        if (i % 8u == 7u) {
            const srf::FabricEvidence evidence = harness.fabric().evidence()->snapshot();
            const srf::CoordinatorEpoch epoch = harness.store().authority().epoch();
            for (const srf::SegmentList& list : harness.store().lists()) {
                check_active_staleness_explained(srf_ctx, harness, list, evidence, epoch);
            }
        }
        if (i % 16u == 15u) {
            (void)harness.store().refresh_currentness();
        }
        // The gate the worker counts: the mutation and its invariant checks are
        // complete, so the worker's next advance provably races the next mutation.
        main_ops.fetch_add(1, std::memory_order_relaxed);
    }

    main_done.store(true);
    worker.join();

    SRF_EXPECT(!revision_regressed.load());
    SRF_EXPECT(!worker_step_failed.load());
    SRF_EXPECT(worker_advances.load() > 0);
    SRF_EXPECT_EQ(tally.acknowledged + tally.rejected, kOperations);
    SRF_EXPECT(tally.acknowledged > 0);

    // ---- the threads are joined: refresh, then assert the settled shape.
    (void)harness.store().refresh_currentness();
    const srf::FabricEvidence settled_evidence = harness.fabric().evidence()->snapshot();
    const srf::CoordinatorEpoch settled_epoch = harness.store().authority().epoch();
    check_settled_lists(srf_ctx, harness, settled_evidence, settled_epoch);

    // Deterministic tail: with no worker running these are acknowledged, and an
    // acknowledged mutation must leave the store consistent.
    const srf::MutationOutcome settled_create = harness.store().create_list(
        harness.caller(), make_draft(kSettledList, false, 0x77));
    SRF_EXPECT_OK(settled_create);
    if (settled_create.ok()) {
        check_acknowledged_mutation(srf_ctx, harness, settled_create);
        const std::optional<srf::SegmentList> created = harness.store().get(settled_create.list);
        SRF_EXPECT(created.has_value());
        if (created.has_value()) {
            srf::ListDraft draft = make_draft(kSettledList, false, 0x78);
            draft.expected_generation = created->generation;
            const srf::MutationOutcome settled_replace =
                harness.store().replace_list(harness.caller(), draft);
            SRF_EXPECT_OK(settled_replace);
            if (settled_replace.ok()) {
                SRF_EXPECT_EQ(settled_replace.generation.value(), 2);
                check_acknowledged_mutation(srf_ctx, harness, settled_replace);
            }
        }
    }

    // The worker's advances only ever replace an existing evidence record; the
    // synthetic record count must be exactly what it was before the race, and it
    // stays far below the configured evidence bound.
    const std::size_t final_records = evidence_record_count(harness.fabric().evidence()->snapshot());
    SRF_EXPECT_EQ(final_records, initial_records);
    SRF_EXPECT(final_records < static_cast<std::size_t>(srf::Limits{}.max_evidence_records));

    std::printf("    %s worker: %zu advances, %zu acknowledged, %zu rejected "
                "(%zu watermark, %zu profile generation, %zu list generation)\n",
                kind == ConcurrentWorker::Evidence ? "evidence" : "registry",
                worker_advances.load(), tally.acknowledged, tally.rejected,
                tally.watermark_conflicts, tally.profile_generation_mismatches,
                tally.list_generation_conflicts);
    std::fflush(stdout);
}

/// Evidence worker: topology, capability and Path Authority generations advance
/// while the main thread creates, replaces and revalidates lists.
SRF_TEST(race, concurrent_evidence_advances_race_acknowledged_mutations) {
    run_concurrent_race(srf_ctx, ConcurrentWorker::Evidence);
}

/// Registry worker: profile and policy generations advance while the main thread
/// creates, replaces and revalidates lists.
SRF_TEST(race, concurrent_registry_advances_race_acknowledged_mutations) {
    run_concurrent_race(srf_ctx, ConcurrentWorker::Registry);
}

} // namespace
