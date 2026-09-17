// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "srf/bytes.hpp"
#include "srf/reason.hpp"

namespace srf {

/// Minimal loopback stream socket. This is real OS transport (TCP over the
/// loopback interface), used by the worker/coordinator process proofs. It is not
/// a WAN transport and this runtime makes no availability claim about one.
class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// Bind and listen on 127.0.0.1. Port 0 selects an ephemeral port.
    [[nodiscard]] static Socket listen_loopback(std::uint16_t port, ValidationResult& out);
    [[nodiscard]] static Socket connect_loopback(std::uint16_t port, ValidationResult& out);

    [[nodiscard]] Socket accept(ValidationResult& out) const;
    [[nodiscard]] bool send_all(std::span<const std::byte> bytes);
    /// Returns true when bytes were read. Returns false with received == 0 on an
    /// orderly peer close, and false with received unchanged on error.
    [[nodiscard]] bool recv_some(std::span<std::byte> buffer, std::size_t& received);
    void close();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const;

private:
    std::uintptr_t handle_{0};
};

/// Publish and read the coordinator's actual listening port.
[[nodiscard]] bool write_port_file(const std::filesystem::path& path, std::uint16_t port);
[[nodiscard]] bool read_port_file(const std::filesystem::path& path, std::uint16_t& port);

/// Absolute path of the directory containing the running executable.
[[nodiscard]] std::filesystem::path executable_directory();

} // namespace srf
