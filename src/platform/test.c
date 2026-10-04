#include "platform/test.h"

static int test_open(void *self, const char *title, int32_t w, int32_t h) {
    (void)self;
    (void)title;
    (void)w;
    (void)h;
    return 1;
}

static void test_present(void *self, IoCtx *ctx) {
    (void)self;
    (void)ctx;
}

static void test_wait(void *self, int timeout_ms) {
    (void)self;
    (void)timeout_ms;
}

static int test_pump(void *self, IoCtx *ctx) {
    (void)self;
    (void)ctx;
    return 1;
}

static void test_close(void *self) {
    (void)self;
}

const IoBackend io_backend_test = {
    NULL,
    test_open,
    test_present,
    test_wait,
    test_pump,
    test_close
};

/* The platform entry point when the build has no windowing target. Exactly one
 * platform file may define this, so a build that does have one keeps the guard. */
#if !defined(IO_X11)
const IoBackend *io_platform_backend(void) {
    return &io_backend_test;
}
#endif
