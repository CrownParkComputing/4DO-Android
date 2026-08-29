#include <cstdio>

#include "test_harness.h"

int main() {
    int failed_cases = 0;

    for (const testing::Case& test_case : testing::registry()) {
        const int before = testing::failure_count();
        testing::current_case() = test_case.name;
        std::printf("%s\n", test_case.name);
        test_case.fn();
        if (testing::failure_count() != before) {
            ++failed_cases;
        }
    }

    const int total = static_cast<int>(testing::registry().size());
    if (failed_cases == 0) {
        std::printf("\n%d test%s passed.\n", total, total == 1 ? "" : "s");
        return 0;
    }

    std::printf("\n%d of %d test%s FAILED (%d check%s).\n", failed_cases, total,
                total == 1 ? "" : "s", testing::failure_count(),
                testing::failure_count() == 1 ? "" : "s");
    return 1;
}
