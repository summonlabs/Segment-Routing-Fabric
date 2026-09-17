// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/authority.hpp"

#include <algorithm>

namespace srf {

CoordinatorEpoch AuthorityTable::epoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
}

bool AuthorityTable::advance_epoch() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto next = epoch_.next();
    if (!next.has_value()) {
        return false;
    }
    epoch_ = *next;
    return true;
}

bool AuthorityTable::restore_epoch(CoordinatorEpoch epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!epoch.valid() || epoch < epoch_) {
        return false;
    }
    epoch_ = epoch;
    return true;
}

ValidationResult AuthorityTable::register_publisher(PublisherId id, WorkerBootId boot,
                                                    ScopeId scope, CoordinatorEpoch epoch,
                                                    const Limits& limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    result.set_limits(&limits);
    if (!id.valid()) {
        result.add(ReasonCode::CallerMissingPublisher);
        return result;
    }
    if (!boot.valid()) {
        result.add(ReasonCode::CallerMissingWorkerBoot);
        return result;
    }
    if (!scope.valid()) {
        result.add(ReasonCode::CallerMissingScope);
        return result;
    }
    if (!epoch.valid()) {
        result.add(ReasonCode::EpochInvalid);
        return result;
    }
    if (epoch != epoch_) {
        result.add(ReasonCode::EpochStale, 0, epoch.value());
        return result;
    }
    if (std::find(fenced_boots_.begin(), fenced_boots_.end(), boot) != fenced_boots_.end()) {
        result.add(ReasonCode::BootAlreadyFenced, 0, boot.value());
        return result;
    }
    for (auto& p : publishers_) {
        if (p.id == id) {
            if (p.boot != boot) {
                result.add(ReasonCode::CallerStaleWorkerBoot, 0, p.boot.value());
                return result;
            }
            p.scope = scope;
            p.epoch = epoch;
            p.alive = true;
            p.fenced = false;
            return result;
        }
    }
    if (publishers_.size() >= limits.max_publishers) {
        result.add(ReasonCode::LimitMaxPublishers, 0, limits.max_publishers);
        return result;
    }
    publishers_.push_back(PublisherState{id, boot, scope, epoch, true, false});
    return result;
}

ValidationResult AuthorityTable::heartbeat(PublisherId id, WorkerBootId boot,
                                           CoordinatorEpoch epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    if (!id.valid()) {
        result.add(ReasonCode::CallerMissingPublisher);
        return result;
    }
    if (!boot.valid()) {
        result.add(ReasonCode::CallerMissingWorkerBoot);
        return result;
    }
    if (epoch != epoch_) {
        result.add(ReasonCode::EpochStale, 0, epoch.value());
        return result;
    }
    for (auto& p : publishers_) {
        if (p.id == id) {
            if (p.boot != boot) {
                result.add(ReasonCode::CallerStaleWorkerBoot, 0, p.boot.value());
                return result;
            }
            if (p.fenced || std::find(fenced_boots_.begin(), fenced_boots_.end(), boot) !=
                                fenced_boots_.end()) {
                result.add(ReasonCode::CallerFencedPublisher, 0, boot.value());
                return result;
            }
            p.alive = true;
            return result;
        }
    }
    result.add(ReasonCode::CallerUnknownPublisher, 0, id.value());
    return result;
}

ValidationResult AuthorityTable::mark_dead(PublisherId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    for (auto& p : publishers_) {
        if (p.id == id) {
            p.alive = false;
            p.fenced = true;
            if (std::find(fenced_boots_.begin(), fenced_boots_.end(), p.boot) ==
                fenced_boots_.end()) {
                fenced_boots_.push_back(p.boot);
            }
            return result;
        }
    }
    result.add(ReasonCode::CallerUnknownPublisher, 0, id.value());
    return result;
}

ValidationResult AuthorityTable::fence_boot(WorkerBootId boot) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidationResult result;
    if (!boot.valid()) {
        result.add(ReasonCode::CallerMissingWorkerBoot);
        return result;
    }
    if (std::find(fenced_boots_.begin(), fenced_boots_.end(), boot) == fenced_boots_.end()) {
        fenced_boots_.push_back(boot);
    }
    for (auto& p : publishers_) {
        if (p.boot == boot) {
            p.fenced = true;
            p.alive = false;
        }
    }
    return result;
}

bool AuthorityTable::is_fenced(WorkerBootId boot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(fenced_boots_.begin(), fenced_boots_.end(), boot) != fenced_boots_.end();
}

bool AuthorityTable::is_alive(PublisherId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : publishers_) {
        if (p.id == id) {
            return p.alive && !p.fenced;
        }
    }
    return false;
}

const PublisherState* AuthorityTable::find(PublisherId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : publishers_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<PublisherState> AuthorityTable::publishers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishers_;
}

std::vector<WorkerBootId> AuthorityTable::fenced_boots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fenced_boots_;
}

std::size_t AuthorityTable::publisher_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishers_.size();
}

void AuthorityTable::restore_publisher(const PublisherState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : publishers_) {
        if (p.id == state.id) {
            p = state;
            return;
        }
    }
    publishers_.push_back(state);
}

void AuthorityTable::restore_fenced_boot(WorkerBootId boot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(fenced_boots_.begin(), fenced_boots_.end(), boot) == fenced_boots_.end()) {
        fenced_boots_.push_back(boot);
    }
}

void AuthorityTable::fence_all_on_recovery() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : publishers_) {
        p.alive = false;
        p.fenced = true;
        if (std::find(fenced_boots_.begin(), fenced_boots_.end(), p.boot) ==
            fenced_boots_.end()) {
            fenced_boots_.push_back(p.boot);
        }
    }
}

} // namespace srf
