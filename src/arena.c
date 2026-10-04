#include "arena.h"

#include <stdint.h>
#include <string.h>

int arena_init(Arena *a, void *mem, size_t cap) {
    if (a == NULL) {
        return -1;
    }
    a->base = (unsigned char *)mem;
    a->cap = cap;
    a->off = 0;
    a->peak = 0;
    a->status = ARENA_OK;
    if (mem == NULL && cap != 0) {
        a->status = ARENA_E_NULL;
        return -1;
    }
    return 0;
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    uintptr_t base;
    uintptr_t addr;
    uintptr_t mask;
    uintptr_t aligned;
    size_t avail;
    size_t pad;
    unsigned char *ptr;

    if (a == NULL) {
        return NULL;
    }
    if (align == 0 || (align & (align - 1)) != 0) {
        a->status = ARENA_E_UNALIGNED;
        return NULL;
    }
    if (a->off > a->cap) {
        a->status = ARENA_E_NOMEM;
        return NULL;
    }

    avail = a->cap - a->off;
    base = (uintptr_t)(void *)a->base;
    addr = base + (uintptr_t)a->off;
    mask = (uintptr_t)align - 1u;
    aligned = (addr + mask) & ~mask;
    pad = (size_t)(aligned - addr);

    if (pad > avail || size > avail - pad) {
        a->status = ARENA_E_NOMEM;
        return NULL;
    }

    ptr = a->base + a->off + pad;
    a->off += pad + size;
    if (a->off > a->peak) {
        a->peak = a->off;
    }
    return ptr;
}

void *arena_alloc_array(Arena *a, size_t count, size_t size, size_t align) {
    if (a == NULL) {
        return NULL;
    }
    if (size != 0 && count > SIZE_MAX / size) {
        a->status = ARENA_E_NOMEM;
        return NULL;
    }
    return arena_alloc(a, count * size, align);
}

char *arena_copy(Arena *a, const void *src, size_t len) {
    void *dst;

    if (a == NULL) {
        return NULL;
    }
    if (len != 0 && src == NULL) {
        a->status = ARENA_E_NULL;
        return NULL;
    }
    dst = arena_alloc(a, len, 1);
    if (dst == NULL) {
        return NULL;
    }
    if (len != 0) {
        memcpy(dst, src, len);
    }
    return (char *)dst;
}

size_t arena_used(const Arena *a) {
    return (a == NULL) ? 0 : a->off;
}

size_t arena_peak(const Arena *a) {
    return (a == NULL) ? 0 : a->peak;
}

size_t arena_left(const Arena *a) {
    if (a == NULL) {
        return 0;
    }
    return (a->cap >= a->off) ? (a->cap - a->off) : 0;
}

void arena_reset(Arena *a) {
    if (a == NULL) {
        return;
    }
    a->off = 0;
    a->status = ARENA_OK;
}

ArenaStatus arena_last_status(const Arena *a) {
    return (a == NULL) ? ARENA_E_NULL : a->status;
}