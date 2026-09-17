// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 10 - hard restart over the durable image: the epoch increases, durable
// intent survives, live authority does not, and ACTIVE is never restored.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"10 - coordinator restart over the durable image"};
    Scenario scenario = make_scenario(true);
    CHECK(scenario.setup.ok());
    CHECK(scenario.persistence != nullptr);

    transcript.step("declare and activate one list");
    const MutationOutcome created = scenario.create(1, {node_segment(1), node_segment(2)});
    CHECK(created.ok());
    const CoordinatorEpoch epoch_before = scenario.epoch();
    transcript.note("epoch before restart", epoch_before.value());
    transcript.note("committed digest", created.digest);
    transcript.note("durable image bytes",
                    static_cast<std::uint64_t>(scenario.persistence->bytes().size()));
    CHECK(scenario.persistence->save_count() > 0);
    CHECK_EQ(scenario.s().list_count(), std::size_t{1});

    transcript.step("hard restart: a fresh store loads the byte image of the old one");
    const std::vector<std::byte> image(scenario.persistence->bytes().begin(),
                                       scenario.persistence->bytes().end());
    auto restarted_persistence = std::make_shared<MemoryPersistence>();
    restarted_persistence->set_bytes(image);
    StoreConfig config{};
    config.limits = Limits{};
    config.evidence = scenario.evidence;
    config.persistence = restarted_persistence;
    SegmentListStore restarted{config};
    const LoadReport report = restarted.load();
    CHECK(report.loaded);
    CHECK(!report.empty);
    CHECK(report.result.ok());
    transcript.note("report.lists_recovered", static_cast<std::uint64_t>(report.lists_recovered));
    transcript.note("report.lists_requiring_revalidation",
                    static_cast<std::uint64_t>(report.lists_requiring_revalidation));
    transcript.note("report.publishers_fenced",
                    static_cast<std::uint64_t>(report.publishers_fenced));
    CHECK_EQ(report.lists_recovered, std::size_t{1});
    CHECK_EQ(report.lists_requiring_revalidation, std::size_t{1});
    CHECK_EQ(report.publishers_fenced, std::size_t{1});

    transcript.step("the epoch increases and ACTIVE is not restored");
    const CoordinatorEpoch epoch_after = restarted.authority().epoch();
    transcript.note("epoch after restart", epoch_after.value());
    CHECK_EQ(epoch_after.value(), epoch_before.value() + 1);
    const std::optional<SegmentList> restored = restarted.get(list_id(1));
    CHECK(restored.has_value());
    transcript.note("restored state", lifecycle_state_name(restored->state));
    transcript.note("restored currentness", currentness_name(restored->currentness));
    CHECK_EQ(as_int(restored->state), as_int(LifecycleState::RevalidationRequired));
    CHECK_EQ(as_int(restored->currentness), as_int(Currentness::RevalidationRequired));
    CHECK(!lifecycle_is_available(restored->state));
    CHECK_EQ(restored->content_digest, created.digest);

    transcript.step("live authority is never restored: publisher dead, boot fenced");
    CHECK(!restarted.authority().is_alive(PublisherId{kPublisher}));
    CHECK(restarted.authority().is_fenced(WorkerBootId{kBoot}));
    CHECK_EQ(restarted.authority().publisher_count(), std::size_t{1});

    transcript.step("a fresh publisher registers under the new epoch");
    const ValidationResult registered = restarted.authority().register_publisher(
        PublisherId{kReincarnatedPublisher}, WorkerBootId{kReincarnatedBoot}, ScopeId{kScope},
        epoch_after, restarted.limits());
    CHECK(registered.ok());

    transcript.step("a write under the old epoch is rejected");
    CallerIdentity stale{};
    stale.epoch = epoch_before;
    stale.publisher = PublisherId{kReincarnatedPublisher};
    stale.boot = WorkerBootId{kReincarnatedBoot};
    stale.scope = ScopeId{kScope};
    stale.attempt = MutationAttemptId{0xE900};
    ListDraft draft{};
    draft.id = list_id(2);
    draft.scope = ScopeId{kScope};
    draft.profile = SegmentProfileId{kGovernanceProfile};
    draft.segments = {node_segment(3)};
    const MutationOutcome old_epoch = restarted.create_list(stale, draft);
    CHECK(!old_epoch.ok());
    transcript.note("old_epoch.status", status_code_name(old_epoch.status));
    transcript.note("old_epoch.primary", old_epoch.result.primary_name());
    CHECK_REASON(old_epoch.result, ReasonCode::EpochStale);
    CHECK_PRIMARY(old_epoch.result, ReasonCode::EpochStale);
    CHECK_EQ(restarted.list_count(), std::size_t{1});

    transcript.step("the rewritten durable image carries the higher epoch");
    transcript.note("exported epoch", restarted.export_state().epoch.value());
    CHECK_EQ(restarted.export_state().epoch.value(), epoch_after.value());

    transcript.finish();
    return transcript.exit_code();
}
