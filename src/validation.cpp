// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/validation.hpp"

#include <algorithm>

namespace srf {
namespace {

constexpr std::uint32_t kNoIndex = 0;

void check_caller_identity(const ValidationRequest& r, ValidationResult& out) {
    const CallerIdentity* caller = r.caller;
    if (caller == nullptr) {
        return;
    }
    if (!caller->publisher.valid()) {
        out.add(ReasonCode::CallerMissingPublisher);
    }
    if (!caller->boot.valid()) {
        out.add(ReasonCode::CallerMissingWorkerBoot);
    }
    if (!caller->attempt.valid()) {
        out.add(ReasonCode::CallerMissingMutationAttempt);
    }
    if (!caller->scope.valid()) {
        out.add(ReasonCode::CallerMissingScope);
    }
    if (r.authority == nullptr) {
        return;
    }
    const PublisherState* state = r.authority->find(caller->publisher);
    if (caller->publisher.valid() && state == nullptr) {
        out.add(ReasonCode::CallerUnknownPublisher, kNoIndex, caller->publisher.value());
        return;
    }
    if (state != nullptr) {
        if (state->boot != caller->boot) {
            out.add(ReasonCode::CallerStaleWorkerBoot, kNoIndex, state->boot.value());
        }
        // A publisher that is no longer alive is fenced, never merely unknown: the
        // distinction is what makes a stale writer's rejection diagnosable.
        if (state->fenced || !state->alive || r.authority->boot_fenced(caller->boot)) {
            out.add(ReasonCode::CallerFencedPublisher, kNoIndex, caller->boot.value());
        }
        if (caller->scope.valid() && state->scope != caller->scope) {
            out.add(ReasonCode::CallerScopeMismatch, kNoIndex, state->scope.value());
        }
    }
}

void check_epoch_boot_scope(const ValidationRequest& r, ValidationResult& out) {
    const CallerIdentity* caller = r.caller;
    if (caller == nullptr) {
        return;
    }
    if (!caller->epoch.valid()) {
        out.add(ReasonCode::EpochInvalid);
        return;
    }
    if (r.authority == nullptr) {
        return;
    }
    if (caller->epoch != r.authority->epoch) {
        out.add(ReasonCode::EpochStale, kNoIndex, r.authority->epoch.value());
    }
    if (r.authority->epoch.exhausted()) {
        out.add(ReasonCode::EpochExhausted, kNoIndex, r.authority->epoch.value());
    }
}

void check_list_lifecycle(const ValidationRequest& r, ValidationResult& out) {
    if (!r.list_id.valid()) {
        out.add(ReasonCode::ListIdInvalid);
    }
    const bool creating = r.existing == nullptr;
    if (r.require_existing && creating) {
        out.add(ReasonCode::ListNotFound, kNoIndex, r.list_id.value());
    }
    if (r.require_absent && !creating) {
        out.add(ReasonCode::ListAlreadyExists, kNoIndex, r.list_id.value());
    }
    if (creating) {
        if (r.expected_generation.valid()) {
            out.add(ReasonCode::ListNotFound, kNoIndex, r.list_id.value());
        }
    } else {
        if (r.expected_generation != r.existing->generation) {
            out.add(ReasonCode::ListGenerationConflict, kNoIndex, r.existing->generation.value());
        }
        if (!r.expected_generation.valid()) {
            out.add(ReasonCode::ListGenerationInvalid);
        }
        if (r.existing->generation.exhausted()) {
            out.add(ReasonCode::ListGenerationExhausted, kNoIndex, r.existing->generation.value());
        }
        // A terminal-state reason is only meaningful when the requested transition
        // is actually blocked by that state; a legal transition out of REVOKED must
        // not be reported as if the lineage were still frozen.
        const TransitionOutcome outcome = lifecycle_transition(r.existing->state, r.intent);
        if (!outcome.allowed) {
            if (r.existing->state == LifecycleState::Retired) {
                out.add(ReasonCode::ListRetired, kNoIndex, r.existing->generation.value());
            } else if (r.existing->state == LifecycleState::Revoked) {
                out.add(ReasonCode::ListRevoked, kNoIndex, r.existing->generation.value());
            } else if (r.existing->state == LifecycleState::Superseded) {
                out.add(ReasonCode::ListSuperseded, kNoIndex, r.existing->generation.value());
            } else if (r.existing->state == LifecycleState::Withdrawn) {
                out.add(ReasonCode::ListWithdrawn, kNoIndex, r.existing->generation.value());
            }
            out.add(ReasonCode::LifecycleTransitionDenied, kNoIndex,
                    (static_cast<std::uint64_t>(r.existing->state) << 8) |
                        static_cast<std::uint64_t>(r.intent));
        }
    }
}

void check_list_content_lifecycle(const ValidationRequest& r, ValidationResult& out) {
    if (r.segments.empty() && (r.profile == nullptr || !r.profile->allow_empty)) {
        out.add(ReasonCode::ListEmptyNotPermitted);
    }
}

void check_profile(const ValidationRequest& r, ValidationResult& out) {
    if (r.profile == nullptr) {
        if (r.profile_id.valid()) {
            out.add(ReasonCode::ProfileUnknown, kNoIndex, r.profile_id.value());
        } else {
            out.add(ReasonCode::ProfileIdInvalid);
        }
        return;
    }
    if (!r.profile_found) {
        out.add(ReasonCode::ProfileUnknown, kNoIndex, r.profile_id.value());
        return;
    }
    if (r.profile->id != r.profile_id) {
        out.add(ReasonCode::ProfileUnknown, kNoIndex, r.profile_id.value());
    }
    if (!r.profile->generation.valid()) {
        out.add(ReasonCode::ProfileGenerationInvalid);
    }
    if (r.profile_generation.valid() && r.profile_generation != r.profile->generation) {
        out.add(ReasonCode::ProfileGenerationMismatch, kNoIndex, r.profile->generation.value());
    }
    if (!r.profile->allow_empty && r.segments.empty()) {
        out.add(ReasonCode::ProfileEmptyNotPermitted);
    }
    if (r.strictness == Strictness::Loose && !r.profile->allow_loose) {
        out.add(ReasonCode::ProfileStrictnessUnsupported);
    }
    if (r.segments.size() > r.profile->max_depth) {
        out.add(ReasonCode::ProfileMaxDepthExceeded, kNoIndex, r.profile->max_depth);
    }
    const std::size_t guard = r.limits != nullptr
                                  ? static_cast<std::size_t>(r.limits->max_segments_per_list) + 1u
                                  : r.segments.size();
    const std::size_t scan = std::min(r.segments.size(), guard);
    for (std::size_t i = 0; i < scan; ++i) {
        const Segment& s = r.segments[i];
        const auto index = static_cast<std::uint32_t>(i);
        if (!r.profile->allows_kind(s.kind)) {
            out.add(ReasonCode::ProfileKindNotAllowed, index, static_cast<std::uint64_t>(s.kind));
        }
        if (expected_encoding_for(s.kind) == SegmentEncodingId::Invalid) {
            // A kind with no encoding in this profile family cannot be admitted at
            // all; the structural phase additionally names the malformed kind.
            out.add(ReasonCode::ProfileKindMismatch, index, static_cast<std::uint64_t>(s.kind));
        }
        if (s.payload.size() > r.profile->payload_width) {
            out.add(ReasonCode::ProfilePayloadWidthExceeded, index, r.profile->payload_width);
        }
    }
    if (!r.profile->allow_consecutive_repeats || !r.profile->allow_nonconsecutive_repeats) {
        for (std::size_t i = 1; i < scan; ++i) {
            const auto index = static_cast<std::uint32_t>(i);
            for (std::size_t j = 0; j < i; ++j) {
                if (!segment_semantic_equal(r.segments[j], r.segments[i],
                                            r.profile->payload_width)) {
                    continue;
                }
                const bool consecutive = (j + 1 == i);
                if (consecutive && !r.profile->allow_consecutive_repeats) {
                    out.add(ReasonCode::ProfileRepeatNotPermitted, index, r.segments[i].id.value());
                }
                if (!consecutive && !r.profile->allow_nonconsecutive_repeats) {
                    out.add(ReasonCode::ProfileRepeatNotPermitted, index, r.segments[i].id.value());
                }
            }
        }
    }
}

void check_policy(const ValidationRequest& r, ValidationResult& out) {
    if (r.policy == nullptr) {
        if (r.policy_required) {
            if (r.policy_id.valid()) {
                out.add(ReasonCode::PolicyUnknown, kNoIndex, r.policy_id.value());
            } else {
                out.add(ReasonCode::PolicyIdInvalid);
            }
        }
        return;
    }
    if (!r.policy_found) {
        out.add(ReasonCode::PolicyUnknown, kNoIndex, r.policy_id.value());
        return;
    }
    const SegmentPolicy& p = *r.policy;
    if (!p.generation.valid()) {
        out.add(ReasonCode::PolicyGenerationInvalid);
    }
    if (r.policy_generation.valid() && r.policy_generation != p.generation) {
        out.add(ReasonCode::PolicyGenerationMismatch, kNoIndex, p.generation.value());
    }
    if (p.owner_scope.valid() && p.owner_scope != r.scope) {
        out.add(ReasonCode::PolicyOwnershipMismatch, kNoIndex, p.owner_scope.value());
    }
    if (!p.allowed_profiles.empty() &&
        std::find(p.allowed_profiles.begin(), p.allowed_profiles.end(), r.profile_id) ==
            p.allowed_profiles.end()) {
        out.add(ReasonCode::PolicyDeniedProfile, kNoIndex, r.profile_id.value());
    }
    if (p.max_depth != 0 && r.segments.size() > p.max_depth) {
        out.add(ReasonCode::PolicyMaxDepthExceeded, kNoIndex, p.max_depth);
    }
    if (r.existing != nullptr && !p.allow_replacement) {
        out.add(ReasonCode::PolicyReplacementDenied);
    }
    if (r.derived && !p.allow_derivation) {
        out.add(ReasonCode::PolicyDerivationDenied);
    }
    if (p.require_strict && r.strictness != Strictness::Strict) {
        out.add(ReasonCode::PolicyStrictnessRequired);
    }
    for (const SegmentId required : p.required_segments) {
        bool found = false;
        for (const Segment& s : r.segments) {
            if (s.id == required) {
                found = true;
                break;
            }
        }
        if (!found) {
            out.add(ReasonCode::PolicyRequiredEntityMissing, kNoIndex, required.value());
        }
    }
    for (const SegmentId forbidden : p.forbidden_segments) {
        for (std::size_t i = 0; i < r.segments.size(); ++i) {
            if (r.segments[i].id == forbidden) {
                out.add(ReasonCode::PolicyForbiddenEntityPresent, static_cast<std::uint32_t>(i),
                        forbidden.value());
                break;
            }
        }
    }
    if (p.allowed_kinds_mask != 0) {
        for (std::size_t i = 0; i < r.segments.size(); ++i) {
            const Segment& s = r.segments[i];
            if (s.kind == SegmentKind::Invalid) {
                continue;
            }
            if ((p.allowed_kinds_mask & kind_bit(s.kind)) == 0) {
                out.add(ReasonCode::PolicyDeniedSegmentKind, static_cast<std::uint32_t>(i),
                        static_cast<std::uint64_t>(s.kind));
            }
        }
    }
}

void check_structural(const ValidationRequest& r, ValidationResult& out,
                      std::uint32_t payload_width) {
    // Repetition is legal only where the profile explicitly permits it. The
    // profile phase reports the profile-level gate; this phase reports the exact
    // offending pairs, and only for repetitions the profile actually forbids.
    const bool forbid_consecutive =
        r.profile == nullptr || !r.profile->allow_consecutive_repeats;
    const bool forbid_nonconsecutive =
        r.profile == nullptr || !r.profile->allow_nonconsecutive_repeats;
    // The scan is bounded by the configured depth plus one, so a defect at exactly
    // index max_segments_per_list is still named while the work stays bounded. The
    // depth violation itself is reported by the resource-limit phase.
    const std::size_t cap = r.limits != nullptr
                                ? static_cast<std::size_t>(r.limits->max_segments_per_list) + 1u
                                : r.segments.size();
    const std::size_t scan = std::min(r.segments.size(), cap);
    for (std::size_t i = 0; i < scan; ++i) {
        const Segment& s = r.segments[i];
        const auto index = static_cast<std::uint32_t>(i);
        if (s.kind == SegmentKind::Invalid) {
            out.add(ReasonCode::SegmentKindInvalid, index);
            continue;
        }
        if (!s.id.valid()) {
            out.add(ReasonCode::SegmentIdInvalid, index);
        }
        if (!s.generation.valid()) {
            out.add(ReasonCode::SegmentGenerationInvalid, index);
        }
        const SegmentEncodingId expected = expected_encoding_for(s.kind);
        if (s.encoding != SegmentEncodingId::Invalid && s.encoding != expected) {
            out.add(ReasonCode::SegmentEncodingMismatch, index,
                    static_cast<std::uint64_t>(s.encoding));
        }
        const bool has_node = s.node.valid();
        const bool has_adjacency = s.adjacency.valid();
        const bool has_endpoint = s.endpoint.valid();
        const bool has_binding = s.binding.valid();
        const bool has_policy = s.policy.valid();
        const bool has_topology = s.topology.valid();
        const bool has_policy_generation = s.policy_generation.valid();
        // For each kind exactly one entity reference is required; every other
        // reference is a reserved field that must be zero. Topology is a required
        // binding for ADJACENCY and a reserved field for every other kind.
        switch (s.kind) {
            case SegmentKind::Node:
                if (!has_node) {
                    out.add(ReasonCode::SegmentBindingIncomplete, index);
                }
                if (has_adjacency || has_endpoint || has_binding || has_policy || has_topology ||
                    has_policy_generation) {
                    out.add(ReasonCode::SegmentReservedFieldSet, index);
                }
                break;
            case SegmentKind::Adjacency:
                if (!has_adjacency || !has_topology) {
                    out.add(ReasonCode::SegmentBindingIncomplete, index);
                }
                if (has_node || has_endpoint || has_binding || has_policy ||
                    has_policy_generation) {
                    out.add(ReasonCode::SegmentReservedFieldSet, index);
                }
                break;
            case SegmentKind::Endpoint:
                if (!has_endpoint) {
                    out.add(ReasonCode::SegmentBindingIncomplete, index);
                }
                if (has_node || has_adjacency || has_binding || has_policy || has_topology ||
                    has_policy_generation) {
                    out.add(ReasonCode::SegmentReservedFieldSet, index);
                }
                break;
            case SegmentKind::Binding:
                if (!has_binding) {
                    out.add(ReasonCode::SegmentBindingIncomplete, index);
                }
                if (has_node || has_adjacency || has_endpoint || has_policy || has_topology ||
                    has_policy_generation) {
                    out.add(ReasonCode::SegmentReservedFieldSet, index);
                }
                break;
            case SegmentKind::Policy:
                if (!has_policy || !has_policy_generation) {
                    out.add(ReasonCode::SegmentBindingIncomplete, index);
                }
                if (has_node || has_adjacency || has_endpoint || has_binding || has_topology) {
                    out.add(ReasonCode::SegmentReservedFieldSet, index);
                }
                break;
            case SegmentKind::Invalid:
                break;
        }
        if (!has_node && !has_adjacency && !has_endpoint && !has_binding && !has_policy &&
            s.kind != SegmentKind::Invalid) {
            out.add(ReasonCode::SegmentMissingBinding, index);
        }
        if (r.evidence != nullptr) {
            const SegmentRecord* record = r.evidence->find_segment(s.id);
            if (record == nullptr) {
                out.add(ReasonCode::SegmentEntityUnknown, index, s.id.value());
            } else {
                if (record->kind != s.kind) {
                    out.add(ReasonCode::SegmentEntityKindMismatch, index,
                            static_cast<std::uint64_t>(record->kind));
                }
                if (record->generation != s.generation) {
                    out.add(ReasonCode::SegmentEntityGenerationMismatch, index,
                            record->generation.value());
                }
            }
            switch (s.kind) {
                case SegmentKind::Node:
                    if (has_node && r.evidence->find_node(s.node) == nullptr) {
                        out.add(ReasonCode::SegmentEntityUnknown, index, s.node.value());
                    }
                    break;
                case SegmentKind::Endpoint:
                    if (has_endpoint && r.evidence->find_endpoint(s.endpoint) == nullptr) {
                        out.add(ReasonCode::SegmentEntityUnknown, index, s.endpoint.value());
                    }
                    break;
                case SegmentKind::Binding:
                    if (has_binding && r.evidence->find_binding(s.binding) == nullptr) {
                        out.add(ReasonCode::SegmentEntityUnknown, index, s.binding.value());
                    }
                    break;
                default:
                    break;
            }
        }
    }
    for (std::size_t i = 1; i < scan; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        for (std::size_t j = 0; j < i; ++j) {
            if (!segment_semantic_equal(r.segments[j], r.segments[i], payload_width)) {
                continue;
            }
            if (j + 1 == i) {
                if (forbid_consecutive) {
                    out.add(ReasonCode::SegmentDuplicateConsecutive, index,
                            r.segments[i].id.value());
                }
            } else if (forbid_nonconsecutive) {
                out.add(ReasonCode::SegmentDuplicateNonConsecutive, index,
                        r.segments[i].id.value());
            }
        }
    }
}

void check_capability(const ValidationRequest& r, ValidationResult& out) {
    if (r.profile == nullptr) {
        return;
    }
    if (r.evidence == nullptr) {
        if (!r.profile->required_capabilities.empty()) {
            out.add(ReasonCode::CapabilityEvidenceUnavailable);
        }
        return;
    }
    if (r.capability.valid() && r.capability != r.evidence->capability) {
        out.add(ReasonCode::CapabilityGenerationMismatch, kNoIndex, r.evidence->capability.value());
    }
    for (const CapabilityKey key : r.profile->required_capabilities) {
        if (!key.valid()) {
            out.add(ReasonCode::CapabilityKeyInvalid);
            continue;
        }
        const CapabilityRecord* record = r.evidence->find_capability(key);
        if (record == nullptr) {
            out.add(ReasonCode::CapabilityMissing, kNoIndex, key.value());
            continue;
        }
        if (!record->supported) {
            out.add(ReasonCode::CapabilityUnsupported, kNoIndex, key.value());
        }
        if (record->generation != r.evidence->capability) {
            out.add(ReasonCode::CapabilityGenerationMismatch, kNoIndex, record->generation.value());
        }
    }
}

void check_topology(const ValidationRequest& r, ValidationResult& out) {
    if (r.evidence == nullptr) {
        for (const Segment& s : r.segments) {
            if (s.kind == SegmentKind::Adjacency) {
                out.add(ReasonCode::TopologyEvidenceUnavailable);
                break;
            }
        }
        return;
    }
    if (r.topology.valid() && !r.evidence->topology.valid()) {
        out.add(ReasonCode::TopologyGenerationInvalid);
    }
    if (r.topology.valid() && r.evidence->topology.valid() &&
        r.topology != r.evidence->topology) {
        out.add(ReasonCode::TopologyGenerationMismatch, kNoIndex, r.evidence->topology.value());
    }
    for (std::size_t i = 0; i < r.segments.size(); ++i) {
        const Segment& s = r.segments[i];
        if (s.kind != SegmentKind::Adjacency) {
            continue;
        }
        const auto index = static_cast<std::uint32_t>(i);
        const AdjacencyRecord* record = r.evidence->find_adjacency(s.adjacency);
        if (record == nullptr) {
            out.add(ReasonCode::TopologyAdjacencyUnknown, index, s.adjacency.value());
            continue;
        }
        if (record->topology != s.topology) {
            out.add(ReasonCode::TopologyAdjacencyStale, index, record->topology.value());
        }
    }
}

void check_path_authority(const ValidationRequest& r, ValidationResult& out) {
    if (!r.path.valid()) {
        if (r.path_authority.valid()) {
            out.add(ReasonCode::PathBindingMissing, kNoIndex, r.path_authority.value());
        }
        return;
    }
    if (!r.path_authority.valid()) {
        out.add(ReasonCode::PathAuthorityGenerationInvalid, kNoIndex, r.path.value());
    }
    if (r.evidence == nullptr) {
        return;
    }
    const PathRecord* record = r.evidence->find_path(r.path);
    if (record == nullptr) {
        out.add(ReasonCode::PathUnknown, kNoIndex, r.path.value());
        return;
    }
    if (record->authority != r.path_authority) {
        out.add(ReasonCode::PathAuthorityGenerationStale, kNoIndex, record->authority.value());
    }
    if (record->scope.valid() && r.scope.valid() && record->scope != r.scope) {
        out.add(ReasonCode::PathBelongsToOtherScope, kNoIndex, record->scope.value());
    }
}

void check_route_binding(const ValidationRequest& r, ValidationResult& out) {
    if (!r.route.valid()) {
        return;
    }
    if (!r.route_generation.valid()) {
        out.add(ReasonCode::RouteBindingInvalid, kNoIndex, r.route.value());
    }
    if (r.evidence == nullptr) {
        return;
    }
    const RouteRecord* record = r.evidence->find_route(r.route);
    if (record == nullptr) {
        out.add(ReasonCode::RouteBindingUnknown, kNoIndex, r.route.value());
        return;
    }
    if (record->generation != r.route_generation) {
        out.add(ReasonCode::RouteBindingGenerationStale, kNoIndex, record->generation.value());
    }
    if (record->list.valid() && record->list != r.list_id) {
        out.add(ReasonCode::RouteBindingInvalid, kNoIndex, record->list.value());
    }
}

void check_constraint_binding(const ValidationRequest& r, ValidationResult& out) {
    if (!r.constraint.valid()) {
        return;
    }
    if (!r.constraint_generation.valid()) {
        out.add(ReasonCode::ConstraintBindingInvalid, kNoIndex, r.constraint.value());
    }
    if (r.evidence == nullptr) {
        return;
    }
    const ConstraintRecord* record = r.evidence->find_constraint(r.constraint);
    if (record == nullptr) {
        out.add(ReasonCode::ConstraintBindingUnknown, kNoIndex, r.constraint.value());
        return;
    }
    if (record->generation != r.constraint_generation) {
        out.add(ReasonCode::ConstraintBindingGenerationStale, kNoIndex, record->generation.value());
    }
}

void check_resource_limits(const ValidationRequest& r, ValidationResult& out) {
    if (r.limits == nullptr) {
        return;
    }
    if (r.segments.size() > r.limits->max_segments_per_list) {
        out.add(ReasonCode::LimitMaxSegmentsPerList, kNoIndex, r.limits->max_segments_per_list);
    }
    for (std::size_t i = 0; i < r.segments.size(); ++i) {
        if (r.segments[i].payload.size() > r.limits->max_segment_payload_bytes) {
            out.add(ReasonCode::LimitMaxSegmentPayloadBytes, static_cast<std::uint32_t>(i),
                    r.limits->max_segment_payload_bytes);
        }
    }
}

} // namespace

const PublisherState* AuthorityView::find(PublisherId id) const noexcept {
    for (const auto& p : publishers) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

bool AuthorityView::boot_fenced(WorkerBootId boot) const noexcept {
    if (!boot.valid()) {
        return false;
    }
    return std::find(fenced_boots.begin(), fenced_boots.end(), boot) != fenced_boots.end();
}

std::vector<Segment> canonicalize_segments(std::span<const Segment> segments) {
    std::vector<Segment> out(segments.begin(), segments.end());
    for (Segment& s : out) {
        if (s.encoding == SegmentEncodingId::Invalid) {
            s.encoding = expected_encoding_for(s.kind);
        }
    }
    return out;
}

ValidationResult validate_request(const ValidationRequest& request) {
    ValidationResult out;
    out.set_limits(request.limits);

    const std::uint32_t payload_width =
        request.profile != nullptr ? request.profile->payload_width : 0;

    check_caller_identity(request, out);
    check_epoch_boot_scope(request, out);
    check_list_lifecycle(request, out);
    if (request.full) {
        check_list_content_lifecycle(request, out);
        check_profile(request, out);
        check_policy(request, out);
        check_structural(request, out, payload_width);
        check_capability(request, out);
        check_topology(request, out);
        check_path_authority(request, out);
        check_route_binding(request, out);
        check_constraint_binding(request, out);
        check_resource_limits(request, out);
    }
    return out;
}

ValidationResult validate_segment_sequence(const SegmentSequenceRequest& request) {
    ValidationRequest r{};
    r.limits = request.limits;
    r.profile = request.profile;
    r.profile_found = request.profile != nullptr;
    r.profile_id = request.profile != nullptr ? request.profile->id : SegmentProfileId{};
    r.segments = request.segments;
    r.evidence = request.evidence;
    r.topology = request.topology;

    ValidationResult out;
    out.set_limits(request.limits);
    const std::uint32_t payload_width =
        request.profile != nullptr ? request.profile->payload_width : 0;
    check_list_content_lifecycle(r, out);
    check_profile(r, out);
    check_structural(r, out, payload_width);
    check_capability(r, out);
    check_topology(r, out);
    check_resource_limits(r, out);
    return out;
}

Currentness evaluate_currentness(const SegmentList& list, const SegmentProfile* profile,
                                 const SegmentPolicy* policy, const EvidenceWatermark& watermark,
                                 CoordinatorEpoch epoch) {
    if (list.state == LifecycleState::Retired || list.state == LifecycleState::Withdrawn ||
        list.state == LifecycleState::Superseded || list.state == LifecycleState::Revoked) {
        return Currentness::Unknown;
    }
    if (epoch.valid() && list.authority.epoch.valid() && epoch != list.authority.epoch) {
        return Currentness::StaleEpoch;
    }
    if (profile == nullptr) {
        return Currentness::StaleProfile;
    }
    if (profile->generation != list.content.profile_generation) {
        return Currentness::StaleProfile;
    }
    if (list.content.policy.valid()) {
        if (policy == nullptr) {
            return Currentness::StalePolicy;
        }
        if (policy->generation != list.content.policy_generation) {
            return Currentness::StalePolicy;
        }
    }
    if (list.content.capability.valid() && watermark.capability.valid() &&
        list.content.capability != watermark.capability) {
        return Currentness::StaleCapability;
    }
    if (list.content.topology.valid() && watermark.topology.valid() &&
        list.content.topology != watermark.topology) {
        return Currentness::StaleTopology;
    }
    if (list.content.path.valid() && list.content.path_authority.valid() &&
        watermark.path_authority.valid() && list.content.path_authority != watermark.path_authority) {
        return Currentness::StalePathAuthority;
    }
    if (list.content.route.valid() && list.content.route_generation.valid() &&
        watermark.route.valid() && list.content.route_generation != watermark.route) {
        return Currentness::StaleRouteBinding;
    }
    if (list.content.constraint.valid() && list.content.constraint_generation.valid() &&
        watermark.constraint.valid() &&
        list.content.constraint_generation != watermark.constraint) {
        return Currentness::StaleConstraintBinding;
    }
    if (list.state == LifecycleState::RevalidationRequired) {
        return Currentness::RevalidationRequired;
    }
    if (list.state == LifecycleState::Active) {
        return Currentness::Current;
    }
    return Currentness::Unknown;
}

} // namespace srf
