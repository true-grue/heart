#ifndef PLATFORM_TEST_H
#define PLATFORM_TEST_H

#include "io.h"

/* The backend the tests and the walkthrough run on: no window, present is a no-op,
 * no events. The whole drawing path therefore runs with no display at all.
 *
 * It lives here rather than in io.h on purpose. io.h must not name a platform, and a
 * build that does have a windowing target must not read a mention of this one. The
 * single exception is the walkthrough, which deliberately asks for a headless run on
 * whatever platform it was built for, and that is why this is a header of its own
 * instead of a symbol in the common one. */
extern const IoBackend io_backend_test;

#endif
