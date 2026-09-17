// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "srf/canonical.hpp"
#include "srf/store.hpp"

namespace srf {

std::string_view diff_kind_name(DiffKind kind) noexcept {
    switch (kind) {
        case DiffKind::Invalid: return "Invalid";
        case DiffKind::Insert: return "Insert";
        case DiffKind::Remove: return "Remove";
        case DiffKind::Replace: return "Replace";
        case DiffKind::Reorder: return "Reorder";
        case DiffKind::ProfileChange: return "ProfileChange";
        case DiffKind::PolicyChange: return "PolicyChange";
        case DiffKind::DependencyChange: return "DependencyChange";
        case DiffKind::CurrentnessChange: return "CurrentnessChange";
        case DiffKind::LifecycleChange: return "LifecycleChange";
        case DiffKind::GenerationAdvance: return "GenerationAdvance";
        case DiffKind::StrictnessChange: return "StrictnessChange";
        case DiffKind::AuthorityChange: return "AuthorityChange";
        case DiffKind::Created: return "Created";
        case DiffKind::Destroyed: return "Destroyed";
    }
    return "Unknown";
}

std::string_view explanation_topic_name(ExplanationTopic topic) noexcept {
    switch (topic) {
        case ExplanationTopic::Invalid: return "Invalid";
        case ExplanationTopic::WhyActive: return "WhyActive";
        case ExplanationTopic::WhyNotActive: return "WhyNotActive";
        case ExplanationTopic::WhySegmentInvalid: return "WhySegmentInvalid";
        case ExplanationTopic::MissingCapability: return "MissingCapability";
        case ExplanationTopic::StaleTopology: return "StaleTopology";
        case ExplanationTopic::WhySuperseded: return "WhySuperseded";
        case ExplanationTopic::WhyRevalidationRequired: return "WhyRevalidationRequired";
        case ExplanationTopic::Currentness: return "Currentness";
        case ExplanationTopic::Lineage: return "Lineage";
    }
    return "Unknown";
}

bool SnapshotDiff::contains(DiffKind kind) const noexcept {
    for (const DiffEntry& entry : entries) {
        if (entry.kind == kind) {
            return true;
        }
    }
    return false;
}

bool Explanation::contains(ReasonCode code) const noexcept {
    for (const ExplanationEntry& entry : entries) {
        if (entry.code == code) {
            return true;
        }
    }
    return false;
}

namespace {

[[nodiscard]] std::uint32_t payload_width_of(const SegmentProfile& profile) noexcept {
    return profile.payload_width == 0 ? 1u : profile.payload_width;
}

[[nodiscard]] std::vector<Digest128> segment_digests(const SegmentList& list,
                                                     std::uint32_t payload_width) {
    std::vector<Digest128> out;
    out.reserve(list.content.segments.size());
    for (const Segment& s : list.content.segments) {
        const ByteBuffer bytes = canonical_segment_bytes(s, payload_width);
        out.push_back(digest128(as_bytes(bytes)));
    }
    return out;
}

void push_entry(Explanation& explanation, const Limits& limits, ReasonCode code,
                std::uint32_t index = 0, std::uint64_t detail = 0) {
    if (explanation.entries.size() >= limits.max_explanation_entries) {
        explanation.truncated = true;
        return;
    }
    explanation.entries.push_back(ExplanationEntry{code, index, detail});
}

} // namespace

SnapshotDiff SegmentListStore::diff(const SegmentListSnapshot& from,
                                    const SegmentListSnapshot& to) const {
    SnapshotDiff result{};
    result.from = from.id;
    result.to = to.id;
    result.list = to.valid() ? to.list.content.id : from.list.content.id;

    if (!from.valid() && !to.valid()) {
        return result;
    }
    const auto limit = static_cast<std::size_t>(limits_.max_diff_entries);
    const auto add = [&result, limit](DiffKind kind, std::uint32_t index, std::uint32_t other,
                                      std::uint64_t detail) {
        if (result.entries.size() >= limit) {
            result.truncated = true;
            return;
        }
        result.entries.push_back(DiffEntry{kind, index, other, detail});
    };

    if (!from.valid()) {
        add(DiffKind::Created, 0, 0, to.list.generation.value());
        return result;
    }
    if (!to.valid()) {
        add(DiffKind::Destroyed, 0, 0, from.list.generation.value());
        return result;
    }
    if (from.list.content.id != to.list.content.id) {
        add(DiffKind::Destroyed, 0, 0, from.list.content.id.value());
        add(DiffKind::Created, 0, 0, to.list.content.id.value());
        return result;
    }
    if (to.list.generation.value() != from.list.generation.value()) {
        add(DiffKind::GenerationAdvance, 0, 0, to.list.generation.value());
    }
    if (from.list.state != to.list.state) {
        add(DiffKind::LifecycleChange, 0, 0, static_cast<std::uint64_t>(to.list.state));
    }
    if (from.list.currentness != to.list.currentness) {
        add(DiffKind::CurrentnessChange, 0, 0, static_cast<std::uint64_t>(to.list.currentness));
    }
    if (from.list.content.profile != to.list.content.profile ||
        from.list.content.profile_generation != to.list.content.profile_generation) {
        add(DiffKind::ProfileChange, 0, 0, to.list.content.profile.value());
    }
    if (from.list.content.policy != to.list.content.policy ||
        from.list.content.policy_generation != to.list.content.policy_generation) {
        add(DiffKind::PolicyChange, 0, 0, to.list.content.policy.value());
    }
    if (from.list.content.strictness != to.list.content.strictness) {
        add(DiffKind::StrictnessChange, 0, 0,
            static_cast<std::uint64_t>(to.list.content.strictness));
    }
    if (from.list.content.topology != to.list.content.topology) {
        add(DiffKind::DependencyChange, 0, 0, to.list.content.topology.value());
    }
    if (from.list.content.capability != to.list.content.capability) {
        add(DiffKind::DependencyChange, 0, 0, to.list.content.capability.value());
    }
    if (from.list.content.path_authority != to.list.content.path_authority) {
        add(DiffKind::DependencyChange, 0, 0, to.list.content.path_authority.value());
    }
    if (from.list.content.route_generation != to.list.content.route_generation) {
        add(DiffKind::DependencyChange, 0, 0, to.list.content.route_generation.value());
    }
    if (from.list.content.constraint_generation != to.list.content.constraint_generation) {
        add(DiffKind::DependencyChange, 0, 0, to.list.content.constraint_generation.value());
    }
    if (!(from.list.authority == to.list.authority)) {
        add(DiffKind::AuthorityChange, 0, 0, to.list.authority.epoch.value());
    }

    // Segment-level comparison uses the widest payload present in either revision
    // so that the comparison itself never depends on registry state.
    std::uint32_t compare_width = 1;
    for (const Segment& s : from.list.content.segments) {
        compare_width = std::max(compare_width, static_cast<std::uint32_t>(s.payload.size()));
    }
    for (const Segment& s : to.list.content.segments) {
        compare_width = std::max(compare_width, static_cast<std::uint32_t>(s.payload.size()));
    }

    const std::vector<Digest128> before = segment_digests(from.list, compare_width);
    const std::vector<Digest128> after = segment_digests(to.list, compare_width);
    const std::size_t common = std::min(before.size(), after.size());

    if (before != after) {
        std::vector<Digest128> sorted_before = before;
        std::vector<Digest128> sorted_after = after;
        std::sort(sorted_before.begin(), sorted_before.end());
        std::sort(sorted_after.begin(), sorted_after.end());
        const bool same_multiset = (sorted_before == sorted_after);
        result.order_changed = same_multiset && before.size() == after.size();
        if (result.order_changed) {
            for (std::size_t i = 0; i < common; ++i) {
                if (!(before[i] == after[i])) {
                    add(DiffKind::Reorder, static_cast<std::uint32_t>(i),
                        static_cast<std::uint32_t>(i), after[i].lo);
                }
            }
        } else {
            for (std::size_t i = 0; i < common; ++i) {
                if (!(before[i] == after[i])) {
                    add(DiffKind::Replace, static_cast<std::uint32_t>(i),
                        static_cast<std::uint32_t>(i), after[i].lo);
                }
            }
            for (std::size_t i = common; i < after.size(); ++i) {
                add(DiffKind::Insert, static_cast<std::uint32_t>(i),
                    static_cast<std::uint32_t>(i), after[i].lo);
            }
            for (std::size_t i = common; i < before.size(); ++i) {
                add(DiffKind::Remove, static_cast<std::uint32_t>(i),
                    static_cast<std::uint32_t>(i), before[i].lo);
            }
        }
    }
    return result;
}

Explanation SegmentListStore::explain(SegmentListId id) const {
    Explanation explanation{};
    explanation.list = id;
    std::unique_lock<std::mutex> lock(mutex_);
    const SegmentList* list = find_locked(id);
    if (list == nullptr) {
        explanation.topic = ExplanationTopic::WhyNotActive;
        push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
        return explanation;
    }
    explanation.generation = list->generation;
    explanation.state = list->state;
    explanation.currentness = list->currentness;
    if (list->state == LifecycleState::Active && list->currentness == Currentness::Current) {
        explanation.topic = ExplanationTopic::WhyActive;
        push_entry(explanation, limits_, ReasonCode::CurrentnessCurrent, 0,
                   list->generation.value());
    } else if (list->currentness == Currentness::RevalidationRequired ||
               list->currentness == Currentness::StaleCapability ||
               list->currentness == Currentness::StaleTopology ||
               list->currentness == Currentness::StaleProfile ||
               list->currentness == Currentness::StalePolicy ||
               list->currentness == Currentness::StaleEpoch ||
               list->currentness == Currentness::StalePathAuthority ||
               list->currentness == Currentness::StaleRouteBinding ||
               list->currentness == Currentness::StaleConstraintBinding ||
               list->currentness == Currentness::FencedPublisher) {
        explanation.topic = ExplanationTopic::WhyRevalidationRequired;
        push_entry(explanation, limits_, currentness_reason(list->currentness), 0,
                   list->generation.value());
    } else {
        explanation.topic = ExplanationTopic::WhyNotActive;
        push_entry(explanation, limits_, currentness_reason(list->currentness), 0,
                   list->generation.value());
    }
    if (explanation.topic != ExplanationTopic::WhyActive) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessCurrent, 0,
                   static_cast<std::uint64_t>(list->state));
    }
    return explanation;
}

Explanation SegmentListStore::explain_segment(SegmentListId id, std::uint32_t index) const {
    Explanation explanation{};
    explanation.list = id;
    explanation.topic = ExplanationTopic::WhySegmentInvalid;

    SegmentList copy{};
    bool found = false;
    SegmentProfile profile{};
    bool profile_found = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const SegmentList* list = find_locked(id);
        if (list == nullptr) {
            push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
            return explanation;
        }
        copy = *list;
        found = true;
        profile_found = registry_.find_profile(copy.content.profile, profile);
    }
    if (!found) {
        return explanation;
    }
    explanation.generation = copy.generation;
    explanation.state = copy.state;
    explanation.currentness = copy.currentness;
    if (index >= copy.content.segments.size()) {
        push_entry(explanation, limits_, ReasonCode::InvalidArgument, index,
                   copy.content.segments.size());
        return explanation;
    }
    FabricEvidence evidence{};
    if (evidence_ != nullptr) {
        evidence = evidence_->snapshot();
    }
    ValidationRequest request{};
    request.limits = &limits_;
    request.list_id = copy.content.id;
    request.scope = copy.content.scope;
    request.profile = profile_found ? &profile : nullptr;
    request.profile_found = profile_found;
    request.profile_id = copy.content.profile;
    request.segments = copy.content.segments;
    request.evidence = evidence_ != nullptr ? &evidence : nullptr;
    request.topology = copy.content.topology;
    request.full = true;
    // Explaining one segment explains the content, not the commit-time authority
    // envelope, so lifecycle and caller phases are deliberately out of scope here.
    const ValidationResult result = validate_request(request);
    bool any = false;
    for (const Reason& reason : result.reasons()) {
        if (reason.index != index) {
            continue;
        }
        any = true;
        push_entry(explanation, limits_, reason.code, reason.index, reason.detail);
    }
    if (!any) {
        push_entry(explanation, limits_, ReasonCode::Ok, index, 0);
    }
    return explanation;
}

Explanation SegmentListStore::explain_currentness(SegmentListId id) const {
    Explanation explanation{};
    explanation.list = id;
    explanation.topic = ExplanationTopic::Currentness;

    SegmentList copy{};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const SegmentList* list = find_locked(id);
        if (list == nullptr) {
            push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
            return explanation;
        }
        copy = *list;
    }
    explanation.generation = copy.generation;
    explanation.state = copy.state;
    explanation.currentness = copy.currentness;
    push_entry(explanation, limits_, currentness_reason(copy.currentness), 0,
               copy.generation.value());
    if (copy.content.profile_generation.valid()) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessStaleProfile, 0,
                   copy.content.profile_generation.value());
    }
    if (copy.content.capability.valid()) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessStaleCapability, 0,
                   copy.content.capability.value());
    }
    if (copy.content.topology.valid()) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessStaleTopology, 0,
                   copy.content.topology.value());
    }
    if (copy.content.path_authority.valid()) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessStalePathAuthority, 0,
                   copy.content.path_authority.value());
    }
    if (copy.content.route_generation.valid()) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessStaleRouteBinding, 0,
                   copy.content.route_generation.value());
    }
    if (copy.content.constraint_generation.valid()) {
        push_entry(explanation, limits_, ReasonCode::CurrentnessStaleConstraintBinding, 0,
                   copy.content.constraint_generation.value());
    }
    push_entry(explanation, limits_, ReasonCode::CurrentnessStaleEpoch, 0,
               copy.authority.epoch.value());
    return explanation;
}

Explanation SegmentListStore::explain_supersession(SegmentListId id) const {
    Explanation explanation{};
    explanation.list = id;
    explanation.topic = ExplanationTopic::WhySuperseded;
    std::unique_lock<std::mutex> lock(mutex_);
    const SegmentList* list = find_locked(id);
    if (list == nullptr) {
        push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
        return explanation;
    }
    explanation.generation = list->generation;
    explanation.state = list->state;
    explanation.currentness = list->currentness;
    if (list->state == LifecycleState::Superseded) {
        push_entry(explanation, limits_, ReasonCode::ListSuperseded, 0,
                   list->lineage.superseded_by.value());
        push_entry(explanation, limits_, ReasonCode::CurrentnessCurrent, 0,
                   list->lineage.supersedes.value());
    } else if (list->lineage.supersedes.valid()) {
        push_entry(explanation, limits_, ReasonCode::Ok, 0, list->lineage.supersedes.value());
    } else {
        push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
    }
    return explanation;
}

Explanation SegmentListStore::explain_missing_capability(SegmentListId id) const {
    Explanation explanation{};
    explanation.list = id;
    explanation.topic = ExplanationTopic::MissingCapability;

    SegmentList copy{};
    SegmentProfile profile{};
    bool profile_found = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const SegmentList* list = find_locked(id);
        if (list == nullptr) {
            push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
            return explanation;
        }
        copy = *list;
        profile_found = registry_.find_profile(copy.content.profile, profile);
    }
    explanation.generation = copy.generation;
    explanation.state = copy.state;
    explanation.currentness = copy.currentness;
    if (!profile_found) {
        push_entry(explanation, limits_, ReasonCode::ProfileUnknown, 0,
                   copy.content.profile.value());
        return explanation;
    }
    FabricEvidence evidence{};
    if (evidence_ != nullptr) {
        evidence = evidence_->snapshot();
    }
    for (const CapabilityKey key : profile.required_capabilities) {
        const CapabilityRecord* record = evidence.find_capability(key);
        if (record == nullptr) {
            push_entry(explanation, limits_, ReasonCode::CapabilityMissing, 0, key.value());
        } else if (!record->supported) {
            push_entry(explanation, limits_, ReasonCode::CapabilityUnsupported, 0, key.value());
        } else if (record->generation != evidence.capability) {
            push_entry(explanation, limits_, ReasonCode::CapabilityGenerationMismatch, 0,
                       record->generation.value());
        }
    }
    if (explanation.entries.empty()) {
        push_entry(explanation, limits_, ReasonCode::Ok, 0, profile.id.value());
    }
    return explanation;
}

Explanation SegmentListStore::explain_stale_topology(SegmentListId id) const {
    Explanation explanation{};
    explanation.list = id;
    explanation.topic = ExplanationTopic::StaleTopology;

    SegmentList copy{};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const SegmentList* list = find_locked(id);
        if (list == nullptr) {
            push_entry(explanation, limits_, ReasonCode::ListNotFound, 0, id.value());
            return explanation;
        }
        copy = *list;
    }
    explanation.generation = copy.generation;
    explanation.state = copy.state;
    explanation.currentness = copy.currentness;
    FabricEvidence evidence{};
    if (evidence_ != nullptr) {
        evidence = evidence_->snapshot();
    }
    bool any = false;
    for (std::size_t i = 0; i < copy.content.segments.size(); ++i) {
        const Segment& segment = copy.content.segments[i];
        if (segment.kind != SegmentKind::Adjacency) {
            continue;
        }
        any = true;
        const auto index = static_cast<std::uint32_t>(i);
        const AdjacencyRecord* record = evidence.find_adjacency(segment.adjacency);
        if (record == nullptr) {
            push_entry(explanation, limits_, ReasonCode::TopologyAdjacencyUnknown, index,
                       segment.adjacency.value());
        } else if (record->topology != segment.topology) {
            push_entry(explanation, limits_, ReasonCode::TopologyAdjacencyStale, index,
                       record->topology.value());
        } else {
            push_entry(explanation, limits_, ReasonCode::Ok, index, record->topology.value());
        }
    }
    if (copy.content.topology.valid() && evidence.topology.valid() &&
        copy.content.topology != evidence.topology) {
        push_entry(explanation, limits_, ReasonCode::TopologyGenerationMismatch, 0,
                   evidence.topology.value());
    }
    if (!any && !copy.content.topology.valid()) {
        push_entry(explanation, limits_, ReasonCode::Ok, 0, 0);
    }
    return explanation;
}

} // namespace srf
