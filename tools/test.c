#include "test.h"

#include <stdint.h>

#include <stdio.h>
#include <string.h>

static unsigned long g_failures;

static void report(const char *file, int line, const char *expr, const char *detail) {
    g_failures++;
    printf("  FAIL %s:%d: %s", file, line, expr);
    if (detail != NULL && detail[0] != '\0') {
        printf(" [%s]", detail);
    }
    printf("\n");
}

void test_fail(const char *expr, const char *file, int line) {
    report(file, line, expr, NULL);
}

void test_fail_int(intptr_t got, intptr_t want, const char *expr, const char *file, int line) {
    char detail[96];
    snprintf(detail, sizeof detail, "got %lld, want %lld", (long long)got,
                 (long long)want);
    report(file, line, expr, detail);
}

static int name_matches(const char *name, const char *filter) {
    if (filter == NULL || filter[0] == '\0') {
        return 1;
    }
    return strstr(name, filter) != NULL;
}

int test_run(const test_case *cases, size_t count, const char *filter) {
    size_t i;
    size_t ran = 0;

    g_failures = 0;

    for (i = 0; i < count; i++) {
        if (!name_matches(cases[i].name, filter)) {
            continue;
        }
        ran++;
        cases[i].fn();
    }

    printf("%lu tests run, %lu failures\n", (unsigned long)ran, g_failures);
    return (ran > 0 && g_failures == 0) ? 0 : 1;
}