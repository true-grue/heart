#include "core/utf8.h"

size_t utf8_decode(const uint8_t *p, size_t avail, uint32_t *cp) {
    uint8_t b0;
    uint8_t lo;
    uint8_t hi;
    uint32_t acc;
    size_t need;
    size_t i;

    if (p == NULL || cp == NULL || avail == 0) {
        return 0;
    }
    b0 = p[0];

    if (b0 < 0x80u) {
        *cp = b0;
        return 1;
    }
    if (b0 < 0xC2u) {
        /* 0x80..0xBF are continuations, 0xC0/0xC1 would be overlong. */
        return 0;
    }
    if (b0 < 0xE0u) {
        need = 2;
        acc = (uint32_t)(b0 & 0x1Fu);
        lo = 0x80u;
        hi = 0xBFu;
    } else if (b0 < 0xF0u) {
        need = 3;
        acc = (uint32_t)(b0 & 0x0Fu);
        lo = (b0 == 0xE0u) ? 0xA0u : 0x80u;
        hi = (b0 == 0xEDu) ? 0x9Fu : 0xBFu; /* excludes surrogates */
    } else if (b0 < 0xF5u) {
        need = 4;
        acc = (uint32_t)(b0 & 0x07u);
        lo = (b0 == 0xF0u) ? 0x90u : 0x80u;
        hi = (b0 == 0xF4u) ? 0x8Fu : 0xBFu; /* excludes > U+10FFFF */
    } else {
        return 0;
    }

    if (avail < need) {
        return 0;
    }
    for (i = 1; i < need; i++) {
        uint8_t b = p[i];
        uint8_t l = (i == 1) ? lo : 0x80u;
        uint8_t h = (i == 1) ? hi : 0xBFu;
        if (b < l || b > h) {
            return 0;
        }
        acc = (acc << 6) | (uint32_t)(b & 0x3Fu);
    }
    *cp = acc;
    return need;
}

size_t utf8_encode(uint32_t cp, uint8_t *out) {
    if (out == NULL) {
        return 0;
    }
    if (cp < 0x80u) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        return 0; /* surrogate half */
    }
    if (cp < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp > 0x10FFFFu) {
        return 0;
    }
    out[0] = (uint8_t)(0xF0u | (cp >> 18));
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4;
}

size_t utf8_length(const uint8_t *p, size_t len) {
    size_t i = 0;
    size_t n = 0;

    while (i < len) {
        uint32_t cp;
        size_t step = utf8_decode(p + i, len - i, &cp);
        i += (step != 0) ? step : 1;
        n++;
    }
    return n;
}

size_t utf8_offset(const uint8_t *p, size_t len, size_t index) {
    size_t i = 0;
    size_t n = 0;

    while (i < len && n < index) {
        uint32_t cp;
        size_t step = utf8_decode(p + i, len - i, &cp);
        i += (step != 0) ? step : 1;
        n++;
    }
    return i;
}

int utf8_valid(const uint8_t *p, size_t len) {
    size_t i = 0;

    if (p == NULL && len != 0) {
        return 0;
    }
    while (i < len) {
        uint32_t cp;
        size_t step = utf8_decode(p + i, len - i, &cp);
        if (step == 0) {
            return 0;
        }
        i += step;
    }
    return 1;
}