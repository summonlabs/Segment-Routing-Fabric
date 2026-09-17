// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Real coordinator process. Loads durable state, raises its epoch, listens on
// loopback TCP and serves the Segment Routing Fabric wire protocol.
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
    std::filesystem::path state_path{};
    std::filesystem::path port_path{};
    std::uint16_t port{0};
    std::uint32_t lease_ms{4000};
    std::uint32_t hold_ms{0};
    bool load_persisted{true};
    bool advance_epoch{true};
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
        if (arg == "--state") {
            if (!next(value)) {
                return false;
            }
            options.state_path = value;
        } else if (arg == "--port-file") {
            if (!next(value)) {
                return false;
            }
            options.port_path = value;
        } else if (arg == "--port") {
            if (!next(value)) {
                return false;
            }
            options.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (arg == "--lease-ms") {
            if (!next(value)) {
                return false;
            }
            options.lease_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (arg == "--hold-ms") {
            if (!next(value)) {
                return false;
            }
            options.hold_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (arg == "--no-load") {
            options.load_persisted = false;
        } else if (arg == "--no-epoch-bump") {
            options.advance_epoch = false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr, "usage: srf_coordinator --state <file> --port-file <file> "
                             "[--port n] [--lease-ms n] [--hold-ms n] [--no-load] "
                             "[--no-epoch-bump]\n");
        return 2;
    }
    srf::CoordinatorConfig config{};
    config.state_path = options.state_path;
    config.port_path = options.port_path;
    config.port = options.port;
    config.lease_milliseconds = options.lease_ms;
    config.load_persisted = options.load_persisted;
    config.advance_epoch = options.advance_epoch;

    srf::CoordinatorServer server(config);
    srf::ValidationResult result;
    if (!server.start(result)) {
        std::fprintf(stderr, "start failed: %s\n", result.to_string().c_str());
        return 3;
    }
    std::printf("SRF_COORDINATOR_READY epoch=%llu port=%u recovered=%zu\n",
                static_cast<unsigned long long>(server.epoch().value()),
                static_cast<unsigned>(server.port()), server.load_report().lists_recovered);
    std::fflush(stdout);

    if (options.hold_ms != 0) {
        std::thread stopper([&server, &options] {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.hold_ms));
            server.stop();
        });
        server.serve();
        stopper.join();
    } else {
        server.serve();
    }
    server.stop();
    std::printf("SRF_COORDINATOR_STOPPED epoch=%llu\n",
                static_cast<unsigned long long>(server.epoch().value()));
    std::fflush(stdout);
    return 0;
}
