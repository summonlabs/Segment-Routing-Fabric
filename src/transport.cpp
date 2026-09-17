// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/transport.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace srf {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

struct WinsockGuard {
    WinsockGuard() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (ready_) {
            WSACleanup();
        }
    }
    [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
    bool ready_{false};
};

/// Starts Winsock exactly once. Returns false when the transport stack is
/// unavailable, which is a hard rejection rather than an ignored failure.
[[nodiscard]] bool ensure_winsock() {
    static WinsockGuard guard;
    return guard.ready();
}

[[nodiscard]] bool would_block() noexcept {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

[[nodiscard]] bool ensure_winsock() { return true; }

[[nodiscard]] bool would_block() noexcept {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
}
#endif

} // namespace

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

bool Socket::valid() const noexcept {
    return handle_ != 0;
}

void Socket::close() {
    if (handle_ == 0) {
        return;
    }
    const NativeSocket sock = static_cast<NativeSocket>(handle_);
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
    handle_ = 0;
}

Socket Socket::listen_loopback(std::uint16_t port, ValidationResult& out) {
    if (!ensure_winsock()) {
        out.add(ReasonCode::NotSupported, 0, 1);
        return Socket{};
    }
    Socket socket;
    const NativeSocket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) {
        out.add(ReasonCode::InternalError, 0, 1);
        return socket;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        out.add(ReasonCode::InternalError, 0, 2);
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        return socket;
    }
    if (::listen(sock, 8) != 0) {
        out.add(ReasonCode::InternalError, 0, 3);
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        return socket;
    }
    socket.handle_ = static_cast<std::uintptr_t>(sock);
    return socket;
}

Socket Socket::connect_loopback(std::uint16_t port, ValidationResult& out) {
    if (!ensure_winsock()) {
        out.add(ReasonCode::NotSupported, 0, 2);
        return Socket{};
    }
    Socket socket;
    const NativeSocket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) {
        out.add(ReasonCode::InternalError, 0, 4);
        return socket;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        out.add(ReasonCode::InternalError, 0, 5);
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        return socket;
    }
    socket.handle_ = static_cast<std::uintptr_t>(sock);
    return socket;
}

Socket Socket::accept(ValidationResult& out) const {
    Socket peer;
    if (handle_ == 0) {
        out.add(ReasonCode::InternalError, 0, 6);
        return peer;
    }
    const NativeSocket listener = static_cast<NativeSocket>(handle_);
    const NativeSocket accepted = ::accept(listener, nullptr, nullptr);
    if (accepted == kInvalidSocket) {
        out.add(ReasonCode::InternalError, 0, 7);
        return peer;
    }
    peer.handle_ = static_cast<std::uintptr_t>(accepted);
    return peer;
}

bool Socket::send_all(std::span<const std::byte> bytes) {
    if (handle_ == 0) {
        return false;
    }
    const NativeSocket sock = static_cast<NativeSocket>(handle_);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = static_cast<int>(
            bytes.size() - sent > 1u << 20 ? 1u << 20 : bytes.size() - sent);
        const int written = ::send(sock, reinterpret_cast<const char*>(bytes.data() + sent), chunk, 0);
        if (written <= 0) {
            if (would_block()) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool Socket::recv_some(std::span<std::byte> buffer, std::size_t& received) {
    received = 0;
    if (handle_ == 0 || buffer.empty()) {
        return false;
    }
    const NativeSocket sock = static_cast<NativeSocket>(handle_);
    for (;;) {
        const int chunk = static_cast<int>(buffer.size() > 1u << 20 ? 1u << 20 : buffer.size());
        const int got = ::recv(sock, reinterpret_cast<char*>(buffer.data()), chunk, 0);
        if (got == 0) {
            return false;  // orderly close
        }
        if (got < 0) {
            if (would_block()) {
                continue;
            }
            return false;
        }
        received = static_cast<std::size_t>(got);
        return true;
    }
}

std::uint16_t Socket::local_port() const {
    if (handle_ == 0) {
        return 0;
    }
    sockaddr_in address{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address),
                    &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

bool write_port_file(const std::filesystem::path& path, std::uint16_t port) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        return false;
    }
    file << port << "\n";
    file.flush();
    return static_cast<bool>(file);
}

bool read_port_file(const std::filesystem::path& path, std::uint16_t& port) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    unsigned int value = 0;
    file >> value;
    if (!file || value == 0 || value > 65535u) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

std::filesystem::path executable_directory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : self.parent_path();
#endif
}

} // namespace srf
