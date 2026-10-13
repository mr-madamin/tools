// A test runner in one header and one .cpp: register a function, assert with
// CHECK, get a section per suite and a tally at the end. The C port's runners
// are separate programs against a live server on 8765; these run the real
// session loop in-process on an ephemeral port, so the whole suite is one
// binary that needs nothing started first.
#pragma once

#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace t {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void register_test(std::string suite, std::string name, std::function<void()> body);

struct Registrar {
    Registrar(std::string suite, std::string name, void (*body)())
    {
        register_test(std::move(suite), std::move(name), body);
    }
};

[[noreturn]] void fail_at(std::string_view expression, std::string_view message,
                          const char* file, int line);

inline void check_at(bool condition, std::string_view expression, std::string_view message,
                     const char* file, int line)
{
    if (!condition)
        fail_at(expression, message, file, line);
}

int run_all();

} // namespace t

// TEST(suite, name) { ... }
#define TEST(suite_, name_)                                                                 \
    static void test_##suite_##_##name_();                                                  \
    static const ::t::Registrar registrar_##suite_##_##name_(#suite_, #name_,               \
                                                             test_##suite_##_##name_);      \
    static void test_##suite_##_##name_()

#define CHECK(cond, ...) ::t::check_at((cond), #cond, std::format(__VA_ARGS__), __FILE__, __LINE__)
