// Segment Routing Fabric 1.0.0 - independent consumer.
// Copyright 2026 Summon Software Labs.
//
// This program is built only against an installed SegmentRoutingFabric package. It
// exercises the exported API the way an unrelated project would: declare an
// abstract profile, publish an ordered list, prove capability rejection, change
// order and observe a different digest, and revalidate.
#include <cstdio>
#include <memory>
#include <vector>

#include "srf/srf.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("  %-62s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition) {
        ++failures;
    }
}

constexpr std::uint64_t kScope = 0x51;
constexpr std::uint64_t kCapabilityA = 0x1001;
constexpr std::uint64_t kCapabilityB = 0x1002;

std::shared_ptr<srf::SyntheticEvidence> make_evidence(bool with_capability_b) {
    auto evidence = std::make_shared<srf::SyntheticEvidence>();
    evidence->set_topology_generation(srf::TopologyGeneration{1});
    evidence->set_capability_generation(srf::CapabilityGeneration{1});
    evidence->add_capability(srf::CapabilityKey{kCapabilityA}, srf::CapabilityGeneration{1}, true);
    if (with_capability_b) {
        evidence->add_capability(srf::CapabilityKey{kCapabilityB},
                                 srf::CapabilityGeneration{1}, true);
    }
    for (std::uint64_t n = 1; n <= 4; ++n) {
        evidence->add_node(srf::NodeId{0x2000 + n}, srf::ScopeId{kScope});
        evidence->add_segment(srf::SegmentId{0x3000 + n}, srf::SegmentKind::Node,
                              srf::SegmentGeneration{1}, srf::ScopeId{kScope});
    }
    return evidence;
}

srf::Segment node(std::uint64_t n) {
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Node;
    segment.id = srf::SegmentId{0x3000 + n};
    segment.generation = srf::SegmentGeneration{1};
    segment.node = srf::NodeId{0x2000 + n};
    segment.payload = {std::byte{0x01}};
    return segment;
}

srf::CallerIdentity make_caller(const srf::SegmentListStore& store, std::uint64_t attempt) {
    srf::CallerIdentity caller{};
    caller.epoch = store.authority().epoch();
    caller.publisher = srf::PublisherId{0x9001};
    caller.boot = srf::WorkerBootId{0xB001};
    caller.scope = srf::ScopeId{kScope};
    caller.attempt = srf::MutationAttemptId{attempt};
    return caller;
}

} // namespace

int main() {
    std::printf("Segment Routing Fabric %s - independent consumer\n",
                std::string(srf::version_string()).c_str());
    std::printf("support classification: %s\n\n",
                std::string(srf::support_classification()).c_str());

    // 1. Create the abstract profile and a store over it.
    const srf::SegmentProfile profile = srf::abstract_governance_profile();
    srf::StoreConfig config{};
    config.evidence = make_evidence(true);
    srf::SegmentListStore store(std::move(config));
    const srf::ValidationResult registered = store.register_profile(profile);
    check(registered.ok(), "abstract governance profile registered");
    const srf::ValidationResult publisher = store.authority().register_publisher(
        srf::PublisherId{0x9001}, srf::WorkerBootId{0xB001}, srf::ScopeId{kScope},
        store.authority().epoch(), store.limits());
    check(publisher.ok(), "publisher registered against the current epoch");

    // 2. Publish an ordered list.
    srf::ListDraft draft{};
    draft.id = srf::derive_segment_list_id("consumer", "primary-path");
    draft.scope = srf::ScopeId{kScope};
    draft.profile = profile.id;
    draft.segments = {node(1), node(2), node(3)};
    const srf::MutationOutcome created =
        store.create_list(make_caller(store, 1), draft);
    check(created.ok(), "ordered list published and ACTIVE");
    check(created.generation.value() == 1, "first revision has generation 1");
    check(!created.digest.is_zero(), "content digest is present");
    std::printf("  content digest: %s\n", created.digest.hex().c_str());

    // 3. Prove capability rejection with a store whose evidence lacks a required
    //    capability.
    {
        srf::StoreConfig weak_config{};
        weak_config.evidence = make_evidence(false);
        srf::SegmentListStore weak(std::move(weak_config));
        (void)weak.register_profile(profile);
        (void)weak.authority().register_publisher(
            srf::PublisherId{0x9001}, srf::WorkerBootId{0xB001}, srf::ScopeId{kScope},
            weak.authority().epoch(), weak.limits());
        const srf::MutationOutcome rejected =
            weak.create_list(make_caller(weak, 2), draft);
        check(!rejected.ok(), "missing required capability is rejected");
        check(rejected.result.contains(srf::ReasonCode::CapabilityMissing),
              "rejection names CapabilityMissing");
    }

    // 4. Alter the order and observe a different digest.
    srf::ListDraft reordered = draft;
    reordered.expected_generation = srf::SegmentListGeneration{1};
    reordered.segments = {node(3), node(2), node(1)};
    const srf::MutationOutcome replaced =
        store.replace_list(make_caller(store, 3), reordered);
    check(replaced.ok(), "reordered replacement accepted");
    check(!(replaced.digest == created.digest), "reordering changed the content digest");
    check(replaced.generation.value() == 2, "replacement advanced the generation");
    std::printf("  reordered digest: %s\n", replaced.digest.hex().c_str());

    const std::optional<srf::SegmentList> stored = store.get(draft.id);
    check(stored.has_value(), "list is readable");
    if (stored.has_value()) {
        check(stored->content.segments.size() == 3, "order is preserved, not sorted");
        check(stored->content.segments[0].id.value() == 0x3003,
              "first stored segment is the one submitted first");
        check(stored->lineage.supersedes.value() == 1, "lineage is preserved");
        check(stored->state == srf::LifecycleState::Active, "list is ACTIVE");
        check(stored->currentness == srf::Currentness::Current, "list is CURRENT");
    }

    // 5. Revalidate against the current dependency generations.
    const srf::MutationOutcome revalidated = store.revalidate(
        make_caller(store, 4), draft.id, srf::SegmentListGeneration{2});
    check(revalidated.ok(), "explicit revalidation accepted");
    check(revalidated.generation.value() == 3, "revalidation produced a new revision");

    // 6. A snapshot diff sees the reorder.
    const srf::SegmentListSnapshot before = store.snapshot(draft.id);
    srf::ListDraft again = draft;
    again.expected_generation = srf::SegmentListGeneration{3};
    again.segments = {node(1), node(2), node(3)};
    (void)store.replace_list(make_caller(store, 5), again);
    const srf::SegmentListSnapshot after = store.snapshot(draft.id);
    const srf::SnapshotDiff diff = store.diff(before, after);
    check(diff.contains(srf::DiffKind::Reorder) || diff.contains(srf::DiffKind::Replace),
          "diff reports the sequence change");

    std::printf("\n%s\n", failures == 0 ? "CONSUMER OK" : "CONSUMER FAILED");
    return failures == 0 ? 0 : 1;
}
