/* test_util.h - tiny test harness for libpuci unit tests. */
#ifndef PUCI_TEST_UTIL_H
#define PUCI_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failures++;                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                    #cond);                                            \
        }                                                              \
    } while (0)

#define TEST_END()                                        \
    do {                                                  \
        if (g_failures) {                                 \
            fprintf(stderr, "%d/%d checks failed\n",      \
                    g_failures, g_checks);                \
            return EXIT_FAILURE;                          \
        }                                                 \
        printf("ok (%d checks)\n", g_checks);             \
        return EXIT_SUCCESS;                              \
    } while (0)

#endif /* PUCI_TEST_UTIL_H */