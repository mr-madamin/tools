#include "test.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <print>
#include <unistd.h>
#include <vector>

namespace t {
namespace {

struct Test {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

// Function-local, so registration from another translation unit's static
// initialiser can never run before the vector exists.
std::vector<Test>& registry()
{
    static std::vector<Test> tests;
    return tests;
}

bool colour()
{
    static const bool tty = isatty(STDOUT_FILENO) != 0;
    return tty;
}

std::string dim(std::string_view s) { return colour() ? std::format("\x1b[2m{}\x1b[0m", s) : std::string(s); }
std::string green(std::string_view s) { return colour() ? std::format("\x1b[32m{}\x1b[0m", s) : std::string(s); }
std::string red(std::string_view s) { return colour() ? std::format("\x1b[31m{}\x1b[0m", s) : std::string(s); }
std::string bold(std::string_view s) { return colour() ? std::format("\x1b[1m{}\x1b[0m", s) : std::string(s); }

} // namespace

void register_test(std::string suite, std::string name, std::function<void()> body)
{
    registry().push_back(Test{std::move(suite), std::move(name), std::move(body)});
}

void fail_at(std::string_view expression, std::string_view message, const char* file, int line)
{
    throw Failure(std::format("{}\n      {} ({}:{})", message, dim(expression), file, line));
}

int run_all()
{
    int passed = 0;
    std::vector<std::string> failures;
    auto started = std::chrono::steady_clock::now();

    // Suites in the order they first appear, with their tests gathered under
    // them -- registration order follows link order, which would otherwise
    // split a suite in two whenever a test is appended to a file.
    std::vector<std::string> suites;
    for (const auto& test : registry())
        if (std::ranges::find(suites, test.suite) == suites.end())
            suites.push_back(test.suite);

    std::vector<const Test*> ordered;
    for (const auto& suite : suites)
        for (const auto& test : registry())
            if (test.suite == suite)
                ordered.push_back(&test);

    std::string current_suite;
    for (const Test* test_ptr : ordered) {
        const Test& test = *test_ptr;
        if (test.suite != current_suite) {
            current_suite = test.suite;
            std::print("\n{}\n", bold(current_suite));
        }

        try {
            test.body();
            std::print("  {} {}\n", green("✓"), test.name);
            passed++;
        } catch (const Failure& e) {
            std::print("  {} {}\n      {}\n", red("✗"), test.name, e.what());
            failures.push_back(std::format("{} / {}", test.suite, test.name));
        } catch (const std::exception& e) {
            std::print("  {} {}\n      {} {}\n", red("✗"), test.name,
                       red("unexpected exception:"), e.what());
            failures.push_back(std::format("{} / {}", test.suite, test.name));
        }
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count();

    if (failures.empty()) {
        std::print("\n{} {} tests passed in {}ms\n", green("✓"), passed, ms);
        return 0;
    }
    std::print("\n{} {} failed, {} passed ({}ms):\n", red("✗"), failures.size(), passed, ms);
    for (const auto& name : failures)
        std::print("    {}\n", name);
    return 1;
}

} // namespace t

int main()
{
    return t::run_all();
}
