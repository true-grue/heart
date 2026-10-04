#include "test.h"

#include <stddef.h>

extern const test_case test_cases[];
extern const size_t test_case_count;

int main(int argc, char **argv) {
    const char *filter = (argc > 1) ? argv[1] : NULL;
    return test_run(test_cases, test_case_count, filter);
}