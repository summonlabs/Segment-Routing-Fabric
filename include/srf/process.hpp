// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "srf/reason.hpp"

namespace srf {

/// A real child process with a real, killable operating-system handle. Used by the
/// worker-death and coordinator-restart proofs: those proofs terminate an actual
/// process, they do not simulate one.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] static ChildProcess spawn(const std::string& executable,
                                            const std::vector<std::string>& arguments,
                                            const std::string& working_directory,
                                            ValidationResult& out);

    [[nodiscard]] bool started() const noexcept { return handle_ != 0; }
    [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }
    [[nodiscard]] bool running() const;
    /// Forceful termination. On Windows this is TerminateProcess; on POSIX SIGKILL.
    void kill();
    /// Wait for exit and return the exit code, or -1 when unavailable.
    [[nodiscard]] int wait();

private:
    std::uintptr_t handle_{0};
    std::uint64_t pid_{0};
};

/// Poll a predicate until it reports true or the explicit bound elapses. The bound
/// is a hard product failure condition, not a test timeout: callers assert on the
/// return value.
template <class Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, std::uint32_t bound_milliseconds) {
    const std::uint32_t step = 5;
    std::uint32_t elapsed = 0;
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (elapsed >= bound_milliseconds) {
            return predicate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        elapsed += step;
    }
}

} // namespace srf
