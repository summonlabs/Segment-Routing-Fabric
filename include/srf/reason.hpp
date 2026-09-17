// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "srf/limits.hpp"

namespace srf {

/// Fixed rejection precedence. Validation visits phases in ascending order and a
/// defect in an earlier phase always dominates a defect in a later phase.
///
/// WireDecode through PathAuthority is exactly the ordering required by the
/// specification. RouteBinding and ConstraintBinding are external-authority
/// bindings of the same class as PathAuthority, so they are inserted between
/// PathAuthority and ResourceLimits without disturbing the required subsequence.
enum class ValidationPhase : std::uint8_t {
    Ok = 0,
    WireDecode = 1,
    CallerIdentity = 2,
    EpochBootScope = 3,
    ListLifecycleGeneration = 4,
    Profile = 5,
    Policy = 6,
    StructuralSegment = 7,
    CapabilityEvidence = 8,
    TopologyEvidence = 9,
    PathAuthority = 10,
    RouteBinding = 11,
    ConstraintBinding = 12,
    ResourceLimits = 13,
    Commit = 14,
};

[[nodiscard]] std::string_view phase_name(ValidationPhase phase) noexcept;

/// Deterministic machine-readable rejection reasons. Each reason belongs to
/// exactly one phase.
enum class ReasonCode : std::uint16_t {
    Ok = 0,

    // ---- WireDecode
    WireMagicMismatch = 1,
    WireUnsupportedVersion,
    WireUnknownMessageId,
    WireMalformedFrame,
    WireHeaderTruncated,
    WirePayloadLengthMismatch,
    WireIntegrityMismatch,
    WireTrailingBytes,
    WireUnknownEnumValue,
    WireUnsupportedFlags,
    WireAssemblyOverflow,
    WireFieldOutOfRange,

    // ---- CallerIdentity
    CallerMissingPublisher = 40,
    CallerUnknownPublisher,
    CallerFencedPublisher,
    CallerMissingWorkerBoot,
    CallerStaleWorkerBoot,
    CallerMissingMutationAttempt,
    CallerMutationAttemptReuseMismatch,
    CallerMissingScope,
    CallerScopeMismatch,
    CallerInvalidIdentity,

    // ---- EpochBootScope
    EpochInvalid = 80,
    EpochStale,
    EpochExhausted,
    BootUnknown,
    BootAlreadyFenced,
    ScopeNotPermitted,
    ScopeMismatch,

    // ---- ListLifecycleGeneration
    ListIdInvalid = 120,
    ListNotFound,
    ListAlreadyExists,
    ListGenerationInvalid,
    ListGenerationConflict,
    ListGenerationExhausted,
    ListGenerationRegressed,
    LifecycleTransitionDenied,
    ListRetired,
    ListRevoked,
    ListWithdrawn,
    ListSuperseded,
    ListEmptyNotPermitted,
    ListSupersedesUnknown,
    ListKeyMismatch,
    ListLineageMismatch,

    // ---- Profile
    ProfileUnknown = 170,
    ProfileIdInvalid,
    ProfileGenerationInvalid,
    ProfileGenerationMismatch,
    ProfileKindNotAllowed,
    ProfilePayloadWidthExceeded,
    ProfileMaxDepthExceeded,
    ProfileRepeatNotPermitted,
    ProfileEmptyNotPermitted,
    ProfileEncodingUnsupported,
    ProfileKindMismatch,
    ProfileDigestMismatch,
    ProfileStrictnessUnsupported,

    // ---- Policy
    PolicyUnknown = 210,
    PolicyIdInvalid,
    PolicyGenerationInvalid,
    PolicyGenerationMismatch,
    PolicyDeniedProfile,
    PolicyDeniedSegmentKind,
    PolicyMaxDepthExceeded,
    PolicyRequiredEntityMissing,
    PolicyForbiddenEntityPresent,
    PolicyOwnershipMismatch,
    PolicyReplacementDenied,
    PolicyDerivationDenied,
    PolicyDigestMismatch,
    PolicyStrictnessRequired,

    // ---- StructuralSegment
    SegmentKindInvalid = 250,
    SegmentIdInvalid,
    SegmentGenerationInvalid,
    SegmentPayloadTooLarge,
    SegmentReservedFieldSet,
    SegmentMissingBinding,
    SegmentUnexpectedBinding,
    SegmentBindingIncomplete,
    SegmentEncodingMismatch,
    SegmentEntityUnknown,
    SegmentEntityGenerationMismatch,
    SegmentEntityKindMismatch,
    SegmentCountMismatch,
    SegmentDuplicateConsecutive,
    SegmentDuplicateNonConsecutive,
    SegmentAggregateDigestMismatch,

    // ---- CapabilityEvidence
    CapabilityMissing = 290,
    CapabilityUnsupported,
    CapabilityGenerationMismatch,
    CapabilityEvidenceUnavailable,
    CapabilityKeyInvalid,

    // ---- TopologyEvidence
    TopologyGenerationInvalid = 320,
    TopologyGenerationMismatch,
    TopologyAdjacencyUnknown,
    TopologyAdjacencyStale,
    TopologyEvidenceUnavailable,
    TopologyAdjacencyKindMismatch,

    // ---- PathAuthority
    PathIdInvalid = 350,
    PathUnknown,
    PathAuthorityGenerationInvalid,
    PathAuthorityGenerationStale,
    PathAuthorityDenied,
    PathBindingMissing,
    PathBelongsToOtherScope,

    // ---- RouteBinding
    RouteBindingUnknown = 380,
    RouteBindingGenerationStale,
    RouteBindingInvalid,

    // ---- ConstraintBinding
    ConstraintBindingUnknown = 400,
    ConstraintBindingGenerationStale,
    ConstraintBindingInvalid,

    // ---- ResourceLimits
    LimitMaxLists = 420,
    LimitMaxSegmentsPerList,
    LimitMaxSegmentPayloadBytes,
    LimitMaxProfiles,
    LimitMaxPolicies,
    LimitMaxBatchSize,
    LimitMaxPublishers,
    LimitMaxFrameBytes,
    LimitMaxHistory,
    LimitMaxExplanationEntries,
    LimitMaxPersistenceRecordBytes,
    LimitMaxReasons,
    LimitMaxDiffEntries,
    LimitMaxTotalSegments,
    LimitMaxAttemptRecords,
    LimitMaxEvidenceRecords,

    // ---- Commit
    CommitWatermarkChanged = 470,
    CommitConflict,
    CommitPersistenceFailure,
    CommitGenerationExhausted,
    CommitReplayPayloadMismatch,
    CommitAborted,
    CommitNotDurable,

    // ---- Currentness (reported on reads, not a rejection class of its own)
    CurrentnessCurrent = 520,
    CurrentnessStaleProfile,
    CurrentnessStalePolicy,
    CurrentnessStaleTopology,
    CurrentnessStaleCapability,
    CurrentnessStalePathAuthority,
    CurrentnessStaleRouteBinding,
    CurrentnessStaleConstraintBinding,
    CurrentnessStaleEpoch,
    CurrentnessFencedPublisher,
    CurrentnessRevalidationRequired,
    CurrentnessUnknown,

    // ---- Persistence decode of durable state
    PersistBadMagic = 560,
    PersistBadVersion,
    PersistTruncated,
    PersistCorrupt,
    PersistBadIntegrity,
    PersistBadRecord,
    PersistDuplicateIdentity,
    PersistImpossibleGeneration,
    PersistDanglingReference,
    PersistAbsurdCount,
    PersistTrailingBytes,
    PersistIoError,
    PersistIndexInconsistent,
    PersistCountMismatch,
    PersistOverflow,
    PersistKindInvalid,
    PersistProfileInvalid,
    PersistEncodingInvalid,
    PersistDepthInvalid,
    PersistDigestMismatch,
    PersistEmpty,

    // ---- Generic
    InvalidArgument = 620,
    InternalError,
    ResourceExhausted,
    NotSupported,
};

[[nodiscard]] std::string_view reason_code_name(ReasonCode code) noexcept;
[[nodiscard]] ValidationPhase phase_of(ReasonCode code) noexcept;

/// One machine-readable reason. Deterministic: no strings, no pointers and no
/// timestamps. The detail field carries a domain value (identifier, index,
/// generation) that is meaningful for the code, or zero.
struct Reason {
    ReasonCode code{ReasonCode::Ok};
    std::uint32_t index{0};
    std::uint64_t detail{0};

    friend constexpr bool operator==(const Reason&, const Reason&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Reason& a, const Reason& b) noexcept {
        if (a.code != b.code) {
            return static_cast<std::uint16_t>(a.code) <=> static_cast<std::uint16_t>(b.code);
        }
        if (a.index != b.index) {
            return a.index <=> b.index;
        }
        return a.detail <=> b.detail;
    }
};

/// Result of a validation. Never a bool: it always carries a deterministic primary
/// reason plus a bounded, deduplicated, phase-ordered complete reason list.
class ValidationResult {
public:
    ValidationResult() = default;

    [[nodiscard]] static ValidationResult success();
    [[nodiscard]] static ValidationResult failure(Reason r);
    [[nodiscard]] static ValidationResult failure(ReasonCode code, std::uint32_t index = 0,
                                                  std::uint64_t detail = 0);

    void add(ReasonCode code, std::uint32_t index = 0, std::uint64_t detail = 0);
    void absorb(const ValidationResult& other);
    /// Append every reason from other, then clear it, so the source buffer does not
    /// have to be copied.
    void absorb(ValidationResult&& other);

    void set_limits(const Limits* limits) noexcept { limits_ = limits; }

    [[nodiscard]] bool ok() const noexcept { return reasons_.empty(); }
    [[nodiscard]] bool failed() const noexcept { return !reasons_.empty(); }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] const Reason& primary() const noexcept;
    [[nodiscard]] std::span<const Reason> reasons() const noexcept {
        return {reasons_.data(), reasons_.size()};
    }
    [[nodiscard]] std::size_t size() const noexcept { return reasons_.size(); }
    [[nodiscard]] bool contains(ReasonCode code) const noexcept;
    [[nodiscard]] ValidationPhase primary_phase() const noexcept;
    [[nodiscard]] std::string_view primary_name() const noexcept;
    [[nodiscard]] std::string to_string() const;

private:
    void sort_and_dedup();

    std::vector<Reason> reasons_{};
    const Limits* limits_{nullptr};
    bool truncated_{false};
};

} // namespace srf
