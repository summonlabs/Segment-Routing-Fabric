// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

#include <stdexcept>
#include <utility>

namespace srf::test {

Fabric::Fabric() : evidence_(std::make_shared<SyntheticEvidence>()) {
    evidence_->set_topology_generation(TopologyGeneration{kTopology});
    evidence_->set_capability_generation(CapabilityGeneration{kCapabilityGeneration});
    evidence_->set_path_authority_generation(PathAuthorityGeneration{kPathAuthority});
    evidence_->set_route_generation(RouteGeneration{kRouteGeneration});
    evidence_->add_capability(CapabilityKey{kCapabilityA}, CapabilityGeneration{kCapabilityGeneration},
                              true);
    evidence_->add_capability(CapabilityKey{kCapabilityB}, CapabilityGeneration{kCapabilityGeneration},
                              true);
    for (std::uint64_t n = 1; n <= 64; ++n) {
        evidence_->add_node(node_id(n), ScopeId{kScope});
        evidence_->add_segment(segment_id(n), SegmentKind::Node, SegmentGeneration{1},
                               ScopeId{kScope});
        evidence_->add_adjacency(adjacency_id(n), TopologyGeneration{kTopology}, node_id(n),
                                 node_id(n + 1), ScopeId{kScope});
        evidence_->add_endpoint(endpoint_id(n), node_id(n), ScopeId{kScope});
        evidence_->add_binding(binding_id(n), segment_id(n), ScopeId{kScope});
        evidence_->add_constraint(ConstraintEvaluationId{kConstraint},
                                  ConstraintGeneration{kConstraintGeneration},
                                  ScopeId{kScope});
    }
    // Adjacency-typed and endpoint-typed segment records for the typed kinds.
    for (std::uint64_t n = 1; n <= 64; ++n) {
        evidence_->add_segment(segment_id(100 + n), SegmentKind::Adjacency, SegmentGeneration{1},
                               ScopeId{kScope});
        evidence_->add_segment(segment_id(200 + n), SegmentKind::Endpoint, SegmentGeneration{1},
                               ScopeId{kScope});
        evidence_->add_segment(segment_id(300 + n), SegmentKind::Binding, SegmentGeneration{1},
                               ScopeId{kScope});
        evidence_->add_segment(segment_id(400 + n), SegmentKind::Policy, SegmentGeneration{1},
                               ScopeId{kScope});
    }
    evidence_->add_path(PathId{kPath}, PathAuthorityGeneration{kPathAuthority}, ScopeId{kScope});
    evidence_->add_route(RouteId{kRoute}, RouteGeneration{kRouteGeneration}, SegmentListId{},
                         SegmentListGeneration{}, ScopeId{kScope});
}

void Fabric::advance_topology() {
    topology_generation_ += 1;
    evidence_->set_topology_generation(TopologyGeneration{topology_generation_});
    for (std::uint64_t n = 1; n <= 64; ++n) {
        evidence_->add_adjacency(adjacency_id(n), TopologyGeneration{topology_generation_},
                                 node_id(n), node_id(n + 1), ScopeId{kScope});
    }
}

void Fabric::remove_adjacency(std::uint64_t n) {
    evidence_->remove_adjacency(adjacency_id(n));
}

void Fabric::set_capability_supported(std::uint64_t key, bool supported) {
    evidence_->add_capability(CapabilityKey{key},
                              CapabilityGeneration{evidence_->watermark().capability.value()},
                              supported);
}

void Fabric::remove_capability(std::uint64_t key) {
    evidence_->remove_capability(CapabilityKey{key});
}

void Fabric::advance_capability_generation() {
    capability_generation_ += 1;
    evidence_->set_capability_generation(CapabilityGeneration{capability_generation_});
    evidence_->add_capability(CapabilityKey{kCapabilityA},
                              CapabilityGeneration{capability_generation_}, true);
    evidence_->add_capability(CapabilityKey{kCapabilityB},
                              CapabilityGeneration{capability_generation_}, true);
}

void Fabric::advance_path_authority() {
    path_authority_ += 1;
    evidence_->set_path_authority_generation(PathAuthorityGeneration{path_authority_});
    evidence_->add_path(PathId{kPath}, PathAuthorityGeneration{path_authority_}, ScopeId{kScope});
}

void Fabric::advance_route_generation() {
    route_generation_ += 1;
    evidence_->set_route_generation(RouteGeneration{route_generation_});
    evidence_->add_route(RouteId{kRoute}, RouteGeneration{route_generation_}, SegmentListId{},
                         SegmentListGeneration{}, ScopeId{kScope});
}

void Fabric::advance_constraint_generation() {
    constraint_generation_ += 1;
    evidence_->add_constraint(ConstraintEvaluationId{kConstraint},
                              ConstraintGeneration{constraint_generation_}, ScopeId{kScope});
}

void Fabric::remove_path() {
    evidence_->remove_path(PathId{kPath});
}

void Fabric::bump_segment_generation(std::uint64_t n, std::uint64_t generation) {
    evidence_->add_segment(segment_id(n), SegmentKind::Node, SegmentGeneration{generation},
                           ScopeId{kScope});
}

Harness::Harness(bool persist, Limits limits) : fabric_(std::make_shared<Fabric>()) {
    StoreConfig config{};
    config.limits = limits;
    config.evidence = fabric_->evidence();
    if (persist) {
        memory_ = std::make_shared<MemoryPersistence>();
        config.persistence = memory_;
    }
    store_ = std::make_unique<SegmentListStore>(std::move(config));
    const ValidationResult profile_a = store_->register_profile(abstract_governance_profile());
    const ValidationResult profile_b = store_->register_profile(abstract_ordered_profile());
    const ValidationResult profile_c = store_->register_profile(abstract_nullable_profile());
    const ValidationResult publisher = store_->authority().register_publisher(
        PublisherId{kPublisher}, WorkerBootId{kBoot}, ScopeId{kScope},
        store_->authority().epoch(), limits);
    if (profile_a.failed() || profile_b.failed() || profile_c.failed() || publisher.failed()) {
        throw std::runtime_error("test harness setup failed");
    }
}

CallerIdentity Harness::caller() {
    ++attempt_counter_;
    CallerIdentity identity{};
    identity.epoch = store_->authority().epoch();
    identity.publisher = PublisherId{kPublisher};
    identity.boot = WorkerBootId{kBoot};
    identity.scope = ScopeId{kScope};
    identity.attempt = MutationAttemptId{0xE000 + attempt_counter_};
    return identity;
}

CallerIdentity Harness::stale_epoch_caller() {
    CallerIdentity identity = caller();
    identity.epoch = CoordinatorEpoch{identity.epoch.value() + 1};
    return identity;
}

CallerIdentity Harness::fenced_caller() {
    CallerIdentity identity = caller();
    identity.boot = WorkerBootId{kBoot + 777};
    return identity;
}

ListDraft Harness::draft(std::uint64_t list, std::uint64_t profile_id) {
    ListDraft d{};
    d.id = list_id(list);
    d.scope = ScopeId{kScope};
    d.profile = SegmentProfileId{profile_id};
    return d;
}

MutationOutcome Harness::create(std::uint64_t list, std::vector<Segment> segments) {
    ListDraft d = draft(list);
    d.segments = std::move(segments);
    return store_->create_list(caller(), d);
}

MutationOutcome Harness::replace(std::uint64_t list, std::vector<Segment> segments,
                                 std::uint64_t expected_generation) {
    ListDraft d = draft(list);
    d.segments = std::move(segments);
    d.expected_generation = SegmentListGeneration{expected_generation};
    return store_->replace_list(caller(), d);
}

} // namespace srf::test
