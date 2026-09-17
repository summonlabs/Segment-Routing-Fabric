// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "srf/ids.hpp"
#include "srf/segment.hpp"

namespace srf {

// Fabric evidence is supplied to Segment Routing Fabric by the runtimes that
// actually own the truth. Segment Routing Fabric consumes identity evidence; it
// does not issue it.

struct CapabilityRecord {
    CapabilityKey key{};
    CapabilityGeneration generation{};
    /// Explicit tri-state: true (supported), false (unsupported), and the absence
    /// of the record (unknown). Unknown is never inferred into support.
    bool supported{false};
};

struct SegmentRecord {
    SegmentId id{};
    SegmentKind kind{SegmentKind::Invalid};
    SegmentGeneration generation{};
    ScopeId scope{};
};

struct NodeRecord {
    NodeId id{};
    ScopeId scope{};
};

struct AdjacencyRecord {
    AdjacencyId id{};
    TopologyGeneration topology{};
    NodeId from{};
    NodeId to{};
    ScopeId scope{};
};

struct EndpointRecord {
    EndpointId id{};
    NodeId node{};
    ScopeId scope{};
};

struct BindingRecord {
    BindingId id{};
    SegmentId bound_segment{};
    ScopeId scope{};
};

struct PathRecord {
    PathId id{};
    PathAuthorityGeneration authority{};
    ScopeId scope{};
};

struct RouteRecord {
    RouteId id{};
    RouteGeneration generation{};
    SegmentListId list{};
    SegmentListGeneration list_generation{};
    ScopeId scope{};
};

struct ConstraintRecord {
    ConstraintEvaluationId id{};
    ConstraintGeneration generation{};
    ScopeId scope{};
};

/// Generations of every external dependency, captured at one instant. Comparing
/// two watermarks is how the runtime proves that no dependency moved underneath a
/// validation.
struct EvidenceWatermark {
    TopologyGeneration topology{};
    CapabilityGeneration capability{};
    PathAuthorityGeneration path_authority{};
    RouteGeneration route{};
    ConstraintGeneration constraint{};
    std::uint64_t revision{0};

    friend bool operator==(const EvidenceWatermark&, const EvidenceWatermark&) = default;
};

struct FabricEvidence {
    TopologyGeneration topology{};
    CapabilityGeneration capability{};
    PathAuthorityGeneration path_authority{};
    RouteGeneration route{};
    ConstraintGeneration constraint{};

    std::vector<CapabilityRecord> capabilities{};
    std::vector<SegmentRecord> segments{};
    std::vector<NodeRecord> nodes{};
    std::vector<AdjacencyRecord> adjacencies{};
    std::vector<EndpointRecord> endpoints{};
    std::vector<BindingRecord> bindings{};
    std::vector<PathRecord> paths{};
    std::vector<RouteRecord> routes{};
    std::vector<ConstraintRecord> constraints{};

    [[nodiscard]] const CapabilityRecord* find_capability(CapabilityKey key) const noexcept;
    [[nodiscard]] const SegmentRecord* find_segment(SegmentId id) const noexcept;
    [[nodiscard]] const NodeRecord* find_node(NodeId id) const noexcept;
    [[nodiscard]] const AdjacencyRecord* find_adjacency(AdjacencyId id) const noexcept;
    [[nodiscard]] const EndpointRecord* find_endpoint(EndpointId id) const noexcept;
    [[nodiscard]] const BindingRecord* find_binding(BindingId id) const noexcept;
    [[nodiscard]] const PathRecord* find_path(PathId id) const noexcept;
    [[nodiscard]] const RouteRecord* find_route(RouteId id) const noexcept;
    [[nodiscard]] const ConstraintRecord* find_constraint(ConstraintEvaluationId id) const noexcept;
};

/// Read-only view of the owning runtimes. snapshot() must return a value that is
/// internally consistent: a validation never reads the live truth twice.
class IEvidenceSource {
public:
    IEvidenceSource() = default;
    virtual ~IEvidenceSource() = default;
    IEvidenceSource(const IEvidenceSource&) = delete;
    IEvidenceSource& operator=(const IEvidenceSource&) = delete;
    IEvidenceSource(IEvidenceSource&&) = delete;
    IEvidenceSource& operator=(IEvidenceSource&&) = delete;

    [[nodiscard]] virtual FabricEvidence snapshot() const = 0;
    [[nodiscard]] virtual EvidenceWatermark watermark() const = 0;
};

/// Deterministic synthetic evidence used for governance tests, examples, the
/// benchmarks and the independent oracle. Every value is fabricated and labelled
/// SYNTHETIC. Nothing here proves anything about a physical network.
class SyntheticEvidence final : public IEvidenceSource {
public:
    SyntheticEvidence();
    ~SyntheticEvidence() override = default;
    SyntheticEvidence(const SyntheticEvidence&) = delete;
    SyntheticEvidence& operator=(const SyntheticEvidence&) = delete;

    [[nodiscard]] FabricEvidence snapshot() const override;
    [[nodiscard]] EvidenceWatermark watermark() const override;

    // ---- fabrication ------------------------------------------------------
    void set_topology_generation(TopologyGeneration g);
    void set_capability_generation(CapabilityGeneration g);
    void set_path_authority_generation(PathAuthorityGeneration g);
    void set_route_generation(RouteGeneration g);

    void add_capability(CapabilityKey key, CapabilityGeneration generation, bool supported);
    void remove_capability(CapabilityKey key);
    void add_segment(SegmentId id, SegmentKind kind, SegmentGeneration generation,
                     ScopeId scope);
    void remove_segment(SegmentId id);
    void add_node(NodeId id, ScopeId scope);
    void add_adjacency(AdjacencyId id, TopologyGeneration topology, NodeId from, NodeId to,
                       ScopeId scope);
    void remove_adjacency(AdjacencyId id);
    void add_endpoint(EndpointId id, NodeId node, ScopeId scope);
    void add_binding(BindingId id, SegmentId bound, ScopeId scope);
    void add_path(PathId id, PathAuthorityGeneration authority, ScopeId scope);
    void set_path_authority(PathId id, PathAuthorityGeneration authority);
    void remove_path(PathId id);
    void add_route(RouteId id, RouteGeneration generation, SegmentListId list,
                   SegmentListGeneration list_generation, ScopeId scope);
    void set_route(SegmentListId list, SegmentListGeneration generation);
    void add_constraint(ConstraintEvaluationId id, ConstraintGeneration generation,
                        ScopeId scope);
    void set_constraint_generation(ConstraintEvaluationId id, ConstraintGeneration generation);

    /// Install a hook that runs immediately after a snapshot is materialized and
    /// before the caller receives it. The deterministic race proofs use it to move
    /// a dependency in the middle of a validation without depending on timing.
    void set_observation_hook(std::function<void()> hook);

    [[nodiscard]] std::uint64_t revision() const;

private:
    mutable std::mutex mutex_{};
    FabricEvidence evidence_{};
    std::uint64_t revision_{0};
    std::function<void()> hook_{};
};

/// A deterministic SYNTHETIC fabric: topology generation 1, capability generation 1,
/// two supported capabilities, and a bounded set of nodes, node/adjacency/endpoint/
/// binding/policy segments, adjacencies, endpoints, bindings, one path, one route and
/// one constraint evaluation. Every value is fabricated. Nothing here proves anything
/// about a physical network.
[[nodiscard]] std::shared_ptr<SyntheticEvidence> make_synthetic_fabric(
    std::uint64_t node_count = 64);

inline constexpr std::uint64_t kSyntheticScope = 0x100;
inline constexpr std::uint64_t kSyntheticTopologyGeneration = 1;
inline constexpr std::uint64_t kSyntheticCapabilityGeneration = 1;
inline constexpr std::uint64_t kSyntheticCapabilityA = 0x1001;
inline constexpr std::uint64_t kSyntheticCapabilityB = 0x1002;
inline constexpr std::uint64_t kSyntheticPath = 0x4001;
inline constexpr std::uint64_t kSyntheticPathAuthority = 1;
inline constexpr std::uint64_t kSyntheticRoute = 0x5001;
inline constexpr std::uint64_t kSyntheticRouteGeneration = 1;
inline constexpr std::uint64_t kSyntheticConstraint = 0x6001;
inline constexpr std::uint64_t kSyntheticConstraintGeneration = 1;

} // namespace srf
