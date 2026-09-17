// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// srf_bench - throughput and latency benchmarks for the abstract governance
// runtime. Every value measured here is produced from SYNTHETIC evidence: the
// numbers describe this library on fabricated data, never a physical network.
//
// Usage: srf_bench [scale]
//   scale is an optional positive integer that multiplies the per-benchmark time
//   budget (and therefore the measured iteration counts). Default: 1, which
//   completes in a few seconds.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

#include "srf/srf.hpp"

namespace {

// ------------------------------------------------------------ synthetic fabric
constexpr std::uint64_t kScope = 0x100;
constexpr std::uint64_t kCapabilityA = 0x1001;
constexpr std::uint64_t kCapabilityB = 0x1002;
constexpr std::uint64_t kPublisher = 0x9001;
constexpr std::uint64_t kBoot = 0xB001;
constexpr std::uint64_t kTopology = 1;
constexpr std::uint64_t kPath = 0x4001;
constexpr std::uint64_t kRoute = 0x5001;
constexpr std::uint64_t kConstraint = 0x6001;
constexpr std::uint64_t kFabricSize = 32;

[[nodiscard]] srf::SegmentId segment_id(std::uint64_t n) { return srf::SegmentId{0x3000 + n}; }
[[nodiscard]] srf::NodeId node_id(std::uint64_t n) { return srf::NodeId{0x2000 + n}; }
[[nodiscard]] srf::AdjacencyId adjacency_id(std::uint64_t n) { return srf::AdjacencyId{0x7000 + n}; }
[[nodiscard]] srf::EndpointId endpoint_id(std::uint64_t n) { return srf::EndpointId{0x8000 + n}; }
[[nodiscard]] srf::BindingId binding_id(std::uint64_t n) { return srf::BindingId{0xA000 + n}; }
[[nodiscard]] srf::SegmentListId list_id(std::uint64_t n) { return srf::SegmentListId{0xC000 + n}; }

[[nodiscard]] srf::Segment node_segment(std::uint64_t n) {
    srf::Segment s{};
    s.kind = srf::SegmentKind::Node;
    s.id = segment_id(n);
    s.generation = srf::SegmentGeneration{1};
    s.node = node_id(n);
    s.payload = {std::byte{0x11}};
    return s;
}

[[nodiscard]] std::vector<srf::Segment> node_sequence(std::uint64_t count) {
    std::vector<srf::Segment> out;
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t n = 1; n <= count; ++n) {
        out.push_back(node_segment(n));
    }
    return out;
}

// ------------------------------------------------------------------- plumbing
/// Consumes every measured result so that the optimizer cannot delete the work.
struct Sink {
    std::uint64_t value{0};

    void mix(std::uint64_t v) noexcept {
        value = (value ^ (v + 0x9E3779B97F4A7C15ull)) * 1099511628211ull;
    }
    void mix(const srf::Digest128& digest) noexcept {
        mix(digest.hi);
        mix(digest.lo);
    }
};

Sink g_sink{};

struct Harness {
    std::shared_ptr<srf::SyntheticEvidence> evidence{};
    std::shared_ptr<srf::MemoryPersistence> persistence{};
    std::unique_ptr<srf::SegmentListStore> store{};
    std::uint64_t attempt_counter{0};
};

[[nodiscard]] Harness make_harness() {
    Harness harness{};
    harness.evidence = std::make_shared<srf::SyntheticEvidence>();
    srf::SyntheticEvidence& fabric = *harness.evidence;
    fabric.set_topology_generation(srf::TopologyGeneration{kTopology});
    fabric.set_capability_generation(srf::CapabilityGeneration{1});
    fabric.set_path_authority_generation(srf::PathAuthorityGeneration{1});
    fabric.set_route_generation(srf::RouteGeneration{1});
    fabric.add_capability(srf::CapabilityKey{kCapabilityA}, srf::CapabilityGeneration{1}, true);
    fabric.add_capability(srf::CapabilityKey{kCapabilityB}, srf::CapabilityGeneration{1}, true);
    fabric.add_path(srf::PathId{kPath}, srf::PathAuthorityGeneration{1}, srf::ScopeId{kScope});
    fabric.add_route(srf::RouteId{kRoute}, srf::RouteGeneration{1}, srf::SegmentListId{},
                     srf::SegmentListGeneration{}, srf::ScopeId{kScope});
    fabric.add_constraint(srf::ConstraintEvaluationId{kConstraint}, srf::ConstraintGeneration{1},
                          srf::ScopeId{kScope});
    for (std::uint64_t n = 1; n <= kFabricSize; ++n) {
        fabric.add_node(node_id(n), srf::ScopeId{kScope});
        fabric.add_segment(segment_id(n), srf::SegmentKind::Node, srf::SegmentGeneration{1},
                           srf::ScopeId{kScope});
        fabric.add_segment(segment_id(100 + n), srf::SegmentKind::Adjacency,
                           srf::SegmentGeneration{1}, srf::ScopeId{kScope});
        fabric.add_segment(segment_id(200 + n), srf::SegmentKind::Endpoint,
                           srf::SegmentGeneration{1}, srf::ScopeId{kScope});
        fabric.add_segment(segment_id(300 + n), srf::SegmentKind::Binding,
                           srf::SegmentGeneration{1}, srf::ScopeId{kScope});
        fabric.add_adjacency(adjacency_id(n), srf::TopologyGeneration{kTopology}, node_id(n),
                             node_id(n + 1), srf::ScopeId{kScope});
        fabric.add_endpoint(endpoint_id(n), node_id(n), srf::ScopeId{kScope});
        fabric.add_binding(binding_id(n), segment_id(n), srf::ScopeId{kScope});
    }

    srf::StoreConfig config{};
    config.limits = srf::Limits{};
    config.evidence = harness.evidence;
    harness.persistence = std::make_shared<srf::MemoryPersistence>();
    config.persistence = harness.persistence;
    harness.store = std::make_unique<srf::SegmentListStore>(std::move(config));
    const srf::ValidationResult profile_result =
        harness.store->register_profile(srf::abstract_governance_profile());
    const srf::ValidationResult publisher_result = harness.store->authority().register_publisher(
        srf::PublisherId{kPublisher}, srf::WorkerBootId{kBoot}, srf::ScopeId{kScope},
        harness.store->authority().epoch(), harness.store->limits());
    if (profile_result.failed() || publisher_result.failed()) {
        std::printf("srf_bench: harness setup failed\n");
        std::exit(2);
    }
    return harness;
}

[[nodiscard]] srf::CallerIdentity next_caller(Harness& harness) {
    ++harness.attempt_counter;
    srf::CallerIdentity caller{};
    caller.epoch = harness.store->authority().epoch();
    caller.publisher = srf::PublisherId{kPublisher};
    caller.boot = srf::WorkerBootId{kBoot};
    caller.scope = srf::ScopeId{kScope};
    caller.attempt = srf::MutationAttemptId{0xF000 + harness.attempt_counter};
    return caller;
}

[[nodiscard]] srf::ListDraft make_draft(std::uint64_t list) {
    srf::ListDraft draft{};
    draft.id = list_id(list);
    draft.scope = srf::ScopeId{kScope};
    draft.profile = srf::SegmentProfileId{0x0001'0001ull};
    return draft;
}

// ----------------------------------------------------------------- timing rows
struct BenchRow {
    std::string_view name{};
    std::size_t iterations{0};
    double total_ns{0.0};
};

constexpr std::size_t kMaxIterations = 200000;
constexpr std::size_t kMaxCommitIterations = 4000;

template <class Fn>
[[nodiscard]] double measure_ns(std::size_t iterations, Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        fn();
    }
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(stop - start).count();
}

[[nodiscard]] std::size_t planned_iterations(double pilot_ns, std::size_t pilot,
                                             std::size_t budget_ns, std::size_t cap) {
    const double per_op = pilot_ns / static_cast<double>(pilot);
    if (!(per_op > 0.0)) {
        return cap;
    }
    const double wanted = static_cast<double>(budget_ns) / per_op;
    if (!(wanted > 1.0)) {
        return 1;
    }
    const double capped = wanted > static_cast<double>(cap) ? static_cast<double>(cap) : wanted;
    return static_cast<std::size_t>(capped);
}

/// Stateless benchmark: one op is one call of fn.
template <class Fn>
[[nodiscard]] BenchRow run_benchmark(std::string_view name, std::size_t budget_ns, Fn&& fn) {
    constexpr std::size_t kPilot = 4;
    const double pilot_ns = measure_ns(kPilot, fn);
    const std::size_t iterations = planned_iterations(pilot_ns, kPilot, budget_ns, kMaxIterations);
    const double total_ns = measure_ns(iterations, fn);
    return BenchRow{name, iterations, total_ns};
}

/// Stateful benchmark: the chunk callback sets up its own state outside the timed
/// region and returns the nanoseconds spent on exactly n operations inside it.
template <class ChunkFn>
[[nodiscard]] BenchRow run_chunked(std::string_view name, std::size_t budget_ns,
                                   std::size_t chunk_size, ChunkFn&& chunk) {
    const std::size_t pilot = chunk_size;
    const double pilot_ns = chunk(pilot);
    const std::size_t iterations =
        planned_iterations(pilot_ns, pilot, budget_ns, kMaxCommitIterations);
    double total_ns = 0.0;
    std::size_t done = 0;
    while (done < iterations) {
        const std::size_t n = std::min(chunk_size, iterations - done);
        total_ns += chunk(n);
        done += n;
    }
    return BenchRow{name, iterations, total_ns};
}

// ---------------------------------------------------------------- benchmarks
[[nodiscard]] BenchRow bench_canonical(std::size_t budget_ns) {
    const srf::SegmentProfile profile = srf::abstract_governance_profile();
    const std::vector<srf::Segment> segments = node_sequence(profile.max_depth);
    srf::SegmentListContent content{};
    content.id = list_id(1);
    content.scope = srf::ScopeId{kScope};
    content.profile = profile.id;
    content.profile_generation = profile.generation;
    content.capability = srf::CapabilityGeneration{1};
    content.strictness = srf::Strictness::Strict;
    content.segments = segments;
    const std::uint32_t width = profile.payload_width;

    return run_benchmark("canonical_encode_digest_full_depth", budget_ns, [&]() {
        std::uint64_t unit_bytes = 0;
        for (const srf::Segment& segment : segments) {
            const srf::ByteBuffer bytes = srf::canonical_segment_bytes(segment, width);
            unit_bytes += static_cast<std::uint64_t>(bytes.size());
            g_sink.mix(srf::digest128(srf::as_bytes(bytes)));
        }
        const srf::ByteBuffer sequence = srf::canonical_sequence_bytes(segments, width);
        const srf::ByteBuffer whole = srf::canonical_content_bytes(content, width);
        g_sink.mix(unit_bytes);
        g_sink.mix(static_cast<std::uint64_t>(sequence.size()));
        g_sink.mix(srf::sequence_digest(segments, width));
        g_sink.mix(srf::content_digest(content, width));
        g_sink.mix(static_cast<std::uint64_t>(whole.size()));
    });
}

struct ValidationFixture {
    srf::Limits limits{};
    srf::SegmentProfile profile{};
    std::vector<srf::Segment> valid_segments{};
    std::vector<srf::Segment> invalid_segments{};
    srf::FabricEvidence evidence{};
    srf::AuthorityView authority{};
    srf::CallerIdentity caller{};
};

[[nodiscard]] ValidationFixture make_validation_fixture() {
    ValidationFixture fixture{};
    Harness harness = make_harness();
    fixture.profile = srf::abstract_governance_profile();
    fixture.valid_segments = node_sequence(fixture.profile.max_depth);
    fixture.invalid_segments = fixture.valid_segments;
    fixture.invalid_segments.push_back(node_segment(1));
    fixture.evidence = harness.evidence->snapshot();
    fixture.authority.epoch = harness.store->authority().epoch();
    fixture.authority.publishers = harness.store->authority().publishers();
    fixture.authority.fenced_boots = harness.store->authority().fenced_boots();
    fixture.caller = next_caller(harness);
    return fixture;
}

[[nodiscard]] srf::ValidationRequest make_validation_request(ValidationFixture& fixture,
                                                             std::vector<srf::Segment>& segments) {
    srf::ValidationRequest request{};
    request.limits = &fixture.limits;
    request.caller = &fixture.caller;
    request.authority = &fixture.authority;
    request.list_id = list_id(1);
    request.scope = srf::ScopeId{kScope};
    request.profile_id = fixture.profile.id;
    request.profile = &fixture.profile;
    request.profile_found = true;
    request.segments = segments;
    request.evidence = &fixture.evidence;
    request.full = true;
    request.require_absent = true;
    return request;
}

/// Commit benchmarks rebuild a store every chunk so that the measured operation
/// always runs against a bounded durable state (a fixed number of lists, bounded
/// history and a bounded attempt table). The rebuild itself is never timed.
[[nodiscard]] BenchRow bench_create(std::size_t budget_ns) {
    constexpr std::size_t kChunk = 32;
    constexpr std::size_t kSegmentsPerList = 8;
    std::size_t failures = 0;
    const BenchRow row =
        run_chunked("commit_create_list_with_persistence", budget_ns, kChunk,
                    [&](std::size_t n) {
                        Harness harness = make_harness();
                        return measure_ns(n, [&]() {
                            const std::size_t index = harness.store->list_count() + 1;
                            srf::ListDraft draft = make_draft(static_cast<std::uint64_t>(index));
                            draft.segments = node_sequence(kSegmentsPerList);
                            const srf::MutationOutcome outcome =
                                harness.store->create_list(next_caller(harness), draft);
                            if (!outcome.ok()) {
                                ++failures;
                            }
                            g_sink.mix(outcome.generation.value());
                            g_sink.mix(outcome.digest);
                        });
                    });
    if (failures != 0) {
        std::printf("srf_bench: %zu create commits were rejected\n", failures);
        std::exit(2);
    }
    return row;
}

[[nodiscard]] BenchRow bench_replace(std::size_t budget_ns) {
    constexpr std::size_t kChunk = 32;
    constexpr std::size_t kSegmentsPerList = 8;
    std::size_t failures = 0;
    const BenchRow row =
        run_chunked("commit_replace_list_with_persistence", budget_ns, kChunk,
                    [&](std::size_t n) {
                        Harness harness = make_harness();
                        std::vector<srf::SegmentListGeneration> generations;
                        generations.reserve(kChunk);
                        for (std::size_t i = 1; i <= kChunk; ++i) {
                            srf::ListDraft draft = make_draft(i);
                            draft.segments = node_sequence(kSegmentsPerList);
                            const srf::MutationOutcome created =
                                harness.store->create_list(next_caller(harness), draft);
                            if (!created.ok()) {
                                std::printf("srf_bench: replace fixture create failed\n");
                                std::exit(2);
                            }
                            generations.push_back(created.generation);
                        }
                        std::size_t cursor = 0;
                        return measure_ns(n, [&]() {
                            const std::size_t slot = cursor % kChunk;
                            ++cursor;
                            srf::ListDraft draft = make_draft(static_cast<std::uint64_t>(slot) + 1);
                            draft.segments = node_sequence(kSegmentsPerList);
                            draft.expected_generation = generations[slot];
                            const srf::MutationOutcome outcome =
                                harness.store->replace_list(next_caller(harness), draft);
                            if (!outcome.ok()) {
                                ++failures;
                            }
                            generations[slot] = outcome.generation;
                            g_sink.mix(outcome.generation.value());
                            g_sink.mix(outcome.digest);
                        });
                    });
    if (failures != 0) {
        std::printf("srf_bench: %zu replace commits were rejected\n", failures);
        std::exit(2);
    }
    return row;
}

[[nodiscard]] BenchRow bench_snapshot_diff(std::size_t budget_ns) {
    Harness harness = make_harness();
    srf::ListDraft first = make_draft(1);
    first.segments = node_sequence(8);
    const srf::MutationOutcome created = harness.store->create_list(next_caller(harness), first);
    if (!created.ok()) {
        std::printf("srf_bench: snapshot fixture create failed\n");
        std::exit(2);
    }
    const srf::SegmentListSnapshot from = harness.store->snapshot(list_id(1));

    srf::ListDraft reversed = make_draft(1);
    reversed.segments = node_sequence(8);
    std::reverse(reversed.segments.begin(), reversed.segments.end());
    reversed.expected_generation = created.generation;
    const srf::MutationOutcome replaced =
        harness.store->replace_list(next_caller(harness), reversed);
    if (!replaced.ok()) {
        std::printf("srf_bench: snapshot fixture replace failed\n");
        std::exit(2);
    }
    const srf::SegmentListSnapshot to = harness.store->snapshot(list_id(1));
    const srf::SnapshotDiff proof = harness.store->diff(from, to);
    if (proof.empty() || !proof.contains(srf::DiffKind::GenerationAdvance)) {
        std::printf("srf_bench: snapshot diff premise failed\n");
        std::exit(2);
    }

    return run_benchmark("snapshot_take_and_diff", budget_ns, [&]() {
        const srf::SegmentListSnapshot live = harness.store->snapshot(list_id(1));
        const srf::SnapshotDiff diff = harness.store->diff(from, to);
        g_sink.mix(live.revision);
        g_sink.mix(live.digest);
        g_sink.mix(static_cast<std::uint64_t>(diff.entries.size()));
        g_sink.mix(static_cast<std::uint64_t>(diff.order_changed ? 1 : 0));
    });
}

[[nodiscard]] BenchRow bench_wire(std::size_t budget_ns) {
    const srf::Limits limits{};
    srf::ValidationResult encode_result;
    encode_result.set_limits(&limits);
    srf::MutationRequest mutation{};
    mutation.attempt = srf::MutationAttemptId{0x1234};
    mutation.draft = make_draft(1);
    mutation.draft.segments = node_sequence(8);
    const srf::ByteBuffer payload = srf::encode_mutation(mutation, limits, encode_result);
    srf::Frame frame{};
    frame.message = srf::MessageId::CreateList;
    frame.request_id = 1;
    frame.payload = payload;
    srf::ValidationResult frame_result;
    frame_result.set_limits(&limits);
    const srf::ByteBuffer bytes = srf::encode_frame(frame, limits, frame_result);
    if (encode_result.failed() || frame_result.failed() || bytes.empty()) {
        std::printf("srf_bench: wire fixture encode failed\n");
        std::exit(2);
    }

    srf::ValidationResult round_result;
    round_result.set_limits(&limits);
    srf::Frame probe{};
    srf::ValidationResult probe_result;
    probe_result.set_limits(&limits);
    srf::MutationRequest probe_mutation{};
    if (!srf::decode_frame(srf::as_bytes(bytes), limits, probe, probe_result) ||
        !srf::decode_mutation(srf::as_bytes(probe.payload), limits, probe_mutation,
                              probe_result) ||
        probe_result.failed() ||
        probe_mutation.draft.segments.size() != mutation.draft.segments.size()) {
        std::printf("srf_bench: wire fixture round trip failed\n");
        std::exit(2);
    }

    return run_benchmark("wire_encode_decode_round_trip", budget_ns, [&]() {
        srf::ValidationResult out_encode;
        out_encode.set_limits(&limits);
        const srf::ByteBuffer encoded = srf::encode_frame(frame, limits, out_encode);
        srf::ValidationResult out_decode;
        out_decode.set_limits(&limits);
        srf::Frame decoded{};
        const bool frame_ok = srf::decode_frame(srf::as_bytes(encoded), limits, decoded, out_decode);
        srf::ValidationResult out_mutation;
        out_mutation.set_limits(&limits);
        srf::MutationRequest decoded_mutation{};
        const bool mutation_ok = srf::decode_mutation(srf::as_bytes(decoded.payload), limits,
                                                      decoded_mutation, out_mutation);
        g_sink.mix(static_cast<std::uint64_t>(encoded.size()));
        g_sink.mix(static_cast<std::uint64_t>(frame_ok && mutation_ok ? 1 : 0));
        g_sink.mix(static_cast<std::uint64_t>(decoded_mutation.draft.segments.size()));
        g_sink.mix(decoded_mutation.draft.id.value());
    });
}

[[nodiscard]] BenchRow bench_persistence(std::size_t budget_ns) {
    const srf::Limits limits{};
    Harness harness = make_harness();
    for (std::size_t i = 1; i <= 32; ++i) {
        srf::ListDraft draft = make_draft(i);
        draft.segments = node_sequence(8);
        const srf::MutationOutcome outcome = harness.store->create_list(next_caller(harness), draft);
        if (!outcome.ok()) {
            std::printf("srf_bench: persistence fixture create failed\n");
            std::exit(2);
        }
    }
    const srf::DurableState state = harness.store->export_state();
    srf::ValidationResult encode_result;
    encode_result.set_limits(&limits);
    const srf::ByteBuffer image = srf::encode_durable_state(state, limits, encode_result);
    if (encode_result.failed() || image.empty()) {
        std::printf("srf_bench: persistence fixture encode failed\n");
        std::exit(2);
    }

    return run_benchmark("persistence_encode_decode_round_trip", budget_ns, [&]() {
        srf::ValidationResult out_encode;
        out_encode.set_limits(&limits);
        const srf::ByteBuffer encoded = srf::encode_durable_state(state, limits, out_encode);
        srf::ValidationResult out_decode;
        out_decode.set_limits(&limits);
        srf::DurableState decoded{};
        const bool ok = srf::decode_durable_state(srf::as_bytes(encoded), limits, decoded, out_decode);
        g_sink.mix(static_cast<std::uint64_t>(encoded.size()));
        g_sink.mix(static_cast<std::uint64_t>(ok ? 1 : 0));
        g_sink.mix(static_cast<std::uint64_t>(decoded.lists.size()));
        g_sink.mix(static_cast<std::uint64_t>(decoded.history.size()));
    });
}

// ------------------------------------------------------------------- printing
void print_row(const BenchRow& row) {
    const double total_ms = row.total_ns / 1'000'000.0;
    const double ns_per_op =
        row.iterations == 0 ? 0.0 : row.total_ns / static_cast<double>(row.iterations);
    const double ops_per_second = ns_per_op > 0.0 ? 1'000'000'000.0 / ns_per_op : 0.0;
    std::printf("%-38s %12zu %12.3f %14.1f %14.0f\n", row.name.data(), row.iterations, total_ms,
                ns_per_op, ops_per_second);
}

} // namespace

int main(int argc, char** argv) {
    std::size_t scale = 1;
    if (argc > 2) {
        std::printf("usage: srf_bench [scale]\n");
        return 2;
    }
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        std::size_t value = 0;
        bool ok = !argument.empty();
        for (const char c : argument) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            value = value * 10 + static_cast<std::size_t>(c - '0');
            if (value > 1000) {
                value = 1000;
            }
        }
        if (!ok || value == 0) {
            std::printf("usage: srf_bench [scale]\n");
            return 2;
        }
        scale = value;
    }

    const std::size_t budget_ns = 150'000'000 * scale;

    ValidationFixture fixture = make_validation_fixture();
    srf::ValidationRequest valid_request = make_validation_request(fixture, fixture.valid_segments);
    const srf::ValidationResult valid_proof = srf::validate_request(valid_request);
    if (valid_proof.failed()) {
        std::printf("srf_bench: valid-list premise failed: %s\n", valid_proof.primary_name().data());
        return 2;
    }
    srf::ValidationRequest invalid_request =
        make_validation_request(fixture, fixture.invalid_segments);
    const srf::ValidationResult invalid_proof = srf::validate_request(invalid_request);
    if (!invalid_proof.contains(srf::ReasonCode::ProfileMaxDepthExceeded) ||
        !invalid_proof.contains(srf::ReasonCode::SegmentDuplicateNonConsecutive)) {
        std::printf("srf_bench: invalid-list premise failed: %s\n",
                    invalid_proof.primary_name().data());
        return 2;
    }
    std::printf("srf_bench %s  Segment Routing Fabric  [%s]\n", srf::version_string().data(),
                srf::support_classification().data());
    std::printf("scale=%zu  budget=%zu ms per benchmark  clock=std::chrono::steady_clock\n", scale,
                budget_ns / 1'000'000);
    std::printf("evidence=SYNTHETIC  persistence=MemoryPersistence  iterations=pilot-measured\n");
    std::printf("premise: valid-list primary=%s  invalid-list primary=%s (reasons=%zu)\n",
                valid_proof.ok() ? "Ok" : valid_proof.primary_name().data(),
                invalid_proof.primary_name().data(), invalid_proof.size());
    std::printf("%-38s %12s %12s %14s %14s\n", "benchmark", "iterations", "total ms", "ns/op",
                "ops/s");
    std::printf("-------------------------------------------------------------------------------"
                "-------------\n");

    std::vector<BenchRow> rows;
    rows.push_back(bench_canonical(budget_ns));
    rows.push_back(run_benchmark("validate_valid_list", budget_ns, [&]() {
        const srf::ValidationResult result = srf::validate_request(valid_request);
        g_sink.mix(static_cast<std::uint64_t>(result.size()));
    }));
    rows.push_back(run_benchmark("validate_invalid_list", budget_ns, [&]() {
        const srf::ValidationResult result = srf::validate_request(invalid_request);
        g_sink.mix(static_cast<std::uint64_t>(result.size()));
        g_sink.mix(static_cast<std::uint64_t>(result.primary().code));
    }));
    rows.push_back(bench_create(budget_ns));
    rows.push_back(bench_replace(budget_ns));
    rows.push_back(bench_snapshot_diff(budget_ns));
    rows.push_back(bench_wire(budget_ns));
    rows.push_back(bench_persistence(budget_ns));

    for (const BenchRow& row : rows) {
        print_row(row);
    }
    std::printf("-------------------------------------------------------------------------------"
                "-------------\n");
    std::printf("sink=%llu\n", static_cast<unsigned long long>(g_sink.value));
    std::printf("srf_bench completed %zu benchmarks\n", rows.size());
    return 0;
}
