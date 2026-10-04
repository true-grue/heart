#include "font.h"

#include "cur.h"
#include "utf8.h"

#include <string.h>

/* Line based, same shape as the script and atlas loaders: one pass to count, one
 * to fill. Glyphs arrive sorted by codepoint, so lookup is a binary search and no
 * sorting code is needed here. */

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}


/* Reads up to max integers from a line of whitespace separated numbers. */
static int read_numbers(const char *s, size_t n, int32_t *out, int max, int *got) {
    const char *end = s + n;
    int k = 0;

    while (k < max) {
        int64_t acc = 0;
        int neg = 0;
        int digits = 0;

        s = skip_ws(s, end);
        if (s < end && (*s == '-' || *s == '+')) {
            neg = (*s == '-');
            s++;
        }
        while (s < end && *s >= '0' && *s <= '9') {
            acc = acc * 10 + (*s - '0');
            if (acc > 2147483647) {
                return 0;
            }
            digits++;
            s++;
        }
        if (digits == 0) {
            break;
        }
        out[k++] = neg ? -(int32_t)acc : (int32_t)acc;
    }
    *got = k;
    return 1;
}

/* Matches a leading keyword and leaves the cursor after it. This has to stay a
 * macro: in a function the parameter decays to a pointer and sizeof yields the
 * pointer size, so the comparison would read past the literal. */
#define AFTER_KEY(s, n, key)                                                  \
    (((n) < sizeof(key) - 1 || memcmp((s), (key), sizeof(key) - 1) != 0)      \
         ? NULL                                                               \
         : (s) + sizeof(key) - 1)

static size_t count_glyphs(const char *text, size_t len) {
    Cur c;
    const char *s;
    size_t l;
    size_t n = 0;

    c.p = text;
    c.end = text + len;
    c.line = 0;
    while (cur_next_line(&c, &s, &l)) {
        if (AFTER_KEY(s, l, "glyph ") != NULL) {
            n++;
        }
    }
    return n;
}

TextStatus text_font_load(Arena *a, TextFont *out, const char *text, size_t len,
                          int32_t px_size) {
    Cur c;
    const char *s;
    size_t l;
    int32_t num[4];
    int got;
    size_t capacity;
    size_t index = 0;
    int32_t asc = 0;
    int32_t desc = 0;
    int32_t line = 0;
    IoFixed scale;

    if (a == NULL || out == NULL || text == NULL || px_size <= 0) {
        return TEXT_E_SYNTAX;
    }
    if (!utf8_valid((const uint8_t *)text, len)) {
        return TEXT_E_SYNTAX;
    }
    memset(out, 0, sizeof *out);

    c.p = text;
    c.end = text + len;
    c.line = 0;
    while (cur_next_line(&c, &s, &l)) {
        const char *after = AFTER_KEY(s, l, "font ");
        if (after != NULL) {
            if (!read_numbers(after, (size_t)(s + l - after), num, 1, &got) || got != 1) {
                return TEXT_E_SYNTAX;
            }
            if (num[0] != 1) {
                return TEXT_E_VERSION;
            }
            continue;
        }
        after = AFTER_KEY(s, l, "upem ");
        if (after != NULL) {
            if (!read_numbers(after, (size_t)(s + l - after), num, 1, &got) || got != 1 ||
                num[0] <= 0) {
                return TEXT_E_SYNTAX;
            }
            out->upem = num[0];
            continue;
        }
        after = AFTER_KEY(s, l, "asc ");
        if (after != NULL) {
            if (!read_numbers(after, (size_t)(s + l - after), num, 1, &got) || got != 1) {
                return TEXT_E_SYNTAX;
            }
            asc = num[0];
            continue;
        }
        after = AFTER_KEY(s, l, "desc ");
        if (after != NULL) {
            if (!read_numbers(after, (size_t)(s + l - after), num, 1, &got) || got != 1) {
                return TEXT_E_SYNTAX;
            }
            desc = -num[0];
            continue;
        }
        after = AFTER_KEY(s, l, "line ");
        if (after != NULL) {
            if (!read_numbers(after, (size_t)(s + l - after), num, 1, &got) || got != 1) {
                return TEXT_E_SYNTAX;
            }
            line = num[0];
            continue;
        }
    }
    if (out->upem <= 0) {
        return TEXT_E_SYNTAX;
    }

    capacity = count_glyphs(text, len);
    if (capacity == 0) {
        return TEXT_E_SYNTAX;
    }
    out->glyphs = arena_alloc_array(a, capacity, sizeof(TextGlyph), ARENA_DEFAULT_ALIGN);
    if (out->glyphs == NULL) {
        return TEXT_E_NOMEM;
    }

    /* font units to 16.16 pixels; division keeps it deterministic on negatives */
    scale = (IoFixed)(((int64_t)px_size * IO_FX_ONE) / out->upem);
    out->px_size = px_size;
    out->scale = scale;
    out->ascent = (int32_t)(((int64_t)asc * px_size) / out->upem);
    out->descent = (int32_t)(((int64_t)desc * px_size) / out->upem);
    out->line_height = (int32_t)(((int64_t)line * px_size) / out->upem);
    if (out->line_height <= 0) {
        out->line_height = px_size;
    }

    c.p = text;
    c.end = text + len;
    c.line = 0;
    while (cur_next_line(&c, &s, &l)) {
        const char *after = AFTER_KEY(s, l, "glyph ");
        IoSeg *segs;
        uint32_t seg_count;
        uint32_t codepoint;
        int32_t advance;
        uint32_t k;

        if (after == NULL) {
            continue;
        }
        if (!read_numbers(after, (size_t)(s + l - after), num, 3, &got) || got != 3) {
            return TEXT_E_SYNTAX;
        }
        if (num[0] < 0 || num[1] < 0 || num[2] < 0) {
            return TEXT_E_SYNTAX;
        }
        /* Saved before the segment loop reuses num. */
        codepoint = (uint32_t)num[0];
        advance = num[1];
        seg_count = (uint32_t)num[2];

        segs = arena_alloc_array(a, seg_count != 0 ? seg_count : 1u, sizeof(IoSeg),
                                 ARENA_DEFAULT_ALIGN);
        if (segs == NULL) {
            return TEXT_E_NOMEM;
        }
        for (k = 0; k < seg_count; k++) {
            const char *ls;
            size_t ll;
            if (!cur_next_line(&c, &ls, &ll)) {
                return TEXT_E_SYNTAX;
            }
            if (!read_numbers(ls, ll, num, 4, &got) || got != 4) {
                return TEXT_E_SYNTAX;
            }
            /* Font units have y pointing up, the screen has y down, so the
             * vertical axis is mirrored. Both windings flip together, which is
             * why holes keep cancelling without any correction. */
            segs[k].x0 = (IoFixed)((int64_t)num[0] * scale);
            segs[k].x1 = (IoFixed)((int64_t)num[2] * scale);
            segs[k].y0 = -(IoFixed)((int64_t)num[1] * scale);
            segs[k].y1 = -(IoFixed)((int64_t)num[3] * scale);
        }

        out->glyphs[index].codepoint = codepoint;
        out->glyphs[index].advance = advance;
        out->glyphs[index].seg_count = seg_count;
        out->glyphs[index].segs = segs;
        index++;
    }
    out->count = index;
    return TEXT_OK;
}

const TextGlyph *text_glyph(const TextFont *f, uint32_t codepoint) {
    size_t lo = 0;
    size_t hi;

    if (f == NULL || f->glyphs == NULL) {
        return NULL;
    }
    hi = f->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->glyphs[mid].codepoint < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < f->count && f->glyphs[lo].codepoint == codepoint) {
        return &f->glyphs[lo];
    }
    return NULL;
}

/* An absent glyph still has to move the pen, otherwise a stray byte freezes the
 * line. One em is the conventional fallback. */
static int32_t advance_of(const TextFont *f, uint32_t cp) {
    const TextGlyph *g = text_glyph(f, cp);
    if (g != NULL) {
        return (int32_t)(((int64_t)g->advance * f->px_size) / f->upem);
    }
    return f->px_size;
}

int32_t text_width(const TextFont *f, const char *utf8, uint32_t len) {
    size_t i = 0;
    int32_t total = 0;

    if (f == NULL || utf8 == NULL) {
        return 0;
    }
    while (i < len) {
        uint32_t cp;
        size_t step = utf8_decode((const uint8_t *)utf8 + i, len - i, &cp);
        if (step == 0) {
            step = 1;
            cp = 0xFFFDu;
        }
        total += advance_of(f, cp);
        i += step;
    }
    return total;
}

int32_t text_head_width(const TextFont *f, const char *utf8, uint32_t len) {
    uint32_t cp;
    size_t step;

    if (f == NULL || utf8 == NULL || len == 0) {
        return 0;
    }
    step = utf8_decode((const uint8_t *)utf8, len, &cp);
    if (step == 0) {
        return advance_of(f, 0xFFFDu);
    }
    return advance_of(f, cp);
}

int32_t text_draw(IoCtx *ctx, const TextFont *f, int32_t x, int32_t y,
                  const char *utf8, uint32_t len, IoColor color) {
    size_t i = 0;

    if (ctx == NULL || f == NULL || utf8 == NULL) {
        return x;
    }
    while (i < len) {
        uint32_t cp;
        size_t step = utf8_decode((const uint8_t *)utf8 + i, len - i, &cp);
        const TextGlyph *g;
        int32_t adv;

        if (step == 0) {
            step = 1;
            cp = 0xFFFDu;
        }
        i += step;
        g = text_glyph(f, cp);
        if (g != NULL && g->seg_count > 0) {
            io_fill_poly(ctx, g->segs, g->seg_count, io_fx(x), io_fx(y), color);
        }
        adv = advance_of(f, cp);
        x += adv;
    }
    return x;
}

const char *text_status_str(TextStatus st) {
    switch (st) {
    case TEXT_OK:        return "ok";
    case TEXT_E_SYNTAX:  return "malformed font file";
    case TEXT_E_RANGE:   return "value out of range";
    case TEXT_E_VERSION: return "unsupported font version";
    case TEXT_E_NOMEM:   return "not enough arena memory";
    default:             return "unknown error";
    }
}
