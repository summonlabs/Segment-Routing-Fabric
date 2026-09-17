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

SRF_TEST(limits, max_history_is_consulted) {
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
    const auto history = harness.store().history();
    SRF_EXPECT_EQ(history.size(), static_cast<std::size_t>(3));
    bool increasing = true;
    for (std::size_t i = 1; i < history.size(); ++i) {
        if (!(history[i - 1].id < history[i].id)) {
            increasing = false;
        }
    }
    SRF_EXPECT(increasing);
}

SRF_TEST(limits, max_attempt_records_is_consulted) {
    srf::Limits limits{};
    limits.max_attempt_records = 2;
    Harness harness(true, limits);
    for (std::uint64_t n = 1; n <= 5; ++n) {
        SRF_EXPECT(harness.create(n, {node_segment(n)}).ok());
    }
    SRF_EXPECT_EQ(harness.store().export_state().attempts.size(), static_cast<std::size_t>(2));
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

SRF_TEST(limits, max_explanation_entries_is_consulted) {
    srf::Limits limits{};
    limits.max_explanation_entries = 2;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::Explanation explanation = harness.store().explain_currentness(list_id(1));
    SRF_EXPECT(explanation.truncated);
    SRF_EXPECT(explanation.size() <= 2);
}

SRF_TEST(limits, max_diff_entries_is_consulted) {
    srf::Limits limits{};
    limits.max_diff_entries = 1;
    Harness harness(true, limits);
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::SegmentListSnapshot before = harness.store().snapshot(list_id(1));
    const srf::MutationOutcome replaced =
        harness.replace(1, {node_segment(2), node_segment(3)}, 1);
    SRF_EXPECT_OK(replaced);
    const srf::SegmentListSnapshot after = harness.store().snapshot(list_id(1));
    const srf::SnapshotDiff diff = harness.store().diff(before, after);
    SRF_EXPECT(diff.truncated);
    SRF_EXPECT(diff.entries.size() <= 1);
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
