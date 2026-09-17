// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace srf::test {

class Context {
public:
    void record(bool passed, const std::string& expression, const char* file, int line) {
        ++checks_;
        if (!passed) {
            ++failures_;
            std::printf("    FAIL %s:%d  %s\n", file, line, expression.c_str());
            std::fflush(stdout);
        }
    }

    [[nodiscard]] std::size_t checks() const noexcept { return checks_; }
    [[nodiscard]] std::size_t failures() const noexcept { return failures_; }

private:
    std::size_t checks_{0};
    std::size_t failures_{0};
};

using TestFn = void (*)(Context&);

struct TestCase {
    const char* suite;
    const char* name;
    TestFn fn;
};

void register_test(const TestCase& test);
/// Runs every registered test. Returns the process exit code.
int run_all(int argc, char** argv);

/// Deterministic splitmix64. The emitted seed is printed on failure so a failing
/// property run can be replayed exactly.
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : state_(seed), seed_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    [[nodiscard]] std::uint32_t below(std::uint32_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
    }

    [[nodiscard]] bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t state_{0};
    std::uint64_t seed_{0};
};

/// The seed in effect for the current test, printed when a property test fails.
void set_active_seed(std::uint64_t seed);
[[nodiscard]] std::uint64_t active_seed();

template <class T>
concept Streamable = requires(std::ostream& out, const T& value) { out << value; };

template <class T>
concept HasValueMember = requires(const T& value) {
    { value.value() } -> std::convertible_to<unsigned long long>;
};

/// Renders a value for an assertion message. Streamable values stream; enums and
/// strongly typed identifiers fall back to their numeric value.
template <class T>
[[nodiscard]] std::string show(const T& value) {
    if constexpr (Streamable<T>) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(
            static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
    } else if constexpr (HasValueMember<T>) {
        return std::to_string(static_cast<unsigned long long>(value.value()));
    } else {
        return "<value>";
    }
}

} // namespace srf::test

#define SRF_TEST(suite, name)                                                          \
    static void suite##_##name##_body(::srf::test::Context&);                          \
    namespace {                                                                        \
    const bool suite##_##name##_registered = [] {                                      \
        ::srf::test::register_test(                                                    \
            ::srf::test::TestCase{#suite, #name, &suite##_##name##_body});             \
        return true;                                                                   \
    }();                                                                               \
    }                                                                                  \
    static void suite##_##name##_body(::srf::test::Context& srf_ctx)

#define SRF_EXPECT(condition)                                                          \
    srf_ctx.record((condition), #condition, __FILE__, __LINE__)

#define SRF_EXPECT_EQ(actual, expected)                                                \
    do {                                                                               \
        const auto srf_lhs = (actual);                                                 \
        const auto srf_rhs = (expected);                                               \
        srf_ctx.record(srf_lhs == srf_rhs,                                             \
                       std::string(#actual " == " #expected) + " [" +                  \
                           ::srf::test::show(srf_lhs) + " vs " + ::srf::test::show(srf_rhs) + \
                           "]",                                                        \
                       __FILE__, __LINE__);                                            \
    } while (false)

#define SRF_EXPECT_NE(actual, expected)                                                \
    do {                                                                               \
        const auto srf_lhs = (actual);                                                 \
        const auto srf_rhs = (expected);                                               \
        srf_ctx.record(srf_lhs != srf_rhs, #actual " != " #expected, __FILE__, __LINE__); \
    } while (false)

/// Records a reason-code expectation with a readable message.
#define SRF_EXPECT_REASON(result, code)                                                \
    do {                                                                               \
        const auto& srf_result = (result);                                             \
        srf_ctx.record(srf_result.contains(code),                                      \
                       std::string("reason ") + std::string(::srf::reason_code_name(code)) + \
                           " present, primary=" + std::string(srf_result.primary_name()) + \
                           " all=" + srf_result.to_string(),                            \
                       __FILE__, __LINE__);                                            \
    } while (false)

#define SRF_EXPECT_OK(outcome)                                                         \
    do {                                                                               \
        const auto& srf_outcome = (outcome);                                           \
        srf_ctx.record(srf_outcome.ok(),                                               \
                       std::string("expected success but status=") +                   \
                           std::string(::srf::status_code_name(srf_outcome.status)) +  \
                           " reasons=" + srf_outcome.result.to_string(),               \
                       __FILE__, __LINE__);                                            \
    } while (false)

#define SRF_EXPECT_REJECTED(outcome, code)                                             \
    do {                                                                               \
        const auto& srf_outcome = (outcome);                                           \
        srf_ctx.record(srf_outcome.result.contains(code),                              \
                       std::string("expected rejection ") +                            \
                           std::string(::srf::reason_code_name(code)) + " but status=" + \
                           std::string(::srf::status_code_name(srf_outcome.status)) +  \
                           " reasons=" + srf_outcome.result.to_string(),               \
                       __FILE__, __LINE__);                                            \
    } while (false)

#define SRF_EXPECT_PRIMARY(result, code)                                               \
    do {                                                                               \
        const auto& srf_result = (result);                                             \
        srf_ctx.record(srf_result.primary().code == (code),                            \
                       std::string("primary ") + std::string(::srf::reason_code_name(code)) + \
                           " but was " + std::string(srf_result.primary_name()),        \
                       __FILE__, __LINE__);                                            \
    } while (false)
