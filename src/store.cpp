// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/store.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "srf/canonical.hpp"

namespace srf {
namespace {

[[nodiscard]] bool advance_state(LifecycleState& state, LifecycleEvent event) noexcept {
    const TransitionOutcome outcome = lifecycle_transition(state, event);
    if (!outcome.allowed) {
        return false;
    }
    state = outcome.next;
    return true;
}

[[nodiscard]] bool sequence_has_adjacency(std::span<const Segment> segments) noexcept {
    for (const Segment& s : segments) {
        if (s.kind == SegmentKind::Adjacency) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] HistoryId next_history_id(const DurableState& state) noexcept {
    if (state.history.empty()) {
        return HistoryId{1};
    }
    const std::uint64_t last = state.history.back().id.value();
    if (last >= std::numeric_limits<std::uint64_t>::max()) {
        return HistoryId{0};
    }
    return HistoryId{last + 1};
}

struct Resolved {
    SegmentList existing{};
    bool has_existing{false};
    SegmentProfile profile{};
    bool profile_found{false};
    SegmentPolicy policy{};
    bool policy_found{false};
    SegmentList superseded{};
    bool has_superseded{false};
};

[[nodiscard]] SegmentList* find_in_state(DurableState& state, SegmentListId id) noexcept {
    for (SegmentList& list : state.lists) {
        if (list.content.id == id) {
            return &list;
        }
    }
    return nullptr;
}

[[nodiscard]] const SegmentList* find_in_state(const DurableState& state,
                                               SegmentListId id) noexcept {
    for (const SegmentList& list : state.lists) {
        if (list.content.id == id) {
            return &list;
        }
    }
    return nullptr;
}

struct PreparedContext {
    const Limits* limits{nullptr};
    AuthorityView authority{};
    CoordinatorEpoch epoch{};
    std::uint64_t registry_revision{0};
    std::uint64_t store_revision{0};
    EvidenceWatermark watermark{};
    FabricEvidence evidence{};
};

struct PreparedMutation {
    SegmentListId id{};
    bool has_existing{false};
    SegmentListGeneration existing_generation{};
    bool has_superseded{false};
    SegmentListId superseded_id{};
    SegmentListGeneration superseded_generation{};
    SegmentList list{};
    bool replaces{false};
};

void build_request(const CallerIdentity& caller, const ListDraft& draft, LifecycleEvent intent,
                   bool full, const PreparedContext& ctx, const Resolved& res,
                   ValidationRequest& request) {
    request = ValidationRequest{};
    request.limits = ctx.limits;
    request.caller = &caller;
    request.authority = &ctx.authority;
    request.list_id = draft.id;
    request.scope = draft.scope;
    request.existing = res.has_existing ? &res.existing : nullptr;
    request.expected_generation = draft.expected_generation;
    request.intent = intent;
    request.full = full;
    request.profile_id = draft.profile;
    request.profile_generation = draft.profile_generation;
    request.profile = res.profile_found ? &res.profile : nullptr;
    request.profile_found = res.profile_found;
    request.policy_id = draft.policy;
    request.policy_generation = draft.policy_generation;
    request.policy = res.policy_found ? &res.policy : nullptr;
    request.policy_found = res.policy_found;
    request.policy_required = draft.policy.valid();
    request.strictness = draft.strictness;
    request.topology = draft.topology;
    request.capability = draft.capability;
    request.path = draft.path;
    request.path_authority = draft.path_authority;
    request.route = draft.route;
    request.route_generation = draft.route_generation;
    request.constraint = draft.constraint;
    request.constraint_generation = draft.constraint_generation;
    request.segments = draft.segments;
    request.derived = draft.derived;
    request.evidence = &ctx.evidence;
}

/// Resolve the exact dependency generations this revision binds. Bindings are
/// captured, never guessed: a dependency that is not required by the content is
/// left unbound so that its later movement cannot invalidate this list.
[[nodiscard]] SegmentList make_list(const CallerIdentity& caller, const ListDraft& draft,
                                    const PreparedContext& ctx, const Resolved& res,
                                    const SegmentList* previous, CoordinatorEpoch epoch,
                                    std::uint64_t committed_revision) {
    SegmentList list{};
    SegmentListContent& content = list.content;
    content.id = draft.id;
    content.scope = draft.scope;
    content.profile = res.profile.id;
    content.profile_generation = res.profile.generation;
    content.policy = draft.policy;
    content.policy_generation = res.policy_found ? res.policy.generation
                                                 : SegmentPolicyGeneration{};
    content.strictness = draft.strictness;

    const bool adjacency_bound = sequence_has_adjacency(draft.segments);
    if (draft.topology.valid()) {
        content.topology = draft.topology;
    } else if (adjacency_bound) {
        content.topology = ctx.evidence.topology;
    }
    if (draft.capability.valid()) {
        content.capability = draft.capability;
    } else if (!res.profile.required_capabilities.empty()) {
        content.capability = ctx.evidence.capability;
    }
    content.path = draft.path;
    content.path_authority = draft.path_authority;
    content.route = draft.route;
    content.route_generation = draft.route_generation;
    content.constraint = draft.constraint;
    content.constraint_generation = draft.constraint_generation;
    content.segments = canonicalize_segments(draft.segments);

    list.generation = previous != nullptr ? SegmentListGeneration{previous->generation.value() + 1}
                                          : SegmentListGeneration::initial();
    list.lineage = SegmentListLineage{};
    list.lineage.supersedes = previous != nullptr ? previous->generation : SegmentListGeneration{};
    list.authority = SegmentListAuthority{caller.publisher, caller.boot, epoch, caller.attempt};
    list.provenance = SegmentListProvenance{};
    list.provenance.source_path = draft.path;
    list.provenance.source_path_authority = draft.path_authority;
    list.provenance.source_route = draft.route;
    list.provenance.source_route_generation = draft.route_generation;
    list.provenance.source_constraint = draft.constraint;
    list.provenance.source_constraint_generation = draft.constraint_generation;
    list.provenance.derivation_policy = content.policy_generation;
    list.provenance.observed_topology = content.topology;
    list.provenance.observed_capability = content.capability;
    list.provenance.publisher = caller.publisher;
    list.provenance.boot = caller.boot;
    list.provenance.epoch = epoch;

    list.state = LifecycleState::Declared;
    list.currentness = Currentness::Unknown;
    list.committed_revision = committed_revision;
    list.sequence_digest = sequence_digest(content.segments, res.profile.payload_width);
    list.content_digest = content_digest(content, res.profile.payload_width);
    // A committed revision is always a new revision, so its own state path is
    // DECLARED -> VALIDATING -> ACTIVE regardless of which event the *previous*
    // revision had to accept.
    if (!advance_state(list.state, LifecycleEvent::ValidateBegin) ||
        !advance_state(list.state, LifecycleEvent::ValidateSucceed)) {
        list.state = LifecycleState::Declared;
    }
    list.currentness = Currentness::Current;
    return list;
}

} // namespace

std::string_view status_code_name(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::Rejected: return "Rejected";
        case StatusCode::Conflict: return "Conflict";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::PersistenceFailed: return "PersistenceFailed";
        case StatusCode::LimitExceeded: return "LimitExceeded";
        case StatusCode::Exhausted: return "Exhausted";
        case StatusCode::Internal: return "Internal";
    }
    return "Unknown";
}

StatusCode status_for(const ValidationResult& result) noexcept {
    if (result.ok()) {
        return StatusCode::Ok;
    }
    const ReasonCode primary = result.primary().code;
    switch (primary) {
        case ReasonCode::CommitPersistenceFailure:
        case ReasonCode::CommitNotDurable:
            return StatusCode::PersistenceFailed;
        case ReasonCode::CommitGenerationExhausted:
        case ReasonCode::ListGenerationExhausted:
        case ReasonCode::EpochExhausted:
            return StatusCode::Exhausted;
        case ReasonCode::ListGenerationConflict:
        case ReasonCode::CommitConflict:
        case ReasonCode::CommitWatermarkChanged:
            return StatusCode::Conflict;
        case ReasonCode::ListNotFound:
        case ReasonCode::ListSupersedesUnknown:
        case ReasonCode::CallerUnknownPublisher:
        case ReasonCode::ProfileUnknown:
        case ReasonCode::PolicyUnknown:
        case ReasonCode::PathUnknown:
        case ReasonCode::RouteBindingUnknown:
        case ReasonCode::ConstraintBindingUnknown:
        case ReasonCode::TopologyAdjacencyUnknown:
        case ReasonCode::SegmentEntityUnknown:
            return StatusCode::NotFound;
        default:
            break;
    }
    switch (result.primary_phase()) {
        case ValidationPhase::ResourceLimits:
            return StatusCode::LimitExceeded;
        case ValidationPhase::Commit:
            return StatusCode::Conflict;
        default:
            return StatusCode::Rejected;
    }
}

Digest128 draft_payload_digest(const ListDraft& draft) {
    ByteWriter w;
    w.u16(0xD101);
    w.u64(draft.id.value());
    w.u64(draft.scope.value());
    w.u64(draft.profile.value());
    w.u64(draft.profile_generation.value());
    w.u64(draft.policy.value());
    w.u64(draft.policy_generation.value());
    w.u8(static_cast<std::uint8_t>(draft.strictness));
    w.u64(draft.topology.value());
    w.u64(draft.capability.value());
    w.u64(draft.path.value());
    w.u64(draft.path_authority.value());
    w.u64(draft.route.value());
    w.u64(draft.route_generation.value());
    w.u64(draft.constraint.value());
    w.u64(draft.constraint_generation.value());
    w.u8(draft.derived ? 1 : 0);
    w.u64(draft.expected_generation.value());
    w.u64(draft.supersedes.value());
    w.u64(draft.supersedes_generation.value());
    std::uint32_t width = 0;
    for (const Segment& s : draft.segments) {
        width = std::max(width, static_cast<std::uint32_t>(s.payload.size()));
    }
    w.u32(width);
    w.u32(static_cast<std::uint32_t>(draft.segments.size()));
    for (Segment s : draft.segments) {
        if (s.encoding == SegmentEncodingId::Invalid) {
            s.encoding = expected_encoding_for(s.kind);
        }
        w.bytes(as_bytes(canonical_segment_bytes(s, width == 0 ? 1u : width)));
    }
    return digest128(w.span());
}

SegmentListStore::SegmentListStore(StoreConfig config)
    : limits_(config.limits),
      evidence_(std::move(config.evidence)),
      persistence_(std::move(config.persistence)) {
    state_.format_version = kPersistenceFormatVersion;
    state_.epoch = CoordinatorEpoch::initial();
    state_.revision = 0;
    const bool restored = authority_.restore_epoch(CoordinatorEpoch::initial());
    (void)restored;
    rebuild_index_locked();
}

ValidationResult SegmentListStore::register_profile(const SegmentProfile& profile) {
    ValidationResult result = registry_.register_profile(profile, limits_);
    if (result.failed()) {
        return result;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    DurableState next = state_;
    next.profiles = registry_.profiles();
    next.policies = registry_.policies();
    next.publishers = authority_.publishers();
    next.fenced_boots = authority_.fenced_boots();
    if (!persist_locked(next, result)) {
        return result;
    }
    next.revision = state_.revision + 1;
    state_ = std::move(next);
    rebuild_index_locked();
    return result;
}

ValidationResult SegmentListStore::register_policy(const SegmentPolicy& policy) {
    ValidationResult result = registry_.register_policy(policy, limits_);
    if (result.failed()) {
        return result;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    DurableState next = state_;
    next.profiles = registry_.profiles();
    next.policies = registry_.policies();
    next.publishers = authority_.publishers();
    next.fenced_boots = authority_.fenced_boots();
    if (!persist_locked(next, result)) {
        return result;
    }
    next.revision = state_.revision + 1;
    state_ = std::move(next);
    rebuild_index_locked();
    return result;
}

bool SegmentListStore::persist_locked(const DurableState& next, ValidationResult& out) {
    out.set_limits(&limits_);
    if (!persistence_) {
        return true;
    }
    if (!persistence_->save(next, limits_, out)) {
        if (out.ok()) {
            out.add(ReasonCode::CommitPersistenceFailure);
        }
        return false;
    }
    return true;
}

AuthorityView SegmentListStore::authority_view() const {
    AuthorityView view{};
    view.epoch = authority_.epoch();
    view.publishers = authority_.publishers();
    view.fenced_boots = authority_.fenced_boots();
    return view;
}

void SegmentListStore::rebuild_index_locked() {
    index_.clear();
    index_.reserve(state_.lists.size());
    for (std::size_t i = 0; i < state_.lists.size(); ++i) {
        index_.emplace_back(state_.lists[i].content.id, i);
    }
    std::sort(index_.begin(), index_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

bool SegmentListStore::index_consistent_locked() const {
    if (index_.size() != state_.lists.size()) {
        return false;
    }
    for (std::size_t i = 0; i < index_.size(); ++i) {
        if (index_[i].second >= state_.lists.size()) {
            return false;
        }
        if (state_.lists[index_[i].second].content.id != index_[i].first) {
            return false;
        }
        if (i > 0 && !(index_[i - 1].first < index_[i].first)) {
            return false;
        }
    }
    return true;
}

const SegmentList* SegmentListStore::find_locked(SegmentListId id, std::size_t* index_out) const {
    const auto it = std::lower_bound(
        index_.begin(), index_.end(), id,
        [](const std::pair<SegmentListId, std::size_t>& entry, SegmentListId value) {
            return entry.first < value;
        });
    if (it == index_.end() || it->first != id) {
        return nullptr;
    }
    if (index_out != nullptr) {
        *index_out = it->second;
    }
    return &state_.lists[it->second];
}

SegmentList* SegmentListStore::find_mutable_locked(SegmentListId id) {
    std::size_t index = 0;
    if (find_locked(id, &index) == nullptr) {
        return nullptr;
    }
    return &state_.lists[index];
}

void SegmentListStore::record_attempt_locked(DurableState& state, const CallerIdentity& caller,
                                             SegmentListId list, const Digest128& payload,
                                             SegmentListGeneration generation) {
    for (auto& record : state.attempts) {
        if (record.attempt == caller.attempt) {
            record.list = list;
            record.payload = payload;
            record.generation = generation;
            record.committed = true;
            return;
        }
    }
    if (state.attempts.size() >= limits_.max_attempt_records) {
        state.attempts.erase(state.attempts.begin());
    }
    state.attempts.push_back(DurableAttemptRecord{caller.attempt, list, payload, generation, true});
}

const DurableAttemptRecord* SegmentListStore::find_attempt_locked(const DurableState& state,
                                                                  MutationAttemptId attempt) const {
    for (const auto& record : state.attempts) {
        if (record.attempt == attempt) {
            return &record;
        }
    }
    return nullptr;
}

void SegmentListStore::append_history_locked(DurableState& state, const SegmentList& list) {
    const HistoryId id = next_history_id(state);
    if (!id.valid()) {
        return;
    }
    if (state.history.size() >= limits_.max_history) {
        state.history.erase(state.history.begin());
    }
    DurableHistoryEntry entry{};
    entry.id = id;
    entry.list = list.content.id;
    entry.generation = list.generation;
    entry.state = list.state;
    entry.currentness = list.currentness;
    entry.digest = list.content_digest;
    entry.revision = state.revision + 1;
    state.history.push_back(entry);
}

void SegmentListStore::set_precommit_hook(std::function<void()> hook) {
    std::unique_lock<std::mutex> lock(mutex_);
    precommit_hook_ = std::move(hook);
}

MutationOutcome SegmentListStore::apply(const CallerIdentity& caller, const ListDraft& draft,
                                        LifecycleEvent existing_intent, bool require_existing,
                                        bool require_absent) {
    return run_batch(caller, std::span<const ListDraft>(&draft, 1), existing_intent,
                     require_existing, require_absent);
}

MutationOutcome SegmentListStore::batch(const CallerIdentity& caller,
                                        std::span<const ListDraft> drafts) {
    return run_batch(caller, drafts, LifecycleEvent::ReplaceBegin, false, false);
}

MutationOutcome SegmentListStore::run_batch(const CallerIdentity& caller,
                                            std::span<const ListDraft> drafts,
                                            LifecycleEvent existing_intent, bool require_existing,
                                            bool require_absent) {
    MutationOutcome outcome;
    outcome.result.set_limits(&limits_);
    outcome.list = drafts.empty() ? SegmentListId{} : drafts[0].id;

    if (drafts.empty()) {
        outcome.status = StatusCode::Rejected;
        outcome.result.add(ReasonCode::InvalidArgument);
        return outcome;
    }
    if (drafts.size() > limits_.max_batch_size) {
        outcome.status = StatusCode::LimitExceeded;
        outcome.result.add(ReasonCode::LimitMaxBatchSize, 0, limits_.max_batch_size);
        return outcome;
    }

    // ---- phase A: snapshot the dependency generations, outside the store lock.
    PreparedContext ctx{};
    ctx.limits = &limits_;
    if (evidence_) {
        ctx.watermark = evidence_->watermark();
        ctx.evidence = evidence_->snapshot();
        if (ctx.evidence.capabilities.size() + ctx.evidence.segments.size() +
                ctx.evidence.nodes.size() + ctx.evidence.adjacencies.size() +
                ctx.evidence.endpoints.size() + ctx.evidence.bindings.size() +
                ctx.evidence.paths.size() + ctx.evidence.routes.size() +
                ctx.evidence.constraints.size() >
            limits_.max_evidence_records) {
            outcome.status = StatusCode::LimitExceeded;
            outcome.result.add(ReasonCode::LimitMaxEvidenceRecords, 0, limits_.max_evidence_records);
            return outcome;
        }
    }

    // The mutation-attempt identity digest depends only on the drafts.
    Digest128 payload_digest{};
    {
        ByteWriter w;
        w.u16(0xD102);
        for (const ListDraft& draft : drafts) {
            const Digest128 one = draft_payload_digest(draft);
            w.u64(one.hi);
            w.u64(one.lo);
        }
        payload_digest = digest128(w.span());
    }

    // ---- phase B: capture store state under the lock, then release it.
    std::vector<Resolved> resolved(drafts.size());
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (caller.attempt.valid()) {
            if (const DurableAttemptRecord* record =
                    find_attempt_locked(state_, caller.attempt)) {
                if (record->payload == payload_digest) {
                    // Exact replay of unchanged semantics: idempotent, advances
                    // nothing and writes nothing.
                    outcome.status = StatusCode::Ok;
                    outcome.idempotent_replay = true;
                    outcome.durable = persistence_ != nullptr;
                    for (const ListDraft& draft : drafts) {
                        const SegmentList* current = find_locked(draft.id);
                        if (current == nullptr) {
                            continue;
                        }
                        outcome.list = draft.id;
                        outcome.generation = current->generation;
                        outcome.digest = current->content_digest;
                    }
                    outcome.snapshot = SnapshotId{snapshot_counter_.fetch_add(1)};
                    return outcome;
                }
                outcome.status = StatusCode::Conflict;
                outcome.result.add(ReasonCode::CommitReplayPayloadMismatch, 0,
                                   caller.attempt.value());
                return outcome;
            }
        }
        ctx.authority = authority_view();
        ctx.epoch = ctx.authority.epoch;
        ctx.registry_revision = registry_.revision();
        ctx.store_revision = state_.revision;
        for (std::size_t i = 0; i < drafts.size(); ++i) {
            const ListDraft& draft = drafts[i];
            Resolved& res = resolved[i];
            if (const SegmentList* existing = find_locked(draft.id)) {
                res.existing = *existing;
                res.has_existing = true;
            }
            res.profile_found = registry_.find_profile(draft.profile, res.profile);
            if (draft.policy.valid()) {
                res.policy_found = registry_.find_policy(draft.policy, res.policy);
            }
            if (draft.supersedes.valid()) {
                if (const SegmentList* superseded = find_locked(draft.supersedes)) {
                    res.superseded = *superseded;
                    res.has_superseded = true;
                }
            }
        }
    }

    // ---- phase C: validate outside the lock.
    std::vector<PreparedMutation> prepared(drafts.size());
    ValidationResult combined;
    combined.set_limits(&limits_);
    for (std::size_t i = 0; i < drafts.size(); ++i) {
        const ListDraft& draft = drafts[i];
        ValidationRequest request{};
        build_request(caller, draft, existing_intent, true, ctx, resolved[i], request);
        request.require_existing = require_existing;
        request.require_absent = require_absent;
        ValidationResult result = validate_request(request);
        if (resolved[i].has_existing) {
            if (draft.expected_generation != resolved[i].existing.generation) {
                result.add(ReasonCode::ListGenerationConflict, 0,
                           resolved[i].existing.generation.value());
            }
        }
        if (draft.supersedes.valid()) {
            if (!resolved[i].has_superseded) {
                result.add(ReasonCode::ListSupersedesUnknown, 0, draft.supersedes.value());
            } else {
                if (draft.supersedes_generation != resolved[i].superseded.generation) {
                    result.add(ReasonCode::ListGenerationConflict, 0,
                               resolved[i].superseded.generation.value());
                }
                const TransitionOutcome t =
                    lifecycle_transition(resolved[i].superseded.state, LifecycleEvent::SupersedeBegin);
                if (!t.allowed) {
                    if (resolved[i].superseded.state == LifecycleState::Retired) {
                        result.add(ReasonCode::ListRetired, 0,
                                   resolved[i].superseded.generation.value());
                    } else if (resolved[i].superseded.state == LifecycleState::Revoked) {
                        result.add(ReasonCode::ListRevoked, 0,
                                   resolved[i].superseded.generation.value());
                    }
                    result.add(ReasonCode::LifecycleTransitionDenied, 0,
                               resolved[i].superseded.generation.value());
                }
            }
        }
        if (result.failed()) {
            combined.absorb(result);
            continue;
        }
        PreparedMutation& pm = prepared[i];
        pm.id = draft.id;
        pm.has_existing = resolved[i].has_existing;
        pm.existing_generation =
            resolved[i].has_existing ? resolved[i].existing.generation : SegmentListGeneration{};
        pm.has_superseded = resolved[i].has_superseded;
        pm.superseded_id = draft.supersedes;
        pm.superseded_generation = resolved[i].has_superseded
                                       ? resolved[i].superseded.generation
                                       : SegmentListGeneration{};
        pm.replaces = resolved[i].has_existing;
        const SegmentList* previous = resolved[i].has_existing ? &resolved[i].existing : nullptr;
        if (previous != nullptr && previous->generation.exhausted()) {
            combined.absorb(ValidationResult::failure(ReasonCode::ListGenerationExhausted, 0,
                                                      previous->generation.value()));
            continue;
        }
        pm.list = make_list(caller, draft, ctx, resolved[i], previous, ctx.epoch,
                            ctx.store_revision + 1);
        if (pm.list.generation.value() == 0 || !pm.list.generation.valid()) {
            combined.absorb(ValidationResult::failure(ReasonCode::CommitGenerationExhausted));
        }
    }
    if (combined.failed()) {
        outcome.result.absorb(combined);
        outcome.status = status_for(outcome.result);
        return outcome;
    }

    // ---- phase D: deterministic race injection point (tests and proofs only).
    std::function<void()> hook;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        hook = precommit_hook_;
    }
    if (hook) {
        hook();
    }

    // ---- phase E: reacquire, re-verify every generation and watermark, commit.
    std::unique_lock<std::mutex> lock(mutex_);
    ValidationResult verify;
    verify.set_limits(&limits_);
    if (state_.revision != ctx.store_revision) {
        verify.add(ReasonCode::CommitWatermarkChanged, 0, state_.revision);
    }
    if (registry_.revision() != ctx.registry_revision) {
        verify.add(ReasonCode::CommitWatermarkChanged, 0, registry_.revision());
    }
    if (authority_.epoch() != ctx.epoch) {
        verify.add(ReasonCode::EpochStale, 0, authority_.epoch().value());
    }
    if (evidence_) {
        if (!(evidence_->watermark() == ctx.watermark)) {
            verify.add(ReasonCode::CommitWatermarkChanged, 0, evidence_->watermark().revision);
        }
    }
    for (const PreparedMutation& pm : prepared) {
        const SegmentList* current = find_locked(pm.id);
        if (pm.has_existing) {
            if (current == nullptr || current->generation != pm.existing_generation) {
                verify.add(ReasonCode::ListGenerationConflict, 0, pm.id.value());
            }
        } else if (current != nullptr) {
            verify.add(ReasonCode::ListAlreadyExists, 0, pm.id.value());
        }
        if (pm.has_superseded) {
            const SegmentList* other = find_locked(pm.superseded_id);
            if (other == nullptr || other->generation != pm.superseded_generation) {
                verify.add(ReasonCode::ListGenerationConflict, 0, pm.superseded_id.value());
            }
        }
    }
    if (verify.failed()) {
        outcome.result.absorb(verify);
        outcome.status = status_for(outcome.result);
        return outcome;
    }

    DurableState next = state_;
    std::size_t new_lists = 0;
    for (const PreparedMutation& pm : prepared) {
        if (!pm.has_existing) {
            ++new_lists;
        }
    }
    if (next.lists.size() + new_lists > limits_.max_lists) {
        outcome.status = StatusCode::LimitExceeded;
        outcome.result.add(ReasonCode::LimitMaxLists, 0, limits_.max_lists);
        return outcome;
    }
    std::uint64_t projected_segments = 0;
    for (const SegmentList& existing : next.lists) {
        projected_segments += existing.content.segments.size();
    }
    for (const PreparedMutation& pm : prepared) {
        if (!pm.has_existing) {
            projected_segments += pm.list.content.segments.size();
        }
    }
    if (projected_segments > limits_.max_total_segments) {
        outcome.status = StatusCode::LimitExceeded;
        outcome.result.add(ReasonCode::LimitMaxTotalSegments, 0, limits_.max_total_segments);
        return outcome;
    }

    const SnapshotId snapshot_id{snapshot_counter_.fetch_add(1)};
    next.revision = state_.revision + 1;
    for (const PreparedMutation& pm : prepared) {
        if (pm.has_superseded) {
            SegmentList* other = find_in_state(next, pm.superseded_id);
            if (other != nullptr) {
                if (!advance_state(other->state, LifecycleEvent::SupersedeBegin)) {
                    outcome.status = StatusCode::Rejected;
                    outcome.result.add(ReasonCode::LifecycleTransitionDenied, 0,
                                       pm.superseded_id.value());
                    return outcome;
                }
                other->lineage.superseded_by = pm.id;
                other->currentness = Currentness::Unknown;
                other->committed_revision = next.revision;
                append_history_locked(next, *other);
            }
        }
        SegmentList* slot = find_in_state(next, pm.id);
        if (slot != nullptr) {
            SegmentList replaced = *slot;
            replaced.state = LifecycleState::Superseded;
            replaced.currentness = Currentness::Unknown;
            append_history_locked(next, replaced);
            *slot = pm.list;
        } else {
            next.lists.push_back(pm.list);
        }
        append_history_locked(next, pm.list);
        record_attempt_locked(next, caller, pm.id, payload_digest, pm.list.generation);
    }
    next.profiles = registry_.profiles();
    next.policies = registry_.policies();
    next.publishers = authority_.publishers();
    next.fenced_boots = authority_.fenced_boots();
    next.epoch = ctx.epoch;

    if (!persist_locked(next, verify)) {
        outcome.result.absorb(verify);
        outcome.status = StatusCode::PersistenceFailed;
        return outcome;
    }

    state_ = std::move(next);
    rebuild_index_locked();
    if (!index_consistent_locked()) {
        outcome.status = StatusCode::Internal;
        outcome.result.add(ReasonCode::PersistIndexInconsistent);
        return outcome;
    }

    outcome.status = StatusCode::Ok;
    outcome.durable = persistence_ != nullptr;
    const PreparedMutation& last = prepared.back();
    outcome.list = last.id;
    if (const SegmentList* committed = find_locked(last.id)) {
        outcome.generation = committed->generation;
        outcome.digest = committed->content_digest;
    }
    outcome.snapshot = snapshot_id;
    return outcome;
}

MutationOutcome SegmentListStore::create_list(const CallerIdentity& caller, const ListDraft& draft) {
    return apply(caller, draft, LifecycleEvent::ValidateBegin, false, true);
}

MutationOutcome SegmentListStore::replace_list(const CallerIdentity& caller, const ListDraft& draft) {
    return apply(caller, draft, LifecycleEvent::ReplaceBegin, true, false);
}

MutationOutcome SegmentListStore::revalidate(const CallerIdentity& caller, SegmentListId id,
                                             SegmentListGeneration expected) {
    ListDraft draft{};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const SegmentList* existing = find_locked(id);
        if (existing == nullptr) {
            MutationOutcome outcome;
            outcome.result.set_limits(&limits_);
            outcome.status = StatusCode::NotFound;
            outcome.result.add(ReasonCode::ListNotFound, 0, id.value());
            return outcome;
        }
        draft.id = id;
        draft.scope = existing->content.scope;
        draft.profile = existing->content.profile;
        draft.policy = existing->content.policy;
        draft.strictness = existing->content.strictness;
        draft.path = existing->content.path;
        draft.route = existing->content.route;
        draft.constraint = existing->content.constraint;
        draft.segments = existing->content.segments;
        draft.expected_generation = expected.valid() ? expected : existing->generation;
        draft.derived = existing->provenance.source_constraint.valid();
    }
    // Revalidation re-binds dependency generations to what is current now. It never
    // searches for an alternative sequence and never repairs a broken list.
    MutationOutcome outcome = apply(caller, draft, LifecycleEvent::RevalidateBegin, true, false);
    return outcome;
}

MutationOutcome SegmentListStore::transition(const CallerIdentity& caller, SegmentListId id,
                                             SegmentListGeneration expected,
                                             LifecycleEvent event) {
    MutationOutcome outcome;
    outcome.result.set_limits(&limits_);
    outcome.list = id;

    PreparedContext ctx{};
    ctx.limits = &limits_;
    if (evidence_) {
        ctx.watermark = evidence_->watermark();
        ctx.evidence = evidence_->snapshot();
    }
    Resolved res{};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ctx.authority = authority_view();
        ctx.epoch = ctx.authority.epoch;
        ctx.registry_revision = registry_.revision();
        ctx.store_revision = state_.revision;
        if (const SegmentList* existing = find_locked(id)) {
            res.existing = *existing;
            res.has_existing = true;
        }
    }
    if (!res.has_existing) {
        outcome.status = StatusCode::NotFound;
        outcome.result.add(ReasonCode::ListNotFound, 0, id.value());
        return outcome;
    }
    if (!expected.valid() || expected != res.existing.generation) {
        outcome.status = StatusCode::Conflict;
        outcome.result.add(ReasonCode::ListGenerationConflict, 0, res.existing.generation.value());
        return outcome;
    }

    ValidationRequest request{};
    ListDraft draft{};
    draft.id = id;
    draft.scope = res.existing.content.scope;
    draft.expected_generation = expected;
    build_request(caller, draft, event, false, ctx, res, request);
    ValidationResult result = validate_request(request);
    if (result.failed()) {
        outcome.result.absorb(result);
        outcome.status = status_for(outcome.result);
        return outcome;
    }

    std::function<void()> hook;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        hook = precommit_hook_;
    }
    if (hook) {
        hook();
    }

    std::unique_lock<std::mutex> lock(mutex_);
    ValidationResult verify;
    verify.set_limits(&limits_);
    if (state_.revision != ctx.store_revision) {
        verify.add(ReasonCode::CommitWatermarkChanged, 0, state_.revision);
    }
    if (authority_.epoch() != ctx.epoch) {
        verify.add(ReasonCode::EpochStale, 0, authority_.epoch().value());
    }
    if (evidence_ && !(evidence_->watermark() == ctx.watermark)) {
        verify.add(ReasonCode::CommitWatermarkChanged, 0, evidence_->watermark().revision);
    }
    SegmentList* target = find_mutable_locked(id);
    if (target == nullptr || target->generation != expected) {
        verify.add(ReasonCode::ListGenerationConflict, 0, id.value());
    }
    if (verify.failed()) {
        outcome.result.absorb(verify);
        outcome.status = status_for(outcome.result);
        return outcome;
    }

    DurableState next = state_;
    SegmentList* next_target = nullptr;
    for (auto& list : next.lists) {
        if (list.content.id == id) {
            next_target = &list;
            break;
        }
    }
    if (next_target == nullptr) {
        outcome.status = StatusCode::NotFound;
        outcome.result.add(ReasonCode::ListNotFound, 0, id.value());
        return outcome;
    }
    if (!advance_state(next_target->state, event)) {
        outcome.status = StatusCode::Rejected;
        outcome.result.add(ReasonCode::LifecycleTransitionDenied, 0, id.value());
        return outcome;
    }
    next_target->currentness =
        next_target->state == LifecycleState::Active ? Currentness::Current : Currentness::Unknown;
    next.revision = state_.revision + 1;
    next_target->committed_revision = next.revision;
    next_target->authority = SegmentListAuthority{caller.publisher, caller.boot, ctx.epoch,
                                                  caller.attempt};
    append_history_locked(next, *next_target);
    next.profiles = registry_.profiles();
    next.policies = registry_.policies();
    next.publishers = authority_.publishers();
    next.fenced_boots = authority_.fenced_boots();
    next.epoch = ctx.epoch;
    if (caller.attempt.valid()) {
        record_attempt_locked(next, caller, id, draft_payload_digest(draft),
                              next_target->generation);
    }

    if (!persist_locked(next, verify)) {
        outcome.result.absorb(verify);
        outcome.status = StatusCode::PersistenceFailed;
        return outcome;
    }
    state_ = std::move(next);
    rebuild_index_locked();
    outcome.status = StatusCode::Ok;
    outcome.durable = persistence_ != nullptr;
    if (const SegmentList* committed = find_locked(id)) {
        outcome.generation = committed->generation;
        outcome.digest = committed->content_digest;
        outcome.result = ValidationResult{};
        outcome.result.set_limits(&limits_);
    }
    outcome.snapshot = SnapshotId{snapshot_counter_.fetch_add(1)};
    return outcome;
}


CurrentnessReport SegmentListStore::refresh_currentness() {
    CurrentnessReport report{};
    EvidenceWatermark watermark{};
    FabricEvidence evidence{};
    if (evidence_) {
        watermark = evidence_->watermark();
        evidence = evidence_->snapshot();
    }
    const CoordinatorEpoch epoch = authority_.epoch();

    std::unique_lock<std::mutex> lock(mutex_);
    DurableState next = state_;
    bool changed = false;
    for (SegmentList& list : next.lists) {
        switch (list.state) {
            case LifecycleState::Retired:
            case LifecycleState::Withdrawn:
            case LifecycleState::Superseded:
            case LifecycleState::Revoked:
                continue;
            default:
                break;
        }
        ++report.examined;

        SegmentProfile profile{};
        const bool profile_found = registry_.find_profile(list.content.profile, profile);
        SegmentPolicy policy{};
        const bool policy_found = list.content.policy.valid()
                                      ? registry_.find_policy(list.content.policy, policy)
                                      : false;

        Currentness currentness = Currentness::Current;
        if (authority_.is_fenced(list.authority.boot)) {
            currentness = Currentness::FencedPublisher;
        } else if (!profile_found) {
            currentness = Currentness::StaleProfile;
        } else if (profile.generation != list.content.profile_generation) {
            currentness = Currentness::StaleProfile;
        } else if (list.content.policy.valid() &&
                   (!policy_found || policy.generation != list.content.policy_generation)) {
            currentness = Currentness::StalePolicy;
        }
        if (currentness == Currentness::Current && evidence_) {
            if (list.content.capability.valid() && list.content.capability != evidence.capability) {
                currentness = Currentness::StaleCapability;
            } else {
                for (const CapabilityKey key : profile.required_capabilities) {
                    const CapabilityRecord* record = evidence.find_capability(key);
                    if (record == nullptr || !record->supported ||
                        record->generation != evidence.capability) {
                        currentness = Currentness::StaleCapability;
                        break;
                    }
                }
            }
        }
        if (currentness == Currentness::Current && evidence_) {
            if (list.content.topology.valid() && list.content.topology != evidence.topology) {
                currentness = Currentness::StaleTopology;
            } else {
                for (const Segment& segment : list.content.segments) {
                    if (segment.kind != SegmentKind::Adjacency) {
                        continue;
                    }
                    const AdjacencyRecord* record = evidence.find_adjacency(segment.adjacency);
                    if (record == nullptr || record->topology != segment.topology) {
                        currentness = Currentness::StaleTopology;
                        break;
                    }
                }
            }
        }
        if (currentness == Currentness::Current && evidence_ && list.content.path.valid()) {
            const PathRecord* record = evidence.find_path(list.content.path);
            if (record == nullptr || record->authority != list.content.path_authority) {
                currentness = Currentness::StalePathAuthority;
            }
        }
        if (currentness == Currentness::Current && evidence_ && list.content.route.valid()) {
            const RouteRecord* record = evidence.find_route(list.content.route);
            if (record == nullptr || record->generation != list.content.route_generation) {
                currentness = Currentness::StaleRouteBinding;
            }
        }
        if (currentness == Currentness::Current && evidence_ && list.content.constraint.valid()) {
            const ConstraintRecord* record = evidence.find_constraint(list.content.constraint);
            if (record == nullptr || record->generation != list.content.constraint_generation) {
                currentness = Currentness::StaleConstraintBinding;
            }
        }
        if (currentness == Currentness::Current && list.authority.epoch.valid() && epoch.valid() &&
            list.authority.epoch != epoch) {
            currentness = Currentness::StaleEpoch;
        }

        const bool was_current =
            list.state == LifecycleState::Active && list.currentness == Currentness::Current;
        if (currentness != list.currentness) {
            report.changes.push_back(CurrentnessChange{list.content.id, list.generation,
                                                       list.currentness, currentness});
            list.currentness = currentness;
            changed = true;
        }
        // Refresh only ever degrades. Availability is restored exclusively by an
        // explicit, fully validated revalidation.
        if (currentness != Currentness::Current && list.state == LifecycleState::Active) {
            list.state = LifecycleState::RevalidationRequired;
            changed = true;
        }
        if (was_current && currentness != Currentness::Current) {
            ++report.invalidated;
        }
    }
    if (changed) {
        next.revision = state_.revision + 1;
        ValidationResult persist_result;
        persist_result.set_limits(&limits_);
        if (persist_locked(next, persist_result)) {
            state_ = std::move(next);
            rebuild_index_locked();
        }
    }
    return report;
}

LoadReport SegmentListStore::load() {
    LoadReport report{};
    report.result.set_limits(&limits_);
    if (!persistence_) {
        report.empty = true;
        return report;
    }
    if (!persistence_->load(report, limits_)) {
        return report;
    }
    if (!report.loaded) {
        report.empty = true;
        return report;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    DurableState next = report.state;
    next.format_version = kPersistenceFormatVersion;

    // The epoch is monotonic across restarts and always strictly higher than the
    // epoch the durable state was written under, so every old epoch is fenced.
    CoordinatorEpoch base = next.epoch.valid() ? next.epoch : CoordinatorEpoch::initial();
    if (authority_.epoch() > base) {
        base = authority_.epoch();
    }
    if (!authority_.restore_epoch(base)) {
        report.result.add(ReasonCode::EpochStale, 0, base.value());
        report.loaded = false;
        return report;
    }
    if (!authority_.advance_epoch()) {
        report.result.add(ReasonCode::EpochExhausted, 0, base.value());
        report.loaded = false;
        return report;
    }

    for (const SegmentProfile& profile : next.profiles) {
        registry_.restore_profile(profile);
    }
    for (const SegmentPolicy& policy : next.policies) {
        registry_.restore_policy(policy);
    }
    next.profiles = registry_.profiles();
    next.policies = registry_.policies();

    // Live authority is never restored: every restored publisher is dead and every
    // restored boot is permanently fenced.
    for (const PublisherState& publisher : next.publishers) {
        PublisherState dead = publisher;
        dead.alive = false;
        dead.fenced = true;
        authority_.restore_publisher(dead);
        if (publisher.boot.valid()) {
            authority_.restore_fenced_boot(publisher.boot);
        }
        ++report.publishers_fenced;
    }
    for (const WorkerBootId boot : next.fenced_boots) {
        authority_.restore_fenced_boot(boot);
    }
    next.publishers = authority_.publishers();
    next.fenced_boots = authority_.fenced_boots();

    std::size_t requiring = 0;
    for (SegmentList& list : next.lists) {
        const TransitionOutcome outcome =
            lifecycle_transition(list.state, LifecycleEvent::RecoveryLoad);
        list.state = outcome.allowed ? outcome.next : LifecycleState::RevalidationRequired;
        if (list.state == LifecycleState::RevalidationRequired) {
            list.currentness = Currentness::RevalidationRequired;
            ++requiring;
        } else {
            list.currentness = Currentness::Unknown;
        }
    }
    next.epoch = authority_.epoch();
    next.revision = state_.revision + next.revision + 1;

    state_ = std::move(next);
    rebuild_index_locked();
    report.lists_recovered = state_.lists.size();
    report.lists_requiring_revalidation = requiring;
    report.history_entries = state_.history.size();

    ValidationResult persist_result;
    persist_result.set_limits(&limits_);
    if (!persist_locked(state_, persist_result)) {
        report.result.absorb(persist_result);
    }
    return report;
}

MutationOutcome SegmentListStore::withdraw(const CallerIdentity& caller, SegmentListId id,
                                           SegmentListGeneration expected) {
    return transition(caller, id, expected, LifecycleEvent::WithdrawBegin);
}

MutationOutcome SegmentListStore::withdraw_commit(const CallerIdentity& caller, SegmentListId id,
                                                  SegmentListGeneration expected) {
    return transition(caller, id, expected, LifecycleEvent::WithdrawCommit);
}

MutationOutcome SegmentListStore::revoke(const CallerIdentity& caller, SegmentListId id,
                                         SegmentListGeneration expected) {
    return transition(caller, id, expected, LifecycleEvent::Revoke);
}

MutationOutcome SegmentListStore::retire(const CallerIdentity& caller, SegmentListId id,
                                         SegmentListGeneration expected) {
    return transition(caller, id, expected, LifecycleEvent::Retire);
}

std::optional<SegmentList> SegmentListStore::get(SegmentListId id) const {
    std::unique_lock<std::mutex> lock(mutex_);
    const SegmentList* list = find_locked(id);
    if (list == nullptr) {
        return std::nullopt;
    }
    return *list;
}

std::vector<SegmentList> SegmentListStore::lists() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_.lists;
}

std::size_t SegmentListStore::list_count() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_.lists.size();
}

std::size_t SegmentListStore::total_segments() const {
    std::unique_lock<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const SegmentList& list : state_.lists) {
        total += list.content.segments.size();
    }
    return total;
}

std::uint64_t SegmentListStore::revision() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_.revision;
}

ValidationResult SegmentListStore::persist_now() {
    std::unique_lock<std::mutex> lock(mutex_);
    ValidationResult result;
    result.set_limits(&limits_);
    if (!persistence_) {
        return result;
    }
    DurableState next = state_;
    next.epoch = authority_.epoch();
    next.profiles = registry_.profiles();
    next.policies = registry_.policies();
    next.publishers = authority_.publishers();
    next.fenced_boots = authority_.fenced_boots();
    next.revision = state_.revision + 1;
    if (!persist_locked(next, result)) {
        return result;
    }
    state_ = std::move(next);
    rebuild_index_locked();
    return result;
}

std::vector<DurableHistoryEntry> SegmentListStore::history() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_.history;
}

DurableState SegmentListStore::export_state() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_;
}

SegmentListSnapshot SegmentListStore::snapshot(SegmentListId id) const {
    SegmentListSnapshot snap{};
    std::unique_lock<std::mutex> lock(mutex_);
    const SegmentList* list = find_locked(id);
    if (list == nullptr) {
        return snap;
    }
    snap.id = SnapshotId{snapshot_counter_.fetch_add(1)};
    snap.revision = state_.revision;
    snap.epoch = authority_.epoch();
    snap.list = *list;
    snap.digest = list->content_digest;
    return snap;
}

} // namespace srf
