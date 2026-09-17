// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "srf/authority.hpp"
#include "srf/evidence.hpp"
#include "srf/list.hpp"
#include "srf/persistence.hpp"
#include "srf/registry.hpp"
#include "srf/snapshot.hpp"
#include "srf/validation.hpp"

namespace srf {

enum class StatusCode : std::uint8_t {
    Ok = 0,
    Rejected = 1,
    Conflict = 2,
    NotFound = 3,
    PersistenceFailed = 4,
    LimitExceeded = 5,
    Exhausted = 6,
    Internal = 7,
};

[[nodiscard]] std::string_view status_code_name(StatusCode code) noexcept;
/// Deterministic mapping from the primary rejection phase to a coarse status.
[[nodiscard]] StatusCode status_for(const ValidationResult& result) noexcept;

/// Exactly what a caller supplies. Nothing here is inferred, searched or repaired.
struct ListDraft {
    SegmentListId id{};
    ScopeId scope{};

    SegmentProfileId profile{};
    /// Zero means "bind the generation that is current when this is validated".
    SegmentProfileGeneration profile_generation{};
    SegmentPolicyId policy{};
    SegmentPolicyGeneration policy_generation{};

    Strictness strictness{Strictness::Strict};

    /// Required when any ADJACENCY segment is present.
    TopologyGeneration topology{};
    /// Zero means "bind the generation that is current when this is validated".
    CapabilityGeneration capability{};

    PathId path{};
    PathAuthorityGeneration path_authority{};
    RouteId route{};
    RouteGeneration route_generation{};
    ConstraintEvaluationId constraint{};
    ConstraintGeneration constraint_generation{};

    std::vector<Segment> segments{};

    /// Zero means "this is a create". A non-zero value must match the current
    /// generation of the existing revision.
    SegmentListGeneration expected_generation{};

    /// When set, the named existing lineage is superseded by this create, in the
    /// same atomic commit. A retired or revoked lineage can never be superseded.
    SegmentListId supersedes{};
    SegmentListGeneration supersedes_generation{};

    /// Set when the list was derived from explicit external intent. Derivation
    /// never performs a search, and it is refused unless policy permits it.
    bool derived{false};
};

struct MutationOutcome {
    StatusCode status{StatusCode::Ok};
    ValidationResult result{};
    SegmentListId list{};
    SegmentListGeneration generation{};
    Digest128 digest{};
    SnapshotId snapshot{};
    bool idempotent_replay{false};
    bool durable{false};

    [[nodiscard]] bool ok() const noexcept { return status == StatusCode::Ok; }
};

/// A bounded view of the durable lineage history.
///
/// History is a bounded audit trail, not a complete log: once at_capacity is true
/// the oldest entries are no longer retained. Structural validity, canonical bytes,
/// digests, lifecycle, currentness and replay are never derived from history, so
/// bounded retention cannot change any of them.
struct HistoryReport {
    std::vector<DurableHistoryEntry> entries{};
    std::uint32_t retained{0};
    std::uint32_t limit{0};
    /// True once the retained window has reached the configured bound, which means
    /// older entries were dropped and this view is not a complete record.
    bool at_capacity{false};

    [[nodiscard]] bool complete() const noexcept { return !at_capacity; }
};

struct CurrentnessChange {
    SegmentListId list{};
    SegmentListGeneration generation{};
    Currentness was{Currentness::Unknown};
    Currentness now{Currentness::Unknown};
};

struct CurrentnessReport {
    std::size_t examined{0};
    std::size_t invalidated{0};
    std::vector<CurrentnessChange> changes{};
};

struct StoreConfig {
    Limits limits{};
    std::shared_ptr<IEvidenceSource> evidence{};
    std::shared_ptr<IPersistence> persistence{};
};

/// The governed segment-list store.
///
/// Two-phase validation: dependency generations are captured, validation runs
/// outside the store lock, the lock is reacquired, every generation and watermark
/// is re-verified, and only then is the change committed atomically. A mutation is
/// never acknowledged before it is durable.
class SegmentListStore {
public:
    explicit SegmentListStore(StoreConfig config);
    ~SegmentListStore() = default;
    SegmentListStore(const SegmentListStore&) = delete;
    SegmentListStore& operator=(const SegmentListStore&) = delete;

    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    [[nodiscard]] AuthorityTable& authority() noexcept { return authority_; }
    [[nodiscard]] const AuthorityTable& authority() const noexcept { return authority_; }
    [[nodiscard]] Registry& registry() noexcept { return registry_; }
    [[nodiscard]] const Registry& registry() const noexcept { return registry_; }
    [[nodiscard]] IEvidenceSource* evidence() noexcept { return evidence_.get(); }

    [[nodiscard]] ValidationResult register_profile(const SegmentProfile& profile);
    [[nodiscard]] ValidationResult register_policy(const SegmentPolicy& policy);

    [[nodiscard]] MutationOutcome create_list(const CallerIdentity& caller, const ListDraft& draft);
    [[nodiscard]] MutationOutcome replace_list(const CallerIdentity& caller, const ListDraft& draft);
    [[nodiscard]] MutationOutcome batch(const CallerIdentity& caller,
                                        std::span<const ListDraft> drafts);
    /// Two-phase withdrawal: withdraw() moves ACTIVE to WITHDRAWING and removes
    /// availability immediately; withdraw_commit() finalises WITHDRAWN.
    [[nodiscard]] MutationOutcome withdraw(const CallerIdentity& caller, SegmentListId id,
                                           SegmentListGeneration expected);
    [[nodiscard]] MutationOutcome withdraw_commit(const CallerIdentity& caller, SegmentListId id,
                                                  SegmentListGeneration expected);
    [[nodiscard]] MutationOutcome revoke(const CallerIdentity& caller, SegmentListId id,
                                         SegmentListGeneration expected);
    [[nodiscard]] MutationOutcome retire(const CallerIdentity& caller, SegmentListId id,
                                         SegmentListGeneration expected);
    [[nodiscard]] MutationOutcome revalidate(const CallerIdentity& caller, SegmentListId id,
                                             SegmentListGeneration expected);

    [[nodiscard]] std::optional<SegmentList> get(SegmentListId id) const;
    [[nodiscard]] std::vector<SegmentList> lists() const;
    [[nodiscard]] std::size_t list_count() const;
    [[nodiscard]] std::size_t total_segments() const;

    [[nodiscard]] SegmentListSnapshot snapshot(SegmentListId id) const;
    [[nodiscard]] SnapshotDiff diff(const SegmentListSnapshot& from,
                                    const SegmentListSnapshot& to) const;

    [[nodiscard]] Explanation explain(SegmentListId id) const;
    [[nodiscard]] Explanation explain_segment(SegmentListId id, std::uint32_t index) const;
    [[nodiscard]] Explanation explain_currentness(SegmentListId id) const;
    [[nodiscard]] Explanation explain_supersession(SegmentListId id) const;
    [[nodiscard]] Explanation explain_missing_capability(SegmentListId id) const;
    [[nodiscard]] Explanation explain_stale_topology(SegmentListId id) const;

    /// Re-evaluate every non-terminal list against the current dependency
    /// generations. Never repairs, never reactivates a fenced lineage.
    [[nodiscard]] CurrentnessReport refresh_currentness();

    /// Conservative recovery. Durable intent survives; live authority and
    /// unproven currentness do not.
    [[nodiscard]] LoadReport load();

    /// Write the current state durably without changing any list. Used after a
    /// coordinator raises its epoch so that the new epoch is durable before the
    /// coordinator starts serving.
    [[nodiscard]] ValidationResult persist_now();

    [[nodiscard]] std::vector<DurableHistoryEntry> history() const;
    /// The same entries plus the retained/limit/at_capacity indicator that makes
    /// bounded retention explicit to the caller.
    [[nodiscard]] HistoryReport history_report() const;
    [[nodiscard]] DurableState export_state() const;
    [[nodiscard]] std::uint64_t revision() const;

    /// Deterministic race injection point: runs after a successful validation and
    /// before the commit watermark re-check reacquires the lock.
    void set_precommit_hook(std::function<void()> hook);

private:
    struct Locked {
        std::unique_lock<std::mutex> lock;
        DurableState& state;
    };

    /// existing_intent is the event the current revision of the lineage must
    /// accept. The committed revision is always a new revision and follows the
    /// DECLARED -> VALIDATING -> ACTIVE path.
    [[nodiscard]] MutationOutcome apply(const CallerIdentity& caller, const ListDraft& draft,
                                        LifecycleEvent existing_intent, bool require_existing,
                                        bool require_absent);
    [[nodiscard]] MutationOutcome run_batch(const CallerIdentity& caller,
                                            std::span<const ListDraft> drafts,
                                            LifecycleEvent existing_intent, bool require_existing,
                                            bool require_absent);
    [[nodiscard]] MutationOutcome transition(const CallerIdentity& caller, SegmentListId id,
                                             SegmentListGeneration expected,
                                             LifecycleEvent event);
    [[nodiscard]] bool persist_locked(const DurableState& next, ValidationResult& out);
    [[nodiscard]] AuthorityView authority_view() const;
    [[nodiscard]] bool index_consistent_locked() const;
    void rebuild_index_locked();
    [[nodiscard]] const SegmentList* find_locked(SegmentListId id,
                                                 std::size_t* index_out = nullptr) const;
    [[nodiscard]] SegmentList* find_mutable_locked(SegmentListId id);
    void record_attempt_locked(DurableState& state, const CallerIdentity& caller,
                               SegmentListId list, const Digest128& payload,
                               SegmentListGeneration generation);
    void append_history_locked(DurableState& state, const SegmentList& list);
    [[nodiscard]] const DurableAttemptRecord* find_attempt_locked(const DurableState& state,
                                                                  MutationAttemptId attempt) const;
    /// A mutation attempt is acceptable when it is already recorded, or when the
    /// bounded replay table still has room. The table is never silently evicted:
    /// forgetting a record would let a replay be treated as a fresh mutation and
    /// would let a reused attempt identifier escape detection.
    [[nodiscard]] bool attempt_table_accepts_locked(MutationAttemptId attempt) const noexcept;

    Limits limits_{};
    std::shared_ptr<IEvidenceSource> evidence_{};
    std::shared_ptr<IPersistence> persistence_{};
    AuthorityTable authority_{};
    Registry registry_{};

    mutable std::mutex mutex_{};
    DurableState state_{};
    std::vector<std::pair<SegmentListId, std::size_t>> index_{};
    mutable std::atomic<std::uint64_t> snapshot_counter_{1};
    std::function<void()> precommit_hook_{};
};

/// Deterministic identity digest of a draft, independent of any profile. Used for
/// mutation-attempt replay detection.
[[nodiscard]] Digest128 draft_payload_digest(const ListDraft& draft);

} // namespace srf
