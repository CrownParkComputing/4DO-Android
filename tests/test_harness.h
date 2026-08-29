// A deliberately tiny test harness.
//
// No external framework: this has to build and run on a bare Android or iOS
// toolchain as easily as on the host, and a dependency that only works on the
// desktop would defeat the point.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

inline const char*& current_case() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failure_count();
    std::printf("  FAIL %s:%d\n    %s\n", file, line, what.c_str());
}

}  // namespace testing

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::testing::Registrar registrar_##name(#name, name);            \
    static void name()

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            ::testing::report_failure(__FILE__, __LINE__,                 \
                                      "expected: " #condition);           \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                        \
    do {                                                                  \
        const auto actual_value = (actual);                               \
        const auto expected_value = (decltype(actual_value))(expected);   \
        if (!(actual_value == expected_value)) {                          \
            char buffer[256];                                             \
            std::snprintf(buffer, sizeof(buffer),                         \
                          "%s\n      actual:   0x%llX\n"                  \
                          "      expected: 0x%llX",                       \
                          #actual " == " #expected,                       \
                          (unsigned long long)actual_value,               \
                          (unsigned long long)expected_value);            \
            ::testing::report_failure(__FILE__, __LINE__, buffer);        \
        }                                                                 \
    } while (0)
