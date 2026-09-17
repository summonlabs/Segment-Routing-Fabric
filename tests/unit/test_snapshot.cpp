// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

using srf::test::Harness;
using srf::test::list_id;
using srf::test::node_segment;

namespace {

srf::SegmentListSnapshot take(Harness& harness, std::uint64_t id) {
    return harness.store().snapshot(list_id(id));
}

} // namespace

SRF_TEST(snapshot, snapshots_are_immutable_values) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::SegmentListSnapshot before = take(harness, 1);
    SRF_EXPECT(before.valid());
    SRF_EXPECT_EQ(before.list.content.segments.size(), static_cast<std::size_t>(1));
    SRF_EXPECT(harness.replace(1, {node_segment(1), node_segment(2)}, 1).ok());
    SRF_EXPECT_EQ(before.list.content.segments.size(), static_cast<std::size_t>(1));
    SRF_EXPECT_EQ(before.list.generation.value(), 1u);
    const srf::SegmentListSnapshot after = take(harness, 1);
    SRF_EXPECT_EQ(after.list.generation.value(), 2u);
    SRF_EXPECT(!(after.id == before.id));
    const srf::SegmentListSnapshot missing = take(harness, 99);
    SRF_EXPECT(!missing.valid());
}

SRF_TEST(snapshot, diffs_report_every_change_class) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1), node_segment(2)}).ok());
    const srf::SegmentListSnapshot base = take(harness, 1);

    const srf::SegmentListSnapshot created = take(harness, 99);
    const srf::SnapshotDiff creation = harness.store().diff(created, base);
    SRF_EXPECT(creation.contains(srf::DiffKind::Created));

    SRF_EXPECT(harness.replace(1, {node_segment(1), node_segment(3)}, 1).ok());
    const srf::SegmentListSnapshot replaced = take(harness, 1);
    const srf::SnapshotDiff replacement = harness.store().diff(base, replaced);
    SRF_EXPECT(replacement.contains(srf::DiffKind::GenerationAdvance));
    SRF_EXPECT(replacement.contains(srf::DiffKind::Replace));
    SRF_EXPECT(!replacement.contains(srf::DiffKind::Reorder));

    // Reordering the same segments is reported as a reorder, not a replace.
    SRF_EXPECT(harness.replace(1, {node_segment(3), node_segment(1)}, 2).ok());
    const srf::SegmentListSnapshot reordered = take(harness, 1);
    const srf::SnapshotDiff reorder_diff = harness.store().diff(replaced, reordered);
    SRF_EXPECT(reorder_diff.contains(srf::DiffKind::Reorder));
    SRF_EXPECT(reorder_diff.order_changed);

    // Insert, remove.
    SRF_EXPECT(harness.replace(1, {node_segment(3), node_segment(1), node_segment(2)}, 3).ok());
    const srf::SegmentListSnapshot inserted = take(harness, 1);
    SRF_EXPECT(harness.store().diff(reordered, inserted).contains(srf::DiffKind::Insert));
    SRF_EXPECT(harness.replace(1, {node_segment(3), node_segment(1)}, 4).ok());
    const srf::SegmentListSnapshot removed = take(harness, 1);
    SRF_EXPECT(harness.store().diff(inserted, removed).contains(srf::DiffKind::Remove));

    // Lifecycle and currentness changes.
    SRF_EXPECT(harness.store()
                   .withdraw(harness.caller(), list_id(1), srf::SegmentListGeneration{5})
                   .ok());
    const srf::SegmentListSnapshot withdrawn = take(harness, 1);
    const srf::SnapshotDiff lifecycle = harness.store().diff(removed, withdrawn);
    SRF_EXPECT(lifecycle.contains(srf::DiffKind::LifecycleChange));
    SRF_EXPECT(lifecycle.contains(srf::DiffKind::CurrentnessChange));

    // Destroyed.
    const srf::SnapshotDiff destroyed = harness.store().diff(withdrawn, srf::SegmentListSnapshot{});
    SRF_EXPECT(destroyed.contains(srf::DiffKind::Destroyed));
}

SRF_TEST(snapshot, dependency_and_profile_changes_appear_in_the_diff) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::SegmentListSnapshot base = take(harness, 1);
    harness.fabric().advance_capability_generation();
    (void)harness.store().refresh_currentness();
    const srf::SegmentListSnapshot stale = take(harness, 1);
    // Refresh changes lifecycle and currentness only; the bound generations are
    // untouched because refresh never mutates content.
    const srf::SnapshotDiff invalidation = harness.store().diff(base, stale);
    SRF_EXPECT(invalidation.contains(srf::DiffKind::CurrentnessChange));
    SRF_EXPECT(invalidation.contains(srf::DiffKind::LifecycleChange));
    SRF_EXPECT(!invalidation.contains(srf::DiffKind::DependencyChange));

    const srf::MutationOutcome outcome =
        harness.store().revalidate(harness.caller(), list_id(1), srf::SegmentListGeneration{1});
    SRF_EXPECT_OK(outcome);
    const srf::SegmentListSnapshot after = take(harness, 1);
    const srf::SnapshotDiff diff = harness.store().diff(stale, after);
    SRF_EXPECT(diff.contains(srf::DiffKind::DependencyChange));
    SRF_EXPECT(diff.contains(srf::DiffKind::CurrentnessChange));
    SRF_EXPECT(diff.contains(srf::DiffKind::GenerationAdvance));
}

SRF_TEST(snapshot, diff_entry_names_are_total) {
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(srf::DiffKind::Destroyed);
         ++raw) {
        SRF_EXPECT(!srf::diff_kind_name(static_cast<srf::DiffKind>(raw)).empty());
    }
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(srf::ExplanationTopic::Lineage);
         ++raw) {
        SRF_EXPECT(!srf::explanation_topic_name(static_cast<srf::ExplanationTopic>(raw)).empty());
    }
}

SRF_TEST(snapshot, explanations_answer_the_governance_questions) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());

    const srf::Explanation active = harness.store().explain(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(active.topic),
                  static_cast<int>(srf::ExplanationTopic::WhyActive));
    SRF_EXPECT(active.contains(srf::ReasonCode::CurrentnessCurrent));

    const srf::Explanation missing = harness.store().explain(list_id(42));
    SRF_EXPECT(missing.contains(srf::ReasonCode::ListNotFound));

    // A valid segment reports no defect.
    const srf::Explanation good_segment = harness.store().explain_segment(list_id(1), 0);
    SRF_EXPECT(good_segment.contains(srf::ReasonCode::Ok));
    const srf::Explanation bad_index = harness.store().explain_segment(list_id(1), 9);
    SRF_EXPECT(bad_index.contains(srf::ReasonCode::InvalidArgument));

    // A structurally invalid segment is rejected outright and is therefore never
    // stored: the rejection itself is the explanation.
    srf::ListDraft invalid = harness.draft(2);
    srf::Segment broken = node_segment(2);
    broken.node = srf::NodeId{};
    invalid.segments = {broken};
    const srf::MutationOutcome rejected = harness.store().create_list(harness.caller(), invalid);
    SRF_EXPECT(rejected.result.failed());
    SRF_EXPECT(rejected.result.contains(srf::ReasonCode::SegmentBindingIncomplete) ||
               rejected.result.contains(srf::ReasonCode::SegmentMissingBinding));
    const srf::Explanation absent_segment = harness.store().explain_segment(list_id(2), 0);
    SRF_EXPECT(absent_segment.contains(srf::ReasonCode::ListNotFound));

    // Stale topology.
    srf::ListDraft adjacency = harness.draft(3);
    adjacency.topology = srf::TopologyGeneration{srf::test::kTopology};
    adjacency.segments = {srf::test::adjacency_segment(1)};
    SRF_EXPECT_OK(harness.store().create_list(harness.caller(), adjacency));
    harness.fabric().remove_adjacency(1);
    const srf::Explanation topology = harness.store().explain_stale_topology(list_id(3));
    SRF_EXPECT_EQ(static_cast<int>(topology.topic),
                  static_cast<int>(srf::ExplanationTopic::StaleTopology));
    SRF_EXPECT(topology.contains(srf::ReasonCode::TopologyAdjacencyUnknown));

    // Revalidation required.
    (void)harness.store().refresh_currentness();
    const srf::Explanation currentness = harness.store().explain_currentness(list_id(3));
    SRF_EXPECT_EQ(static_cast<int>(currentness.topic),
                  static_cast<int>(srf::ExplanationTopic::Currentness));
    SRF_EXPECT(currentness.contains(srf::ReasonCode::CurrentnessStaleTopology));

    const srf::Explanation revalidation = harness.store().explain(list_id(3));
    SRF_EXPECT_EQ(static_cast<int>(revalidation.topic),
                  static_cast<int>(srf::ExplanationTopic::WhyRevalidationRequired));

    // Supersession.
    SRF_EXPECT_OK(harness.create(4, {node_segment(4)}));
    srf::ListDraft superseding = harness.draft(5);
    superseding.segments = {node_segment(5)};
    superseding.supersedes = list_id(4);
    superseding.supersedes_generation = srf::SegmentListGeneration{1};
    SRF_EXPECT_OK(harness.store().create_list(harness.caller(), superseding));
    const srf::Explanation supersession = harness.store().explain_supersession(list_id(4));
    SRF_EXPECT_EQ(static_cast<int>(supersession.topic),
                  static_cast<int>(srf::ExplanationTopic::WhySuperseded));
    SRF_EXPECT(supersession.contains(srf::ReasonCode::ListSuperseded));

    // Missing capability. This is checked last because it removes a capability
    // that every other list in this test depends on.
    harness.fabric().remove_capability(srf::test::kCapabilityB);
    const srf::Explanation capability = harness.store().explain_missing_capability(list_id(1));
    SRF_EXPECT_EQ(static_cast<int>(capability.topic),
                  static_cast<int>(srf::ExplanationTopic::MissingCapability));
    SRF_EXPECT(capability.contains(srf::ReasonCode::CapabilityMissing));
    const srf::Explanation segment_capability = harness.store().explain_segment(list_id(1), 0);
    SRF_EXPECT(segment_capability.contains(srf::ReasonCode::CapabilityMissing));
}
