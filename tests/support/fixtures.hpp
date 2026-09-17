// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Shared deterministic fixtures. Every value here is SYNTHETIC: fabricated
// topology and capability evidence used to prove governance behaviour. Nothing in
// this file proves anything about a physical network.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "srf/srf.hpp"
#include "test_support.hpp"

namespace srf::test {

inline constexpr std::uint64_t kScope = 0x100;
inline constexpr std::uint64_t kOtherScope = 0x101;
inline constexpr std::uint64_t kCapabilityA = 0x1001;
inline constexpr std::uint64_t kCapabilityB = 0x1002;
inline constexpr std::uint64_t kPublisher = 0x9001;
inline constexpr std::uint64_t kBoot = 0xB001;
inline constexpr std::uint64_t kTopology = 1;
inline constexpr std::uint64_t kCapabilityGeneration = 1;
inline constexpr std::uint64_t kPath = 0x4001;
inline constexpr std::uint64_t kPathAuthority = 1;
inline constexpr std::uint64_t kRoute = 0x5001;
inline constexpr std::uint64_t kRouteGeneration = 1;
inline constexpr std::uint64_t kConstraint = 0x6001;
inline constexpr std::uint64_t kConstraintGeneration = 1;

[[nodiscard]] inline SegmentId segment_id(std::uint64_t n) { return SegmentId{0x3000 + n}; }
[[nodiscard]] inline NodeId node_id(std::uint64_t n) { return NodeId{0x2000 + n}; }
[[nodiscard]] inline AdjacencyId adjacency_id(std::uint64_t n) { return AdjacencyId{0x7000 + n}; }
[[nodiscard]] inline EndpointId endpoint_id(std::uint64_t n) { return EndpointId{0x8000 + n}; }
[[nodiscard]] inline BindingId binding_id(std::uint64_t n) { return BindingId{0xA000 + n}; }
[[nodiscard]] inline SegmentListId list_id(std::uint64_t n) { return SegmentListId{0xC000 + n}; }
[[nodiscard]] inline SegmentPolicyId policy_id(std::uint64_t n) { return SegmentPolicyId{0xD000 + n}; }

[[nodiscard]] inline Segment node_segment(std::uint64_t n, std::uint64_t payload_byte = 0x01) {
    Segment s{};
    s.kind = SegmentKind::Node;
    s.id = segment_id(n);
    s.generation = SegmentGeneration{1};
    s.node = node_id(n);
    s.payload = {std::byte{static_cast<unsigned char>(payload_byte)}};
    return s;
}

[[nodiscard]] inline Segment adjacency_segment(std::uint64_t n,
                                               TopologyGeneration topology = TopologyGeneration{kTopology}) {
    Segment s{};
    s.kind = SegmentKind::Adjacency;
    s.id = segment_id(100 + n);
    s.generation = SegmentGeneration{1};
    s.adjacency = adjacency_id(n);
    s.topology = topology;
    s.payload = {std::byte{0x0A}};
    return s;
}

[[nodiscard]] inline Segment endpoint_segment(std::uint64_t n) {
    Segment s{};
    s.kind = SegmentKind::Endpoint;
    s.id = segment_id(200 + n);
    s.generation = SegmentGeneration{1};
    s.endpoint = endpoint_id(n);
    s.payload = {std::byte{0x0B}};
    return s;
}

[[nodiscard]] inline Segment binding_segment(std::uint64_t n) {
    Segment s{};
    s.kind = SegmentKind::Binding;
    s.id = segment_id(300 + n);
    s.generation = SegmentGeneration{1};
    s.binding = binding_id(n);
    s.payload = {std::byte{0x0C}};
    return s;
}

[[nodiscard]] inline Segment policy_segment(std::uint64_t n) {
    Segment s{};
    s.kind = SegmentKind::Policy;
    s.id = segment_id(400 + n);
    s.generation = SegmentGeneration{1};
    s.policy = policy_id(n);
    s.policy_generation = SegmentPolicyGeneration{1};
    s.payload = {std::byte{0x0D}};
    return s;
}

/// The synthetic fabric used by every test: one topology generation, two
/// capabilities, and a handful of nodes, adjacencies, endpoints and bindings.
class Fabric {
public:
    Fabric();

    [[nodiscard]] std::shared_ptr<SyntheticEvidence> evidence() const { return evidence_; }
    [[nodiscard]] SyntheticEvidence& raw() const { return *evidence_; }

    /// Advance the topology generation and move every adjacency with it.
    void advance_topology();
    /// Remove one adjacency without moving the topology generation.
    void remove_adjacency(std::uint64_t n);
    void set_capability_supported(std::uint64_t key, bool supported);
    void remove_capability(std::uint64_t key);
    void advance_capability_generation();
    void advance_path_authority();
    void advance_route_generation();
    void advance_constraint_generation();
    void remove_path();
    void bump_segment_generation(std::uint64_t n, std::uint64_t generation);

private:
    std::shared_ptr<SyntheticEvidence> evidence_{};
    std::uint64_t topology_generation_{kTopology};
    std::uint64_t capability_generation_{kCapabilityGeneration};
    std::uint64_t path_authority_{kPathAuthority};
    std::uint64_t route_generation_{kRouteGeneration};
    std::uint64_t constraint_generation_{kConstraintGeneration};
};

/// A fully wired store: governance profile registered, authority table populated
/// with one live publisher, and synthetic evidence attached.
class Harness {
public:
    explicit Harness(bool persist = true, Limits limits = Limits{});

    [[nodiscard]] SegmentListStore& store() const { return *store_; }
    [[nodiscard]] Fabric& fabric() const { return *fabric_; }
    [[nodiscard]] std::shared_ptr<MemoryPersistence> memory() const { return memory_; }

    [[nodiscard]] CallerIdentity caller();
    [[nodiscard]] CallerIdentity stale_epoch_caller();
    [[nodiscard]] CallerIdentity fenced_caller();

    [[nodiscard]] ListDraft draft(std::uint64_t list, std::uint64_t profile_id = 0x0001'0001ull);
    [[nodiscard]] MutationOutcome create(std::uint64_t list, std::vector<Segment> segments);
    [[nodiscard]] MutationOutcome replace(std::uint64_t list, std::vector<Segment> segments,
                                          std::uint64_t expected_generation);

private:
    std::shared_ptr<Fabric> fabric_{};
    std::shared_ptr<MemoryPersistence> memory_{};
    std::unique_ptr<SegmentListStore> store_{};
    std::uint64_t attempt_counter_{0};
};

} // namespace srf::test
