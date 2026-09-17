// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/reason.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace srf {
namespace {

struct CodeName {
    ReasonCode code;
    std::string_view name;
    ValidationPhase phase;
};

// Ordered table. A code that is missing here is an internal defect: lookups fall
// back to UnnamedReason and the reason-phase mapping defaults to Commit, which
// makes an omission loud in tests rather than silent.
constexpr CodeName kCodes[] = {
    {ReasonCode::Ok, "Ok", ValidationPhase::Ok},
    {ReasonCode::WireMagicMismatch, "WireMagicMismatch", ValidationPhase::WireDecode},
    {ReasonCode::WireUnsupportedVersion, "WireUnsupportedVersion", ValidationPhase::WireDecode},
    {ReasonCode::WireUnknownMessageId, "WireUnknownMessageId", ValidationPhase::WireDecode},
    {ReasonCode::WireMalformedFrame, "WireMalformedFrame", ValidationPhase::WireDecode},
    {ReasonCode::WireHeaderTruncated, "WireHeaderTruncated", ValidationPhase::WireDecode},
    {ReasonCode::WirePayloadLengthMismatch, "WirePayloadLengthMismatch", ValidationPhase::WireDecode},
    {ReasonCode::WireIntegrityMismatch, "WireIntegrityMismatch", ValidationPhase::WireDecode},
    {ReasonCode::WireTrailingBytes, "WireTrailingBytes", ValidationPhase::WireDecode},
    {ReasonCode::WireUnknownEnumValue, "WireUnknownEnumValue", ValidationPhase::WireDecode},
    {ReasonCode::WireUnsupportedFlags, "WireUnsupportedFlags", ValidationPhase::WireDecode},
    {ReasonCode::WireAssemblyOverflow, "WireAssemblyOverflow", ValidationPhase::WireDecode},
    {ReasonCode::WireFieldOutOfRange, "WireFieldOutOfRange", ValidationPhase::WireDecode},

    {ReasonCode::CallerMissingPublisher, "CallerMissingPublisher", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerUnknownPublisher, "CallerUnknownPublisher", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerFencedPublisher, "CallerFencedPublisher", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerMissingWorkerBoot, "CallerMissingWorkerBoot", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerStaleWorkerBoot, "CallerStaleWorkerBoot", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerMissingMutationAttempt, "CallerMissingMutationAttempt", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerMutationAttemptReuseMismatch, "CallerMutationAttemptReuseMismatch", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerMissingScope, "CallerMissingScope", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerScopeMismatch, "CallerScopeMismatch", ValidationPhase::CallerIdentity},
    {ReasonCode::CallerInvalidIdentity, "CallerInvalidIdentity", ValidationPhase::CallerIdentity},

    {ReasonCode::EpochInvalid, "EpochInvalid", ValidationPhase::EpochBootScope},
    {ReasonCode::EpochStale, "EpochStale", ValidationPhase::EpochBootScope},
    {ReasonCode::EpochExhausted, "EpochExhausted", ValidationPhase::EpochBootScope},
    {ReasonCode::BootUnknown, "BootUnknown", ValidationPhase::EpochBootScope},
    {ReasonCode::BootAlreadyFenced, "BootAlreadyFenced", ValidationPhase::EpochBootScope},
    {ReasonCode::ScopeNotPermitted, "ScopeNotPermitted", ValidationPhase::EpochBootScope},
    {ReasonCode::ScopeMismatch, "ScopeMismatch", ValidationPhase::EpochBootScope},

    {ReasonCode::ListIdInvalid, "ListIdInvalid", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListNotFound, "ListNotFound", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListAlreadyExists, "ListAlreadyExists", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListGenerationInvalid, "ListGenerationInvalid", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListGenerationConflict, "ListGenerationConflict", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListGenerationExhausted, "ListGenerationExhausted", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListGenerationRegressed, "ListGenerationRegressed", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::LifecycleTransitionDenied, "LifecycleTransitionDenied", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListRetired, "ListRetired", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListRevoked, "ListRevoked", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListWithdrawn, "ListWithdrawn", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListSuperseded, "ListSuperseded", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListEmptyNotPermitted, "ListEmptyNotPermitted", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListSupersedesUnknown, "ListSupersedesUnknown", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListKeyMismatch, "ListKeyMismatch", ValidationPhase::ListLifecycleGeneration},
    {ReasonCode::ListLineageMismatch, "ListLineageMismatch", ValidationPhase::ListLifecycleGeneration},

    {ReasonCode::ProfileUnknown, "ProfileUnknown", ValidationPhase::Profile},
    {ReasonCode::ProfileIdInvalid, "ProfileIdInvalid", ValidationPhase::Profile},
    {ReasonCode::ProfileGenerationInvalid, "ProfileGenerationInvalid", ValidationPhase::Profile},
    {ReasonCode::ProfileGenerationMismatch, "ProfileGenerationMismatch", ValidationPhase::Profile},
    {ReasonCode::ProfileKindNotAllowed, "ProfileKindNotAllowed", ValidationPhase::Profile},
    {ReasonCode::ProfilePayloadWidthExceeded, "ProfilePayloadWidthExceeded", ValidationPhase::Profile},
    {ReasonCode::ProfileMaxDepthExceeded, "ProfileMaxDepthExceeded", ValidationPhase::Profile},
    {ReasonCode::ProfileRepeatNotPermitted, "ProfileRepeatNotPermitted", ValidationPhase::Profile},
    {ReasonCode::ProfileEmptyNotPermitted, "ProfileEmptyNotPermitted", ValidationPhase::Profile},
    {ReasonCode::ProfileEncodingUnsupported, "ProfileEncodingUnsupported", ValidationPhase::Profile},
    {ReasonCode::ProfileKindMismatch, "ProfileKindMismatch", ValidationPhase::Profile},
    {ReasonCode::ProfileDigestMismatch, "ProfileDigestMismatch", ValidationPhase::Profile},
    {ReasonCode::ProfileStrictnessUnsupported, "ProfileStrictnessUnsupported", ValidationPhase::Profile},

    {ReasonCode::PolicyUnknown, "PolicyUnknown", ValidationPhase::Policy},
    {ReasonCode::PolicyIdInvalid, "PolicyIdInvalid", ValidationPhase::Policy},
    {ReasonCode::PolicyGenerationInvalid, "PolicyGenerationInvalid", ValidationPhase::Policy},
    {ReasonCode::PolicyGenerationMismatch, "PolicyGenerationMismatch", ValidationPhase::Policy},
    {ReasonCode::PolicyDeniedProfile, "PolicyDeniedProfile", ValidationPhase::Policy},
    {ReasonCode::PolicyDeniedSegmentKind, "PolicyDeniedSegmentKind", ValidationPhase::Policy},
    {ReasonCode::PolicyMaxDepthExceeded, "PolicyMaxDepthExceeded", ValidationPhase::Policy},
    {ReasonCode::PolicyRequiredEntityMissing, "PolicyRequiredEntityMissing", ValidationPhase::Policy},
    {ReasonCode::PolicyForbiddenEntityPresent, "PolicyForbiddenEntityPresent", ValidationPhase::Policy},
    {ReasonCode::PolicyOwnershipMismatch, "PolicyOwnershipMismatch", ValidationPhase::Policy},
    {ReasonCode::PolicyReplacementDenied, "PolicyReplacementDenied", ValidationPhase::Policy},
    {ReasonCode::PolicyDerivationDenied, "PolicyDerivationDenied", ValidationPhase::Policy},
    {ReasonCode::PolicyDigestMismatch, "PolicyDigestMismatch", ValidationPhase::Policy},
    {ReasonCode::PolicyStrictnessRequired, "PolicyStrictnessRequired", ValidationPhase::Policy},

    {ReasonCode::SegmentKindInvalid, "SegmentKindInvalid", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentIdInvalid, "SegmentIdInvalid", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentGenerationInvalid, "SegmentGenerationInvalid", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentPayloadTooLarge, "SegmentPayloadTooLarge", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentReservedFieldSet, "SegmentReservedFieldSet", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentMissingBinding, "SegmentMissingBinding", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentUnexpectedBinding, "SegmentUnexpectedBinding", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentBindingIncomplete, "SegmentBindingIncomplete", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentEncodingMismatch, "SegmentEncodingMismatch", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentEntityUnknown, "SegmentEntityUnknown", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentEntityGenerationMismatch, "SegmentEntityGenerationMismatch", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentEntityKindMismatch, "SegmentEntityKindMismatch", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentCountMismatch, "SegmentCountMismatch", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentDuplicateConsecutive, "SegmentDuplicateConsecutive", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentDuplicateNonConsecutive, "SegmentDuplicateNonConsecutive", ValidationPhase::StructuralSegment},
    {ReasonCode::SegmentAggregateDigestMismatch, "SegmentAggregateDigestMismatch", ValidationPhase::StructuralSegment},

    {ReasonCode::CapabilityMissing, "CapabilityMissing", ValidationPhase::CapabilityEvidence},
    {ReasonCode::CapabilityUnsupported, "CapabilityUnsupported", ValidationPhase::CapabilityEvidence},
    {ReasonCode::CapabilityGenerationMismatch, "CapabilityGenerationMismatch", ValidationPhase::CapabilityEvidence},
    {ReasonCode::CapabilityEvidenceUnavailable, "CapabilityEvidenceUnavailable", ValidationPhase::CapabilityEvidence},
    {ReasonCode::CapabilityKeyInvalid, "CapabilityKeyInvalid", ValidationPhase::CapabilityEvidence},

    {ReasonCode::TopologyGenerationInvalid, "TopologyGenerationInvalid", ValidationPhase::TopologyEvidence},
    {ReasonCode::TopologyGenerationMismatch, "TopologyGenerationMismatch", ValidationPhase::TopologyEvidence},
    {ReasonCode::TopologyAdjacencyUnknown, "TopologyAdjacencyUnknown", ValidationPhase::TopologyEvidence},
    {ReasonCode::TopologyAdjacencyStale, "TopologyAdjacencyStale", ValidationPhase::TopologyEvidence},
    {ReasonCode::TopologyEvidenceUnavailable, "TopologyEvidenceUnavailable", ValidationPhase::TopologyEvidence},
    {ReasonCode::TopologyAdjacencyKindMismatch, "TopologyAdjacencyKindMismatch", ValidationPhase::TopologyEvidence},

    {ReasonCode::PathIdInvalid, "PathIdInvalid", ValidationPhase::PathAuthority},
    {ReasonCode::PathUnknown, "PathUnknown", ValidationPhase::PathAuthority},
    {ReasonCode::PathAuthorityGenerationInvalid, "PathAuthorityGenerationInvalid", ValidationPhase::PathAuthority},
    {ReasonCode::PathAuthorityGenerationStale, "PathAuthorityGenerationStale", ValidationPhase::PathAuthority},
    {ReasonCode::PathAuthorityDenied, "PathAuthorityDenied", ValidationPhase::PathAuthority},
    {ReasonCode::PathBindingMissing, "PathBindingMissing", ValidationPhase::PathAuthority},
    {ReasonCode::PathBelongsToOtherScope, "PathBelongsToOtherScope", ValidationPhase::PathAuthority},

    {ReasonCode::RouteBindingUnknown, "RouteBindingUnknown", ValidationPhase::RouteBinding},
    {ReasonCode::RouteBindingGenerationStale, "RouteBindingGenerationStale", ValidationPhase::RouteBinding},
    {ReasonCode::RouteBindingInvalid, "RouteBindingInvalid", ValidationPhase::RouteBinding},

    {ReasonCode::ConstraintBindingUnknown, "ConstraintBindingUnknown", ValidationPhase::ConstraintBinding},
    {ReasonCode::ConstraintBindingGenerationStale, "ConstraintBindingGenerationStale", ValidationPhase::ConstraintBinding},
    {ReasonCode::ConstraintBindingInvalid, "ConstraintBindingInvalid", ValidationPhase::ConstraintBinding},

    {ReasonCode::LimitMaxLists, "LimitMaxLists", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxSegmentsPerList, "LimitMaxSegmentsPerList", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxSegmentPayloadBytes, "LimitMaxSegmentPayloadBytes", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxProfiles, "LimitMaxProfiles", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxPolicies, "LimitMaxPolicies", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxBatchSize, "LimitMaxBatchSize", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxPublishers, "LimitMaxPublishers", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxFrameBytes, "LimitMaxFrameBytes", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxHistory, "LimitMaxHistory", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxExplanationEntries, "LimitMaxExplanationEntries", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxPersistenceRecordBytes, "LimitMaxPersistenceRecordBytes", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxReasons, "LimitMaxReasons", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxDiffEntries, "LimitMaxDiffEntries", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxTotalSegments, "LimitMaxTotalSegments", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxAttemptRecords, "LimitMaxAttemptRecords", ValidationPhase::ResourceLimits},
    {ReasonCode::LimitMaxEvidenceRecords, "LimitMaxEvidenceRecords", ValidationPhase::ResourceLimits},

    {ReasonCode::CommitWatermarkChanged, "CommitWatermarkChanged", ValidationPhase::Commit},
    {ReasonCode::CommitConflict, "CommitConflict", ValidationPhase::Commit},
    {ReasonCode::CommitPersistenceFailure, "CommitPersistenceFailure", ValidationPhase::Commit},
    {ReasonCode::CommitGenerationExhausted, "CommitGenerationExhausted", ValidationPhase::Commit},
    {ReasonCode::CommitReplayPayloadMismatch, "CommitReplayPayloadMismatch", ValidationPhase::Commit},
    {ReasonCode::CommitAborted, "CommitAborted", ValidationPhase::Commit},
    {ReasonCode::CommitNotDurable, "CommitNotDurable", ValidationPhase::Commit},

    {ReasonCode::CurrentnessCurrent, "CurrentnessCurrent", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStaleProfile, "CurrentnessStaleProfile", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStalePolicy, "CurrentnessStalePolicy", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStaleTopology, "CurrentnessStaleTopology", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStaleCapability, "CurrentnessStaleCapability", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStalePathAuthority, "CurrentnessStalePathAuthority", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStaleRouteBinding, "CurrentnessStaleRouteBinding", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStaleConstraintBinding, "CurrentnessStaleConstraintBinding", ValidationPhase::Ok},
    {ReasonCode::CurrentnessStaleEpoch, "CurrentnessStaleEpoch", ValidationPhase::Ok},
    {ReasonCode::CurrentnessFencedPublisher, "CurrentnessFencedPublisher", ValidationPhase::Ok},
    {ReasonCode::CurrentnessRevalidationRequired, "CurrentnessRevalidationRequired", ValidationPhase::Ok},
    {ReasonCode::CurrentnessUnknown, "CurrentnessUnknown", ValidationPhase::Ok},

    {ReasonCode::PersistBadMagic, "PersistBadMagic", ValidationPhase::Commit},
    {ReasonCode::PersistBadVersion, "PersistBadVersion", ValidationPhase::Commit},
    {ReasonCode::PersistTruncated, "PersistTruncated", ValidationPhase::Commit},
    {ReasonCode::PersistCorrupt, "PersistCorrupt", ValidationPhase::Commit},
    {ReasonCode::PersistBadIntegrity, "PersistBadIntegrity", ValidationPhase::Commit},
    {ReasonCode::PersistBadRecord, "PersistBadRecord", ValidationPhase::Commit},
    {ReasonCode::PersistDuplicateIdentity, "PersistDuplicateIdentity", ValidationPhase::Commit},
    {ReasonCode::PersistImpossibleGeneration, "PersistImpossibleGeneration", ValidationPhase::Commit},
    {ReasonCode::PersistDanglingReference, "PersistDanglingReference", ValidationPhase::Commit},
    {ReasonCode::PersistAbsurdCount, "PersistAbsurdCount", ValidationPhase::Commit},
    {ReasonCode::PersistTrailingBytes, "PersistTrailingBytes", ValidationPhase::Commit},
    {ReasonCode::PersistIoError, "PersistIoError", ValidationPhase::Commit},
    {ReasonCode::PersistIndexInconsistent, "PersistIndexInconsistent", ValidationPhase::Commit},
    {ReasonCode::PersistCountMismatch, "PersistCountMismatch", ValidationPhase::Commit},
    {ReasonCode::PersistOverflow, "PersistOverflow", ValidationPhase::Commit},
    {ReasonCode::PersistKindInvalid, "PersistKindInvalid", ValidationPhase::Commit},
    {ReasonCode::PersistProfileInvalid, "PersistProfileInvalid", ValidationPhase::Commit},
    {ReasonCode::PersistEncodingInvalid, "PersistEncodingInvalid", ValidationPhase::Commit},
    {ReasonCode::PersistDepthInvalid, "PersistDepthInvalid", ValidationPhase::Commit},
    {ReasonCode::PersistDigestMismatch, "PersistDigestMismatch", ValidationPhase::Commit},
    {ReasonCode::PersistEmpty, "PersistEmpty", ValidationPhase::Commit},

    {ReasonCode::InvalidArgument, "InvalidArgument", ValidationPhase::CallerIdentity},
    {ReasonCode::InternalError, "InternalError", ValidationPhase::Commit},
    {ReasonCode::ResourceExhausted, "ResourceExhausted", ValidationPhase::ResourceLimits},
    {ReasonCode::NotSupported, "NotSupported", ValidationPhase::Profile},
};

[[nodiscard]] const CodeName* lookup(ReasonCode code) noexcept {
    for (const auto& entry : kCodes) {
        if (entry.code == code) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

std::string_view phase_name(ValidationPhase phase) noexcept {
    switch (phase) {
        case ValidationPhase::Ok: return "Ok";
        case ValidationPhase::WireDecode: return "WireDecode";
        case ValidationPhase::CallerIdentity: return "CallerIdentity";
        case ValidationPhase::EpochBootScope: return "EpochBootScope";
        case ValidationPhase::ListLifecycleGeneration: return "ListLifecycleGeneration";
        case ValidationPhase::Profile: return "Profile";
        case ValidationPhase::Policy: return "Policy";
        case ValidationPhase::StructuralSegment: return "StructuralSegment";
        case ValidationPhase::CapabilityEvidence: return "CapabilityEvidence";
        case ValidationPhase::TopologyEvidence: return "TopologyEvidence";
        case ValidationPhase::PathAuthority: return "PathAuthority";
        case ValidationPhase::RouteBinding: return "RouteBinding";
        case ValidationPhase::ConstraintBinding: return "ConstraintBinding";
        case ValidationPhase::ResourceLimits: return "ResourceLimits";
        case ValidationPhase::Commit: return "Commit";
    }
    return "Unknown";
}

std::string_view reason_code_name(ReasonCode code) noexcept {
    const CodeName* entry = lookup(code);
    return entry != nullptr ? entry->name : std::string_view{"UnnamedReason"};
}

ValidationPhase phase_of(ReasonCode code) noexcept {
    const CodeName* entry = lookup(code);
    return entry != nullptr ? entry->phase : ValidationPhase::Commit;
}

ValidationResult ValidationResult::success() {
    return ValidationResult{};
}

ValidationResult ValidationResult::failure(Reason r) {
    ValidationResult result;
    result.add(r.code, r.index, r.detail);
    return result;
}

ValidationResult ValidationResult::failure(ReasonCode code, std::uint32_t index,
                                            std::uint64_t detail) {
    ValidationResult result;
    result.add(code, index, detail);
    return result;
}

void ValidationResult::sort_and_dedup() {
    std::sort(reasons_.begin(), reasons_.end(), [](const Reason& a, const Reason& b) {
        const ValidationPhase pa = phase_of(a.code);
        const ValidationPhase pb = phase_of(b.code);
        if (pa != pb) {
            return pa < pb;
        }
        return a < b;
    });
    reasons_.erase(std::unique(reasons_.begin(), reasons_.end()), reasons_.end());
}

void ValidationResult::add(ReasonCode code, std::uint32_t index, std::uint64_t detail) {
    if (code == ReasonCode::Ok) {
        return;
    }
    const std::size_t cap =
        limits_ != nullptr ? static_cast<std::size_t>(limits_->max_reasons) : std::size_t{32};
    if (reasons_.size() >= cap) {
        // The cap itself is a reportable condition, and it is reported once.
        bool already = false;
        for (const Reason& r : reasons_) {
            if (r.code == ReasonCode::LimitMaxReasons) {
                already = true;
                break;
            }
        }
        if (!already && cap > 0) {
            reasons_.back() = Reason{ReasonCode::LimitMaxReasons, 0, 0};
            truncated_ = true;
            sort_and_dedup();
        } else if (cap == 0) {
            truncated_ = true;
        }
        return;
    }
    reasons_.push_back(Reason{code, index, detail});
    sort_and_dedup();
}

void ValidationResult::absorb(const ValidationResult& other) {
    truncated_ = truncated_ || other.truncated_;
    for (const Reason& r : other.reasons_) {
        add(r.code, r.index, r.detail);
    }
}

void ValidationResult::absorb(ValidationResult&& other) {
    truncated_ = truncated_ || other.truncated_;
    if (reasons_.empty() && limits_ == other.limits_) {
        reasons_ = std::move(other.reasons_);
        sort_and_dedup();
        return;
    }
    for (const Reason& r : other.reasons_) {
        add(r.code, r.index, r.detail);
    }
}

const Reason& ValidationResult::primary() const noexcept {
    static const Reason kNone{ReasonCode::Ok, 0, 0};
    return reasons_.empty() ? kNone : reasons_.front();
}

bool ValidationResult::contains(ReasonCode code) const noexcept {
    for (const Reason& r : reasons_) {
        if (r.code == code) {
            return true;
        }
    }
    return false;
}

ValidationPhase ValidationResult::primary_phase() const noexcept {
    return reasons_.empty() ? ValidationPhase::Ok : phase_of(reasons_.front().code);
}

std::string_view ValidationResult::primary_name() const noexcept {
    return reasons_.empty() ? std::string_view{"Ok"} : reason_code_name(reasons_.front().code);
}

std::string ValidationResult::to_string() const {
    std::string out;
    if (reasons_.empty()) {
        return "Ok";
    }
    out += std::string(primary_name());
    char buf[96];
    for (std::size_t i = 1; i < reasons_.size(); ++i) {
        std::snprintf(buf, sizeof(buf), ",%s[%u]", std::string(reason_code_name(reasons_[i].code)).c_str(),
                      reasons_[i].index);
        out += buf;
    }
    if (truncated_) {
        out += ",truncated";
    }
    return out;
}

} // namespace srf
