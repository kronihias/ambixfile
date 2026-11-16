// SPDX-License-Identifier: MIT
// Minimal test helper.
#ifndef AMBIXFILE_TESTS_CHECK_HPP
#define AMBIXFILE_TESTS_CHECK_HPP

#include <cstdio>

inline int& failureCount() {
    static int count = 0;
    return count;
}

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            ++failureCount();                                                         \
        }                                                                             \
    } while (0)

inline int finish(const char* name) {
    if (failureCount() == 0) std::printf("%s: all checks passed\n", name);
    else std::printf("%s: %d check(s) FAILED\n", name, failureCount());
    return failureCount() == 0 ? 0 : 1;
}

#endif
