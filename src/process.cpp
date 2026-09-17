// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/process.hpp"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace srf {
namespace {

#ifdef _WIN32
[[nodiscard]] std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(),
                        size);
    return out;
}

[[nodiscard]] std::wstring quote(const std::wstring& value) {
    std::wstring out = L"\"";
    for (const wchar_t c : value) {
        if (c == L'"') {
            out += L'\\';
        }
        out += c;
    }
    out += L"\"";
    return out;
}
#endif

} // namespace

ChildProcess::~ChildProcess() {
    if (handle_ != 0) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
        // The child may already be reaped by wait(); avoid leaving zombies.
        int status = 0;
        waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
#endif
    }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_) {
    other.handle_ = 0;
    other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        if (handle_ != 0) {
#ifdef _WIN32
            CloseHandle(reinterpret_cast<HANDLE>(handle_));
#endif
        }
        handle_ = other.handle_;
        pid_ = other.pid_;
        other.handle_ = 0;
        other.pid_ = 0;
    }
    return *this;
}

ChildProcess ChildProcess::spawn(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& working_directory,
                                 ValidationResult& out) {
    ChildProcess process;
#ifdef _WIN32
    std::wstring command = quote(widen(executable));
    for (const std::string& argument : arguments) {
        command += L' ';
        command += quote(widen(argument));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const std::wstring cwd = widen(working_directory);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &info);
    if (!ok) {
        out.add(ReasonCode::InternalError, 0, static_cast<std::uint64_t>(GetLastError()));
        return process;
    }
    CloseHandle(info.hThread);
    process.handle_ = reinterpret_cast<std::uintptr_t>(info.hProcess);
    process.pid_ = static_cast<std::uint64_t>(info.dwProcessId);
#else
    std::vector<std::string> storage;
    storage.push_back(executable);
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        out.add(ReasonCode::InternalError, 0, static_cast<std::uint64_t>(rc));
        return process;
    }
    process.handle_ = static_cast<std::uintptr_t>(pid);
    process.pid_ = static_cast<std::uint64_t>(pid);
    (void)working_directory;
#endif
    return process;
}

bool ChildProcess::running() const {
    if (handle_ == 0) {
        return false;
    }
#ifdef _WIN32
    DWORD code = 0;
    if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code)) {
        return false;
    }
    return code == STILL_ACTIVE;
#else
    int status = 0;
    const pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    return result == 0;
#endif
}

void ChildProcess::kill() {
    if (handle_ == 0) {
        return;
    }
#ifdef _WIN32
    TerminateProcess(reinterpret_cast<HANDLE>(handle_), 1);
#else
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
}

int ChildProcess::wait() {
    if (handle_ == 0) {
        return -1;
    }
#ifdef _WIN32
    WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code)) {
        return -1;
    }
    return static_cast<int>(code);
#else
    int status = 0;
    if (waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

} // namespace srf
