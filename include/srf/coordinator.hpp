// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "srf/store.hpp"
#include "srf/transport.hpp"
#include "srf/wire.hpp"

namespace srf {

struct CoordinatorConfig {
    Limits limits{};
    /// Durable state file. Empty disables persistence.
    std::filesystem::path state_path{};
    /// File the coordinator writes its actual listening port to.
    std::filesystem::path port_path{};
    std::uint16_t port{0};
    /// A publisher that stops heartbeating for this long is fenced.
    std::uint32_t lease_milliseconds{4000};
    bool load_persisted{true};
    /// Bump the epoch on start so every restart outranks the previous epoch.
    bool advance_epoch{true};
    /// Serve at most this many connections before the accept loop exits. Zero
    /// means unlimited. Used by the process proofs to bound a run.
    std::uint32_t max_connections{0};
};

/// Real coordinator process body: owns the store, the durable state, the epoch
/// and the publisher leases. It listens on loopback TCP and serves the wire
/// protocol described by srf/wire.hpp.
class CoordinatorServer {
public:
    explicit CoordinatorServer(CoordinatorConfig config);
    ~CoordinatorServer();
    CoordinatorServer(const CoordinatorServer&) = delete;
    CoordinatorServer& operator=(const CoordinatorServer&) = delete;

    [[nodiscard]] bool start(ValidationResult& out);
    void serve();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] CoordinatorEpoch epoch() const { return store_->authority().epoch(); }
    [[nodiscard]] SegmentListStore& store() { return *store_; }
    [[nodiscard]] const LoadReport& load_report() const noexcept { return load_report_; }
    [[nodiscard]] std::size_t registered_publishers() const;
    [[nodiscard]] std::size_t connections_served() const noexcept {
        return connections_served_.load();
    }

    /// One iteration of the lease monitor. Exposed so a single-process proof can
    /// drive lease expiry deterministically.
    [[nodiscard]] std::size_t expire_leases();

private:
    struct Session {
        PublisherId publisher{};
        WorkerBootId boot{};
        ScopeId scope{};
        CoordinatorEpoch epoch{};
        bool registered{false};
    };

    void handle_connection(const std::shared_ptr<Socket>& peer);
    void monitor_loop();
    [[nodiscard]] Response dispatch(const Frame& frame, Session& session);
    void touch(PublisherId publisher);

    CoordinatorConfig config_{};
    std::unique_ptr<SegmentListStore> store_{};
    LoadReport load_report_{};
    Socket listener_{};
    std::uint16_t port_{0};

    std::atomic<bool> stopping_{false};
    std::atomic<std::uint32_t> connections_served_{0};
    std::vector<std::thread> workers_{};
    std::vector<std::shared_ptr<Socket>> active_peers_{};
    std::thread monitor_{};
    mutable std::mutex workers_mutex_{};

    mutable std::mutex lease_mutex_{};
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> last_seen_{};
};

} // namespace srf
