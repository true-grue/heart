#ifndef TOOLS_TEST_H
#define TOOLS_TEST_H

#include <stddef.h>

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} test_case;

/* filter is a substring of the test name, or NULL for all of them. */
int test_run(const test_case *cases, size_t count, const char *filter);

void test_fail(const char *expr, const char *file, int line);
void test_fail_int(long got, long want, const char *expr, const char *file, int line);

#define CHECK(cond) \
    ((cond) ? (void)0 : test_fail(#cond, __FILE__, __LINE__))

#define CHECK_INT(got, want) \
    ((long)(got) == (long)(want) \
         ? (void)0 \
         : test_fail_int((long)(got), (long)(want), #got, __FILE__, __LINE__))

#define TEST_CASE(fn) { #fn, (test_fn)(fn) }

#endif