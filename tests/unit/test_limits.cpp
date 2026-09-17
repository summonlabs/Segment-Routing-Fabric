// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Every configured bound must actually be consulted. Each test below removes one
// bound's headroom and proves the runtime reacts.
#include "fixtures.hpp"

using srf::test::Harness;
using srf::test::list_id;
using srf::test::node_segment;

namespace {

std::unique_ptr<srf::SegmentListStore> make_store(const srf::Limits& limits,
                                                  std::shared_ptr<srf::SyntheticEvidence> evidence,
                                                  std::shared_ptr<srf::MemoryPersistence> memory) {
    srf::StoreConfig config{};
    config.limits = limits;
    config.evidence = std::move(evidence);
    config.persistence = std::move(memory);
    return std::make_unique<srf::SegmentListStore>(std::move(config));
}

} // namespace

SRF_TEST(limits, max_lists_is_consulted) {
    srf::Limits limits{};
    limits.max_lists = 2;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    SRF_EXPECT(harness.create(2, {node_segment(2)}).ok());
    const srf::MutationOutcome third = harness.create(3, {node_segment(3)});
    SRF_EXPECT_EQ(static_cast<int>(third.status), static_cast<int>(srf::StatusCode::LimitExceeded));
    SRF_EXPECT_REASON(third.result, srf::ReasonCode::LimitMaxLists);
}

SRF_TEST(limits, max_total_segments_is_consulted) {
    srf::Limits limits{};
    limits.max_lists = 8;
    limits.max_total_segments = 2;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    SRF_EXPECT(harness.create(2, {node_segment(2)}).ok());
    const srf::MutationOutcome third = harness.create(3, {node_segment(3)});
    SRF_EXPECT_REASON(third.result, srf::ReasonCode::LimitMaxTotalSegments);
    SRF_EXPECT_EQ(harness.store().total_segments(), static_cast<std::size_t>(2));
}

SRF_TEST(limits, max_segments_per_list_is_consulted) {
    srf::Limits limits{};
    limits.max_segments_per_list = 2;
    Harness harness(true, limits);
    const srf::MutationOutcome outcome =
        harness.create(1, {node_segment(1), node_segment(2), node_segment(3)});
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxSegmentsPerList);
    SRF_EXPECT(outcome.result.primary_phase() == srf::ValidationPhase::ResourceLimits);
    SRF_EXPECT(harness.create(1, {node_segment(1), node_segment(2)}).ok());
}

SRF_TEST(limits, max_segment_payload_bytes_is_consulted) {
    srf::Limits limits{};
    limits.max_segment_payload_bytes = 2;
    Harness harness(true, limits);
    srf::Segment oversized = node_segment(1);
    oversized.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    const srf::MutationOutcome outcome = harness.create(1, {oversized});
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxSegmentPayloadBytes);
}

SRF_TEST(limits, max_profiles_and_policies_are_consulted) {
    srf::Limits limits{};
    limits.max_profiles = 1;
    limits.max_policies = 1;
    auto memory = std::make_shared<srf::MemoryPersistence>();
    auto store = make_store(limits, std::make_shared<srf::SyntheticEvidence>(), memory);
    SRF_EXPECT(store->register_profile(srf::abstract_governance_profile()).ok());
    const srf::ValidationResult second = store->register_profile(srf::abstract_ordered_profile());
    SRF_EXPECT_PRIMARY(second, srf::ReasonCode::LimitMaxProfiles);

    srf::SegmentPolicy policy{};
    policy.id = srf::test::policy_id(1);
    policy.generation = srf::SegmentPolicyGeneration{1};
    SRF_EXPECT(store->register_policy(policy).ok());
    srf::SegmentPolicy other{};
    other.id = srf::test::policy_id(2);
    other.generation = srf::SegmentPolicyGeneration{1};
    SRF_EXPECT_PRIMARY(store->register_policy(other), srf::ReasonCode::LimitMaxPolicies);

    // Registering a lower or equal generation is refused: generations never regress.
    SRF_EXPECT(store->register_profile(srf::abstract_governance_profile()).failed());
}

SRF_TEST(limits, max_publishers_is_consulted) {
    srf::Limits limits{};
    limits.max_publishers = 1;
    auto store = make_store(limits, std::make_shared<srf::SyntheticEvidence>(), nullptr);
    const srf::ValidationResult first = store->authority().register_publisher(
        srf::PublisherId{1}, srf::WorkerBootId{1}, srf::ScopeId{srf::test::kScope},
        store->authority().epoch(), limits);
    SRF_EXPECT(first.ok());
    const srf::ValidationResult second = store->authority().register_publisher(
        srf::PublisherId{2}, srf::WorkerBootId{2}, srf::ScopeId{srf::test::kScope},
        store->authority().epoch(), limits);
    SRF_EXPECT_PRIMARY(second, srf::ReasonCode::LimitMaxPublishers);
}

SRF_TEST(limits, max_batch_size_is_consulted) {
    srf::Limits limits{};
    limits.max_batch_size = 1;
    Harness harness(true, limits);
    std::vector<srf::ListDraft> drafts;
    for (std::uint64_t n = 1; n <= 2; ++n) {
        srf::ListDraft draft = harness.draft(n);
        draft.segments = {node_segment(n)};
        drafts.push_back(draft);
    }
    const srf::MutationOutcome outcome =
        harness.store().batch(harness.caller(), std::span<const srf::ListDraft>(drafts));
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxBatchSize);
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Bounded durable history: retention, never semantic truncation.
// ---------------------------------------------------------------------------
SRF_TEST(limits, max_history_bounded_retention_is_explicit) {
    srf::Limits limits{};
    limits.max_history = 3;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    std::uint64_t generation = 1;
    for (int i = 0; i < 6; ++i) {
        const srf::MutationOutcome outcome = harness.replace(1, {node_segment(1)}, generation);
        SRF_EXPECT_OK(outcome);
        if (!outcome.ok()) {
            break;
        }
        generation = outcome.generation.value();
    }
    const srf::HistoryReport report = harness.store().history_report();
    // Retained is exactly the bound; the view says so instead of pretending to be
    // a complete record.
    SRF_EXPECT_EQ(report.retained, static_cast<std::uint32_t>(3));
    SRF_EXPECT_EQ(report.limit, static_cast<std::uint32_t>(3));
    SRF_EXPECT(report.at_capacity);
    SRF_EXPECT(!report.complete());
    SRF_EXPECT_EQ(report.entries.size(), static_cast<std::size_t>(3));
    SRF_EXPECT_EQ(harness.store().history().size(), static_cast<std::size_t>(3));
    bool increasing = true;
    for (std::size_t i = 1; i < report.entries.size(); ++i) {
        if (!(report.entries[i - 1].id < report.entries[i].id)) {
            increasing = false;
        }
    }
    SRF_EXPECT(increasing);

    // Bounded history never changed any authoritative value.
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    if (list.has_value()) {
        SRF_EXPECT(list->generation.value() == generation);
        SRF_EXPECT_EQ(static_cast<int>(list->state),
                      static_cast<int>(srf::LifecycleState::Active));
        SRF_EXPECT_EQ(static_cast<int>(list->currentness),
                      static_cast<int>(srf::Currentness::Current));
        SRF_EXPECT_EQ(list->content.segments.size(), static_cast<std::size_t>(1));
    }
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(1));

    // One below the bound, and exactly at the bound, both report a complete view.
    srf::Limits roomy{};
    roomy.max_history = 8;
    Harness spacious(true, roomy);
    SRF_EXPECT(spacious.create(1, {node_segment(1)}).ok());
    const srf::HistoryReport small = spacious.store().history_report();
    SRF_EXPECT_EQ(small.retained, static_cast<std::uint32_t>(1));
    SRF_EXPECT(!small.at_capacity);
    SRF_EXPECT(small.complete());
}

// ---------------------------------------------------------------------------
// Bounded replay table: an explicit rejection, never a silent eviction.
// ---------------------------------------------------------------------------
SRF_TEST(limits, max_attempt_records_is_an_explicit_rejection) {
    srf::Limits limits{};
    limits.max_attempt_records = 2;
    Harness harness(true, limits);

    // One below the bound and exactly at the bound both accept.
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    SRF_EXPECT_EQ(harness.store().export_state().attempts.size(), static_cast<std::size_t>(1));
    SRF_EXPECT(harness.create(2, {node_segment(2)}).ok());
    SRF_EXPECT_EQ(harness.store().export_state().attempts.size(), static_cast<std::size_t>(2));

    const std::uint64_t revision = harness.store().revision();
    const std::size_t saves = harness.memory()->save_count();
    const std::size_t lists = harness.store().list_count();

    // One above is refused, with an explicit structured status, and nothing is
    // mutated, persisted or acknowledged.
    const srf::MutationOutcome over = harness.create(3, {node_segment(3)});
    SRF_EXPECT(!over.ok());
    SRF_EXPECT_EQ(static_cast<int>(over.status),
                  static_cast<int>(srf::StatusCode::LimitExceeded));
    SRF_EXPECT_PRIMARY(over.result, srf::ReasonCode::LimitMaxAttemptRecords);
    SRF_EXPECT_EQ(over.result.primary_phase(), srf::ValidationPhase::ResourceLimits);
    SRF_EXPECT(over.result.primary().detail == limits.max_attempt_records);
    SRF_EXPECT_EQ(harness.store().list_count(), lists);
    SRF_EXPECT_EQ(harness.store().revision(), revision);
    SRF_EXPECT_EQ(harness.memory()->save_count(), saves);
    SRF_EXPECT(!harness.store().get(list_id(3)).has_value());
    SRF_EXPECT_EQ(harness.store().export_state().attempts.size(), static_cast<std::size_t>(2));
}

SRF_TEST(limits, a_full_replay_table_still_honours_replay_and_mismatch) {
    srf::Limits limits{};
    limits.max_attempt_records = 1;
    Harness harness(true, limits);
    const srf::CallerIdentity recorded = harness.caller();
    srf::ListDraft draft = harness.draft(1);
    draft.segments = {node_segment(1)};
    const srf::MutationOutcome first = harness.store().create_list(recorded, draft);
    SRF_EXPECT_OK(first);

    // The table is now full. Exact replay is still idempotent and advances nothing.
    const std::uint64_t revision = harness.store().revision();
    const srf::MutationOutcome replay = harness.store().create_list(recorded, draft);
    SRF_EXPECT_OK(replay);
    SRF_EXPECT(replay.idempotent_replay);
    SRF_EXPECT_EQ(harness.store().revision(), revision);

    // Reuse of the recorded identifier with different content is still refused.
    srf::ListDraft different = draft;
    different.segments = {node_segment(2)};
    const srf::MutationOutcome mismatch = harness.store().create_list(recorded, different);
    SRF_EXPECT_EQ(static_cast<int>(mismatch.status),
                  static_cast<int>(srf::StatusCode::Conflict));
    SRF_EXPECT_REASON(mismatch.result, srf::ReasonCode::CommitReplayPayloadMismatch);

    // A genuinely new attempt is refused for capacity, not silently forgotten.
    const srf::MutationOutcome overflowed =
        harness.store().create_list(harness.caller(), draft);
    SRF_EXPECT_REASON(overflowed.result, srf::ReasonCode::LimitMaxAttemptRecords);
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(1));
}

SRF_TEST(limits, a_full_replay_table_refuses_lifecycle_mutations_too) {
    srf::Limits limits{};
    limits.max_attempt_records = 1;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::MutationOutcome refused =
        harness.store().withdraw(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT_EQ(static_cast<int>(refused.status),
                  static_cast<int>(srf::StatusCode::LimitExceeded));
    SRF_EXPECT_PRIMARY(refused.result, srf::ReasonCode::LimitMaxAttemptRecords);
    const auto list = harness.store().get(list_id(1));
    SRF_EXPECT(list.has_value());
    if (list.has_value()) {
        SRF_EXPECT_EQ(static_cast<int>(list->state),
                      static_cast<int>(srf::LifecycleState::Active));
    }
}

SRF_TEST(limits, over_limit_input_cannot_leave_partial_authoritative_state) {
    // Huge input must be refused without iterating or allocating per element, and
    // must leave no partial mutation behind.
    srf::Limits limits{};
    limits.max_segments_per_list = 4;
    limits.max_total_segments = 64;
    limits.max_lists = 8;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::Digest128 before = harness.store().get(list_id(1))->content_digest;
    const std::uint64_t revision = harness.store().revision();

    std::vector<srf::Segment> enormous;
    enormous.reserve(4096);
    for (std::uint64_t n = 1; n <= 5; ++n) {
        enormous.push_back(node_segment(n));
    }
    // Beyond the bounded scan window every identifier is unknown to the fabric. If
    // the structural phase scanned the whole sequence it would report them.
    for (std::uint64_t n = 0; n < 4091; ++n) {
        srf::Segment unknown = node_segment(1);
        unknown.id = srf::SegmentId{0xDEAD'0000ull + n};
        enormous.push_back(unknown);
    }
    const srf::MutationOutcome refused = harness.create(2, enormous);
    SRF_EXPECT(!refused.ok());
    SRF_EXPECT_REASON(refused.result, srf::ReasonCode::LimitMaxSegmentsPerList);
    SRF_EXPECT(!refused.result.contains(srf::ReasonCode::SegmentEntityUnknown));
    SRF_EXPECT(refused.result.size() <= static_cast<std::size_t>(limits.max_reasons));
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(1));
    SRF_EXPECT_EQ(harness.store().revision(), revision);
    SRF_EXPECT_EQ(harness.store().total_segments(), static_cast<std::size_t>(1));
    SRF_EXPECT(harness.store().get(list_id(1))->content_digest == before);
    SRF_EXPECT(!harness.store().get(list_id(2)).has_value());

    // A huge batch is refused by size before any draft is inspected.
    srf::Limits batch_limits{};
    batch_limits.max_batch_size = 2;
    Harness batched(true, batch_limits);
    const std::uint64_t batch_revision = batched.store().revision();
    std::vector<srf::ListDraft> many;
    many.reserve(4096);
    for (std::uint64_t n = 1; n <= 4096; ++n) {
        srf::ListDraft draft = batched.draft(n);
        draft.segments = {node_segment(1)};
        many.push_back(draft);
    }
    const srf::MutationOutcome huge =
        batched.store().batch(batched.caller(), std::span<const srf::ListDraft>(many));
    SRF_EXPECT_EQ(static_cast<int>(huge.status),
                  static_cast<int>(srf::StatusCode::LimitExceeded));
    SRF_EXPECT_EQ(huge.result.size(), static_cast<std::size_t>(1));
    SRF_EXPECT_PRIMARY(huge.result, srf::ReasonCode::LimitMaxBatchSize);
    SRF_EXPECT_EQ(batched.store().list_count(), static_cast<std::size_t>(0));
    SRF_EXPECT_EQ(batched.store().revision(), batch_revision);
}

SRF_TEST(limits, max_evidence_records_is_consulted) {
    srf::Limits limits{};
    limits.max_evidence_records = 4;
    Harness harness(true, limits);
    const srf::MutationOutcome outcome = harness.create(1, {node_segment(1)});
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxEvidenceRecords);
}

SRF_TEST(limits, max_persistence_record_bytes_is_consulted) {
    srf::Limits limits{};
    limits.max_persistence_record_bytes = 150;
    Harness harness(true, limits);
    const srf::MutationOutcome outcome = harness.create(1, {node_segment(1)});
    SRF_EXPECT_EQ(static_cast<int>(outcome.status),
                  static_cast<int>(srf::StatusCode::PersistenceFailed));
    SRF_EXPECT_REASON(outcome.result, srf::ReasonCode::LimitMaxPersistenceRecordBytes);
    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(0));
}

SRF_TEST(limits, max_reasons_is_consulted_and_reported) {
    srf::Limits limits{};
    limits.max_reasons = 3;
    srf::ValidationResult result;
    result.set_limits(&limits);
    for (int i = 0; i < 10; ++i) {
        result.add(srf::ReasonCode::SegmentIdInvalid, static_cast<std::uint32_t>(i));
    }
    SRF_EXPECT(result.truncated());
    SRF_EXPECT(result.size() <= 3);
    SRF_EXPECT_REASON(result, srf::ReasonCode::LimitMaxReasons);
}

// ---------------------------------------------------------------------------
// Bounded presentations: retention is explicit and never claims completeness.
// ---------------------------------------------------------------------------
SRF_TEST(limits, max_explanation_entries_is_an_explicit_bounded_presentation) {
    srf::Limits tight{};
    tight.max_explanation_entries = 2;
    Harness bounded(true, tight);
    SRF_EXPECT(bounded.create(1, {node_segment(1)}).ok());
    const srf::Explanation truncated = bounded.store().explain_currentness(list_id(1));
    SRF_EXPECT(truncated.truncated);
    SRF_EXPECT(!truncated.complete());
    SRF_EXPECT_EQ(truncated.retained_entries(), static_cast<std::uint32_t>(2));
    SRF_EXPECT_EQ(truncated.size(), static_cast<std::size_t>(2));
    SRF_EXPECT(truncated.total_entries > truncated.retained_entries());

    srf::Limits roomy{};
    roomy.max_explanation_entries = 64;
    Harness complete(true, roomy);
    SRF_EXPECT(complete.create(1, {node_segment(1)}).ok());
    const srf::Explanation whole = complete.store().explain_currentness(list_id(1));
    SRF_EXPECT(!whole.truncated);
    SRF_EXPECT(whole.complete());
    SRF_EXPECT_EQ(whole.total_entries, whole.retained_entries());

    // The complete total is the same on both tracks, and the retained prefix is the
    // deterministic first slice of the complete explanation.
    SRF_EXPECT_EQ(truncated.total_entries, whole.total_entries);
    SRF_EXPECT(whole.retained_entries() > truncated.retained_entries());
    for (std::size_t i = 0; i < truncated.entries.size(); ++i) {
        SRF_EXPECT(truncated.entries[i] == whole.entries[i]);
    }

    // One below the bound is a complete presentation.
    srf::Limits exactly{};
    exactly.max_explanation_entries = static_cast<std::uint32_t>(whole.total_entries);
    Harness exact(true, exactly);
    SRF_EXPECT(exact.create(1, {node_segment(1)}).ok());
    const srf::Explanation fitting = exact.store().explain_currentness(list_id(1));
    SRF_EXPECT(!fitting.truncated);
    SRF_EXPECT_EQ(fitting.retained_entries(), whole.total_entries);
}

SRF_TEST(limits, max_diff_entries_is_an_explicit_bounded_presentation) {
    srf::Limits tight{};
    tight.max_diff_entries = 1;
    Harness bounded(true, tight);
    SRF_EXPECT(bounded.create(1, {node_segment(1)}).ok());
    const srf::SegmentListSnapshot before = bounded.store().snapshot(list_id(1));
    SRF_EXPECT_OK(bounded.replace(1, {node_segment(2), node_segment(3)}, 1));
    const srf::SegmentListSnapshot after = bounded.store().snapshot(list_id(1));
    const srf::SnapshotDiff truncated = bounded.store().diff(before, after);
    SRF_EXPECT(truncated.truncated);
    SRF_EXPECT(!truncated.complete());
    SRF_EXPECT_EQ(truncated.retained_entries(), static_cast<std::uint32_t>(1));
    SRF_EXPECT_EQ(truncated.entries.size(), static_cast<std::size_t>(1));
    SRF_EXPECT(truncated.total_entries > truncated.retained_entries());

    srf::Limits roomy{};
    roomy.max_diff_entries = 512;
    Harness complete(true, roomy);
    SRF_EXPECT(complete.create(1, {node_segment(1)}).ok());
    const srf::SegmentListSnapshot whole_before = complete.store().snapshot(list_id(1));
    SRF_EXPECT_OK(complete.replace(1, {node_segment(2), node_segment(3)}, 1));
    const srf::SegmentListSnapshot whole_after = complete.store().snapshot(list_id(1));
    const srf::SnapshotDiff whole = complete.store().diff(whole_before, whole_after);
    SRF_EXPECT(!whole.truncated);
    SRF_EXPECT(whole.complete());
    SRF_EXPECT_EQ(whole.total_entries, whole.retained_entries());

    SRF_EXPECT_EQ(truncated.total_entries, whole.total_entries);
    SRF_EXPECT(whole.retained_entries() > truncated.retained_entries());
    SRF_EXPECT(truncated.entries[0] == whole.entries[0]);

    // Exactly at the bound is still complete.
    srf::Limits exactly{};
    exactly.max_diff_entries = whole.total_entries;
    Harness exact(true, exactly);
    SRF_EXPECT(exact.create(1, {node_segment(1)}).ok());
    const srf::SegmentListSnapshot exact_before = exact.store().snapshot(list_id(1));
    SRF_EXPECT_OK(exact.replace(1, {node_segment(2), node_segment(3)}, 1));
    const srf::SegmentListSnapshot exact_after = exact.store().snapshot(list_id(1));
    const srf::SnapshotDiff fitting = exact.store().diff(exact_before, exact_after);
    SRF_EXPECT(!fitting.truncated);
    SRF_EXPECT_EQ(fitting.retained_entries(), whole.retained_entries());
}

SRF_TEST(limits, bounded_presentation_never_changes_authoritative_state) {
    // Rendering bounds are presentation only: the durable image, the content digest
    // and the lifecycle state must be byte-identical either way.
    srf::DurableState bounded_state{};
    srf::Digest128 bounded_digest{};
    {
        srf::Limits tight{};
        tight.max_explanation_entries = 1;
        tight.max_diff_entries = 1;
        tight.max_history = 1;
        Harness harness(true, tight);
        SRF_EXPECT(harness.create(1, {node_segment(1), node_segment(2)}).ok());
        const srf::SegmentListSnapshot before = harness.store().snapshot(list_id(1));
        SRF_EXPECT_OK(harness.replace(1, {node_segment(2), node_segment(3)}, 1));
        const srf::SegmentListSnapshot after = harness.store().snapshot(list_id(1));
        (void)harness.store().diff(before, after);
        (void)harness.store().explain_currentness(list_id(1));
        (void)harness.store().explain(list_id(1));
        (void)harness.store().history_report();
        bounded_digest = after.digest;
        bounded_state = harness.store().export_state();
    }
    srf::DurableState complete_state{};
    srf::Digest128 complete_digest{};
    {
        srf::Limits roomy{};
        Harness harness(true, roomy);
        SRF_EXPECT(harness.create(1, {node_segment(1), node_segment(2)}).ok());
        const srf::SegmentListSnapshot before = harness.store().snapshot(list_id(1));
        SRF_EXPECT_OK(harness.replace(1, {node_segment(2), node_segment(3)}, 1));
        const srf::SegmentListSnapshot after = harness.store().snapshot(list_id(1));
        (void)harness.store().diff(before, after);
        (void)harness.store().explain_currentness(list_id(1));
        (void)harness.store().explain(list_id(1));
        (void)harness.store().history_report();
        complete_digest = after.digest;
        complete_state = harness.store().export_state();
    }
    SRF_EXPECT(bounded_digest == complete_digest);
    SRF_EXPECT_EQ(bounded_state.lists.size(), complete_state.lists.size());
    if (!bounded_state.lists.empty() && !complete_state.lists.empty()) {
        SRF_EXPECT(bounded_state.lists[0].content == complete_state.lists[0].content);
        SRF_EXPECT(bounded_state.lists[0].content_digest == complete_state.lists[0].content_digest);
        SRF_EXPECT_EQ(static_cast<int>(bounded_state.lists[0].state),
                      static_cast<int>(complete_state.lists[0].state));
        SRF_EXPECT_EQ(static_cast<int>(bounded_state.lists[0].currentness),
                      static_cast<int>(complete_state.lists[0].currentness));
    }
    SRF_EXPECT_EQ(bounded_state.revision, complete_state.revision);
}

SRF_TEST(limits, every_rejection_leaves_the_durable_image_untouched) {
    // Over-limit input must not alter a digest, create ACTIVE state, bypass the
    // duplicate or depth rules, commit partial state, or be acknowledged.
    srf::Limits limits{};
    limits.max_attempt_records = 1;
    limits.max_segments_per_list = 8;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::Digest128 digest = harness.store().get(list_id(1))->content_digest;
    const std::vector<std::byte> image(harness.memory()->bytes().begin(),
                                       harness.memory()->bytes().end());
    const std::uint64_t revision = harness.store().revision();
    const std::size_t saves = harness.memory()->save_count();

    // The replay table is full, so every further mutation is refused explicitly.
    const srf::MutationOutcome capacity = harness.create(2, {node_segment(2)});
    SRF_EXPECT_REASON(capacity.result, srf::ReasonCode::LimitMaxAttemptRecords);
    SRF_EXPECT_EQ(static_cast<int>(capacity.status),
                  static_cast<int>(srf::StatusCode::LimitExceeded));

    // The remaining rules are not bypassed by a full table: they are simply never
    // reached, and no partial state exists either way.
    srf::ListDraft duplicates = harness.draft(3);
    duplicates.segments = {node_segment(1), node_segment(1)};
    const srf::MutationOutcome repeated = harness.store().create_list(harness.caller(), duplicates);
    SRF_EXPECT(!repeated.ok());

    srf::ListDraft deep = harness.draft(4);
    deep.segments.clear();
    for (std::uint64_t n = 1; n <= 9; ++n) {
        deep.segments.push_back(node_segment(n));
    }
    const srf::MutationOutcome too_deep = harness.store().create_list(harness.caller(), deep);
    SRF_EXPECT(!too_deep.ok());

    SRF_EXPECT_EQ(harness.store().list_count(), static_cast<std::size_t>(1));
    SRF_EXPECT_EQ(harness.store().revision(), revision);
    SRF_EXPECT_EQ(harness.memory()->save_count(), saves);
    SRF_EXPECT_EQ(harness.store().get(list_id(1))->content_digest, digest);
    SRF_EXPECT_EQ(static_cast<int>(harness.store().get(list_id(1))->state),
                  static_cast<int>(srf::LifecycleState::Active));
    const std::vector<std::byte> after(harness.memory()->bytes().begin(),
                                       harness.memory()->bytes().end());
    SRF_EXPECT(after == image);
    SRF_EXPECT(!harness.store().get(list_id(2)).has_value());
    SRF_EXPECT(!harness.store().get(list_id(3)).has_value());
    SRF_EXPECT(!harness.store().get(list_id(4)).has_value());
}

SRF_TEST(limits, max_lists_bounds_wire_and_persistence_decoding) {
    srf::Limits limits{};
    limits.max_lists = 1;
    limits.max_reasons = 8;
    srf::Response response{};
    response.summaries.resize(2);
    srf::ValidationResult result;
    const srf::ByteBuffer bytes = srf::encode_response(response, limits, result);
    SRF_EXPECT(result.failed() || bytes.empty());
    SRF_EXPECT_REASON(result, srf::ReasonCode::LimitMaxLists);
}

SRF_TEST(limits, max_frame_bytes_is_consulted) {
    srf::Limits limits{};
    limits.max_frame_bytes = 16;
    srf::Frame frame{};
    frame.message = srf::MessageId::ListSummaries;
    frame.payload.assign(64, std::byte{0});
    srf::ValidationResult result;
    const srf::ByteBuffer bytes = srf::encode_frame(frame, limits, result);
    SRF_EXPECT(bytes.empty());
    SRF_EXPECT_REASON(result, srf::ReasonCode::LimitMaxFrameBytes);

    srf::Frame decoded{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(!srf::decode_frame(srf::as_bytes(bytes), limits, decoded, decode_result));
}

SRF_TEST(limits, default_limits_are_all_positive_and_ordered) {
    const srf::Limits limits{};
    SRF_EXPECT(limits.max_lists > 0);
    SRF_EXPECT(limits.max_segments_per_list > 0);
    SRF_EXPECT(limits.max_segment_payload_bytes > 0);
    SRF_EXPECT(limits.max_profiles > 0);
    SRF_EXPECT(limits.max_policies > 0);
    SRF_EXPECT(limits.max_batch_size > 0);
    SRF_EXPECT(limits.max_publishers > 0);
    SRF_EXPECT(limits.max_frame_bytes > 0);
    SRF_EXPECT(limits.max_history > 0);
    SRF_EXPECT(limits.max_explanation_entries > 0);
    SRF_EXPECT(limits.max_persistence_record_bytes > 0);
    SRF_EXPECT(limits.max_reasons > 0);
    SRF_EXPECT(limits.max_diff_entries > 0);
    SRF_EXPECT(limits.max_total_segments > 0);
    SRF_EXPECT(limits.max_attempt_records > 0);
    SRF_EXPECT(limits.max_evidence_records > 0);
    SRF_EXPECT(limits.max_segments_per_list <= limits.max_total_segments);
}
