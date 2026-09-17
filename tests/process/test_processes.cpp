// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Real process proofs. These tests start and kill actual operating-system
// processes. Nothing here is simulated: the worker death is TerminateProcess on a
// live child, and the coordinator restart is a hard kill followed by a fresh
// process over the same durable state file.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"

using srf::test::kBoot;
using srf::test::kPublisher;
using srf::test::kScope;
using srf::test::list_id;

namespace {

/// Explicit bound for a proof step. Exceeding it fails the test: the bound is a
/// product failure condition, not a test timeout.
constexpr std::uint32_t kProofBoundMs = 20000;

class Sandbox {
public:
    Sandbox() {
        static std::uint64_t counter = 0;
        ++counter;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
                     ("srf_proof_" + std::to_string(stamp) + "_" + std::to_string(counter));
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
        std::filesystem::create_directories(directory_, ec);
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] std::filesystem::path state() const { return directory_ / "state.srfstate"; }
    [[nodiscard]] std::filesystem::path port() const { return directory_ / "port.srfport"; }
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_{};
};

[[nodiscard]] std::string narrow(const std::filesystem::path& path) {
    return path.string();
}

/// A fresh publisher/boot pair for every probe connection. A probe that connects
/// and then closes permanently fences its own boot, so a reused identifier would
/// be rejected on the next attempt.
[[nodiscard]] std::uint64_t next_probe_identity() {
    static std::atomic<std::uint64_t> counter{0x40000};
    return counter.fetch_add(1) + 1;
}

/// Starts a coordinator process and waits until it actually serves the protocol.
[[nodiscard]] bool start_coordinator(Sandbox& sandbox, srf::ChildProcess& process,
                                     std::uint32_t lease_ms, srf::CoordinatorEpoch& epoch,
                                     srf::test::Context& context, const char* label) {
    srf::ValidationResult spawn_result;
    process = srf::ChildProcess::spawn(
        SRF_COORDINATOR_EXE,
        {"--state", narrow(sandbox.state()), "--port-file", narrow(sandbox.port()), "--lease-ms",
         std::to_string(lease_ms)},
        narrow(sandbox.directory()), spawn_result);
    if (!process.started()) {
        context.record(false, std::string(label) + ": coordinator process started", __FILE__,
                       __LINE__);
        return false;
    }
    bool ready = false;
    const bool served = srf::wait_until(
        [&] {
            const std::uint64_t identity = next_probe_identity();
            srf::WorkerConfig config{};
            config.port_path = sandbox.port();
            config.boot = srf::WorkerBootId{identity};
            config.publisher = srf::PublisherId{identity};
            config.scope = srf::ScopeId{kScope};
            srf::WorkerClient probe;
            srf::ValidationResult open_result;
            if (!probe.open(config, open_result)) {
                return false;
            }
            const srf::Response hello = probe.handshake();
            probe.close();
            if (hello.status != srf::StatusCode::Ok) {
                return false;
            }
            epoch = hello.epoch;
            ready = true;
            return true;
        },
        kProofBoundMs);
    context.record(served && ready, std::string(label) + ": coordinator serves the protocol",
                   __FILE__, __LINE__);
    return served && ready;
}

[[nodiscard]] srf::ListDraft worker_draft(std::uint64_t list) {
    srf::ListDraft draft{};
    draft.id = list_id(list);
    draft.scope = srf::ScopeId{kScope};
    draft.profile = srf::SegmentProfileId{0x0001'0001ull};
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Node;
    segment.id = srf::test::segment_id(1);
    segment.generation = srf::SegmentGeneration{1};
    segment.node = srf::test::node_id(1);
    segment.payload = {std::byte{0x01}};
    draft.segments.push_back(segment);
    return draft;
}

} // namespace

SRF_TEST(process, real_worker_death_fences_the_writer_and_preserves_the_list) {
    Sandbox sandbox;
    srf::ChildProcess coordinator;
    srf::CoordinatorEpoch epoch{};
    // A long lease: the death is detected by the transport, not by a timer.
    if (!start_coordinator(sandbox, coordinator, 60000, epoch, srf_ctx, "worker death")) {
        return;
    }

    srf::ChildProcess worker;
    srf::ValidationResult spawn_result;
    worker = srf::ChildProcess::spawn(
        SRF_WORKER_EXE,
        {"--port-file", narrow(sandbox.port()), "--boot", std::to_string(kBoot), "--publisher",
         std::to_string(kPublisher), "--scope", std::to_string(kScope), "--list",
         std::to_string(list_id(1).value()), "--segment",
         std::to_string(srf::test::segment_id(1).value()), "--node",
         std::to_string(srf::test::node_id(1).value()), "--publish", "--hold-ms", "600000"},
        narrow(sandbox.directory()), spawn_result);
    SRF_EXPECT(worker.started());
    SRF_EXPECT(coordinator.running());

    // Wait until the real worker has actually published an ACTIVE list.
    bool published = false;
    std::uint64_t published_generation = 0;
    srf::Digest128 published_digest{};
    (void)srf::wait_until(
        [&] {
            const std::uint64_t identity = next_probe_identity();
            srf::WorkerConfig config{};
            config.port_path = sandbox.port();
            config.boot = srf::WorkerBootId{identity};
            config.publisher = srf::PublisherId{identity};
            config.scope = srf::ScopeId{kScope};
            srf::WorkerClient observer;
            srf::ValidationResult open_result;
            if (!observer.open(config, open_result)) {
                return false;
            }
            const srf::Response hello = observer.handshake();
            if (hello.status != srf::StatusCode::Ok) {
                observer.close();
                return false;
            }
            const srf::Response got = observer.get(list_id(1));
            observer.close();
            if (got.status != srf::StatusCode::Ok || got.summaries.empty()) {
                return false;
            }
            if (got.summaries[0].state != srf::LifecycleState::Active) {
                return false;
            }
            published_generation = got.summaries[0].generation.value();
            published_digest = got.summaries[0].digest;
            published = true;
            return true;
        },
        kProofBoundMs);
    SRF_EXPECT(published);
    SRF_EXPECT_EQ(published_generation, 1u);
    if (!published) {
        worker.kill();
        (void)worker.wait();
        coordinator.kill();
        (void)coordinator.wait();
        return;
    }

    // Kill the real worker process.
    SRF_EXPECT(worker.running());
    worker.kill();
    (void)worker.wait();
    SRF_EXPECT(!worker.running());

    // The coordinator must notice the loss and permanently fence that boot.
    bool fenced = false;
    (void)srf::wait_until(
        [&] {
            srf::WorkerConfig config{};
            config.port_path = sandbox.port();
            config.boot = srf::WorkerBootId{kBoot};
            config.publisher = srf::PublisherId{kPublisher};
            config.scope = srf::ScopeId{kScope};
            srf::WorkerClient stale;
            srf::ValidationResult open_result;
            if (!stale.open(config, open_result)) {
                return false;
            }
            const srf::Response hello = stale.handshake();
            stale.close();
            fenced = hello.reasons.size() > 0 &&
                     (hello.reasons[0].code == srf::ReasonCode::BootAlreadyFenced ||
                      hello.reasons[0].code == srf::ReasonCode::CallerStaleWorkerBoot ||
                      hello.reasons[0].code == srf::ReasonCode::CallerFencedPublisher);
            return fenced;
        },
        kProofBoundMs);
    SRF_EXPECT(fenced);

    // A stale mutation from the dead boot is rejected.
    srf::WorkerConfig stale_config{};
    stale_config.port_path = sandbox.port();
    stale_config.boot = srf::WorkerBootId{kBoot};
    stale_config.publisher = srf::PublisherId{kPublisher};
    stale_config.scope = srf::ScopeId{kScope};
    srf::WorkerClient stale;
    srf::ValidationResult open_result;
    if (stale.open(stale_config, open_result)) {
        const srf::Response hello = stale.handshake();
        SRF_EXPECT(hello.status != srf::StatusCode::Ok);
        if (hello.status == srf::StatusCode::Ok) {
            const srf::Response mutation =
                stale.send_mutation(srf::MessageId::ReplaceList, worker_draft(1));
            SRF_EXPECT(mutation.status != srf::StatusCode::Ok);
        }
        stale.close();
    }

    // The durable list survives the writer's death, unchanged.
    {
        srf::FilePersistence persistence(sandbox.state());
        srf::Limits limits{};
        srf::LoadReport report{};
        SRF_EXPECT(persistence.load(report, limits));
        SRF_EXPECT(report.loaded);
        SRF_EXPECT_EQ(report.lists_recovered, static_cast<std::size_t>(1));
        bool found = false;
        for (const srf::SegmentList& list : report.state.lists) {
            if (list.content.id == list_id(1)) {
                found = true;
                SRF_EXPECT(list.generation.value() == 1u);
                SRF_EXPECT(list.content_digest == published_digest);
                SRF_EXPECT_EQ(list.content.segments.size(), static_cast<std::size_t>(1));
            }
        }
        SRF_EXPECT(found);
    }

    // A fresh boot registers, and the surviving list is explicitly revalidated by
    // the new writer. Availability is never restored implicitly.
    srf::WorkerConfig fresh_config{};
    fresh_config.port_path = sandbox.port();
    fresh_config.boot = srf::WorkerBootId{0xBEEF};
    fresh_config.publisher = srf::PublisherId{0xBEEF};
    fresh_config.scope = srf::ScopeId{kScope};
    srf::WorkerClient fresh;
    SRF_EXPECT(fresh.open(fresh_config, open_result));
    if (fresh.connected()) {  // NOLINT(readability-use-anyofallof)
        const srf::Response hello = fresh.handshake();
        SRF_EXPECT_EQ(static_cast<int>(hello.status), static_cast<int>(srf::StatusCode::Ok));
        const srf::Response before = fresh.get(list_id(1));
        SRF_EXPECT_EQ(static_cast<int>(before.status), static_cast<int>(srf::StatusCode::Ok));
        SRF_EXPECT(!before.summaries.empty());
        if (!before.summaries.empty()) {
            SRF_EXPECT(before.summaries[0].state != srf::LifecycleState::Active);
            SRF_EXPECT_EQ(static_cast<int>(before.summaries[0].currentness),
                          static_cast<int>(srf::Currentness::FencedPublisher));
        }
        const srf::Response revalidated = fresh.send_lifecycle(
            srf::LifecycleOp::Revalidate, list_id(1), srf::SegmentListGeneration{1});
        SRF_EXPECT_EQ(static_cast<int>(revalidated.status),
                      static_cast<int>(srf::StatusCode::Ok));
        const srf::Response after = fresh.get(list_id(1));
        SRF_EXPECT_EQ(static_cast<int>(after.status), static_cast<int>(srf::StatusCode::Ok));
        if (!after.summaries.empty()) {
            SRF_EXPECT_EQ(static_cast<int>(after.summaries[0].state),
                          static_cast<int>(srf::LifecycleState::Active));
            SRF_EXPECT_EQ(after.summaries[0].generation.value(), 2u);
        }
        fresh.close();
    }

    coordinator.kill();
    (void)coordinator.wait();
    SRF_EXPECT(!coordinator.running());
}

SRF_TEST(process, coordinator_restart_raises_the_epoch_and_never_restores_authority) {
    Sandbox sandbox;
    srf::ChildProcess first;
    srf::CoordinatorEpoch first_epoch{};
    if (!start_coordinator(sandbox, first, 60000, first_epoch, srf_ctx, "restart: first")) {
        return;
    }

    // Publish through a real worker process, then let it exit cleanly.
    srf::ChildProcess worker;
    srf::ValidationResult spawn_result;
    worker = srf::ChildProcess::spawn(
        SRF_WORKER_EXE,
        {"--port-file", narrow(sandbox.port()), "--boot", std::to_string(kBoot), "--publisher",
         std::to_string(kPublisher), "--scope", std::to_string(kScope), "--list",
         std::to_string(list_id(1).value()), "--segment",
         std::to_string(srf::test::segment_id(1).value()), "--node",
         std::to_string(srf::test::node_id(1).value()), "--publish", "--hold-ms", "0"},
        narrow(sandbox.directory()), spawn_result);
    SRF_EXPECT(worker.started());
    const int worker_exit = worker.wait();
    SRF_EXPECT_EQ(worker_exit, 0);

    std::uint64_t generation = 0;
    srf::Digest128 digest{};
    bool published = false;
    (void)srf::wait_until(
        [&] {
            const std::uint64_t identity = next_probe_identity();
            srf::WorkerConfig config{};
            config.port_path = sandbox.port();
            config.boot = srf::WorkerBootId{identity};
            config.publisher = srf::PublisherId{identity};
            config.scope = srf::ScopeId{kScope};
            srf::WorkerClient observer;
            srf::ValidationResult open_result;
            if (!observer.open(config, open_result)) {
                return false;
            }
            const srf::Response hello = observer.handshake();
            if (hello.status != srf::StatusCode::Ok) {
                observer.close();
                return false;
            }
            const srf::Response got = observer.get(list_id(1));
            observer.close();
            if (got.status != srf::StatusCode::Ok || got.summaries.empty()) {
                return false;
            }
            generation = got.summaries[0].generation.value();
            digest = got.summaries[0].digest;
            published = true;
            return true;
        },
        kProofBoundMs);
    SRF_EXPECT(published);

    // Hard kill the coordinator.
    first.kill();
    (void)first.wait();
    SRF_EXPECT(!first.running());

    std::error_code ec;
    std::filesystem::remove(sandbox.port(), ec);

    srf::ChildProcess second;
    srf::CoordinatorEpoch second_epoch{};
    if (!start_coordinator(sandbox, second, 60000, second_epoch, srf_ctx, "restart: second")) {
        return;
    }
    SRF_EXPECT(second_epoch > first_epoch);

    // The durable list is recovered conservatively: present, same content, and not
    // available.
    srf::WorkerConfig config{};
    config.port_path = sandbox.port();
    config.boot = srf::WorkerBootId{0xF303};
    config.publisher = srf::PublisherId{0xF303};
    config.scope = srf::ScopeId{kScope};
    srf::WorkerClient observer;
    srf::ValidationResult open_result;
    SRF_EXPECT(observer.open(config, open_result));
    if (observer.connected()) {
        const srf::Response hello = observer.handshake();
        SRF_EXPECT_EQ(static_cast<int>(hello.status), static_cast<int>(srf::StatusCode::Ok));
        const srf::Response got = observer.get(list_id(1));
        SRF_EXPECT_EQ(static_cast<int>(got.status), static_cast<int>(srf::StatusCode::Ok));
        SRF_EXPECT(!got.summaries.empty());
        if (!got.summaries.empty()) {
            SRF_EXPECT_EQ(got.summaries[0].generation.value(), generation);
            SRF_EXPECT(got.summaries[0].digest == digest);
            SRF_EXPECT_EQ(static_cast<int>(got.summaries[0].state),
                          static_cast<int>(srf::LifecycleState::RevalidationRequired));
            // The pre-restart writer's boot is permanently fenced, so the recovered
            // list is stale because its publisher is fenced.
            SRF_EXPECT_EQ(static_cast<int>(got.summaries[0].currentness),
                          static_cast<int>(srf::Currentness::FencedPublisher));
        }

        // The pre-restart epoch cannot mutate anything.
        srf::WorkerConfig old_config = config;
        old_config.boot = srf::WorkerBootId{0xF304};
        old_config.publisher = srf::PublisherId{0xF304};
        old_config.expected_epoch = first_epoch;
        srf::WorkerClient old_writer;
        srf::ValidationResult old_open;
        if (old_writer.open(old_config, old_open)) {
            (void)old_writer.handshake();
            const srf::Response mutation =
                old_writer.send_mutation(srf::MessageId::ReplaceList, worker_draft(1));
            SRF_EXPECT(mutation.status != srf::StatusCode::Ok);
            old_writer.close();
        }

        // Only an explicit, fully validated revalidation restores availability.
        const srf::Response revalidated = observer.send_lifecycle(
            srf::LifecycleOp::Revalidate, list_id(1), srf::SegmentListGeneration{generation});
        SRF_EXPECT_EQ(static_cast<int>(revalidated.status),
                      static_cast<int>(srf::StatusCode::Ok));
        const srf::Response after = observer.get(list_id(1));
        if (!after.summaries.empty()) {
            SRF_EXPECT_EQ(static_cast<int>(after.summaries[0].state),
                          static_cast<int>(srf::LifecycleState::Active));
        }
        observer.close();
    }

    second.kill();
    (void)second.wait();
}

SRF_TEST(process, repeated_restarts_keep_epochs_strictly_monotonic) {
    Sandbox sandbox;
    srf::CoordinatorEpoch previous{};
    for (int round = 0; round < 3; ++round) {
        std::error_code ec;
        std::filesystem::remove(sandbox.port(), ec);
        srf::ChildProcess coordinator;
        srf::CoordinatorEpoch epoch{};
        const std::string label = "restart round " + std::to_string(round);
        if (!start_coordinator(sandbox, coordinator, 60000, epoch, srf_ctx, label.c_str())) {
            return;
        }
        if (round > 0) {
            SRF_EXPECT(epoch > previous);
        }
        previous = epoch;
        coordinator.kill();
        (void)coordinator.wait();
        SRF_EXPECT(!coordinator.running());
    }
}
