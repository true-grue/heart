#ifndef TOOLS_TEST_H
#define TOOLS_TEST_H

#include <stddef.h>
#include <stdint.h>

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} test_case;

/* filter is a substring of the test name, or NULL for all of them. */
int test_run(const test_case *cases, size_t count, const char *filter);

void test_fail(const char *expr, const char *file, int line);
/* intptr_t and not long: on Windows long is 32 bits and a pointer is 64, so the
 * compare truncated every pointer and the tests would not build for Windows at all.
 * LLP64 is exactly the model that catches a cast written out of habit. */
void test_fail_int(intptr_t got, intptr_t want, const char *expr, const char *file,
                   int line);

#define CHECK(cond) \
    ((cond) ? (void)0 : test_fail(#cond, __FILE__, __LINE__))

#define CHECK_INT(got, want) \
    ((intptr_t)(got) == (intptr_t)(want) \
         ? (void)0 \
         : test_fail_int((intptr_t)(got), (intptr_t)(want), #got, __FILE__, __LINE__))

#define TEST_CASE(fn) { #fn, (test_fn)(fn) }

#endif