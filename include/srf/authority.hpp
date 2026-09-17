// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "srf/ids.hpp"
#include "srf/reason.hpp"

namespace srf {

/// Identity of the writer of one mutation. Default deny: every field must be
/// present and must agree with the current authority table.
struct CallerIdentity {
    CoordinatorEpoch epoch{};
    PublisherId publisher{};
    WorkerBootId boot{};
    ScopeId scope{};
    MutationAttemptId attempt{};

    friend bool operator==(const CallerIdentity&, const CallerIdentity&) = default;
};

struct PublisherState {
    PublisherId id{};
    WorkerBootId boot{};
    ScopeId scope{};
    CoordinatorEpoch epoch{};
    bool alive{false};
    bool fenced{false};

    friend bool operator==(const PublisherState&, const PublisherState&) = default;
};

/// Coordinator-owned authority state: the current epoch, the live publishers and
/// the permanently fenced boots.
class AuthorityTable {
public:
    AuthorityTable() = default;

    [[nodiscard]] CoordinatorEpoch epoch() const;
    /// Monotonic, non-wrapping. Returns false when the epoch is exhausted.
    [[nodiscard]] bool advance_epoch();
    /// Restore a persisted epoch. A value lower than the current one is rejected.
    [[nodiscard]] bool restore_epoch(CoordinatorEpoch epoch);

    [[nodiscard]] ValidationResult register_publisher(PublisherId id, WorkerBootId boot,
                                                      ScopeId scope, CoordinatorEpoch epoch,
                                                      const Limits& limits);
    [[nodiscard]] ValidationResult heartbeat(PublisherId id, WorkerBootId boot,
                                             CoordinatorEpoch epoch);
    /// Mark a publisher dead and permanently fence its boot.
    [[nodiscard]] ValidationResult mark_dead(PublisherId id);
    /// Permanently fence a boot id.
    [[nodiscard]] ValidationResult fence_boot(WorkerBootId boot);
    [[nodiscard]] bool is_fenced(WorkerBootId boot) const;
    [[nodiscard]] bool is_alive(PublisherId id) const;

    [[nodiscard]] const PublisherState* find(PublisherId id) const;
    [[nodiscard]] std::vector<PublisherState> publishers() const;
    [[nodiscard]] std::vector<WorkerBootId> fenced_boots() const;
    [[nodiscard]] std::size_t publisher_count() const;

    void restore_publisher(const PublisherState& state);
    void restore_fenced_boot(WorkerBootId boot);
    /// Every publisher is marked not-alive and its boot fenced. Used on recovery:
    /// durable intent survives, live authority never does.
    void fence_all_on_recovery();

private:
    mutable std::mutex mutex_{};
    CoordinatorEpoch epoch_{CoordinatorEpoch::initial()};
    std::vector<PublisherState> publishers_{};
    std::vector<WorkerBootId> fenced_boots_{};
};

} // namespace srf
