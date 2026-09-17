// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Shared, header-only support for the srf_example_* programs.
//
// Everything fabricated here is SYNTHETIC: abstract governance evidence for an
// abstract runtime. Nothing in this file says anything about a physical network,
// and no physical SR-MPLS or SRv6 encoding exists in Segment Routing Fabric.
//
// This header belongs to the examples target. It is never installed and it is
// never included by the library or by the test suites.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "srf/srf.hpp"

namespace srf::examples {

// ------------------------------------------------------------------- rendering
/// Deterministic textual rendering of a checked value. Deliberately tiny: it
/// exists so a failing CHECK can print both operands without pulling a
/// formatting library into the examples.
template <class T>
[[nodiscard]] inline std::string render_value(const T& value) {
    using V = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<V, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<V>) {
        return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<V>>(value)));
    } else if constexpr (std::is_integral_v<V>) {
        if constexpr (std::is_signed_v<V>) {
            return std::to_string(static_cast<long long>(value));
        } else {
            return std::to_string(static_cast<unsigned long long>(value));
        }
    } else if constexpr (std::is_convertible_v<const V&, std::string_view>) {
        return std::string(static_cast<std::string_view>(value));
    } else if constexpr (requires(const V& probe) { probe.hex(); }) {
        return value.hex();
    } else {
        return "<value>";
    }
}

/// Enum-to-int helper so that CHECK_EQ never compares mismatched signedness.
template <class E>
[[nodiscard]] constexpr int as_int(E value) noexcept {
    return static_cast<int>(value);
}

// ------------------------------------------------------------------ transcript
/// Prints the deterministic transcript of one example and counts every check.
/// An example returns Transcript::exit_code(), so the program exits 0 only when
/// every step met its expectation.
class Transcript {
public:
    explicit Transcript(std::string_view title) {
        std::printf("SRF EXAMPLE %s\n", std::string(title).c_str());
    }

    void step(std::string_view text) { std::printf("  [step] %s\n", std::string(text).c_str()); }

    template <class T>
    void note(std::string_view label, const T& value) {
        const std::string rendered = render_value(value);
        std::printf("    %-28s = %s\n", std::string(label).c_str(), rendered.c_str());
    }

    void check(bool ok, std::string_view expression, int line) {
        ++checks_;
        const std::string text(expression);
        if (ok) {
            std::printf("    check ok     %s\n", text.c_str());
        } else {
            ++failures_;
            std::printf("    check FAILED %s   (line %d)\n", text.c_str(), line);
        }
    }

    template <class A, class B>
    void equal(const A& left, const B& right, std::string_view left_expression,
               std::string_view right_expression, int line) {
        ++checks_;
        const std::string left_text(left_expression);
        const std::string right_text(right_expression);
        if (left == right) {
            std::printf("    check ok     %s == %s\n", left_text.c_str(), right_text.c_str());
            return;
        }
        ++failures_;
        const std::string a = render_value(left);
        const std::string b = render_value(right);
        std::printf("    check FAILED %s == %s   (left=%s right=%s, line %d)\n", left_text.c_str(),
                    right_text.c_str(), a.c_str(), b.c_str(), line);
    }

    [[nodiscard]] std::size_t checks() const noexcept { return checks_; }
    [[nodiscard]] std::size_t failures() const noexcept { return failures_; }
    [[nodiscard]] int exit_code() const noexcept { return failures_ == 0 ? 0 : 1; }

    void finish() const {
        std::printf("  [summary] checks=%zu failures=%zu\n", checks_, failures_);
        std::printf("SRF EXAMPLE EXIT %d\n", exit_code());
    }

private:
    std::size_t checks_{0};
    std::size_t failures_{0};
};

/// A local variable named "transcript" of type srf::examples::Transcript must be
/// in scope for these macros.
#define CHECK(expression) transcript.check((expression), #expression, __LINE__)
#define CHECK_EQ(left, right) transcript.equal((left), (right), #left, #right, __LINE__)
#define CHECK_REASON(result, code) \
    transcript.check((result).contains(code), #result ".contains(" #code ")", __LINE__)
#define CHECK_PRIMARY(result, code) \
    transcript.check((result).primary().code == (code), #result ".primary() == " #code, __LINE__)

// ------------------------------------------------------- synthetic fabrication
inline constexpr std::uint64_t kScope = 0x100;
inline constexpr std::uint64_t kOtherScope = 0x101;
inline constexpr std::uint64_t kCapabilityA = 0x1001;
inline constexpr std::uint64_t kCapabilityB = 0x1002;
inline constexpr std::uint64_t kPublisher = 0x9001;
inline constexpr std::uint64_t kBoot = 0xB001;
inline constexpr std::uint64_t kReincarnatedPublisher = 0x9002;
inline constexpr std::uint64_t kReincarnatedBoot = 0xB002;
inline constexpr std::uint64_t kTopology = 1;
inline constexpr std::uint64_t kCapabilityGeneration = 1;
inline constexpr std::uint64_t kPath = 0x4001;
inline constexpr std::uint64_t kPathAuthority = 1;
inline constexpr std::uint64_t kRoute = 0x5001;
inline constexpr std::uint64_t kRouteGeneration = 1;
inline constexpr std::uint64_t kConstraint = 0x6001;
inline constexpr std::uint64_t kConstraintGeneration = 1;
inline constexpr std::uint64_t kGovernanceProfile = 0x0001'0001ull;
inline constexpr std::uint64_t kOrderedProfile = 0x0001'0002ull;
inline constexpr std::uint64_t kNullableProfile = 0x0001'0003ull;
/// Fabricated records exist for every n in [1, kFabricSize].
inline constexpr std::uint64_t kFabricSize = 16;

[[nodiscard]] inline SegmentId segment_id(std::uint64_t n) { return SegmentId{0x3000 + n}; }
[[nodiscard]] inline NodeId node_id(std::uint64_t n) { return NodeId{0x2000 + n}; }
[[nodiscard]] inline AdjacencyId adjacency_id(std::uint64_t n) { return AdjacencyId{0x7000 + n}; }
[[nodiscard]] inline EndpointId endpoint_id(std::uint64_t n) { return EndpointId{0x8000 + n}; }
[[nodiscard]] inline BindingId binding_id(std::uint64_t n) { return BindingId{0xA000 + n}; }
[[nodiscard]] inline SegmentListId list_id(std::uint64_t n) { return SegmentListId{0xC000 + n}; }
[[nodiscard]] inline SegmentPolicyId policy_id(std::uint64_t n) { return SegmentPolicyId{0xD000 + n}; }

[[nodiscard]] inline Segment node_segment(std::uint64_t n, std::uint8_t payload = 0x11) {
    Segment s{};
    s.kind = SegmentKind::Node;
    s.id = segment_id(n);
    s.generation = SegmentGeneration{1};
    s.node = node_id(n);
    s.payload = {std::byte{payload}};
    return s;
}

[[nodiscard]] inline Segment adjacency_segment(std::uint64_t n,
                                               std::uint64_t topology = kTopology) {
    Segment s{};
    s.kind = SegmentKind::Adjacency;
    s.id = segment_id(100 + n);
    s.generation = SegmentGeneration{1};
    s.adjacency = adjacency_id(n);
    s.topology = TopologyGeneration{topology};
    s.payload = {std::byte{0x22}};
    return s;
}

[[nodiscard]] inline Segment endpoint_segment(std::uint64_t n) {
    Segment s{};
    s.kind = SegmentKind::Endpoint;
    s.id = segment_id(200 + n);
    s.generation = SegmentGeneration{1};
    s.endpoint = endpoint_id(n);
    s.payload = {std::byte{0x33}};
    return s;
}

[[nodiscard]] inline Segment binding_segment(std::uint64_t n) {
    Segment s{};
    s.kind = SegmentKind::Binding;
    s.id = segment_id(300 + n);
    s.generation = SegmentGeneration{1};
    s.binding = binding_id(n);
    s.payload = {std::byte{0x44}};
    return s;
}

[[nodiscard]] inline Segment policy_segment(std::uint64_t n) {
    Segment s{};
    s.kind = SegmentKind::Policy;
    s.id = segment_id(400 + n);
    s.generation = SegmentGeneration{1};
    s.policy = policy_id(n);
    s.policy_generation = SegmentPolicyGeneration{1};
    s.payload = {std::byte{0x55}};
    return s;
}

[[nodiscard]] inline std::vector<Segment> node_sequence(std::uint64_t count) {
    std::vector<Segment> out;
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t n = 1; n <= count; ++n) {
        out.push_back(node_segment(n));
    }
    return out;
}

/// Move every adjacency of the synthetic fabric to a new topology generation.
inline void advance_topology(SyntheticEvidence& evidence, std::uint64_t generation) {
    evidence.set_topology_generation(TopologyGeneration{generation});
    for (std::uint64_t n = 1; n <= kFabricSize; ++n) {
        evidence.add_adjacency(adjacency_id(n), TopologyGeneration{generation}, node_id(n),
                               node_id(n + 1), ScopeId{kScope});
    }
}

// -------------------------------------------------------------------- scenario
/// One fully wired governance environment: synthetic fabric, a store with the
/// three shipped abstract profiles registered, and one live publisher.
struct Scenario {
    std::shared_ptr<SyntheticEvidence> evidence{};
    std::shared_ptr<MemoryPersistence> persistence{};
    std::unique_ptr<SegmentListStore> store{};
    ValidationResult setup{};
    std::uint64_t attempt_counter{0};

    [[nodiscard]] SegmentListStore& s() const noexcept { return *store; }
    [[nodiscard]] SyntheticEvidence& fabric() const noexcept { return *evidence; }
    [[nodiscard]] CoordinatorEpoch epoch() const { return store->authority().epoch(); }

    /// A fresh mutation attempt id per call: reusing one with different content
    /// is a deliberate conflict, so examples never do it by accident.
    [[nodiscard]] CallerIdentity caller(std::uint64_t publisher = kPublisher,
                                        std::uint64_t boot = kBoot) {
        ++attempt_counter;
        CallerIdentity identity{};
        identity.epoch = store->authority().epoch();
        identity.publisher = PublisherId{publisher};
        identity.boot = WorkerBootId{boot};
        identity.scope = ScopeId{kScope};
        identity.attempt = MutationAttemptId{0xE000 + attempt_counter};
        return identity;
    }

    [[nodiscard]] ListDraft draft(std::uint64_t list,
                                  std::uint64_t profile = kGovernanceProfile) const {
        ListDraft d{};
        d.id = list_id(list);
        d.scope = ScopeId{kScope};
        d.profile = SegmentProfileId{profile};
        return d;
    }

    [[nodiscard]] MutationOutcome create(std::uint64_t list, std::vector<Segment> segments,
                                         std::uint64_t profile = kGovernanceProfile) {
        ListDraft d = draft(list, profile);
        d.segments = std::move(segments);
        return store->create_list(caller(), d);
    }
};

[[nodiscard]] inline Scenario make_scenario(bool persist = true) {
    Scenario scenario{};
    scenario.evidence = std::make_shared<SyntheticEvidence>();
    SyntheticEvidence& fabric = *scenario.evidence;

    fabric.set_topology_generation(TopologyGeneration{kTopology});
    fabric.set_capability_generation(CapabilityGeneration{kCapabilityGeneration});
    fabric.set_path_authority_generation(PathAuthorityGeneration{kPathAuthority});
    fabric.set_route_generation(RouteGeneration{kRouteGeneration});
    fabric.add_capability(CapabilityKey{kCapabilityA},
                          CapabilityGeneration{kCapabilityGeneration}, true);
    fabric.add_capability(CapabilityKey{kCapabilityB},
                          CapabilityGeneration{kCapabilityGeneration}, true);
    fabric.add_path(PathId{kPath}, PathAuthorityGeneration{kPathAuthority}, ScopeId{kScope});
    fabric.add_route(RouteId{kRoute}, RouteGeneration{kRouteGeneration}, SegmentListId{},
                     SegmentListGeneration{}, ScopeId{kScope});
    fabric.add_constraint(ConstraintEvaluationId{kConstraint},
                          ConstraintGeneration{kConstraintGeneration}, ScopeId{kScope});
    for (std::uint64_t n = 1; n <= kFabricSize; ++n) {
        fabric.add_node(node_id(n), ScopeId{kScope});
        fabric.add_segment(segment_id(n), SegmentKind::Node, SegmentGeneration{1},
                           ScopeId{kScope});
        fabric.add_segment(segment_id(100 + n), SegmentKind::Adjacency, SegmentGeneration{1},
                           ScopeId{kScope});
        fabric.add_segment(segment_id(200 + n), SegmentKind::Endpoint, SegmentGeneration{1},
                           ScopeId{kScope});
        fabric.add_segment(segment_id(300 + n), SegmentKind::Binding, SegmentGeneration{1},
                           ScopeId{kScope});
        fabric.add_segment(segment_id(400 + n), SegmentKind::Policy, SegmentGeneration{1},
                           ScopeId{kScope});
        fabric.add_adjacency(adjacency_id(n), TopologyGeneration{kTopology}, node_id(n),
                             node_id(n + 1), ScopeId{kScope});
        fabric.add_endpoint(endpoint_id(n), node_id(n), ScopeId{kScope});
        fabric.add_binding(binding_id(n), segment_id(n), ScopeId{kScope});
    }

    StoreConfig config{};
    config.limits = Limits{};
    config.evidence = scenario.evidence;
    if (persist) {
        scenario.persistence = std::make_shared<MemoryPersistence>();
        config.persistence = scenario.persistence;
    }
    scenario.store = std::make_unique<SegmentListStore>(std::move(config));

    ValidationResult setup;
    setup.set_limits(&scenario.store->limits());
    setup.absorb(scenario.store->register_profile(abstract_governance_profile()));
    setup.absorb(scenario.store->register_profile(abstract_ordered_profile()));
    setup.absorb(scenario.store->register_profile(abstract_nullable_profile()));
    setup.absorb(scenario.store->authority().register_publisher(
        PublisherId{kPublisher}, WorkerBootId{kBoot}, ScopeId{kScope},
        scenario.store->authority().epoch(), scenario.store->limits()));
    scenario.setup = setup;
    return scenario;
}

/// The governance policy used by the policy-restriction example.
[[nodiscard]] inline SegmentPolicy restrictive_policy() {
    SegmentPolicy policy{};
    policy.id = policy_id(1);
    policy.generation = SegmentPolicyGeneration{1};
    policy.owner_scope = ScopeId{kScope};
    policy.allowed_profiles = {SegmentProfileId{kGovernanceProfile}};
    policy.allowed_kinds_mask = static_cast<std::uint16_t>(
        kind_bit(SegmentKind::Node) | kind_bit(SegmentKind::Endpoint) |
        kind_bit(SegmentKind::Binding) | kind_bit(SegmentKind::Policy));
    policy.required_segments = {segment_id(2)};
    policy.forbidden_segments = {segment_id(9)};
    policy.allow_replacement = true;
    policy.allow_derivation = false;
    policy.require_strict = true;
    return policy;
}

} // namespace srf::examples
