// SPDX-License-Identifier: Apache-2.0
/* Minimal test helpers shared by the test programs. */
#ifndef HBPL_TEST_UTIL_H
#define HBPL_TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures;
static int test_checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        test_checks++;                                                       \
        if (!(cond)) {                                                       \
            test_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, \
                    #cond);                                                  \
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
        }                                                                    \
    } while (0)

static inline int test_summary(const char *name)
{
    if (test_failures) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, test_failures, test_checks);
        return 1;
    }
    printf("%s: all %d checks passed\n", name, test_checks);
    return 0;
}

/* Read a whole file; returns malloc'd bytes or NULL. */
static inline uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0)
            break;
    }
    fclose(f);
    *len = n;
    return buf;
}

/* Deterministic PRNG (xorshift32). */
static uint32_t test_rng_state = 2463534242u;
static inline uint32_t rnd(void)
{
    uint32_t x = test_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return test_rng_state = x;
}

#endif
