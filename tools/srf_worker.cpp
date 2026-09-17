// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Real worker process. Registers a fresh boot identifier with the coordinator,
// optionally publishes one governed abstract segment list, and then optionally
// holds the connection open until it is killed by the parent.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "srf/srf.hpp"

namespace {

struct Options {
    std::filesystem::path port_path{};
    std::uint64_t boot{1};
    std::uint64_t publisher{1};
    std::uint64_t scope{1};
    std::uint64_t list_id{0};
    std::uint64_t segment_id{0};
    std::uint64_t node_id{0};
    std::uint32_t hold_ms{0};
    std::uint32_t heartbeat_ms{0};
    bool publish{false};
    bool replace{false};
    bool revalidate{false};
};

[[nodiscard]] bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](std::string& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--port-file") {
            if (!next(value)) {
                return false;
            }
            options.port_path = value;
        } else if (arg == "--boot") {
            if (!next(value)) {
                return false;
            }
            options.boot = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--publisher") {
            if (!next(value)) {
                return false;
            }
            options.publisher = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--scope") {
            if (!next(value)) {
                return false;
            }
            options.scope = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--list") {
            if (!next(value)) {
                return false;
            }
            options.list_id = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--segment") {
            if (!next(value)) {
                return false;
            }
            options.segment_id = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--node") {
            if (!next(value)) {
                return false;
            }
            options.node_id = std::strtoull(value.c_str(), nullptr, 10);
        } else if (arg == "--hold-ms") {
            if (!next(value)) {
                return false;
            }
            options.hold_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (arg == "--heartbeat-ms") {
            if (!next(value)) {
                return false;
            }
            options.heartbeat_ms =
                static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (arg == "--publish") {
            options.publish = true;
        } else if (arg == "--replace") {
            options.replace = true;
        } else if (arg == "--revalidate") {
            options.revalidate = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

[[nodiscard]] srf::ListDraft make_draft(const Options& options, std::uint64_t generation) {
    srf::ListDraft draft{};
    draft.id = srf::SegmentListId{options.list_id};
    draft.scope = srf::ScopeId{options.scope};
    draft.profile = srf::SegmentProfileId{0x0001'0001ull};
    draft.expected_generation = srf::SegmentListGeneration{generation};
    srf::Segment segment{};
    segment.kind = srf::SegmentKind::Node;
    segment.id = srf::SegmentId{options.segment_id};
    segment.generation = srf::SegmentGeneration{1};
    segment.node = srf::NodeId{options.node_id};
    segment.payload = {std::byte{0x01}, std::byte{0x02}};
    draft.segments.push_back(segment);
    return draft;
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr, "usage: srf_worker --port-file <file> --boot n --publisher n "
                             "--scope n [--publish] [--replace] [--revalidate] [--list n] "
                             "[--segment n] [--node n] [--heartbeat-ms n] [--hold-ms n]\n");
        return 2;
    }
    srf::WorkerConfig config{};
    config.port_path = options.port_path;
    config.boot = srf::WorkerBootId{options.boot};
    config.publisher = srf::PublisherId{options.publisher};
    config.scope = srf::ScopeId{options.scope};

    srf::WorkerClient client;
    srf::ValidationResult result;
    if (!client.open(config, result)) {
        std::fprintf(stderr, "connect failed: %s\n", result.to_string().c_str());
        return 3;
    }
    const srf::Response hello = client.handshake();
    std::printf("SRF_WORKER_HELLO status=%s epoch=%llu session=%llu\n",
                std::string(srf::status_code_name(hello.status)).c_str(),
                static_cast<unsigned long long>(hello.epoch.value()),
                static_cast<unsigned long long>(hello.session.value()));
    std::fflush(stdout);
    if (hello.status != srf::StatusCode::Ok) {
        std::printf("SRF_WORKER_REJECTED primary=%s\n",
                    hello.reasons.empty()
                        ? "None"
                        : std::string(srf::reason_code_name(hello.reasons.front().code)).c_str());
        std::fflush(stdout);
        return 0;
    }

    const auto report = [](const char* what, const srf::Response& response) {
        std::printf("SRF_WORKER_%s status=%s generation=%llu digest=%s replay=%d\n", what,
                    std::string(srf::status_code_name(response.status)).c_str(),
                    static_cast<unsigned long long>(response.generation.value()),
                    response.digest.hex().c_str(), response.idempotent_replay ? 1 : 0);
        std::fflush(stdout);
    };

    if (options.publish) {
        report("PUBLISH",
               client.send_mutation(srf::MessageId::CreateList, make_draft(options, 0)));
    }
    if (options.replace) {
        const srf::Response current = client.get(srf::SegmentListId{options.list_id});
        std::uint64_t generation = 0;
        if (!current.summaries.empty()) {
            generation = current.summaries.front().generation.value();
        }
        report("REPLACE",
               client.send_mutation(srf::MessageId::ReplaceList, make_draft(options, generation)));
    }
    if (options.revalidate) {
        const srf::Response current = client.get(srf::SegmentListId{options.list_id});
        std::uint64_t generation = 0;
        if (!current.summaries.empty()) {
            generation = current.summaries.front().generation.value();
        }
        report("REVALIDATE", client.send_lifecycle(srf::LifecycleOp::Revalidate,
                                                   srf::SegmentListId{options.list_id},
                                                   srf::SegmentListGeneration{generation}));
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.hold_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (options.heartbeat_ms != 0) {
            const srf::Response beat = client.heartbeat();
            if (beat.status != srf::StatusCode::Ok) {
                std::printf("SRF_WORKER_HEARTBEAT_FAILED primary=%s\n",
                            beat.reasons.empty()
                                ? "None"
                                : std::string(srf::reason_code_name(beat.reasons.front().code))
                                      .c_str());
                std::fflush(stdout);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("SRF_WORKER_DONE\n");
    std::fflush(stdout);
    return 0;
}
