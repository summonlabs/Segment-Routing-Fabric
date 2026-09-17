// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>

#include "srf/reason.hpp"

namespace srf {

enum class LifecycleState : std::uint8_t {
    Invalid = 0,
    Declared = 1,
    Validating = 2,
    Active = 3,
    RevalidationRequired = 4,
    Withdrawing = 5,
    Withdrawn = 6,
    Superseded = 7,
    Revoked = 8,
    Retired = 9,
};

enum class LifecycleEvent : std::uint8_t {
    Invalid = 0,
    ValidateBegin = 1,
    ValidateSucceed = 2,
    ValidateFail = 3,
    RevalidateBegin = 4,
    RevalidateSucceed = 5,
    RevalidateFail = 6,
    DependencyChanged = 7,
    WithdrawBegin = 8,
    WithdrawCommit = 9,
    SupersedeBegin = 10,
    SupersedeCommit = 11,
    Revoke = 12,
    Retire = 13,
    Reactivate = 14,
    RecoveryLoad = 15,
    /// A new revision of the same lineage is being created.
    ReplaceBegin = 16,
};

inline constexpr std::uint32_t kLifecycleStateCount = 9;
inline constexpr std::uint32_t kLifecycleEventCount = 16;

struct TransitionOutcome {
    bool allowed{false};
    LifecycleState next{LifecycleState::Invalid};
};

/// Literal transition table. Every (state, event) pair is defined, including every
/// denial. The table is total: there is no default-allow path and no implicit
/// self-transition.
[[nodiscard]] TransitionOutcome lifecycle_transition(LifecycleState state,
                                                     LifecycleEvent event) noexcept;

[[nodiscard]] std::string_view lifecycle_state_name(LifecycleState state) noexcept;
[[nodiscard]] std::string_view lifecycle_event_name(LifecycleEvent event) noexcept;

/// True when no event can move the state back to Active.
[[nodiscard]] bool lifecycle_is_terminal_for_activation(LifecycleState state) noexcept;

/// Whether a list in this state is currently available to a consumer.
[[nodiscard]] bool lifecycle_is_available(LifecycleState state) noexcept;

// ------------------------------------------------------------------ currentness
enum class Currentness : std::uint8_t {
    Unknown = 0,
    Current = 1,
    RevalidationRequired = 2,
    StaleProfile = 3,
    StalePolicy = 4,
    StaleTopology = 5,
    StaleCapability = 6,
    StalePathAuthority = 7,
    StaleRouteBinding = 8,
    StaleConstraintBinding = 9,
    StaleEpoch = 10,
    FencedPublisher = 11,
};

[[nodiscard]] std::string_view currentness_name(Currentness c) noexcept;
[[nodiscard]] ReasonCode currentness_reason(Currentness c) noexcept;
[[nodiscard]] bool currentness_is_usable(Currentness c) noexcept;

} // namespace srf
