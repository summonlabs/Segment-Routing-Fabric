// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/registry.hpp"

namespace srf {

ValidationResult Registry::register_profile(const SegmentProfile& profile, const Limits& limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    result.set_limits(&limits);
    if (!profile.id.valid()) {
        result.add(ReasonCode::ProfileIdInvalid);
        return result;
    }
    if (!profile.generation.valid()) {
        result.add(ReasonCode::ProfileGenerationInvalid);
        return result;
    }
    if (profile.payload_width == 0 || profile.max_depth == 0) {
        result.add(ReasonCode::ProfileIdInvalid, 0, profile.id.value());
        return result;
    }
    for (auto& p : profiles_) {
        if (p.id == profile.id) {
            if (profile.generation <= p.generation) {
                result.add(ReasonCode::ProfileGenerationInvalid, 0, p.generation.value());
                return result;
            }
            p = profile;
            ++revision_;
            return result;
        }
    }
    if (profiles_.size() >= limits.max_profiles) {
        result.add(ReasonCode::LimitMaxProfiles, 0, limits.max_profiles);
        return result;
    }
    profiles_.push_back(profile);
    ++revision_;
    return result;
}

ValidationResult Registry::register_policy(const SegmentPolicy& policy, const Limits& limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    result.set_limits(&limits);
    if (!policy.id.valid()) {
        result.add(ReasonCode::PolicyIdInvalid);
        return result;
    }
    if (!policy.generation.valid()) {
        result.add(ReasonCode::PolicyGenerationInvalid);
        return result;
    }
    for (auto& p : policies_) {
        if (p.id == policy.id) {
            if (policy.generation <= p.generation) {
                result.add(ReasonCode::PolicyGenerationInvalid, 0, p.generation.value());
                return result;
            }
            p = policy;
            ++revision_;
            return result;
        }
    }
    if (policies_.size() >= limits.max_policies) {
        result.add(ReasonCode::LimitMaxPolicies, 0, limits.max_policies);
        return result;
    }
    policies_.push_back(policy);
    ++revision_;
    return result;
}

ValidationResult Registry::advance_profile_generation(SegmentProfileId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    for (auto& p : profiles_) {
        if (p.id == id) {
            const auto next = p.generation.next();
            if (!next.has_value()) {
                result.add(ReasonCode::ProfileGenerationInvalid, 0, p.generation.value());
                return result;
            }
            p.generation = *next;
            ++revision_;
            return result;
        }
    }
    result.add(ReasonCode::ProfileUnknown, 0, id.value());
    return result;
}

ValidationResult Registry::advance_policy_generation(SegmentPolicyId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    for (auto& p : policies_) {
        if (p.id == id) {
            const auto next = p.generation.next();
            if (!next.has_value()) {
                result.add(ReasonCode::PolicyGenerationInvalid, 0, p.generation.value());
                return result;
            }
            p.generation = *next;
            ++revision_;
            return result;
        }
    }
    result.add(ReasonCode::PolicyUnknown, 0, id.value());
    return result;
}

bool Registry::find_profile(SegmentProfileId id, SegmentProfile& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : profiles_) {
        if (p.id == id) {
            out = p;
            return true;
        }
    }
    return false;
}

bool Registry::find_profile_exact(SegmentProfileId id, SegmentProfileGeneration generation,
                                  SegmentProfile& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : profiles_) {
        if (p.id == id) {
            if (!generation.valid() || p.generation == generation) {
                out = p;
                return true;
            }
            out = p;
            return false;
        }
    }
    return false;
}

bool Registry::find_policy(SegmentPolicyId id, SegmentPolicy& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : policies_) {
        if (p.id == id) {
            out = p;
            return true;
        }
    }
    return false;
}

bool Registry::find_policy_exact(SegmentPolicyId id, SegmentPolicyGeneration generation,
                                 SegmentPolicy& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : policies_) {
        if (p.id == id) {
            out = p;
            if (!generation.valid() || p.generation == generation) {
                return true;
            }
            return false;
        }
    }
    return false;
}

std::vector<SegmentProfile> Registry::profiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_;
}

std::vector<SegmentPolicy> Registry::policies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policies_;
}

std::size_t Registry::profile_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_.size();
}

std::size_t Registry::policy_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policies_.size();
}

std::uint64_t Registry::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

void Registry::restore_profile(const SegmentProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : profiles_) {
        if (p.id == profile.id) {
            p = profile;
            return;
        }
    }
    profiles_.push_back(profile);
}

void Registry::restore_policy(const SegmentPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : policies_) {
        if (p.id == policy.id) {
            p = policy;
            return;
        }
    }
    policies_.push_back(policy);
}

} // namespace srf
