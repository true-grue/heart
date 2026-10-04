#ifndef CORE_ARENA_H
#define CORE_ARENA_H

#include <stddef.h>

typedef enum ArenaStatus {
    ARENA_OK = 0,
    ARENA_E_NULL,
    ARENA_E_UNALIGNED,
    ARENA_E_NOMEM
} ArenaStatus;

/* Bump allocator without free.
 * The caller owns the backing memory; the arena never calls malloc itself.
 * arena_reset returns the whole arena to its initial state.
 * The last error lives in the struct, so there is no global state. */
typedef struct Arena {
    unsigned char *base;
    size_t cap;
    size_t off;
    size_t peak;
    ArenaStatus status;
} Arena;

/* Alignment used by default: large enough for any object. */
#define ARENA_DEFAULT_ALIGN ((size_t)_Alignof(max_align_t))

/* cap may only be 0 together with mem == NULL, meaning the arena is empty. */
int arena_init(Arena *a, void *mem, size_t cap);

/* align must be a power of two. Returns NULL when there is not enough room. */
void *arena_alloc(Arena *a, size_t size, size_t align);

void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align);

/* Copies len bytes into the arena. Returns NULL when there is not enough room. */
char *arena_copy(Arena *a, const void *src, size_t len);

size_t arena_used(const Arena *a);
size_t arena_peak(const Arena *a);
size_t arena_left(const Arena *a);

void arena_reset(Arena *a);

ArenaStatus arena_last_status(const Arena *a);

#endif