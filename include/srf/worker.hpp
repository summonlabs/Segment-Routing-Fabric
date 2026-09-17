// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "srf/limits.hpp"
#include "srf/store.hpp"
#include "srf/transport.hpp"
#include "srf/wire.hpp"

namespace srf {

struct WorkerConfig {
    Limits limits{};
    std::filesystem::path port_path{};
    WorkerBootId boot{};
    PublisherId publisher{};
    ScopeId scope{};
    CoordinatorEpoch expected_epoch{};
};

/// Client half of the distributed protocol. The boot identifier is chosen by the
/// caller and must be fresh for every process incarnation: a fenced boot stays
/// fenced forever, so a recycled boot identifier is rejected by the coordinator.
class WorkerClient {
public:
    WorkerClient() = default;
    ~WorkerClient();
    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    [[nodiscard]] bool open(const WorkerConfig& config, ValidationResult& out);
    [[nodiscard]] bool connected() const noexcept { return socket_.valid(); }
    void close();

    [[nodiscard]] Response handshake();
    [[nodiscard]] Response heartbeat();
    [[nodiscard]] Response send_mutation(MessageId message, const ListDraft& draft);
    [[nodiscard]] Response send_lifecycle(LifecycleOp op, SegmentListId list,
                                          SegmentListGeneration expected);
    [[nodiscard]] Response get(SegmentListId list);
    [[nodiscard]] Response summaries();
    [[nodiscard]] Response explain_currentness(SegmentListId list);
    [[nodiscard]] Response refresh_currentness();
    [[nodiscard]] Response goodbye();

    [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return session_epoch_; }
    [[nodiscard]] WorkerSessionId session() const noexcept { return session_; }
    [[nodiscard]] MutationAttemptId next_attempt();
    [[nodiscard]] StatusCode last_transport_status() const noexcept { return transport_status_; }

private:
    [[nodiscard]] Response exchange(MessageId message, const ByteBuffer& payload);

    Socket socket_{};
    WorkerConfig config_{};
    std::uint64_t next_request_{1};
    std::uint64_t attempt_counter_{0};
    WorkerSessionId session_{};
    CoordinatorEpoch session_epoch_{};
    StatusCode transport_status_{StatusCode::Ok};
    std::mutex mutex_{};
};

} // namespace srf
