#ifndef TEXT_FONT_H
#define TEXT_FONT_H

#include "arena.h"
#include "io.h"

#include <stddef.h>
#include <stdint.h>

/* A vector font: glyph outlines already flattened to line segments, so neither
 * the loader's output nor the rasteriser knows about curves. One TextFont is
 * bound to one pixel size; load another for a different size. */

typedef struct TextGlyph {
    uint32_t codepoint;
    int32_t advance;         /* font units, as in the file */
    uint32_t seg_count;
    const IoSeg *segs;       /* scaled to px_size, y already flipped */
} TextGlyph;

typedef struct TextFont {
    TextGlyph *glyphs;       /* sorted by codepoint, which the baker guarantees */
    size_t count;
    int32_t upem;
    int32_t px_size;
    IoFixed scale;          /* font units to 16.16 pixels */
    int32_t ascent;          /* pixels above the baseline */
    int32_t descent;         /* pixels below the baseline, positive */
    int32_t line_height;
} TextFont;

typedef enum TextStatus {
    TEXT_OK = 0,
    TEXT_E_SYNTAX,
    TEXT_E_RANGE,
    TEXT_E_VERSION,
    TEXT_E_NOMEM
} TextStatus;

/* Parses a baked font and scales it to px_size. Every allocation comes from the
 * arena; the source text is not retained. */
TextStatus text_font_load(Arena *a, TextFont *out, const char *text, size_t len,
                          int32_t px_size);

const TextGlyph *text_glyph(const TextFont *f, uint32_t codepoint);

/* Pen advance for one UTF-8 string. Unknown code points advance by the fallback
 * width so a stray byte cannot stall the layout. */
int32_t text_width(const TextFont *f, const char *utf8, uint32_t len);

/* Width of the first code point of utf8, for measuring a line one word at a time
 * while wrapping. Returns 0 for an empty or undecodable input. */
int32_t text_head_width(const TextFont *f, const char *utf8, uint32_t len);

/* Draws at the given pen position; y is the baseline. Returns the pen x after
 * the string, so callers can chain runs. */
int32_t text_draw(IoCtx *ctx, const TextFont *f, int32_t x, int32_t y,
                  const char *utf8, uint32_t len, IoColor color);

const char *text_status_str(TextStatus st);

#endif