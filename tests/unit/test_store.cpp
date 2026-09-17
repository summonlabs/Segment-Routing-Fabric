// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

using srf::test::Harness;
using srf::test::list_id;
using srf::test::node_segment;

namespace {

srf::ListDraft draft_with_attempt(Harness& harness, std::uint64_t list,
                                  std::vector<srf::Segment> segments) {
    srf::ListDraft draft = harness.draft(list);
    draft.segments = std::move(segments);
    return draft;
}

} // namespace

SRF_TEST(store, create_publishes_an_active_list_with_normalized_encoding) {
    Harness harness;
    const srf::MutationOutcome outcome = harness.create(1, {node_segment(1)});
    SRF_EXPECT(outcome.ok());
    SRF_EXPECT_EQ(static_cast<int>(outcome.status), static_cast<int>(srf::StatusCode::Ok));
    SRF_EXPECT_EQ(outcome.generation.value(), 1u);
    SRF_EXPECT(!outcome.digest.is_zero());
    SRF_EXPECT(outcome.durable);
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    SRF_EXPECT_EQ(static_cast<int>(list->state), static_cast<int>(srf::LifecycleState::Active));
    SRF_EXPECT_EQ(static_cast<int>(list->currentness), static_cast<int>(srf::Currentness::Current));
    SRF_EXPECT_EQ(list->content.segments.size(), static_cast<std::size_t>(1));
    SRF_EXPECT_EQ(static_cast<int>(list->content.segments[0].encoding),
                  static_cast<int>(srf::SegmentEncodingId::AbstractNodeV1));
    SRF_EXPECT_EQ(list->content.profile_generation.value(), 1u);
    SRF_EXPECT_EQ(list->content.capability.value(), srf::test::kCapabilityGeneration);
}

SRF_TEST(store, duplicate_create_is_rejected_and_absent_replace_is_not_found) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::MutationOutcome again = harness.create(1, {node_segment(1)});
    SRF_EXPECT_EQ(static_cast<int>(again.status), static_cast<int>(srf::StatusCode::Rejected));
    SRF_EXPECT_REASON(again.result, srf::ReasonCode::ListAlreadyExists);
    const srf::MutationOutcome missing = harness.replace(7, {node_segment(1)}, 1);
    SRF_EXPECT_EQ(static_cast<int>(missing.status), static_cast<int>(srf::StatusCode::NotFound));
    SRF_EXPECT_REASON(missing.result, srf::ReasonCode::ListNotFound);
}

SRF_TEST(store, exact_replay_advances_nothing_and_reuse_mismatch_rejects) {
    Harness harness;
    const srf::CallerIdentity caller = harness.caller();
    const srf::ListDraft draft = draft_with_attempt(harness, 1, {node_segment(1)});
    const srf::MutationOutcome first = harness.store().create_list(caller, draft);
    SRF_EXPECT(first.ok());
    SRF_EXPECT(!first.idempotent_replay);
    const std::uint64_t revision = harness.store().revision();
    const std::size_t saves = harness.memory()->save_count();

    const srf::MutationOutcome replay = harness.store().create_list(caller, draft);
    SRF_EXPECT(replay.ok());
    SRF_EXPECT(replay.idempotent_replay);
    SRF_EXPECT_EQ(replay.generation.value(), 1u);
    SRF_EXPECT_EQ(harness.store().revision(), revision);
    SRF_EXPECT_EQ(harness.memory()->save_count(), saves);

    srf::ListDraft different = draft;
    different.segments = {node_segment(2)};
    const srf::MutationOutcome mismatch = harness.store().create_list(caller, different);
    SRF_EXPECT_EQ(static_cast<int>(mismatch.status), static_cast<int>(srf::StatusCode::Conflict));
    SRF_EXPECT_REASON(mismatch.result, srf::ReasonCode::CommitReplayPayloadMismatch);
    SRF_EXPECT_EQ(harness.store().revision(), revision);
}

SRF_TEST(store, replacement_preserves_lineage_and_advances_the_generation) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::MutationOutcome second = harness.replace(1, {node_segment(1), node_segment(2)}, 1);
    SRF_EXPECT(second.ok());
    SRF_EXPECT_EQ(second.generation.value(), 2u);
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    SRF_EXPECT_EQ(list->lineage.supersedes.value(), 1u);
    SRF_EXPECT_EQ(list->generation.value(), 2u);

    const auto history = harness.store().history();
    bool saw_superseded = false;
    bool saw_active = false;
    for (const srf::DurableHistoryEntry& entry : history) {
        if (entry.list == list_id(1) && entry.state == srf::LifecycleState::Superseded) {
            saw_superseded = true;
        }
        if (entry.list == list_id(1) && entry.state == srf::LifecycleState::Active) {
            saw_active = true;
        }
    }
    SRF_EXPECT(saw_superseded);
    SRF_EXPECT(saw_active);

    const srf::MutationOutcome stale = harness.replace(1, {node_segment(1)}, 1);
    SRF_EXPECT_EQ(static_cast<int>(stale.status), static_cast<int>(srf::StatusCode::Conflict));
    SRF_EXPECT_REASON(stale.result, srf::ReasonCode::ListGenerationConflict);
}

SRF_TEST(store, generations_never_decrease) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    std::uint64_t previous = 1;
    for (std::uint64_t round = 2; round <= 12; ++round) {
        const srf::MutationOutcome outcome = harness.replace(1, {node_segment(1)}, previous);
        SRF_EXPECT(outcome.ok());
        SRF_EXPECT(outcome.generation.value() > previous);
        previous = outcome.generation.value();
    }
    SRF_EXPECT_EQ(previous, 12u);
}

SRF_TEST(store, segment_order_changes_the_digest) {
    Harness harness;
    const srf::MutationOutcome forward = harness.create(1, {node_segment(1), node_segment(2)});
    SRF_EXPECT(forward.ok());
    const srf::MutationOutcome reversed = harness.replace(1, {node_segment(2), node_segment(1)}, 1);
    SRF_EXPECT(reversed.ok());
    SRF_EXPECT(!(forward.digest == reversed.digest));
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    SRF_EXPECT_EQ(list->content.segments[0].id, srf::test::segment_id(2));
    SRF_EXPECT_EQ(list->content.segments[1].id, srf::test::segment_id(1));
}

SRF_TEST(store, withdrawal_revocation_and_retirement_are_deny_by_default) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::MutationOutcome withdrawing =
        harness.store().withdraw(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT(withdrawing.ok());
    auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    SRF_EXPECT_EQ(static_cast<int>(list->state),
                  static_cast<int>(srf::LifecycleState::Withdrawing));
    SRF_EXPECT(!srf::lifecycle_is_available(list->state));

    const srf::MutationOutcome committed =
        harness.store().withdraw_commit(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT(committed.ok());
    list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state), static_cast<int>(srf::LifecycleState::Withdrawn));
    SRF_EXPECT(!srf::lifecycle_is_available(list->state));

    // A withdrawn lineage can never be replaced back into ACTIVE.
    const srf::MutationOutcome replaced = harness.replace(1, {node_segment(1)}, 1);
    SRF_EXPECT_EQ(static_cast<int>(replaced.status), static_cast<int>(srf::StatusCode::Rejected));
    SRF_EXPECT_REASON(replaced.result, srf::ReasonCode::LifecycleTransitionDenied);

    SRF_EXPECT(harness.store()
                   .revoke(harness.caller(), list_id(1), srf::SegmentListGeneration{1})
                   .ok());
    SRF_EXPECT(harness.store()
                   .retire(harness.caller(), list_id(1), srf::SegmentListGeneration{1})
                   .ok());
    list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state), static_cast<int>(srf::LifecycleState::Retired));
    const srf::MutationOutcome after_retire = harness.replace(1, {node_segment(1)}, 1);
    SRF_EXPECT_REASON(after_retire.result, srf::ReasonCode::ListRetired);
    const srf::MutationOutcome revalidate_retired =
        harness.store().revalidate(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT_REASON(revalidate_retired.result, srf::ReasonCode::ListRetired);
}

SRF_TEST(store, supersession_across_lineages_closes_the_old_lineage) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    srf::ListDraft draft = harness.draft(2);
    draft.segments = {node_segment(1), node_segment(2)};
    draft.supersedes = list_id(1);
    draft.supersedes_generation = srf::SegmentListGeneration{1};
    const srf::MutationOutcome outcome = harness.store().create_list(harness.caller(), draft);
    SRF_EXPECT(outcome.ok());
    const auto old_list = harness.store().get(list_id(1));
    SRF_EXPECT(old_list.has_value());
    SRF_EXPECT_EQ(static_cast<int>(old_list->state),
                  static_cast<int>(srf::LifecycleState::Superseded));
    SRF_EXPECT_EQ(old_list->lineage.superseded_by, list_id(2));

    // A retired lineage can never be superseded.
    SRF_EXPECT(harness.store()
                   .retire(harness.caller(), list_id(1), srf::SegmentListGeneration{1})
                   .ok());
    srf::ListDraft third = harness.draft(3);
    third.segments = {node_segment(3)};
    third.supersedes = list_id(1);
    third.supersedes_generation = srf::SegmentListGeneration{1};
    const srf::MutationOutcome denied = harness.store().create_list(harness.caller(), third);
    SRF_EXPECT_EQ(static_cast<int>(denied.status), static_cast<int>(srf::StatusCode::Rejected));
    SRF_EXPECT_REASON(denied.result, srf::ReasonCode::ListRetired);

    srf::ListDraft unknown = harness.draft(4);
    unknown.segments = {node_segment(4)};
    unknown.supersedes = srf::test::list_id(99);
    unknown.supersedes_generation = srf::SegmentListGeneration{1};
    const srf::MutationOutcome missing = harness.store().create_list(harness.caller(), unknown);
    SRF_EXPECT_REASON(missing.result, srf::ReasonCode::ListSupersedesUnknown);
}

SRF_TEST(store, stale_epoch_and_fenced_boot_cannot_mutate) {
    Harness harness;
    const srf::MutationOutcome stale = harness.store().create_list(
        harness.stale_epoch_caller(), draft_with_attempt(harness, 1, {node_segment(1)}));
    SRF_EXPECT_EQ(static_cast<int>(stale.status), static_cast<int>(srf::StatusCode::Rejected));
    SRF_EXPECT_PRIMARY(stale.result, srf::ReasonCode::EpochStale);
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));

    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    SRF_EXPECT(harness.store().authority().mark_dead(srf::PublisherId{srf::test::kPublisher}).ok());
    const srf::MutationOutcome fenced = harness.store().create_list(
        harness.caller(), draft_with_attempt(harness, 2, {node_segment(2)}));
    SRF_EXPECT_EQ(static_cast<int>(fenced.status), static_cast<int>(srf::StatusCode::Rejected));
    SRF_EXPECT_REASON(fenced.result, srf::ReasonCode::CallerFencedPublisher);

    // A fresh boot with the same publisher identity is a stale writer.
    const srf::MutationOutcome stale_boot = harness.store().create_list(
        harness.fenced_caller(), draft_with_attempt(harness, 3, {node_segment(3)}));
    SRF_EXPECT_REASON(stale_boot.result, srf::ReasonCode::CallerStaleWorkerBoot);

    // A brand new publisher with a brand new boot succeeds.
    const srf::ValidationResult registered = harness.store().authority().register_publisher(
        srf::PublisherId{0x9100}, srf::WorkerBootId{0xB100}, srf::ScopeId{srf::test::kScope},
        harness.store().authority().epoch(), harness.store().limits());
    SRF_EXPECT(registered.ok());
    srf::CallerIdentity fresh{};
    fresh.epoch = harness.store().authority().epoch();
    fresh.publisher = srf::PublisherId{0x9100};
    fresh.boot = srf::WorkerBootId{0xB100};
    fresh.scope = srf::ScopeId{srf::test::kScope};
    fresh.attempt = srf::MutationAttemptId{0xE900};
    SRF_EXPECT(harness.store()
                   .create_list(fresh, draft_with_attempt(harness, 4, {node_segment(4)}))
                   .ok());
}

SRF_TEST(store, a_fenced_boot_can_never_be_reused) {
    Harness harness;
    SRF_EXPECT(harness.store().authority().fence_boot(srf::WorkerBootId{srf::test::kBoot}).ok());
    const srf::ValidationResult reused = harness.store().authority().register_publisher(
        srf::PublisherId{0x9999}, srf::WorkerBootId{srf::test::kBoot},
        srf::ScopeId{srf::test::kScope}, harness.store().authority().epoch(),
        harness.store().limits());
    SRF_EXPECT(reused.failed());
    SRF_EXPECT_PRIMARY(reused, srf::ReasonCode::BootAlreadyFenced);
}

SRF_TEST(store, a_mutation_is_never_acknowledged_before_it_is_durable) {
    Harness harness;
    const std::uint64_t revision = harness.store().revision();
    const std::size_t saves = harness.memory()->save_count();
    harness.memory()->fail_next_save();
    const srf::MutationOutcome outcome = harness.create(1, {node_segment(1)});
    SRF_EXPECT_EQ(static_cast<int>(outcome.status),
                  static_cast<int>(srf::StatusCode::PersistenceFailed));
    SRF_EXPECT(outcome.result.failed());
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));
    SRF_EXPECT_EQ(harness.store().revision(), revision);
    SRF_EXPECT_EQ(harness.memory()->save_count(), saves + 1);
    // The same mutation succeeds once persistence recovers.
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
}

SRF_TEST(store, stale_validation_result_never_becomes_current) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    harness.store().set_precommit_hook(
        [&harness] { harness.fabric().advance_capability_generation(); });
    const srf::MutationOutcome outcome = harness.replace(1, {node_segment(1), node_segment(2)}, 1);
    SRF_EXPECT_EQ(static_cast<int>(outcome.status), static_cast<int>(srf::StatusCode::Conflict));
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::CommitWatermarkChanged);
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    SRF_EXPECT_EQ(list->generation.value(), 1u);
    SRF_EXPECT_EQ(list->content.segments.size(), static_cast<std::size_t>(1));
    harness.store().set_precommit_hook(nullptr);
}

SRF_TEST(store, epoch_advance_before_commit_rejects_the_old_result) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    harness.store().set_precommit_hook(
        [&harness] { (void)harness.store().authority().advance_epoch(); });
    const srf::MutationOutcome outcome = harness.replace(1, {node_segment(1), node_segment(2)}, 1);
    SRF_EXPECT_EQ(static_cast<int>(outcome.status), static_cast<int>(srf::StatusCode::Rejected));
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::EpochStale);
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(list->generation.value(), 1u);
    harness.store().set_precommit_hook(nullptr);
}

SRF_TEST(store, profile_generation_advance_before_commit_is_detected) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    harness.store().set_precommit_hook([&harness] {
        (void)harness.store().registry().advance_profile_generation(
            srf::SegmentProfileId{0x0001'0001ull});
    });
    const srf::MutationOutcome outcome = harness.replace(1, {node_segment(1), node_segment(2)}, 1);
    SRF_EXPECT_EQ(static_cast<int>(outcome.status), static_cast<int>(srf::StatusCode::Conflict));
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::CommitWatermarkChanged);
    harness.store().set_precommit_hook(nullptr);
}

SRF_TEST(store, stale_dependency_marks_revalidation_required_and_rebinds) {
    Harness harness;
    srf::ListDraft draft = harness.draft(1);
    draft.topology = srf::TopologyGeneration{srf::test::kTopology};
    draft.segments = {srf::test::adjacency_segment(1)};
    SRF_EXPECT(harness.store().create_list(harness.caller(), draft).ok());
    auto list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(list->content.topology.value(), srf::test::kTopology);

    harness.fabric().advance_topology();
    const srf::CurrentnessReport report = harness.store().refresh_currentness();
    SRF_EXPECT_EQ(report.invalidated, static_cast<std::size_t>(1));
    list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state),
                  static_cast<int>(srf::LifecycleState::RevalidationRequired));
    SRF_EXPECT_EQ(static_cast<int>(list->currentness),
                  static_cast<int>(srf::Currentness::StaleTopology));

    // Refresh never restores availability on its own.
    SRF_EXPECT_EQ(harness.store().refresh_currentness().invalidated, static_cast<std::size_t>(0));
    list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state),
                  static_cast<int>(srf::LifecycleState::RevalidationRequired));

    // Revalidation is not repair: the bound adjacency generation no longer exists
    // in the authoritative topology, so revalidation rejects explicitly.
    const srf::MutationOutcome revalidated =
        harness.store().revalidate(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT(revalidated.result.failed());
    SRF_EXPECT_REASON(revalidated.result, srf::ReasonCode::TopologyAdjacencyStale);
    list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state),
                  static_cast<int>(srf::LifecycleState::RevalidationRequired));

    // Explicit re-derivation: the caller re-declares the adjacency against the
    // current topology generation. That is a replacement, never a silent repair.
    srf::ListDraft rebound = harness.draft(1);
    rebound.topology = srf::TopologyGeneration{2};
    rebound.segments = {srf::test::adjacency_segment(1, srf::TopologyGeneration{2})};
    rebound.expected_generation = srf::SegmentListGeneration{1};
    const srf::MutationOutcome replaced = harness.store().replace_list(harness.caller(), rebound);
    SRF_EXPECT_OK(replaced);
    list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state), static_cast<int>(srf::LifecycleState::Active));
    SRF_EXPECT_EQ(static_cast<int>(list->currentness), static_cast<int>(srf::Currentness::Current));
    SRF_EXPECT_EQ(list->content.topology.value(), 2u);
    SRF_EXPECT_EQ(list->content.segments[0].topology.value(), 2u);
    SRF_EXPECT_EQ(list->generation.value(), 2u);
    SRF_EXPECT_EQ(harness.store().refresh_currentness().invalidated, static_cast<std::size_t>(0));
}

SRF_TEST(store, capability_advance_only_invalidates_dependent_lists) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    // The nullable profile requires no capabilities and therefore binds none.
    srf::ListDraft independent = harness.draft(2, 0x0001'0003ull);
    independent.segments = {node_segment(2)};
    SRF_EXPECT(harness.store().create_list(harness.caller(), independent).ok());
    const auto before = harness.store().get(list_id(2));
    SRF_EXPECT(!before->content.capability.valid());

    harness.fabric().advance_capability_generation();
    const srf::CurrentnessReport report = harness.store().refresh_currentness();
    SRF_EXPECT_EQ(report.invalidated, static_cast<std::size_t>(1));
    const auto dependent = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(dependent->currentness),
                  static_cast<int>(srf::Currentness::StaleCapability));
    const auto other = harness.store().get(list_id(2));
    SRF_EXPECT_EQ(static_cast<int>(other->state), static_cast<int>(srf::LifecycleState::Active));
    SRF_EXPECT_EQ(static_cast<int>(other->currentness), static_cast<int>(srf::Currentness::Current));

    const srf::MutationOutcome revalidated =
        harness.store().revalidate(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT(revalidated.ok());
    SRF_EXPECT_EQ(harness.store().get(list_id(1))->content.capability.value(), 2u);
}

SRF_TEST(store, removed_adjacency_invalidates_currentness_without_a_topology_advance) {
    Harness harness;
    srf::ListDraft draft = harness.draft(1);
    draft.topology = srf::TopologyGeneration{srf::test::kTopology};
    draft.segments = {srf::test::adjacency_segment(1)};
    SRF_EXPECT(harness.store().create_list(harness.caller(), draft).ok());
    harness.fabric().remove_adjacency(1);
    const srf::CurrentnessReport report = harness.store().refresh_currentness();
    SRF_EXPECT_EQ(report.invalidated, static_cast<std::size_t>(1));
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->currentness),
                  static_cast<int>(srf::Currentness::StaleTopology));
    SRF_EXPECT_EQ(static_cast<int>(list->state),
                  static_cast<int>(srf::LifecycleState::RevalidationRequired));
    // Revalidation cannot repair a missing adjacency: it is an explicit rejection.
    const srf::MutationOutcome outcome =
        harness.store().revalidate(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT(outcome.result.failed());
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::TopologyAdjacencyUnknown);
    SRF_EXPECT_EQ(static_cast<int>(harness.store().get(list_id(1))->state),
                  static_cast<int>(srf::LifecycleState::RevalidationRequired));
}

SRF_TEST(store, retired_list_never_reactivates_through_any_path) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    SRF_EXPECT(harness.store()
                   .retire(harness.caller(), list_id(1), srf::SegmentListGeneration{1})
                   .ok());
    harness.fabric().advance_capability_generation();
    (void)harness.store().refresh_currentness();
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(list->state), static_cast<int>(srf::LifecycleState::Retired));
    for (int i = 0; i < 3; ++i) {
        SRF_EXPECT_EQ(harness.store()
                          .revalidate(harness.caller(), list_id(1),
                                      srf::SegmentListGeneration{1})
                          .status,
                      srf::StatusCode::Rejected);
        SRF_EXPECT_EQ(harness.store()
                          .withdraw(harness.caller(), list_id(1), srf::SegmentListGeneration{1})
                          .status,
                      srf::StatusCode::Rejected);
    }
    SRF_EXPECT_EQ(static_cast<int>(harness.store().get(list_id(1))->state),
                  static_cast<int>(srf::LifecycleState::Retired));
}

SRF_TEST(store, conservative_recovery_preserves_intent_but_not_authority) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    SRF_EXPECT(harness.create(2, {node_segment(2)}).ok());
    SRF_EXPECT(harness.store()
                   .retire(harness.caller(), list_id(2), srf::SegmentListGeneration{1})
                   .ok());
    const srf::DurableState durable = harness.store().export_state();
    const srf::CoordinatorEpoch epoch_before = harness.store().authority().epoch();

    // Restart: a brand new store over the same durable image.
    srf::StoreConfig config{};
    config.evidence = harness.fabric().evidence();
    config.persistence = harness.memory();
    srf::SegmentListStore restarted(std::move(config));
    const srf::LoadReport report = restarted.load();
    SRF_EXPECT(report.loaded);
    SRF_EXPECT_EQ(report.lists_recovered, static_cast<std::size_t>(2));
    SRF_EXPECT_EQ(restarted.list_count(), static_cast<std::size_t>(2));
    SRF_EXPECT(restarted.authority().epoch() > epoch_before);
    SRF_EXPECT(restarted.authority().publisher_count() == 0u ||
               !restarted.authority().is_alive(srf::PublisherId{srf::test::kPublisher}));

    const auto recovered = restarted.get(list_id(1));
    SRF_EXPECT(recovered.has_value());
    SRF_EXPECT_EQ(static_cast<int>(recovered->state),
                  static_cast<int>(srf::LifecycleState::RevalidationRequired));
    SRF_EXPECT_EQ(recovered->content.segments.size(), static_cast<std::size_t>(1));
    SRF_EXPECT_EQ(recovered->content_digest, durable.lists[0].content_digest);
    SRF_EXPECT_EQ(static_cast<int>(restarted.get(list_id(2))->state),
                  static_cast<int>(srf::LifecycleState::Retired));
    // Old authority cannot mutate the recovered state.
    const srf::MutationOutcome rejected = restarted.create_list(
        harness.caller(), draft_with_attempt(harness, 3, {node_segment(3)}));
    SRF_EXPECT(rejected.result.failed());
    SRF_EXPECT(rejected.result.contains(srf::ReasonCode::EpochStale) ||
               rejected.result.contains(srf::ReasonCode::CallerFencedPublisher));
}

SRF_TEST(store, index_stays_consistent_across_many_mutations) {
    Harness harness;
    for (std::uint64_t n = 1; n <= 20; ++n) {
        SRF_EXPECT(harness.create(n, {node_segment(n)}).ok());
    }
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(20));
    for (std::uint64_t n = 1; n <= 20; ++n) {
        const auto list = harness.store().get(list_id(n));
        SRF_EXPECT(list.has_value());
        if (!list.has_value()) {
            continue;
        }
        SRF_EXPECT_EQ(list->content.id, list_id(n));
        SRF_EXPECT_EQ(list->content.segments[0].id, srf::test::segment_id(n));
    }
    SRF_EXPECT_EQ(harness.store().total_segments(), static_cast<std::size_t>(20));
    SRF_EXPECT(!harness.store().get(srf::test::list_id(999)).has_value());
}

SRF_TEST(store, batch_is_atomic_across_all_drafts) {
    Harness harness;
    std::vector<srf::ListDraft> drafts;
    for (std::uint64_t n = 1; n <= 3; ++n) {
        srf::ListDraft draft = harness.draft(n);
        draft.segments = {node_segment(n)};
        drafts.push_back(draft);
    }
    drafts[2].segments = {node_segment(1), node_segment(1)};
    const std::uint64_t revision = harness.store().revision();
    const srf::MutationOutcome rejected =
        harness.store().batch(harness.caller(), std::span<const srf::ListDraft>(drafts));
    SRF_EXPECT(rejected.result.failed());
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));
    SRF_EXPECT_EQ(harness.store().revision(), revision);

    drafts[2].segments = {node_segment(3)};
    const srf::MutationOutcome accepted =
        harness.store().batch(harness.caller(), std::span<const srf::ListDraft>(drafts));
    SRF_EXPECT(accepted.ok());
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(3));
}
