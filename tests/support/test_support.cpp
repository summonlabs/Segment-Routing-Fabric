// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "test_support.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>

#include "srf/srf.hpp"

namespace srf::test {
namespace {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

std::uint64_t g_active_seed = 0;

} // namespace

void register_test(const TestCase& test) {
    registry().push_back(test);
}

void set_active_seed(std::uint64_t seed) {
    g_active_seed = seed;
}

std::uint64_t active_seed() {
    return g_active_seed;
}

int run_all(int argc, char** argv) {
    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }
    std::vector<TestCase>& tests = registry();
    std::stable_sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
        if (std::strcmp(a.suite, b.suite) != 0) {
            return std::strcmp(a.suite, b.suite) < 0;
        }
        return std::strcmp(a.name, b.name) < 0;
    });

    std::size_t executed = 0;
    std::size_t failed = 0;
    const char* current_suite = nullptr;
    for (const TestCase& test : tests) {
        const std::string full = std::string(test.suite) + "." + test.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            continue;
        }
        if (current_suite == nullptr || std::strcmp(current_suite, test.suite) != 0) {
            current_suite = test.suite;
            std::printf("[%s]\n", current_suite);
        }
        Context context;
        const std::size_t before = context.failures();
        std::printf("  %s\n", test.name);
        std::fflush(stdout);
        try {
            test.fn(context);
        } catch (const std::exception& error) {
            std::printf("    FAIL unhandled exception: %s\n", error.what());
            context.record(false, "no exception", __FILE__, __LINE__);
        } catch (...) {
            std::printf("    FAIL unhandled non-standard exception\n");
            context.record(false, "no exception", __FILE__, __LINE__);
        }
        ++executed;
        if (context.failures() != before) {
            ++failed;
            std::printf("  --> FAILED (seed=%llu)\n",
                        static_cast<unsigned long long>(active_seed()));
        }
    }
    std::printf("\n%zu test case(s) executed, %zu failed\n", executed, failed);
    std::fflush(stdout);
    if (executed == 0) {
        std::printf("no test case matched the filter\n");
        return 2;
    }
    return failed == 0 ? 0 : 1;
}

} // namespace srf::test

int main(int argc, char** argv) {
    return srf::test::run_all(argc, argv);
}
