// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

namespace {

constexpr srf::LifecycleState kStates[] = {
    srf::LifecycleState::Declared,   srf::LifecycleState::Validating,
    srf::LifecycleState::Active,     srf::LifecycleState::RevalidationRequired,
    srf::LifecycleState::Withdrawing, srf::LifecycleState::Withdrawn,
    srf::LifecycleState::Superseded, srf::LifecycleState::Revoked,
    srf::LifecycleState::Retired,
};

constexpr srf::LifecycleEvent kEvents[] = {
    srf::LifecycleEvent::ValidateBegin,    srf::LifecycleEvent::ValidateSucceed,
    srf::LifecycleEvent::ValidateFail,     srf::LifecycleEvent::RevalidateBegin,
    srf::LifecycleEvent::RevalidateSucceed, srf::LifecycleEvent::RevalidateFail,
    srf::LifecycleEvent::DependencyChanged, srf::LifecycleEvent::WithdrawBegin,
    srf::LifecycleEvent::WithdrawCommit,   srf::LifecycleEvent::SupersedeBegin,
    srf::LifecycleEvent::SupersedeCommit,  srf::LifecycleEvent::Revoke,
    srf::LifecycleEvent::Retire,           srf::LifecycleEvent::Reactivate,
    srf::LifecycleEvent::RecoveryLoad,     srf::LifecycleEvent::ReplaceBegin,
};

[[nodiscard]] srf::LifecycleState expected_next(srf::LifecycleState state,
                                                srf::LifecycleEvent event) {
    using S = srf::LifecycleState;
    using E = srf::LifecycleEvent;
    switch (state) {
        case S::Declared:
            if (event == E::ReplaceBegin) return S::Validating;
            if (event == E::ValidateBegin) return S::Validating;
            if (event == E::WithdrawBegin) return S::Withdrawing;
            if (event == E::SupersedeBegin) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::Declared;
            return S::Invalid;
        case S::Validating:
            if (event == E::ReplaceBegin) return S::Validating;
            if (event == E::ValidateSucceed) return S::Active;
            if (event == E::ValidateFail) return S::Declared;
            if (event == E::DependencyChanged) return S::Declared;
            if (event == E::WithdrawBegin) return S::Withdrawing;
            if (event == E::SupersedeBegin) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::Declared;
            return S::Invalid;
        case S::Active:
            if (event == E::ReplaceBegin) return S::Validating;
            if (event == E::RevalidateBegin) return S::Validating;
            if (event == E::RevalidateSucceed) return S::Active;
            if (event == E::DependencyChanged) return S::RevalidationRequired;
            if (event == E::WithdrawBegin) return S::Withdrawing;
            if (event == E::SupersedeBegin) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::RevalidationRequired;
            return S::Invalid;
        case S::RevalidationRequired:
            if (event == E::ReplaceBegin) return S::Validating;
            if (event == E::RevalidateBegin) return S::Validating;
            if (event == E::RevalidateSucceed) return S::Active;
            if (event == E::RevalidateFail) return S::RevalidationRequired;
            if (event == E::DependencyChanged) return S::RevalidationRequired;
            if (event == E::WithdrawBegin) return S::Withdrawing;
            if (event == E::SupersedeBegin) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::RevalidationRequired;
            return S::Invalid;
        case S::Withdrawing:
            if (event == E::WithdrawCommit) return S::Withdrawn;
            if (event == E::DependencyChanged) return S::Withdrawing;
            if (event == E::SupersedeBegin) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::Withdrawn;
            return S::Invalid;
        case S::Withdrawn:
            if (event == E::DependencyChanged) return S::Withdrawn;
            if (event == E::SupersedeBegin) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::Withdrawn;
            return S::Invalid;
        case S::Superseded:
            if (event == E::DependencyChanged) return S::Superseded;
            if (event == E::SupersedeCommit) return S::Superseded;
            if (event == E::Revoke) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::Superseded;
            return S::Invalid;
        case S::Revoked:
            if (event == E::DependencyChanged) return S::Revoked;
            if (event == E::Retire) return S::Retired;
            if (event == E::RecoveryLoad) return S::Revoked;
            return S::Invalid;
        case S::Retired:
            if (event == E::RecoveryLoad) return S::Retired;
            return S::Invalid;
        case S::Invalid:
            return S::Invalid;
    }
    return S::Invalid;
}

} // namespace

SRF_TEST(lifecycle, every_state_event_pair_matches_the_literal_table) {
    std::size_t allowed = 0;
    for (const srf::LifecycleState state : kStates) {
        for (const srf::LifecycleEvent event : kEvents) {
            const srf::TransitionOutcome outcome = srf::lifecycle_transition(state, event);
            const srf::LifecycleState expected = expected_next(state, event);
            SRF_EXPECT_EQ(static_cast<int>(outcome.allowed), static_cast<int>(expected != srf::LifecycleState::Invalid));
            SRF_EXPECT_EQ(static_cast<int>(outcome.next), static_cast<int>(expected));
            if (outcome.allowed) {
                ++allowed;
            }
        }
    }
    // 9 states x 16 events: the table is total and permits exactly 55 transitions.
    SRF_EXPECT_EQ(allowed, static_cast<std::size_t>(55));
    SRF_EXPECT_EQ(static_cast<std::size_t>(srf::kLifecycleStateCount) * srf::kLifecycleEventCount,
                  static_cast<std::size_t>(144));
    // Replacement is refused from every state that must never be revived.
    for (const srf::LifecycleState state :
         {srf::LifecycleState::Withdrawing, srf::LifecycleState::Withdrawn,
          srf::LifecycleState::Superseded, srf::LifecycleState::Revoked,
          srf::LifecycleState::Retired}) {
        SRF_EXPECT(!srf::lifecycle_transition(state, srf::LifecycleEvent::ReplaceBegin).allowed);
    }
}

SRF_TEST(lifecycle, retired_and_revoked_lineages_never_reactivate) {
    using S = srf::LifecycleState;
    using E = srf::LifecycleEvent;
    const S terminal[] = {S::Retired, S::Revoked, S::Superseded, S::Withdrawn};
    for (const S state : terminal) {
        SRF_EXPECT(srf::lifecycle_is_terminal_for_activation(state));
        for (const E event : {E::ValidateBegin, E::ValidateSucceed, E::RevalidateBegin,
                              E::RevalidateSucceed, E::Reactivate}) {
            const srf::TransitionOutcome outcome = srf::lifecycle_transition(state, event);
            SRF_EXPECT(!outcome.allowed);
        }
        // Retired is fully terminal: nothing but recovery leaves it, and recovery
        // maps it to itself.
        if (state == S::Retired) {
            for (const E event : kEvents) {
                const srf::TransitionOutcome outcome = srf::lifecycle_transition(state, event);
                if (event == E::RecoveryLoad) {
                    SRF_EXPECT(outcome.allowed);
                    SRF_EXPECT(outcome.next == S::Retired);
                } else {
                    SRF_EXPECT(!outcome.allowed);
                }
            }
        }
    }
}

SRF_TEST(lifecycle, availability_requires_active) {
    for (const srf::LifecycleState state : kStates) {
        SRF_EXPECT_EQ(srf::lifecycle_is_available(state),
                      state == srf::LifecycleState::Active);
    }
}

SRF_TEST(lifecycle, currentness_reason_mapping_is_total) {
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(srf::Currentness::FencedPublisher);
         ++raw) {
        const auto currentness = static_cast<srf::Currentness>(raw);
        SRF_EXPECT(!srf::currentness_name(currentness).empty());
        SRF_EXPECT(srf::currentness_reason(currentness) != srf::ReasonCode::Ok);
    }
    SRF_EXPECT(srf::currentness_is_usable(srf::Currentness::Current));
    SRF_EXPECT(!srf::currentness_is_usable(srf::Currentness::StaleCapability));
    SRF_EXPECT(!srf::currentness_is_usable(srf::Currentness::FencedPublisher));
}
