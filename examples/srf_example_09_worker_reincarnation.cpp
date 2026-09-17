// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Example 09 - a fenced boot can never mutate again; a fresh publisher with a
// fresh boot can. SYNTHETIC evidence.
#include "example_support.hpp"

int main() {
    using namespace srf;
    using namespace srf::examples;

    Transcript transcript{"09 - worker reincarnation under a fenced boot"};
    Scenario scenario = make_scenario();
    CHECK(scenario.setup.ok());
    const CoordinatorEpoch epoch = scenario.epoch();
    transcript.note("epoch", epoch.value());

    transcript.step("publisher 0x9001 writes under boot 0xb001");
    const MutationOutcome first = scenario.create(1, {node_segment(1)});
    CHECK(first.ok());
    CHECK(scenario.s().authority().is_alive(PublisherId{kPublisher}));
    CHECK(!scenario.s().authority().is_fenced(WorkerBootId{kBoot}));

    transcript.step("the publisher dies; its boot is fenced permanently");
    CHECK(scenario.s().authority().mark_dead(PublisherId{kPublisher}).ok());
    transcript.note("is_alive(publisher)", scenario.s().authority().is_alive(PublisherId{kPublisher}));
    transcript.note("is_fenced(boot)", scenario.s().authority().is_fenced(WorkerBootId{kBoot}));
    CHECK(!scenario.s().authority().is_alive(PublisherId{kPublisher}));
    CHECK(scenario.s().authority().is_fenced(WorkerBootId{kBoot}));

    transcript.step("a fenced boot cannot mutate");
    const MutationOutcome fenced = scenario.create(2, {node_segment(2)});
    CHECK(!fenced.ok());
    transcript.note("fenced.primary", fenced.result.primary_name());
    CHECK_REASON(fenced.result, ReasonCode::CallerFencedPublisher);
    CHECK_PRIMARY(fenced.result, ReasonCode::CallerFencedPublisher);
    CHECK_EQ(scenario.s().list_count(), std::size_t{1});

    transcript.step("a fenced boot can never heartbeat back to life");
    const ValidationResult beat =
        scenario.s().authority().heartbeat(PublisherId{kPublisher}, WorkerBootId{kBoot}, epoch);
    CHECK(beat.failed());
    transcript.note("heartbeat.primary", beat.primary_name());
    CHECK_REASON(beat, ReasonCode::CallerFencedPublisher);

    transcript.step("the fenced boot identifier can never be registered again");
    const ValidationResult rebind = scenario.s().authority().register_publisher(
        PublisherId{kPublisher}, WorkerBootId{kBoot}, ScopeId{kScope}, epoch,
        scenario.s().limits());
    CHECK(rebind.failed());
    transcript.note("rebind.primary", rebind.primary_name());
    CHECK_REASON(rebind, ReasonCode::BootAlreadyFenced);
    CHECK_PRIMARY(rebind, ReasonCode::BootAlreadyFenced);

    transcript.step("the same publisher identity with a fresh boot is still a stale writer");
    const ValidationResult stale = scenario.s().authority().register_publisher(
        PublisherId{kPublisher}, WorkerBootId{kReincarnatedBoot}, ScopeId{kScope}, epoch,
        scenario.s().limits());
    CHECK(stale.failed());
    transcript.note("stale.primary", stale.primary_name());
    CHECK_REASON(stale, ReasonCode::CallerStaleWorkerBoot);

    transcript.step("a fresh publisher with a fresh boot is accepted");
    const ValidationResult reincarnated = scenario.s().authority().register_publisher(
        PublisherId{kReincarnatedPublisher}, WorkerBootId{kReincarnatedBoot}, ScopeId{kScope},
        epoch, scenario.s().limits());
    CHECK(reincarnated.ok());
    CHECK(scenario.s().authority().is_alive(PublisherId{kReincarnatedPublisher}));
    CHECK_EQ(scenario.s().authority().publisher_count(), std::size_t{2});

    ListDraft fresh_draft = scenario.draft(3);
    fresh_draft.segments = {node_segment(3)};
    const MutationOutcome fresh = scenario.s().create_list(
        scenario.caller(kReincarnatedPublisher, kReincarnatedBoot), fresh_draft);
    CHECK(fresh.ok());
    transcript.note("fresh.generation", fresh.generation.value());
    const std::optional<SegmentList> stored = scenario.s().get(list_id(3));
    CHECK(stored.has_value());
    CHECK_EQ(stored->authority.publisher, PublisherId{kReincarnatedPublisher});
    CHECK_EQ(stored->authority.boot, WorkerBootId{kReincarnatedBoot});

    transcript.step("the dead writer's list is not silently trusted either");
    const CurrentnessReport report = scenario.s().refresh_currentness();
    transcript.note("report.examined", static_cast<std::uint64_t>(report.examined));
    transcript.note("report.invalidated", static_cast<std::uint64_t>(report.invalidated));
    CHECK_EQ(report.examined, std::size_t{2});
    CHECK_EQ(report.invalidated, std::size_t{1});
    const std::optional<SegmentList> orphan = scenario.s().get(list_id(1));
    CHECK(orphan.has_value());
    transcript.note("orphan.currentness", currentness_name(orphan->currentness));
    CHECK_EQ(as_int(orphan->currentness), as_int(Currentness::FencedPublisher));
    CHECK_EQ(as_int(orphan->state), as_int(LifecycleState::RevalidationRequired));

    transcript.finish();
    return transcript.exit_code();
}
