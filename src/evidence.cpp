// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/evidence.hpp"

#include <algorithm>
#include <utility>

namespace srf {

const CapabilityRecord* FabricEvidence::find_capability(CapabilityKey key) const noexcept {
    for (const auto& r : capabilities) {
        if (r.key == key) {
            return &r;
        }
    }
    return nullptr;
}

const SegmentRecord* FabricEvidence::find_segment(SegmentId id) const noexcept {
    for (const auto& r : segments) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const NodeRecord* FabricEvidence::find_node(NodeId id) const noexcept {
    for (const auto& r : nodes) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const AdjacencyRecord* FabricEvidence::find_adjacency(AdjacencyId id) const noexcept {
    for (const auto& r : adjacencies) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const EndpointRecord* FabricEvidence::find_endpoint(EndpointId id) const noexcept {
    for (const auto& r : endpoints) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const BindingRecord* FabricEvidence::find_binding(BindingId id) const noexcept {
    for (const auto& r : bindings) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const PathRecord* FabricEvidence::find_path(PathId id) const noexcept {
    for (const auto& r : paths) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const RouteRecord* FabricEvidence::find_route(RouteId id) const noexcept {
    for (const auto& r : routes) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const ConstraintRecord* FabricEvidence::find_constraint(ConstraintEvaluationId id) const noexcept {
    for (const auto& r : constraints) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

SyntheticEvidence::SyntheticEvidence() = default;

FabricEvidence SyntheticEvidence::snapshot() const {
    FabricEvidence copy;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = evidence_;
        hook = hook_;
    }
    if (hook) {
        hook();
    }
    return copy;
}

EvidenceWatermark SyntheticEvidence::watermark() const {
    std::lock_guard<std::mutex> lock(mutex_);
    EvidenceWatermark w{};
    w.topology = evidence_.topology;
    w.capability = evidence_.capability;
    w.path_authority = evidence_.path_authority;
    w.route = evidence_.route;
    w.constraint = evidence_.constraint;
    w.revision = revision_;
    return w;
}

std::uint64_t SyntheticEvidence::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

void SyntheticEvidence::set_topology_generation(TopologyGeneration g) {
    std::lock_guard<std::mutex> lock(mutex_);
    evidence_.topology = g;
    ++revision_;
}

void SyntheticEvidence::set_capability_generation(CapabilityGeneration g) {
    std::lock_guard<std::mutex> lock(mutex_);
    evidence_.capability = g;
    ++revision_;
}

void SyntheticEvidence::set_path_authority_generation(PathAuthorityGeneration g) {
    std::lock_guard<std::mutex> lock(mutex_);
    evidence_.path_authority = g;
    ++revision_;
}

void SyntheticEvidence::set_route_generation(RouteGeneration g) {
    std::lock_guard<std::mutex> lock(mutex_);
    evidence_.route = g;
    ++revision_;
}

void SyntheticEvidence::add_capability(CapabilityKey key, CapabilityGeneration generation,
                                       bool supported) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.capabilities) {
        if (r.key == key) {
            r.generation = generation;
            r.supported = supported;
            ++revision_;
            return;
        }
    }
    evidence_.capabilities.push_back(CapabilityRecord{key, generation, supported});
    ++revision_;
}

void SyntheticEvidence::remove_capability(CapabilityKey key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(evidence_.capabilities.begin(), evidence_.capabilities.end(),
                                   [key](const CapabilityRecord& r) { return r.key == key; });
    if (it != evidence_.capabilities.end()) {
        evidence_.capabilities.erase(it, evidence_.capabilities.end());
        ++revision_;
    }
}

void SyntheticEvidence::add_segment(SegmentId id, SegmentKind kind, SegmentGeneration generation,
                                    ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.segments) {
        if (r.id == id) {
            r.kind = kind;
            r.generation = generation;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.segments.push_back(SegmentRecord{id, kind, generation, scope});
    ++revision_;
}

void SyntheticEvidence::remove_segment(SegmentId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(evidence_.segments.begin(), evidence_.segments.end(),
                                   [id](const SegmentRecord& r) { return r.id == id; });
    if (it != evidence_.segments.end()) {
        evidence_.segments.erase(it, evidence_.segments.end());
        ++revision_;
    }
}

void SyntheticEvidence::add_node(NodeId id, ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.nodes) {
        if (r.id == id) {
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.nodes.push_back(NodeRecord{id, scope});
    ++revision_;
}

void SyntheticEvidence::add_adjacency(AdjacencyId id, TopologyGeneration topology, NodeId from,
                                      NodeId to, ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.adjacencies) {
        if (r.id == id) {
            r.topology = topology;
            r.from = from;
            r.to = to;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.adjacencies.push_back(AdjacencyRecord{id, topology, from, to, scope});
    ++revision_;
}

void SyntheticEvidence::remove_adjacency(AdjacencyId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(evidence_.adjacencies.begin(), evidence_.adjacencies.end(),
                                   [id](const AdjacencyRecord& r) { return r.id == id; });
    if (it != evidence_.adjacencies.end()) {
        evidence_.adjacencies.erase(it, evidence_.adjacencies.end());
        ++revision_;
    }
}

void SyntheticEvidence::add_endpoint(EndpointId id, NodeId node, ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.endpoints) {
        if (r.id == id) {
            r.node = node;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.endpoints.push_back(EndpointRecord{id, node, scope});
    ++revision_;
}

void SyntheticEvidence::add_binding(BindingId id, SegmentId bound, ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.bindings) {
        if (r.id == id) {
            r.bound_segment = bound;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.bindings.push_back(BindingRecord{id, bound, scope});
    ++revision_;
}

void SyntheticEvidence::add_path(PathId id, PathAuthorityGeneration authority, ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.paths) {
        if (r.id == id) {
            r.authority = authority;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.paths.push_back(PathRecord{id, authority, scope});
    ++revision_;
}

void SyntheticEvidence::set_path_authority(PathId id, PathAuthorityGeneration authority) {
    add_path(id, authority, ScopeId{});
}

void SyntheticEvidence::remove_path(PathId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(evidence_.paths.begin(), evidence_.paths.end(),
                                   [id](const PathRecord& r) { return r.id == id; });
    if (it != evidence_.paths.end()) {
        evidence_.paths.erase(it, evidence_.paths.end());
        ++revision_;
    }
}

void SyntheticEvidence::add_route(RouteId id, RouteGeneration generation, SegmentListId list,
                                  SegmentListGeneration list_generation, ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.routes) {
        if (r.id == id) {
            r.generation = generation;
            r.list = list;
            r.list_generation = list_generation;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.routes.push_back(RouteRecord{id, generation, list, list_generation, scope});
    ++revision_;
}

void SyntheticEvidence::set_route(SegmentListId list, SegmentListGeneration generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.routes) {
        if (r.list == list) {
            r.list_generation = generation;
            ++revision_;
        }
    }
}

void SyntheticEvidence::add_constraint(ConstraintEvaluationId id, ConstraintGeneration generation,
                                       ScopeId scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.constraints) {
        if (r.id == id) {
            r.generation = generation;
            r.scope = scope;
            ++revision_;
            return;
        }
    }
    evidence_.constraints.push_back(ConstraintRecord{id, generation, scope});
    ++revision_;
}

void SyntheticEvidence::set_constraint_generation(ConstraintEvaluationId id,
                                                  ConstraintGeneration generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : evidence_.constraints) {
        if (r.id == id) {
            r.generation = generation;
            ++revision_;
            return;
        }
    }
    evidence_.constraints.push_back(ConstraintRecord{id, generation, ScopeId{}});
    ++revision_;
}

void SyntheticEvidence::set_observation_hook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    hook_ = std::move(hook);
}

std::shared_ptr<SyntheticEvidence> make_synthetic_fabric(std::uint64_t node_count) {
    auto evidence = std::make_shared<SyntheticEvidence>();
    evidence->set_topology_generation(TopologyGeneration{kSyntheticTopologyGeneration});
    evidence->set_capability_generation(CapabilityGeneration{kSyntheticCapabilityGeneration});
    evidence->set_path_authority_generation(PathAuthorityGeneration{kSyntheticPathAuthority});
    evidence->set_route_generation(RouteGeneration{kSyntheticRouteGeneration});
    evidence->add_capability(CapabilityKey{kSyntheticCapabilityA},
                             CapabilityGeneration{kSyntheticCapabilityGeneration}, true);
    evidence->add_capability(CapabilityKey{kSyntheticCapabilityB},
                             CapabilityGeneration{kSyntheticCapabilityGeneration}, true);
    for (std::uint64_t n = 1; n <= node_count; ++n) {
        evidence->add_node(NodeId{0x2000 + n}, ScopeId{kSyntheticScope});
        evidence->add_segment(SegmentId{0x3000 + n}, SegmentKind::Node, SegmentGeneration{1},
                              ScopeId{kSyntheticScope});
        evidence->add_segment(SegmentId{0x3000 + 100 + n}, SegmentKind::Adjacency,
                              SegmentGeneration{1}, ScopeId{kSyntheticScope});
        evidence->add_segment(SegmentId{0x3000 + 200 + n}, SegmentKind::Endpoint,
                              SegmentGeneration{1}, ScopeId{kSyntheticScope});
        evidence->add_segment(SegmentId{0x3000 + 300 + n}, SegmentKind::Binding,
                              SegmentGeneration{1}, ScopeId{kSyntheticScope});
        evidence->add_segment(SegmentId{0x3000 + 400 + n}, SegmentKind::Policy,
                              SegmentGeneration{1}, ScopeId{kSyntheticScope});
        evidence->add_adjacency(AdjacencyId{0x7000 + n},
                                TopologyGeneration{kSyntheticTopologyGeneration}, NodeId{0x2000 + n},
                                NodeId{0x2000 + n + 1}, ScopeId{kSyntheticScope});
        evidence->add_endpoint(EndpointId{0x8000 + n}, NodeId{0x2000 + n}, ScopeId{kSyntheticScope});
        evidence->add_binding(BindingId{0xA000 + n}, SegmentId{0x3000 + n},
                              ScopeId{kSyntheticScope});
        evidence->add_constraint(ConstraintEvaluationId{kSyntheticConstraint},
                                 ConstraintGeneration{kSyntheticConstraintGeneration},
                                 ScopeId{kSyntheticScope});
    }
    evidence->add_path(PathId{kSyntheticPath}, PathAuthorityGeneration{kSyntheticPathAuthority},
                       ScopeId{kSyntheticScope});
    evidence->add_route(RouteId{kSyntheticRoute}, RouteGeneration{kSyntheticRouteGeneration},
                        SegmentListId{}, SegmentListGeneration{}, ScopeId{kSyntheticScope});
    return evidence;
}

} // namespace srf
