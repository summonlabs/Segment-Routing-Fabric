// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/lifecycle.hpp"

namespace srf {
namespace {

// Encoded next state per (state, event) pair; 0 means denied.
//
// Event order: ValidateBegin, ValidateSucceed, ValidateFail, RevalidateBegin,
//              RevalidateSucceed, RevalidateFail, DependencyChanged,
//              WithdrawBegin, WithdrawCommit, SupersedeBegin, SupersedeCommit,
//              Revoke, Retire, Reactivate, RecoveryLoad, ReplaceBegin
constexpr std::uint8_t kD = static_cast<std::uint8_t>(LifecycleState::Invalid);
constexpr std::uint8_t kDecl = static_cast<std::uint8_t>(LifecycleState::Declared);
constexpr std::uint8_t kVal = static_cast<std::uint8_t>(LifecycleState::Validating);
constexpr std::uint8_t kAct = static_cast<std::uint8_t>(LifecycleState::Active);
constexpr std::uint8_t kRvl = static_cast<std::uint8_t>(LifecycleState::RevalidationRequired);
constexpr std::uint8_t kWbg = static_cast<std::uint8_t>(LifecycleState::Withdrawing);
constexpr std::uint8_t kWdn = static_cast<std::uint8_t>(LifecycleState::Withdrawn);
constexpr std::uint8_t kSup = static_cast<std::uint8_t>(LifecycleState::Superseded);
constexpr std::uint8_t kRev = static_cast<std::uint8_t>(LifecycleState::Revoked);
constexpr std::uint8_t kRet = static_cast<std::uint8_t>(LifecycleState::Retired);

constexpr std::uint8_t kTable[kLifecycleStateCount][kLifecycleEventCount] = {
    //                         VBegin VSucc VFail  RBegin RSucc RFail  DepChg  WBegin WCommit SBegin SCommit Revoke Retire React  Recovery Replace
    /* Declared            */ { kVal,  kD,   kD,    kD,    kD,   kD,    kD,     kWbg,  kD,     kSup,  kD,     kRev,  kRet,  kD,    kDecl,   kVal   },
    /* Validating          */ { kD,    kAct, kDecl, kD,    kD,   kD,    kDecl,  kWbg,  kD,     kSup,  kD,     kRev,  kRet,  kD,    kDecl,   kVal   },
    /* Active              */ { kD,    kD,   kD,    kVal,  kAct, kD,    kRvl,   kWbg,  kD,     kSup,  kD,     kRev,  kRet,  kD,    kRvl,    kVal   },
    /* RevalidationRequired*/ { kD,    kD,   kD,    kVal,  kAct, kRvl,  kRvl,   kWbg,  kD,     kSup,  kD,     kRev,  kRet,  kD,    kRvl,    kVal   },
    /* Withdrawing         */ { kD,    kD,   kD,    kD,    kD,   kD,    kWbg,   kD,    kWdn,   kSup,  kD,     kRev,  kRet,  kD,    kWdn,    kD     },
    /* Withdrawn           */ { kD,    kD,   kD,    kD,    kD,   kD,    kWdn,   kD,    kD,     kSup,  kD,     kRev,  kRet,  kD,    kWdn,    kD     },
    /* Superseded          */ { kD,    kD,   kD,    kD,    kD,   kD,    kSup,   kD,    kD,     kD,    kSup,   kRev,  kRet,  kD,    kSup,    kD     },
    /* Revoked             */ { kD,    kD,   kD,    kD,    kD,   kD,    kRev,   kD,    kD,     kD,    kD,     kD,    kRet,  kD,    kRev,    kD     },
    /* Retired             */ { kD,    kD,   kD,    kD,    kD,   kD,    kD,     kD,    kD,     kD,    kD,     kD,    kD,    kD,    kRet,    kD     },
};

[[nodiscard]] std::uint32_t state_index(LifecycleState s) noexcept {
    const auto v = static_cast<std::uint8_t>(s);
    return (v >= 1 && v <= kLifecycleStateCount) ? static_cast<std::uint32_t>(v) - 1u : 0xFFFF'FFFFu;
}

[[nodiscard]] std::uint32_t event_index(LifecycleEvent e) noexcept {
    const auto v = static_cast<std::uint8_t>(e);
    return (v >= 1 && v <= kLifecycleEventCount) ? static_cast<std::uint32_t>(v) - 1u : 0xFFFF'FFFFu;
}

} // namespace

TransitionOutcome lifecycle_transition(LifecycleState state, LifecycleEvent event) noexcept {
    TransitionOutcome out{};
    const std::uint32_t s = state_index(state);
    const std::uint32_t e = event_index(event);
    if (s == 0xFFFF'FFFFu || e == 0xFFFF'FFFFu) {
        return out;
    }
    const std::uint8_t next = kTable[s][e];
    if (next == 0) {
        return out;
    }
    out.allowed = true;
    out.next = static_cast<LifecycleState>(next);
    return out;
}

std::string_view lifecycle_state_name(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Invalid: return "Invalid";
        case LifecycleState::Declared: return "DECLARED";
        case LifecycleState::Validating: return "VALIDATING";
        case LifecycleState::Active: return "ACTIVE";
        case LifecycleState::RevalidationRequired: return "REVALIDATION_REQUIRED";
        case LifecycleState::Withdrawing: return "WITHDRAWING";
        case LifecycleState::Withdrawn: return "WITHDRAWN";
        case LifecycleState::Superseded: return "SUPERSEDED";
        case LifecycleState::Revoked: return "REVOKED";
        case LifecycleState::Retired: return "RETIRED";
    }
    return "Unknown";
}

std::string_view lifecycle_event_name(LifecycleEvent event) noexcept {
    switch (event) {
        case LifecycleEvent::Invalid: return "Invalid";
        case LifecycleEvent::ValidateBegin: return "ValidateBegin";
        case LifecycleEvent::ValidateSucceed: return "ValidateSucceed";
        case LifecycleEvent::ValidateFail: return "ValidateFail";
        case LifecycleEvent::RevalidateBegin: return "RevalidateBegin";
        case LifecycleEvent::RevalidateSucceed: return "RevalidateSucceed";
        case LifecycleEvent::RevalidateFail: return "RevalidateFail";
        case LifecycleEvent::DependencyChanged: return "DependencyChanged";
        case LifecycleEvent::WithdrawBegin: return "WithdrawBegin";
        case LifecycleEvent::WithdrawCommit: return "WithdrawCommit";
        case LifecycleEvent::SupersedeBegin: return "SupersedeBegin";
        case LifecycleEvent::SupersedeCommit: return "SupersedeCommit";
        case LifecycleEvent::Revoke: return "Revoke";
        case LifecycleEvent::Retire: return "Retire";
        case LifecycleEvent::Reactivate: return "Reactivate";
        case LifecycleEvent::RecoveryLoad: return "RecoveryLoad";
        case LifecycleEvent::ReplaceBegin: return "ReplaceBegin";
    }
    return "Unknown";
}

bool lifecycle_is_terminal_for_activation(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Withdrawn:
        case LifecycleState::Superseded:
        case LifecycleState::Revoked:
        case LifecycleState::Retired:
            return true;
        default:
            return false;
    }
}

bool lifecycle_is_available(LifecycleState state) noexcept {
    return state == LifecycleState::Active;
}

std::string_view currentness_name(Currentness c) noexcept {
    switch (c) {
        case Currentness::Unknown: return "Unknown";
        case Currentness::Current: return "CURRENT";
        case Currentness::RevalidationRequired: return "REVALIDATION_REQUIRED";
        case Currentness::StaleProfile: return "STALE_PROFILE";
        case Currentness::StalePolicy: return "STALE_POLICY";
        case Currentness::StaleTopology: return "STALE_TOPOLOGY";
        case Currentness::StaleCapability: return "STALE_CAPABILITY";
        case Currentness::StalePathAuthority: return "STALE_PATH_AUTHORITY";
        case Currentness::StaleRouteBinding: return "STALE_ROUTE_BINDING";
        case Currentness::StaleConstraintBinding: return "STALE_CONSTRAINT_BINDING";
        case Currentness::StaleEpoch: return "STALE_EPOCH";
        case Currentness::FencedPublisher: return "FENCED_PUBLISHER";
    }
    return "Unknown";
}

ReasonCode currentness_reason(Currentness c) noexcept {
    switch (c) {
        case Currentness::Current: return ReasonCode::CurrentnessCurrent;
        case Currentness::RevalidationRequired: return ReasonCode::CurrentnessRevalidationRequired;
        case Currentness::StaleProfile: return ReasonCode::CurrentnessStaleProfile;
        case Currentness::StalePolicy: return ReasonCode::CurrentnessStalePolicy;
        case Currentness::StaleTopology: return ReasonCode::CurrentnessStaleTopology;
        case Currentness::StaleCapability: return ReasonCode::CurrentnessStaleCapability;
        case Currentness::StalePathAuthority: return ReasonCode::CurrentnessStalePathAuthority;
        case Currentness::StaleRouteBinding: return ReasonCode::CurrentnessStaleRouteBinding;
        case Currentness::StaleConstraintBinding: return ReasonCode::CurrentnessStaleConstraintBinding;
        case Currentness::StaleEpoch: return ReasonCode::CurrentnessStaleEpoch;
        case Currentness::FencedPublisher: return ReasonCode::CurrentnessFencedPublisher;
        case Currentness::Unknown: break;
    }
    return ReasonCode::CurrentnessUnknown;
}

bool currentness_is_usable(Currentness c) noexcept {
    return c == Currentness::Current;
}

} // namespace srf
