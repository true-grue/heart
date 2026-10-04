#define _POSIX_C_SOURCE 199309L

#include "arena.h"
#include "utf8.h"
#include "game.h"
#include "io.h"
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* The window is five horizontal bands, laid out here and nowhere else.
 *
 *   control   the game, and the way out
 *   name      which room this is
 *   desc      the room, as one paragraph
 *   command   the command being assembled, and the choices for the next slot
 *   answer    what the game said back, typed out
 *
 * The command is built out of slots filled from left to right. The slot being
 * filled is the empty one; its choices are offered on the row below. A sentence
 * may have more parts than the format has today, so the slot count is an upper
 * bound rather than the shape, and nothing here assumes two.
 *
 * Every band is a fixed height in virtual pixels, so the layout is the layout on
 * every platform and at every window size.
 */

#define GAME_W 640
#define GAME_H 480
#define SCALE 2

#define FONT_PATH "assets/font/sans.font"

#define MARGIN_X 16
#define PAD 10                     /* text inset inside a band */
#define BTN_H 26                   /* a button in the control band */
#define CHIP_H 44                  /* a verb or object chip */
#define CHIP_GAP 4
#define CHIP_PAD_X 6
/* Kept free at the right end of every chip row so the "and N more" chip has somewhere
 * to go. Seven verbs is the widest row in the shipped games and it fits with sixteen
 * pixels to spare, but a row one word longer must degrade honestly rather than run off
 * the canvas: a chip drawn past the edge is also a chip the pointer cannot reach. */
#define CHIP_ROW_RESERVE 56
/* Kept free at the right end of the item strip so the "and N more" chip always has
 * somewhere to go. Four glyphs is the widest it ever gets: a plus and two digits. */
#define COUNTER_RESERVE 60

/* Band geometry is decided per frame, not fixed here. The system bands grow down from
 * the top, the play bands grow up from the bottom, and the description takes whatever
 * is left between them, so no band is taller than its content and nothing is reserved
 * that goes unused. The answer is the one that gives way when a frame holds more than
 * the canvas has room for: it already scrolls its tail, so shrinking it costs least. */
#define CTRL_H 30
#define NAME_H 32
#define ITEMS_H 26                   /* the strip of carried things */
#define CMD_PAD 8
#define DESC_MIN_H 56                /* below this the description stops being read */
#define RESP_MIN_H 60
#define TILE_H 36                     /* one slot in the command line */
/* The most words one command can have, and so the most slots the palette needs.
 * Equals RULE_MAX_WORDS: a command the screen cannot show is a command the player
 * cannot type. */
#define CMD_SLOTS RULE_MAX_WORDS

#define TYPE_CPS 45                /* characters a second while the answer types */
#define PARA_MAX 4096
#define MAX_CHOICES 16
#define MAX_HITS 48
#define ROWS_MAX 32
#define LABEL_MAX 48

#define HIT_SAVE 0
#define HIT_LOAD 1
#define HIT_EXIT 2
#define HIT_WORD 3
#define HIT_CANCEL 4
#define HIT_NEW 5

#define C_BG IO_RGB(20, 22, 27)
#define C_BAND IO_RGB(29, 32, 39)
#define C_FIELD IO_RGB(20, 21, 26)
#define C_INK IO_RGB(224, 222, 216)
#define C_DIM IO_RGB(150, 154, 164)
#define C_NAME IO_RGB(238, 226, 196)
#define C_CHIP IO_RGB(48, 52, 62)
#define C_CHIP_ON IO_RGB(72, 102, 86)
#define C_QUIT IO_RGB(96, 48, 48)
#define C_RULE IO_RGB(56, 58, 68)
#define C_CARET IO_RGB(226, 200, 140)
/* What the eye is meant to catch: text that turned up because of what the player just
 * did. Warm against the cold ink, and dark enough to keep reading at length. */
#define C_HOT IO_RGB(255, 196, 108)

/* A run of the description to draw in the accent colour. Byte offsets into the text the
 * band is drawing, which is what lets a highlight survive rewrapping: the run is
 * marked in the source text and the row drawer finds it wherever it lands. */
typedef struct Span {
    uint32_t off;
    uint32_t len;
} Span;

#define FRAG_MAX 64


typedef struct GameDef {
    const char *key;
    const char *path;
    const char *title;
} GameDef;

static const GameDef k_games[] = {
    { "tutorial", "assets/script/tutorial.script", "Учебный квест" },
    { "rats", "assets/script/rats.script", "КРЫСОЛОВ" },
    { "field", "assets/script/field.script", "МЕЧ ИЗ ЗАМКА" },
    { "heart", "assets/script/heart.script", "Серое Сердце" }
};

typedef struct Hit {
    IoRect r;
    int kind;
    Sym sym;
} Hit;

/* The command under construction: the slots already chosen, left to right. The
 * format has a verb and an object today; CMD_SLOTS leaves the sentence able to
 * grow without the band learning a new shape. */
typedef struct Command {
    Sym slot[CMD_SLOTS];
    int filled;
} Command;

typedef struct Ui {
    IoCtx *ctx;
    const TextFont *font;
    const Game *game;
    const Script *script;
    const char *game_title;

    Command cmd;
    Command last;        /* the command just run, held on screen while it answers */
    int have_last;

    Hit hits[MAX_HITS];
    size_t hit_n;

    size_t last_log;      /* to notice a new answer and restart the typing */
    double typed;         /* characters revealed so far */
    /* How far the answer band has scrolled, in pixels, and where it is going. Rows
     * would jump a whole line every time the text grew past the bottom, and that jump
     * is the blink. */
    double scroll;
    double scroll_want;
    /* The description scrolls its tail too, for the same reason the answer does: a room
     * can say more than fits, and cutting it off is worse than moving it. */
    double dscroll;
    double dscroll_want;
    int animate;          /* off for the walkthrough: the frames must be comparable */
    int overfull;         /* frames whose bands could not hold their own text */
    size_t last_room;      /* to notice a new room and start its scroll over */
    /* Fragments of this room already on screen, and those that arrived with the last
     * command. Keyed on the fragment's line in the script, the only stable name one
     * has. */
    int frag_line[FRAG_MAX];
    size_t frag_line_n;
    int frag_hot[FRAG_MAX];
    size_t frag_hot_n;
    int done;             /* the answer is fully revealed */
} Ui;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1.0e6;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[(size_t)size] = '\0';
    *len = (size_t)size;
    return buf;
}

/* ------------------------------------------------------------- log read -- */

/* Index of the last block that opens with the given kind of heading. A room block
 * carries its title, a command block the verb and object. */
static size_t last_block(const Game *g, int want_room) {
    size_t i;

    for (i = g->log_count; i > 0; i--) {
        if (g->log_headed[i - 1] && ((g->log_head[i - 1] != NULL) == want_room)) {
            return i - 1;
        }
    }
    return (size_t)-1;
}

/* Where the block that starts at `from` ends: the next heading, or the log end. */
static size_t block_end(const Game *g, size_t from) {
    size_t i;

    for (i = from + 1; i < g->log_count; i++) {
        if (g->log_headed[i]) {
            return i;
        }
    }
    return g->log_count;
}

/* The lines of one block joined into a single paragraph. A room description is
 * written as several says and must not read as several paragraphs, so the lines
 * are glued with a space and wrapped as one piece of prose. */
static size_t block_text(const Game *g, size_t from, size_t to, char *out, size_t cap) {
    size_t k;
    size_t n = 0;

    for (k = from; k < to && n + 1 < cap; k++) {
        uint32_t i = 0;

        if (g->log_len[k] == 0) {
            continue;
        }
        if (n > 0 && n + 1 < cap) {
            out[n++] = ' ';
        }
        while (i < g->log_len[k] && n + 1 < cap) {
            uint32_t cp;
            size_t step = utf8_decode((const uint8_t *)g->log_text[k] + i,
                                      (size_t)(g->log_len[k] - i), &cp);
            if (step == 0) {
                step = 1;
            }
            if (step > cap - 1 - n) {
                break;
            }
            memcpy(out + n, g->log_text[k] + i, step);
            n += step;
            i += (uint32_t)step;
        }
    }
    out[n] = '\0';
    return n;
}

/* Splits off the first visual row that fits in max_w and reports where the next
 * one starts. The break goes after the last space that fits, so words stay whole;
 * a single word wider than the row overhangs rather than being cut. */
static uint32_t wrap_row(const TextFont *f, const char *t, uint32_t len,
                         int32_t max_w, uint32_t *rest) {
    uint32_t i = 0;
    uint32_t cut = 0;
    int32_t w = 0;
    int overflow = 0;

    while (i < len) {
        uint32_t cp;
        size_t step = utf8_decode((const uint8_t *)t + i, (size_t)(len - i), &cp);
        int32_t cw;

        if (step == 0) {
            cp = 0xFFFDu;
            step = 1;
        }
        cw = text_head_width(f, t + i, (uint32_t)(len - i));
        if (i > 0 && w + cw > max_w) {
            overflow = 1;
            break;
        }
        w += cw;
        i += (uint32_t)step;
        if (cp == ' ') {
            cut = i;
        }
    }
    if (overflow && cut > 0 && cut < i) {
        *rest = cut;
        while (*rest < len && t[*rest] == ' ') {
            (*rest)++;
        }
        return cut;
    }
    *rest = i;
    return i;
}

/* Largest codepoint boundary at or before `upto`, so a partially typed answer
 * never ends inside a letter. */
static size_t utf8_floor(const char *t, size_t len, size_t upto) {
    while (upto > 0 && upto < len &&
           ((unsigned char)t[upto] & 0xC0u) == 0x80u) {
        upto--;
    }
    return upto;
}

/* ------------------------------------------------------------ drawing -- */

static void band(IoCtx *c, int32_t y, int32_t h, IoColor bg) {
    io_fill_rect(c, (IoRect){ 0, y, GAME_W, h }, bg);
    io_fill_rect(c, (IoRect){ 0, y + h - 1, GAME_W, 1 }, C_RULE);
}

typedef struct Rows {
    uint32_t at[ROWS_MAX + 1];
    size_t n;
} Rows;

/* Wraps `t` and draws it from `top` downwards. Anything past the band is clipped by
 * the clip stack rather than measured away. */
static void text_top(IoCtx *c, const TextFont *f, int32_t top, int32_t width,
                     const char *t, uint32_t len, IoColor ink) {
    Rows r;
    uint32_t left = len;
    size_t k;

    r.at[0] = 0;
    r.n = 0;
    while (left > 0 && r.n < ROWS_MAX) {
        uint32_t rest = 0;
        (void)wrap_row(f, t, left, width, &rest);
        if (rest == r.at[r.n]) {
            break;
        }
        left -= rest;
        t += rest;
        r.n++;
        r.at[r.n] = r.at[r.n - 1] + rest;
    }
    for (k = 0; k < r.n; k++) {
        text_draw(c, f, PAD, top + f->ascent + (int32_t)k * f->line_height,
                  t - len + r.at[k], r.at[k + 1] - r.at[k], ink);
    }
}

/* The last `fit` rows, laid out from the top of the band downwards. When there is
 * more text than fits, the tail is what matters: the newest sentence is the one
 * being read, and a band that shows the first rows instead hides the end of the
 * answer under the fold.
 *
 * Reports where the text ended, so a caret can sit immediately after the last
 * letter. Working that out here rather than in the caller keeps the two from
 * drifting apart: a caret placed by its own arithmetic ends up on some other row. */
static double text_tail(IoCtx *c, const TextFont *f, int32_t top, int fit, int32_t width,
                        const char *full, uint32_t len, uint32_t vis, IoColor ink,
                        double off, const Span *hl, size_t hl_n, IoColor hot,
                        int32_t *end_x, int32_t *end_y) {
    const char *base = full;
    Rows r;
    uint32_t left = len;
    size_t k;

    /* The rows are cut from the WHOLE paragraph, not from what has been revealed.
     * Laid out on the visible prefix, a word that does not fit is drawn for a while on
     * the line it started on and then jumps whole to the next one, already half typed,
     * so it reads as the word being typed twice. Deciding every break before the first
     * character is drawn puts each word on the line it will stay on, and the text then
     * appears where it belongs instead of moving under the caret. */
    r.at[0] = 0;
    r.n = 0;
    while (left > 0 && r.n < ROWS_MAX) {
        uint32_t rest = 0;
        (void)wrap_row(f, full, left, width, &rest);
        /* Only "no progress" ends the split. Comparing the row's byte count against
         * r.at[r.n], an absolute offset that grows with every row, looks like a guard
         * and is not one: it fires the moment two neighbouring rows happen to be the
         * same length in bytes, and the rest of the paragraph is silently dropped.
         * That is data dependent, which is why it read as a broken room rather than a
         * broken engine. */
        if (rest == 0) {
            break;
        }
        left -= rest;
        full += rest;
        r.n++;
        r.at[r.n] = r.at[r.n - 1] + rest;
    }
    {
        /* The whole block slides by a pixel offset instead of dropping whole rows, and
         * the caller eases toward the offset returned here. Skip is a floor so the row
         * crossing the top edge is drawn half out rather than not at all. */
        double want = (r.n > (size_t)fit)
                    ? (double)(r.n - (size_t)fit) * f->line_height : 0.0;
        size_t skip;
        int32_t shift;

        if (off < 0.0) {
            off = 0.0;
        }
        if (off > want) {
            off = want;
        }
        skip = (size_t)(off / (double)f->line_height);
        shift = (int32_t)off - (int32_t)skip * f->line_height;
        *end_x = PAD;
        *end_y = top + f->ascent - shift;
        for (k = skip; k < r.n; k++) {
            uint32_t from = r.at[k];
            uint32_t to = r.at[k + 1];
            int32_t y = top + f->ascent - shift + (int32_t)(k - skip) * f->line_height;

            if (from >= vis) {
                break;
            }
            if (to > vis) {
                to = vis;
            }
            /* Drawn in pieces so a highlighted run can change colour without the row
             * being laid out twice. Splits fall on fragment boundaries, which are
             * whole UTF-8 sequences, so no piece ever starts mid-character. */
            {
                uint32_t p = from;

                while (p < to) {
                    uint32_t seg = to;
                    IoColor col = ink;
                    size_t q;

                    for (q = 0; q < hl_n; q++) {
                        uint32_t a = hl[q].off;
                        uint32_t b = a + hl[q].len;

                        if (p >= a && p < b) {
                            col = hot;
                            if (b < seg) {
                                seg = b;
                            }
                            break;
                        }
                        if (p < a && a < seg) {
                            seg = a;
                        }
                    }
                    if (seg <= p) {
                        seg = p + 1;
                    }
                    if (seg > to) {
                        seg = to;
                    }
                    /* Each piece starts where the ones before it end. Drawing them all
                     * at the left margin piles the row into itself, which looks like
                     * garbled text rather than a coloured run. */
                    text_draw(c, f, PAD + text_width(f, base + from, p - from), y,
                              base + p, seg - p, col);
                    p = seg;
                }
            }
            if (to == vis) {
                /* The caret belongs after the last character that arrived, which is the
                 * end of whatever row the reveal currently stops in. */
                *end_x = PAD + text_width(f, base + from, to - from);
                *end_y = y;
            }
        }
        return want;
    }
}

static void add_hit(Ui *ui, IoRect r, int kind, Sym sym) {
    Hit *h;

    if (ui->hit_n >= MAX_HITS) {
        return;
    }
    h = &ui->hits[ui->hit_n++];
    h->r = r;
    h->kind = kind;
    h->sym = sym;
}

/* text_draw takes a baseline, not the top of the line. Passing a top coordinate is
 * the easy mistake and it draws the text off the top of the band, so every caller
 * goes through here instead. */
static void text_at(IoCtx *c, const TextFont *f, int32_t x, int32_t top,
                    const char *t, uint32_t len, IoColor ink) {
    text_draw(c, f, x, top + f->ascent, t, len, ink);
}

/* Identifiers use underscores where a name has two words. The chip is the only
 * place that turns them back into spaces: that is a display concern. */
/* The same for a bare script, so the auditor measures labels without inventing a Ui
 * just to borrow this. One implementation, two callers. */
static uint32_t chip_label_sym(const Script *s, Sym sym, char *out, size_t cap) {
    size_t len;
    const char *name = script_sym(s, sym, &len);
    size_t i;
    size_t n = 0;

    for (i = 0; i < len && n + 1 < cap; i++) {
        out[n++] = (name[i] == '_') ? ' ' : name[i];
    }
    out[n] = '\0';
    return (uint32_t)n;
}

static int32_t rows_of(const TextFont *f, const char *t, uint32_t len, int32_t width);

static uint32_t chip_label(const Ui *ui, Sym sym, char *out, size_t cap) {
    return chip_label_sym(ui->script, sym, out, cap);
}

static int32_t chip_w(const TextFont *f, const char *label, uint32_t len) {
    return text_width(f, label, len) + 2 * CHIP_PAD_X;
}

/* One label, one length, taken from the literal itself. Measuring and drawing with
 * two separately typed numbers is how a button ends up reading "Сохр". */
/* Draws one row of word chips and returns how many fitted. Anything past the edge is
 * replaced by a "+N" chip rather than being drawn where the player cannot click it.
 * Room for that chip is reserved from the first chip on, otherwise the row fills the
 * width completely and there is nowhere left to admit the loss. */
static int32_t draw_button(IoCtx *c, Ui *ui, const TextFont *f, int32_t right,
                           int32_t top, int32_t h, const char *label, size_t cap,
                           IoColor bg, int kind) {
    uint32_t len = (uint32_t)(cap - 1);
    int32_t w = chip_w(f, label, len);
    int32_t x = right - w;
    IoRect r = { x, top, w, h };

    io_fill_rect(c, r, bg);
    add_hit(ui, r, kind, 0);
    text_at(c, f, x + CHIP_PAD_X, top + (h - f->line_height) / 2, label, len, C_INK);
    return x - CHIP_GAP;
}

static int32_t draw_tile(IoCtx *c, const TextFont *f, int32_t x, int32_t y,
                         const char *label, uint32_t len, int filled) {
    int32_t w = text_width(f, label, len) + 2 * CHIP_PAD_X;
    IoRect r = { x, y, w, TILE_H };

    io_fill_rect(c, r, filled ? C_CHIP_ON : C_FIELD);
    io_fill_rect(c, (IoRect){ r.x, r.y, r.w, 1 }, filled ? C_CHIP_ON : C_RULE);
    io_fill_rect(c, (IoRect){ r.x, r.y + r.h - 1, r.w, 1 }, C_RULE);
    if (len > 0) {
        text_at(c, f, r.x + CHIP_PAD_X, r.y + (TILE_H - f->line_height) / 2,
                label, len, filled ? C_INK : C_DIM);
    }
    return x + w + CHIP_GAP;
}

/* How many rows of chips these words need at this width, measured with the same
 * arithmetic the drawing uses so the band is never sized for one arrangement and drawn
 * for another. The reserve at the right end is what leaves room for a "+N". */
static int32_t chip_rows_needed(const TextFont *f, Ui *ui, const Sym *syms, size_t n,
                                const char *trailing) {
    int32_t usable = GAME_W - MARGIN_X - CHIP_ROW_RESERVE;
    int32_t x = 0;
    int32_t rows = 1;
    char label[LABEL_MAX];
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t len = chip_label(ui, syms[i], label, sizeof label);
        int32_t w = chip_w(f, label, len);

        if (x > 0 && x + CHIP_GAP + w > usable) {
            rows++;
            x = 0;
        }
        x += w + CHIP_GAP;
    }
    if (trailing != NULL) {
        int32_t w = chip_w(f, trailing, (uint32_t)strlen(trailing));

        if (x > 0 && x + CHIP_GAP + w > usable) {
            rows++;
        }
    }
    return rows;
}

/* Draws the words and the trailing chip as one flow that wraps, rather than as two
 * things each remembering a position. That is what stops "назад" landing on the first
 * word: it is placed after the last word drawn, on whichever row that turns out to be. */
static int32_t draw_chip_rows(IoCtx *c, Ui *ui, const TextFont *f, int32_t y,
                              const Sym *syms, size_t n, int kind,
                              const char *trailing) {
    int32_t usable = GAME_W - MARGIN_X - CHIP_ROW_RESERVE;
    int32_t x = MARGIN_X;
    int32_t rows = 1;
    char label[LABEL_MAX];
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t len = chip_label(ui, syms[i], label, sizeof label);
        int32_t w = chip_w(f, label, len);

        if (x > 0 && x + CHIP_GAP + w > usable) {
            rows++;
            x = MARGIN_X;
            y += CHIP_H + CHIP_GAP;
        }
        {
            IoRect r = { x, y, w, TILE_H };

            io_fill_rect(c, r, C_CHIP);
            io_fill_rect(c, (IoRect){ r.x, r.y, r.w, 1 }, C_RULE);
            text_at(c, f, r.x + CHIP_PAD_X, r.y + (TILE_H - f->line_height) / 2, label,
                    len, C_INK);
            add_hit(ui, r, kind, syms[i]);
        }
        x += w + CHIP_GAP;
    }
    if (trailing != NULL) {
        uint32_t len = (uint32_t)strlen(trailing);
        int32_t w = chip_w(f, trailing, len);

        if (x > 0 && x + CHIP_GAP + w > usable) {
            rows++;
            x = MARGIN_X;
            y += CHIP_H + CHIP_GAP;
        }
        {
            IoRect r = { x, y, w, TILE_H };

            io_fill_rect(c, r, C_CHIP);
            text_at(c, f, r.x + CHIP_PAD_X, r.y + (TILE_H - f->line_height) / 2, trailing,
                    len, C_DIM);
            add_hit(ui, r, HIT_CANCEL, 0);
        }
    }
    return rows;
}

/* The words on offer now. One list, read by both the measurement and the drawing.
 * A word nothing can follow is a dead end rather than an action, so it is filtered
 * here: the source lists every verb the room mentions, and this band is the place that
 * promises the player a choice is possible. */
static size_t gather_pick(Ui *ui, Sym *choices, size_t cap) {
    Sym all[MAX_CHOICES];
    Sym tail[RULE_MAX_WORDS + 1];
    size_t got;
    size_t n = 0;
    size_t i;

    if (ui->cmd.filled >= CMD_SLOTS) {
        return 0;
    }
    got = game_next(ui->game, ui->cmd.slot, (size_t)ui->cmd.filled, all, MAX_CHOICES);
    for (i = 0; i < got && n < cap; i++) {
        size_t len = (size_t)ui->cmd.filled + 1;

        /* The probe needs the whole command so far, not just the word being offered,
         * or the lookup walks off the front of the array. */
        memcpy(tail, ui->cmd.slot, (size_t)ui->cmd.filled * sizeof tail[0]);
        tail[ui->cmd.filled] = all[i];
        if (game_more(ui->game, tail, len) > 0 || game_available(ui->game, tail, len)) {
            choices[n++] = all[i];
        }
    }
    return n;
}

/* Works out which fragments the last command brought in, so the description can point
 * at them. Called once per action, not per frame: the highlight is meant to sit there
 * until the player does something else. */
static void mark_new_fragments(Ui *ui, const Game *g) {
    char buf[PARA_MAX];
    FragSpan sp[FRAG_MAX];
    size_t i;
    size_t j;

    ui->frag_hot_n = 0;
    if (game_room_text_spans(g, buf, sizeof buf, sp, FRAG_MAX) == 0) {
        return;
    }
    for (i = 0; i < FRAG_MAX; i++) {
        if (sp[i].len == 0 && sp[i].off == 0 && i > 0) {
            break;
        }
        for (j = 0; j < ui->frag_line_n; j++) {
            if (ui->frag_line[j] == sp[i].line) {
                break;
            }
        }
        if (j == ui->frag_line_n && ui->frag_line_n < FRAG_MAX) {
            /* Not seen before in this room, so it turned up because of the action. */
            ui->frag_line[ui->frag_line_n++] = sp[i].line;
            if (ui->frag_hot_n < FRAG_MAX) {
                ui->frag_hot[ui->frag_hot_n++] = sp[i].line;
            }
        }
    }
}

/* Entering a room is not news about that room, so nothing in it lights up. That means
 * the room's own fragments have to be recorded as already shown, and they are: forget
 * the marks without doing this and the first action taken inside the room finds every
 * line of the description new, and the whole room turns colour. */
static void note_room_fragments(Ui *ui, const Game *g) {
    char buf[PARA_MAX];
    FragSpan sp[FRAG_MAX];
    size_t i;

    ui->frag_line_n = 0;
    ui->frag_hot_n = 0;
    if (game_room_text_spans(g, buf, sizeof buf, sp, FRAG_MAX) == 0) {
        return;
    }
    for (i = 0; i < FRAG_MAX && sp[i].len > 0; i++) {
        if (ui->frag_line_n < FRAG_MAX) {
            ui->frag_line[ui->frag_line_n++] = sp[i].line;
        }
    }
}

/* Band geometry for this frame.
 *
 * The system bands grow down from the top, the play bands grow up from the bottom, and
 * the description takes what is left in between: no empty space, and no band taller than
 * its content. The answer is the one that gives way, because it already scrolls. */
typedef struct Layout {
    int32_t ctrl_y, ctrl_h;
    int32_t name_y, name_h;
    int32_t desc_y, desc_h;
    int32_t cmd_y, cmd_h, tile_y, pick_y;
    int32_t resp_y, resp_h;
    int32_t items_y, items_h;
    int32_t chip_rows;
    int overfull;                  /* the frame held more than 480 pixels */
} Layout;

static Layout compute_layout(Ui *ui, const char *desc, uint32_t desc_len,
                             const char *ans, uint32_t ans_n) {
    const TextFont *f = ui->font;
    Layout L;
    Sym choices[MAX_CHOICES];
    const char *trailing = NULL;
    size_t n = 0;
    uint32_t desc_n;
    int32_t yb;
    int32_t rows;

    L.ctrl_y = 0;
    L.ctrl_h = CTRL_H;
    L.name_y = CTRL_H;
    L.name_h = NAME_H;
    L.desc_y = CTRL_H + NAME_H;

    L.overfull = 0;
    L.items_h = ITEMS_H;
    L.items_y = GAME_H - L.items_h;
    yb = L.items_y;

    /* While the answer is still being written, the band keeps the command that caused it
     * and offers nothing, so it needs no row of choices yet. */
    if (!ui->done && ui->have_last) {
        L.chip_rows = 0;
    } else {
        n = gather_pick(ui, choices, MAX_CHOICES);
        if (ui->cmd.filled > 0) {
            trailing = "назад";
        }
        L.chip_rows = chip_rows_needed(f, ui, choices, n, trailing);
    }
    L.cmd_h = CMD_PAD + TILE_H + CMD_PAD + L.chip_rows * CHIP_H +
              (L.chip_rows > 0 ? (L.chip_rows - 1) * CHIP_GAP : 0) + CMD_PAD;
    if (L.chip_rows == 0) {
        L.cmd_h = CMD_PAD + TILE_H + CMD_PAD;
    }
    L.cmd_y = yb - L.cmd_h;
    yb = L.cmd_y;
    /* Absolute, not offsets inside the band: the draw calls take canvas coordinates. */
    L.tile_y = L.cmd_y + CMD_PAD;
    L.pick_y = L.cmd_y + CMD_PAD + TILE_H + CMD_PAD;

    /* The description is exactly its own text. It used to take the remainder, which
     * sounds generous and is not: when the answer and the command band together wanted
     * more than the canvas had, the remainder went negative and the description was
     * quietly clipped, with only the first lines showing. A band that must not lose
     * text gets the height of that text. */
    (void)desc_len;
    desc_n = (desc != NULL && desc[0] != '\0')
                 ? rows_of(f, desc, (uint32_t)strlen(desc), GAME_W - 2 * MARGIN_X)
                 : 0;
    {
        int32_t want = desc_n * f->line_height + 2 * PAD;
        int32_t avail = yb - L.desc_y;

        if (want > avail - RESP_MIN_H) {
            want = avail - RESP_MIN_H;
        }
        if (want < f->line_height + 2 * PAD) {
            want = f->line_height + 2 * PAD;
        }
        L.desc_h = want;
    }

    /* The answer takes what is left, and it is the one that gives: it already scrolls
     * its tail, so a short band costs the reader a scroll and a clipped description
     * costs them the room. */
    rows = (ans_n > 0) ? rows_of(f, ans, ans_n, GAME_W - 2 * MARGIN_X) : 1;
    L.resp_h = yb - L.desc_y - L.desc_h;
    (void)rows;
    if (L.resp_h < rows * f->line_height + 2 * PAD) {
        L.resp_h = rows * f->line_height + 2 * PAD;
    }
    if (L.resp_h < RESP_MIN_H) {
        L.resp_h = RESP_MIN_H;
    }
    L.resp_y = L.cmd_y - L.resp_h;
    if (L.resp_y < L.desc_y + L.desc_h) {
        /* Genuinely over-full. Reported rather than hidden, so the walkthrough can name
         * the room instead of the picture just looking wrong. */
        L.overfull = 1;
        L.resp_h = L.cmd_y - L.desc_y - L.desc_h;
        if (L.resp_h < 0) {
            L.resp_h = 0;
        }
        L.resp_y = L.cmd_y - L.resp_h;
    }
    return L;
}

static Layout layout_commands(Ui *ui, const char *desc, uint32_t desc_len,
                             const char *ans, uint32_t ans_n) {
    IoCtx *c = ui->ctx;
    const TextFont *f = ui->font;
    Sym choices[MAX_CHOICES];
    int32_t x = MARGIN_X;
    size_t n = 0;
    size_t i;
    char label[LABEL_MAX];
    Layout L = compute_layout(ui, desc, desc_len, ans, ans_n);

    if (!ui->done && ui->have_last) {
        /* While the answer is still being written, the command that caused it stays in
         * full. Clearing the row the moment the command runs throws away the object,
         * which is the part the player is least likely to remember. */
        for (i = 0; i < (size_t)ui->last.filled; i++) {
            uint32_t len = chip_label(ui, ui->last.slot[i], label, sizeof label);

            x = draw_tile(c, f, x, L.tile_y, label, len, 1);
        }
        return L;
    }
    for (i = 0; i < (size_t)ui->cmd.filled; i++) {
        uint32_t len = chip_label(ui, ui->cmd.slot[i], label, sizeof label);

        x = draw_tile(c, f, x, L.tile_y, label, len, 1);
    }
    {
        static const char pick[] = "?";

        x = draw_tile(c, f, x, L.tile_y, pick, (uint32_t)(sizeof pick - 1), 0);
    }
    (void)x;

    n = gather_pick(ui, choices, MAX_CHOICES);
    if (n == 0) {
        static const char none[] = "здесь нечего делать";

        text_at(c, f, MARGIN_X, L.pick_y + (TILE_H - f->line_height) / 2, none,
                (uint32_t)(sizeof none - 1), C_DIM);
    }
    (void)draw_chip_rows(c, ui, f, L.pick_y, choices, n, HIT_WORD,
                         (ui->cmd.filled > 0) ? "назад" : NULL);
    return L;
}

/* ------------------------------------------------------------ saves -- */

/* A save is the room and the flags, and nothing else. The log is display: it is
 * rebuilt by entering the room again, which is both smaller than writing it out
 * and more correct, because the fragments that print are chosen by the flags that
 * were just restored. One file per game, in the working directory. */

static void save_file(const GameDef *def, char *out, size_t cap) {
    snprintf(out, cap, "%s.save", def->key);
}

static void do_save(const Game *g, const GameDef *def) {
    char path[64];
    FILE *f;
    size_t i;

    save_file(def, path, sizeof path);
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "не удалось записать %s\n", path);
        return;
    }
    {
        size_t len;
        const char *room = script_sym(g->script, g->room, &len);
        fprintf(f, "комната %.*s\n", (int)len, room);
    }
    for (i = 0; i < g->flag_count; i++) {
        size_t len;
        const char *name = script_sym(g->script, g->flag_name[i], &len);
        fprintf(f, "флаг %.*s %u\n", (int)len, name,
                (unsigned)g->flag_present[i]);
    }
    fclose(f);
    printf("сохранено: %s\n", path);
}

/* Splits a line into up to three words. The format is machine written, so a line
 * that does not fit is a corrupt file and is skipped rather than guessed at. */
static int split_words(char *line, char *w[], size_t cap[], int max) {
    int n = 0;

    while (n < max) {
        size_t k = 0;
        while (line[0] == ' ') {
            line++;
        }
        if (line[0] == '\0' || line[0] == '\n') {
            break;
        }
        while (line[0] != '\0' && line[0] != ' ' && line[0] != '\n') {
            if (k + 1 >= cap[n]) {
                return -1;
            }
            w[n][k++] = *line++;
        }
        w[n][k] = '\0';
        n++;
    }
    return n;
}

static int do_load(Ui *ui, Game *g, const GameDef *def) {
    char path[64];
    char line[256];
    FILE *f;
    Sym room = SYM_NONE;

    save_file(def, path, sizeof path);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "нет сохранения: %s\n", path);
        return 0;
    }
    while (fgets(line, (int)sizeof line, f) != NULL) {
        char a[32];
        char b[160];
        char d[8];
        char *w[3] = { a, b, d };
        size_t cap[3] = { sizeof a, sizeof b, sizeof d };
        int n = split_words(line, w, cap, 3);

        if (n <= 0) {
            continue;
        }
        if (n == 2 && strcmp(a, "комната") == 0) {
            room = script_sym_lookup(g->script, b, strlen(b));
        } else if (n == 3 && strcmp(a, "флаг") == 0) {
            /* The name is the second word and the value the third. */
            uint8_t on = (uint8_t)(d[0] == '1');
            size_t blen = strlen(b);
            size_t i;

            for (i = 0; i < g->flag_count; i++) {
                size_t len;
                const char *name = script_sym(g->script, g->flag_name[i], &len);
                if (len == blen && memcmp(name, b, len) == 0) {
                    g->flag_present[i] = on;
                }
            }
        }
    }
    fclose(f);
    if (room == SYM_NONE) {
        fprintf(stderr, "в сохранении нет комнаты: %s\n", path);
        return 0;
    }
    /* Clear the log first so the room is the only thing on screen, then enter it:
     * that is what prints the title and whichever fragments the restored flags
     * call for. */
    g->log_count = 0;
    g->finished = 0;
    g->won = 0;
    game_enter(g, room);
    ui->cmd.filled = 0;
    ui->last_log = g->log_count;
    ui->typed = 0.0;
    ui->done = 1;
    ui->have_last = 0;
    printf("загружено: %s\n", path);
    return 1;
}

static void draw(Ui *ui) {
    IoCtx *c = ui->ctx;
    const TextFont *f = ui->font;
    const Game *g = ui->game;
    char para[PARA_MAX];
    char dpara[PARA_MAX];
    char dpara2[PARA_MAX];
    size_t room = last_block(g, 1);
    size_t cmd = last_block(g, 0);
    size_t answer_n;
    size_t desc_n;
    size_t i;
    Layout L;

    ui->hit_n = 0;

    io_fill_rect(c, (IoRect){ 0, 0, GAME_W, GAME_H }, C_BG);

    /* Bands first, contents after. The geometry is decided from the text this frame is
     * about to show, and every background goes down before anything is drawn into it. */
    answer_n = (cmd != (size_t)-1)
                   ? block_text(g, cmd + 1, block_end(g, cmd), para, sizeof para)
                   : 0;
    desc_n = game_room_text(g, dpara, sizeof dpara);
    L = compute_layout(ui, dpara, (uint32_t)desc_n, para, (uint32_t)answer_n);

    band(c, L.ctrl_y, L.ctrl_h, C_BAND);
    band(c, L.name_y, L.name_h, C_BAND);
    band(c, L.desc_y, L.desc_h, C_FIELD);
    band(c, L.cmd_y, L.cmd_h, C_BAND);
    band(c, L.items_y, L.items_h, C_BAND);
    band(c, L.resp_y, L.resp_h, C_FIELD);

    /* control band: the game on the left, the way out on the right */
    text_at(c, f, MARGIN_X, L.ctrl_y + (L.ctrl_h - f->line_height) / 2, ui->game_title,
            (uint32_t)strlen(ui->game_title), C_DIM);
    {
        static const char new_label[] = "Новая игра";
        static const char save_label[] = "Сохранить";
        static const char load_label[] = "Загрузить";
        static const char exit_label[] = "Выход";
        int32_t right = GAME_W - MARGIN_X;

        /* Laid out right to left, so the way out stays in the corner a hand goes
         * to without looking. */
        right = draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, exit_label,
                            sizeof exit_label, C_QUIT, HIT_EXIT);
        right = draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, load_label,
                            sizeof load_label, C_CHIP, HIT_LOAD);
        right = draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, save_label,
                            sizeof save_label, C_CHIP, HIT_SAVE);
        (void)draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, new_label,
                          sizeof new_label, C_CHIP, HIT_NEW);
    }

    /* name band */
    if (room != (size_t)-1) {
        text_at(c, f, MARGIN_X, L.name_y + (L.name_h - f->line_height) / 2, g->log_head[room],
                g->log_len[room], C_NAME);
    }

    /* Description band: one paragraph, no break where the script has one. It is built
     * from the room's fragments against the flags as they are now, not read out of the
     * log, so a fragment that has appeared or vanished after a command is on screen at
     * once. Reading the log left the player looking at the room as it was on entry. */
    io_push_clip(c, (IoRect){ 0, L.desc_y, GAME_W, L.desc_h });
    if (desc_n > 0) {
        Span hl[FRAG_MAX];
        FragSpan sp[FRAG_MAX];
        size_t hl_n = 0;
        size_t si;
        size_t k;

        /* Which fragments arrived with the last command, as offsets into the text this
         * frame draws. Rebuilt per frame, because the offsets move whenever the
         * description is assembled differently. */
        if (game_room_text_spans(g, dpara2, sizeof dpara2, sp, FRAG_MAX) == desc_n) {
            for (si = 0; si < FRAG_MAX && sp[si].len > 0; si++) {
                for (k = 0; k < ui->frag_hot_n; k++) {
                    if (ui->frag_hot[k] == sp[si].line) {
                        hl[hl_n].off = sp[si].off;
                        hl[hl_n].len = sp[si].len;
                        hl_n++;
                        break;
                    }
                }
            }
        }
        int fit = (L.desc_h - 2 * PAD) / f->line_height;
        int32_t ex = MARGIN_X;
        int32_t ey = L.desc_y + PAD;

        if (fit < 1) {
            fit = 1;
        }
        ui->dscroll_want = text_tail(c, f, L.desc_y + PAD, fit, GAME_W - 2 * MARGIN_X,
                                     dpara, (uint32_t)desc_n, (uint32_t)desc_n, C_INK,
                                     ui->dscroll, hl, hl_n, C_HOT, &ex, &ey);
        if (!ui->animate) {
            /* The walkthrough must land on the frame the player would have seen. */
            ui->dscroll = ui->dscroll_want;
        }
    }
    io_pop_clip(c);

    if (L.overfull) {
        ui->overfull++;
    }
    /* A new room means a new description, and the scroll starts at its head again.
     * Keyed on the room rather than on the click, so loading and starting a new game
     * reset it too. */
    if (room != ui->last_room) {
        ui->last_room = room;
        /* A room just walked into is not news about itself, so nothing in it lights up;
         * what lights up is what turns up later, while the player stays here. */
        note_room_fragments(ui, g);
        ui->dscroll = 0.0;
        ui->dscroll_want = 0.0;
    }

    /* command band */
    (void)layout_commands(ui, dpara, (uint32_t)desc_n, para, (uint32_t)answer_n);

    /* The strip of what the player is carrying. A flag whose name starts with an
     * underscore is state rather than a thing, and the underscore is the only marker
     * the format has, so it is the whole test. */
    {
        int32_t x = MARGIN_X;
        char label[64];
        size_t shown = 0;
        size_t total = 0;

        /* Counted first, so the strip knows whether it is going to have to say so. */
        for (i = 0; i < game_flag_count(g); i++) {
            size_t l2 = 0;

            if (!game_flag_on(g, i)) {
                continue;
            }
            if (game_flag_name(g, i, &l2)[0] == '_') {
                continue;
            }
            total++;
        }
        for (i = 0; i < game_flag_count(g); i++) {
            uint32_t len;
            const char *name;

            if (!game_flag_on(g, i)) {
                continue;
            }
            name = game_flag_name(g, i, (size_t *)&len);
            if (len == 0 || name[0] == '_') {
                continue;
            }
            {
                uint32_t k;
                uint32_t n = (len < sizeof label - 1) ? len : (uint32_t)sizeof label - 1;

                for (k = 0; k < n; k++) {
                    label[k] = (name[k] == '_') ? ' ' : name[k];
                }
                len = n;
            }
            {
                /* Room is kept back for the "and N more" chip from the start. Without
                 * that reserve the leftovers simply vanish: fifteen carried things fit
                 * six across, and a strip that quietly drops nine of them is worse than
                 * one that admits it cannot show them. */
                int32_t need = (int32_t)text_width(f, label, len) + 2 * CHIP_PAD_X;

                if (x + need + COUNTER_RESERVE > GAME_W - MARGIN_X) {
                    break;
                }
            }
            io_fill_rect(c, (IoRect){ x, L.items_y + 3, (int32_t)text_width(f, label, len) +
                                     2 * CHIP_PAD_X, L.items_h - 6 }, C_CHIP);
            text_at(c, f, x + CHIP_PAD_X, L.items_y + 3 + (L.items_h - 6 - f->line_height) / 2,
                    label, len, C_DIM);
            x += (int32_t)text_width(f, label, len) + 2 * CHIP_PAD_X + CHIP_GAP;
            shown++;
        }
        if (shown < total) {
            char more[16];
            uint32_t n = 0;
            size_t k;
            static const char plus[] = "+";

            for (k = 0; k < sizeof plus - 1 && n < sizeof more - 3; k++) {
                more[n++] = plus[k];
            }
            /* Two digits is enough for any list that fits on a screen at all. */
            if (total - shown >= 10) {
                more[n++] = (char)('0' + (total - shown) / 10);
            }
            more[n++] = (char)('0' + (total - shown) % 10);
            {
                int32_t cw = (int32_t)text_width(f, more, n) + 2 * CHIP_PAD_X;
                int32_t cx = GAME_W - MARGIN_X - cw;

                io_fill_rect(c, (IoRect){ cx, L.items_y + 3, cw, L.items_h - 6 }, C_CHIP);
                text_at(c, f, cx + CHIP_PAD_X,
                        L.items_y + 3 + (L.items_h - 6 - f->line_height) / 2, more, n, C_DIM);
            }
        }
        if (total == 0) {
            /* Nothing carried. Not "shown == 0", because a list too long for the
             * strip shows zero chips and a counter, and that is not emptiness. */
            static const char empty[] = "пусто";
            text_at(c, f, x, L.items_y + (L.items_h - f->line_height) / 2, empty,
                    (uint32_t)(sizeof empty - 1), C_DIM);
        }
    }

    /* answer band, typed out */
    io_push_clip(c, (IoRect){ 0, L.resp_y, GAME_W, L.resp_h });
    if (cmd != (size_t)-1) {
        size_t n = answer_n;
        size_t vis = (ui->typed >= (double)n) ? n : utf8_floor(para, n, (size_t)ui->typed);

        int fit = (L.resp_h - 2 * PAD) / f->line_height;
        int32_t end_x = MARGIN_X;
        int32_t end_y = L.resp_y + PAD;
        int32_t top = L.resp_y + PAD;

        /* Bottom anchored: the answer belongs next to the commands it produced, and the
         * slack reads as a gap under the description rather than a hole above them. */
        {
            int32_t rows = rows_of(f, para, (uint32_t)n, GAME_W - 2 * MARGIN_X);

            if (rows < fit) {
                top = L.resp_y + L.resp_h - PAD - rows * f->line_height;
                if (top < L.resp_y + PAD) {
                    top = L.resp_y + PAD;
                }
                fit = rows;
            }
        }

        ui->scroll_want = text_tail(c, f, top, fit, GAME_W - 2 * MARGIN_X, para,
                                    (uint32_t)n, (uint32_t)vis, C_INK, ui->scroll, NULL, 0,
                                    C_INK, &end_x, &end_y);
        if (!ui->animate) {
            /* The walkthrough has to land on the tail of every answer, or the frame it
             * writes is not the frame the player would have seen. */
            ui->scroll = ui->scroll_want;
        }
        if (vis < n) {
            /* The caret goes directly after the last letter that arrived, which is
             * what makes the text look like it is being written rather than
             * revealed. It moves with the text instead of sitting at a fixed spot. */
            io_fill_rect(c, (IoRect){ end_x + 2, end_y - f->ascent + 3, 7,
                                      f->ascent - 3 }, C_CARET);
        }
    } else {
        /* Nothing about a verb and an object: a command is however many words the script
         * gave it, from one to four, and the hint that named two slots was wrong for
         * every command that is not two words long. */
        static const char hint[] = "Выберите действие.";
        text_top(c, f, L.resp_y + PAD, GAME_W - 2 * MARGIN_X, hint,
                 (uint32_t)(sizeof hint - 1), C_DIM);
    }
    io_pop_clip(c);
}

/* ------------------------------------------------------- walkthrough -- */

/* A walkthrough is a path through the state graph: from where the player starts,
 * try every command the current state allows and keep the ones that lead somewhere
 * new. The graph is over states and not over rooms, because a room reached with the
 * key in hand is a different place from the same room reached without it.
 *
 * Nothing here knows the rules of any particular script. It asks the interface the
 * same questions a player answers, so a walkthrough cannot claim a route the game
 * would not offer.
 *
 * It runs on the test backend: no window, no events, only the framebuffer. */

#define WALK_MAX_SEEN 8192

/* One queue entry per state, holding the command that reached it and the entry it came
 * from. Breadth first, so the first win found is the shortest route there is, and a
 * state is never pruned for sitting deep: depth first used to walk into a long branch,
 * hit the depth cap and come back without ever trying the way to the win. */
typedef struct Node {
    GameState st;
    int parent;
    uint8_t word_len;
    Sym words[RULE_MAX_WORDS];
} Node;

static Node g_nodes[WALK_MAX_SEEN];
static size_t g_nodes_n;
static size_t g_head;
static int g_win;
static int same_state(const GameState *a, const GameState *b) {
    size_t i;

    if (a->room != b->room || a->finished != b->finished ||
        a->flag_count != b->flag_count) {
        return 0;
    }
    for (i = 0; i < a->flag_count; i++) {
        if (a->flag_present[i] != b->flag_present[i]) {
            return 0;
        }
    }
    return 1;
}

/* The queue entry that is `depth` commands before the winning one, following parents
 * back. The replay needs them in order and the chain only goes backwards. */
static int node_at_depth(int win, int depth) {
    int n = win;

    while (n > 0) {
        n = g_nodes[n].parent;
        if (--depth == 0) {
            return n;
        }
    }
    return win;
}

static int already_seen(const GameState *st) {
    size_t i;

    /* The whole queue, not just the expanded part. A state is room plus flags and that
     * is the whole of what the future depends on, so a second route to it cannot offer
     * anything the first did not. */
    for (i = 0; i < g_nodes_n; i++) {
        if (same_state(&g_nodes[i].st, st)) {
            return 1;
        }
    }
    return 0;
}

/* Enumerates the commands available from a state, a word at a time, the same way the
 * palette does, and pushes every resulting state onto the queue. Runs on a scratch
 * restore of the base state, because game_next and game_more both read the state that
 * is current and a command has just changed it. */
static void try_words(Game *g, const GameState *base, int parent, const Sym *prefix,
                      int wi) {
    Sym choices[MAX_CHOICES];
    Sym buf[RULE_MAX_WORDS];
    size_t got, i;

    if (g_win >= 0 || wi >= (int)RULE_MAX_WORDS || g_nodes_n >= WALK_MAX_SEEN) {
        return;
    }
    if (wi > 0) {
        memcpy(buf, prefix, sizeof buf);
    }
    got = game_next(g, (wi > 0) ? buf : NULL, (size_t)wi, choices, MAX_CHOICES);
    for (i = 0; i < got && g_win < 0; i++) {
        Sym w[RULE_MAX_WORDS];

        /* The previous candidate left the game somewhere else entirely, and both
         * game_more and game_command read the state that is current. */
        game_restore(g, base);
        if (wi > 0) {
            memcpy(w, prefix, sizeof w);
        }
        w[wi] = choices[i];
        if (game_more(g, w, (size_t)wi + 1) > 0) {
            try_words(g, base, parent, w, wi + 1);
            continue;
        }
        if (!game_command(g, w, (size_t)wi + 1)) {
            continue;
        }
        if (g->won) {
            memcpy(g_nodes[g_nodes_n].words, w, sizeof w);
            g_nodes[g_nodes_n].word_len = (uint8_t)(wi + 1);
            g_nodes[g_nodes_n].parent = parent;
            g_nodes_n++;
            g_win = (int)g_nodes_n - 1;
            return;
        }
        if (g->finished) {
            continue;
        }
        {
            GameState after;

            game_save(g, &after);
            if (already_seen(&after)) {
                continue;
            }
            memcpy(g_nodes[g_nodes_n].words, w, sizeof w);
            g_nodes[g_nodes_n].word_len = (uint8_t)(wi + 1);
            g_nodes[g_nodes_n].parent = parent;
            g_nodes[g_nodes_n].st = after;
            g_nodes_n++;
        }
    }
}


static void dump_ppm(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    static unsigned char row[4096 * 3];
    int x, y;
    int maxw = (w * 3 < (int)sizeof row) ? w : (int)(sizeof row / 3);

    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < maxw; x++) {
            uint32_t c = px[(size_t)y * (size_t)w + (size_t)x];
            row[x * 3 + 0] = (unsigned char)((c >> 16) & 0xFFu);
            row[x * 3 + 1] = (unsigned char)((c >> 8) & 0xFFu);
            row[x * 3 + 2] = (unsigned char)(c & 0xFFu);
        }
        fwrite(row, 1, (size_t)(maxw * 3), f);
    }
    fclose(f);
}

/* ---------------------------------------------------------- layout audit -- */

/* The worst case the layout can ever be asked for is decided by the script, not by the
 * states: a room shows every distinct verb its rules mention, a verb shows every object
 * its rules mention, a description is every fragment at once, and the item strip is
 * every flag without an underscore at once. So the audit walks the script, not the state
 * graph, which is both faster and immune to whatever the graph walk was doing.
 *
 * Every width goes through the same chip_w and text_width the drawing uses, so a row
 * reported as fitting here fits on screen. */

typedef struct Worst {
    int32_t value;
    char where[96];
} Worst;

static void keep_worst(Worst *w, int32_t v, const char *what) {
    if (v > w->value) {
        w->value = v;
        snprintf(w->where, sizeof w->where, "%s", what);
    }
}

/* How many lines a paragraph takes at this width: the same greedy wrap as the drawing. */
static int32_t rows_of(const TextFont *f, const char *t, uint32_t len, int32_t width) {
    uint32_t left = len;
    int32_t rows = 0;

    while (left > 0 && rows < 256) {
        uint32_t rest = 0;
        (void)wrap_row(f, t, left, width, &rest);
        if (rest == 0) {
            break;
        }
        left -= rest;
        t += rest;
        rows++;
    }
    return rows;
}

/* The widest a row of word chips can get, given the words that could appear in it. */
static int32_t chips_width(const TextFont *f, const Script *s, const Sym *syms,
                           size_t n, const char *trailing) {
    char label[64];
    int32_t x = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t len = chip_label_sym(s, syms[i], label, sizeof label);

        x += (int32_t)text_width(f, label, len) + 2 * CHIP_PAD_X + CHIP_GAP;
    }
    if (trailing != NULL) {
        x += (int32_t)text_width(f, trailing, (uint32_t)strlen(trailing)) +
             2 * CHIP_PAD_X + CHIP_GAP;
    }
    return (n > 0 || trailing != NULL) ? x - CHIP_GAP : 0;
}

static int run_layout_audit(Ui *ui, Game *g, const Script *s) {
    const TextFont *f = ui->font;
    Layout L = compute_layout(ui, "", 0, "", 0);
    int32_t fit_desc = (L.desc_h - 2 * PAD) / f->line_height;
    int32_t fit_resp = (L.resp_h - 2 * PAD) / f->line_height;
    int32_t usable = GAME_W - 2 * MARGIN_X;
    Worst w_ctrl = { 0, "" };
    Worst w_name = { 0, "" };
    Worst w_slots = { 0, "" };
    Worst w_pick = { 0, "" };
    Worst w_desc = { 0, "" };
    Worst w_over = { 0, "" };
    Worst w_desc_at = { 0, "" };
    int rooms_over = 0;
    int32_t w_desc_rows = 0;
    Worst w_resp = { 0, "" };
    Worst w_items = { 0, "" };
    char para[PARA_MAX];
    char what[96];
    size_t i, j, k;
    Sym verbs[RULE_MAX_WORDS * 4];
    Sym objects[RULE_MAX_WORDS * 4];

    (void)g;

    /* Control row: the four buttons, plus the longest title that shares the row. */
    {
        static const char *const labels[4] = { "Выход", "Загрузить", "Сохранить",
                                               "Новая игра" };
        static const size_t caps[4] = { sizeof "Выход", sizeof "Загрузить",
                                        sizeof "Сохранить", sizeof "Новая игра" };
        int32_t bw = 0;

        for (i = 0; i < 4; i++) {
            bw += (int32_t)text_width(f, labels[i], (uint32_t)(caps[i] - 1)) +
                  2 * CHIP_PAD_X + CHIP_GAP;
        }
        bw -= CHIP_GAP;
        for (i = 0; i < sizeof k_games / sizeof k_games[0]; i++) {
            int32_t tw = (int32_t)text_width(f, k_games[i].title,
                                             (uint32_t)strlen(k_games[i].title));

            keep_worst(&w_ctrl, bw + MARGIN_X + tw, k_games[i].title);
        }
    }

    for (i = 0; i < s->room_count; i++) {
        const ScriptRoom *r = &s->rooms[i];
        size_t nv = 0;
        size_t no;
        uint32_t n;
        int32_t desc_rows_room;

        snprintf(what, sizeof what, "комната %.*s", (int)r->title_len, r->title);
        keep_worst(&w_name, (int32_t)text_width(f, r->title, r->title_len), what);

        /* Every room, not just the rooms the winning path walks through. The
         * description is the one band that must never lose text, so it is checked
         * against the smallest band the engine can be forced to give it: one row of
         * chips and the minimum answer. If it fits there, it fits anywhere. */
        {
            char room_para[PARA_MAX];
            size_t rn = 0;
            size_t q;

            for (q = 0; q < r->frag_len && rn + 1 < sizeof room_para; q++) {
                rn += (size_t)snprintf(room_para + rn, sizeof room_para - rn, "%s%.*s",
                                       q > 0 ? " " : "", (int)r->frags[q].text_len,
                                       r->frags[q].text);
                if (rn >= sizeof room_para) {
                    rn = sizeof room_para - 1;
                    break;
                }
            }
            desc_rows_room = (rn > 0)
                                 ? rows_of(f, room_para, (uint32_t)strlen(room_para),
                                           usable)
                                 : 0;
            if (desc_rows_room > fit_desc) {
                rooms_over++;
                keep_worst(&w_over, desc_rows_room, what);
            }
            if (desc_rows_room > w_desc_rows) {
                w_desc_rows = desc_rows_room;
                keep_worst(&w_desc_at, desc_rows_room, what);
            }
        }

        /* Verbs: the distinct first words of the room's rules. */
        for (j = 0; j < r->rule_len; j++) {
            const Rule *ru = &r->rules[j];
            size_t t;

            for (t = 0; t < nv; t++) {
                if (verbs[t] == ru->words[0]) {
                    break;
                }
            }
            if (t == nv && nv < sizeof verbs / sizeof verbs[0]) {
                verbs[nv++] = ru->words[0];
            }
        }
        keep_worst(&w_slots, chips_width(f, s, verbs, nv, NULL), what);

        /* Objects: for one verb, the distinct words after it. */
        no = 0;
        for (j = 0; j < r->rule_len; j++) {
            const Rule *ru = &r->rules[j];
            size_t t;

            if (ru->word_len < 2) {
                continue;
            }
            for (t = 0; t < no; t++) {
                if (objects[t] == ru->words[1]) {
                    break;
                }
            }
            if (t == no && no < sizeof objects / sizeof objects[0]) {
                objects[no++] = ru->words[1];
            }
        }
        keep_worst(&w_pick, chips_width(f, s, objects, no, "назад"), what);

        /* Description: every fragment joined, as if every guard held at once. */
        n = 0;
        para[0] = '\0';
        for (j = 0; j < r->frag_len; j++) {
            const Frag *fr = &r->frags[j];

            if (n > 0 && n + 1 < sizeof para) {
                para[n++] = ' ';
            }
            {
                uint32_t take = fr->text_len;

                if (take > sizeof para - 1 - n) {
                    take = (uint32_t)(sizeof para - 1 - n);
                }
                memcpy(para + n, fr->text, take);
                n += take;
            }
        }
        keep_worst(&w_desc, rows_of(f, para, n, usable), what);

        /* Responses: the longest text any rule of this room can print. */
        for (j = 0; j < r->rule_len; j++) {
            const Rule *ru = &r->rules[j];

            if (ru->act.text_len == 0) {
                continue;
            }
            keep_worst(&w_resp, rows_of(f, ru->act.text, ru->act.text_len, usable), what);
        }
    }

    /* Items: every flag without an underscore, as if all of them were carried. */
    {
        int32_t x = 0;

        for (i = 0; i < s->sym_count; i++) {
            size_t l2 = 0;
            char label[64];

            if (script_sym(s, (Sym)i, &l2)[0] == '_') {
                continue;
            }
            uint32_t len = chip_label_sym(s, (Sym)i, label, sizeof label);

            x += (int32_t)text_width(f, label, len) + 2 * CHIP_PAD_X + CHIP_GAP;
        }
        if (x > CHIP_GAP) {
            x -= CHIP_GAP;
        }
        keep_worst(&w_items, x, "все флаги без подчёркивания");
    }

    printf("строка %d px, полоса %d px, предел для ряда %d px с резервом на «+N»\n\n",
           f->line_height, usable, usable - CHIP_ROW_RESERVE);
    printf("%-28s %7s %7s   %s\n", "полоса", "худшее", "предел", "где");
    printf("%-28s %7d %7d   %s\n", "управление", w_ctrl.value, usable, w_ctrl.where);
    printf("%-28s %7d %7d   %s\n", "подпись комнаты", w_name.value, usable, w_name.where);
    printf("%-28s %7d %7d   %s\n", "ряд слотов", w_slots.value,
           usable - CHIP_ROW_RESERVE, w_slots.where);
    printf("%-28s %7d %7d   %s\n", "ряд выбора", w_pick.value,
           usable - CHIP_ROW_RESERVE, w_pick.where);
    printf("%-28s %7d %7d   %s\n", "описание, строк", w_desc.value, fit_desc, w_desc.where);
    printf("%-28s %7d %7d   %s\n", "ответ, строк", w_resp.value, fit_resp, w_resp.where);
    printf("%-28s %7d %7d   %s\n", "полоса предметов", w_items.value,
           usable - CHIP_ROW_RESERVE, w_items.where);

    {
        int bad = 0;

        if (w_ctrl.value > usable || w_name.value > usable ||
            w_slots.value > usable - CHIP_ROW_RESERVE ||
            w_pick.value > usable - CHIP_ROW_RESERVE ||
            w_items.value > usable - CHIP_ROW_RESERVE) {
            printf("\nПЕРЕПОЛНЕНИЕ ПО ШИРИНЕ: ряд уходит за край\n");
            bad = 1;
        }
        if (w_desc.value > fit_desc) {
            printf("\nПЕРЕПОЛНЕНИЕ ПО ВЫСОТЕ: описание обрезается молча, %d строк при %d\n",
                   w_desc.value, fit_desc);
            bad = 1;
        }
        if (w_resp.value > fit_resp) {
            printf("\nответ длиннее полосы, %d строк при %d: хвост уезжает, это по замыслу\n",
                   w_resp.value, fit_resp);
        }
        if (!bad) {
            printf("\nвсё влезает\n");
        }
    }
    (void)k;
    return 0;
}

static int run_walkthrough(Ui *ui, Game *g, const Script *s, const char *dir) {
    char path[1024];
    GameState start;
    int step;
    int steps = 0;

    g_nodes_n = 0;
    g_head = 0;
    g_win = -1;
    game_save(g, &start);
    g_nodes[0].st = start;
    g_nodes[0].parent = -1;
    g_nodes[0].word_len = 0;
    g_nodes_n = 1;

    while (g_head < g_nodes_n && g_win < 0) {
        game_restore(g, &g_nodes[g_head].st);
        try_words(g, &g_nodes[g_head].st, (int)g_head, NULL, 0);
        g_head++;
    }
    if (g_win < 0) {
        fprintf(stderr, "этим скриптом нельзя выиграть\n");
        return 3;
    }

    /* Walk the parent chain back from the winning state. Breadth first means this is
     * the shortest route, which is the one worth printing for a person to read. */
    {
        int n;

        for (n = g_win; n > 0; n = g_nodes[n].parent) {
            steps++;
        }
        path[0] = '\0';
        {
            int idx[WALK_MAX_SEEN];
            int m = 0;

            for (n = g_win; n > 0; n = g_nodes[n].parent) {
                idx[m++] = n;
            }
            for (n = m - 1; n >= 0; n--) {
                size_t at = strlen(path);
                int k;

                snprintf(path + at, sizeof path - at, "%s", (n == m - 1 ? "" : "\n  "));
                for (k = 0; k < (int)g_nodes[idx[n]].word_len; k++) {
                    size_t wl;
                    const char *w = script_sym(s, g_nodes[idx[n]].words[k], &wl);
                    size_t tail = strlen(path);

                    snprintf(path + tail, sizeof path - tail, "%s%.*s",
                             (k ? " " : ""), (int)wl, w);
                }
            }
        }
        printf("прохождение (%d шагов):\n  %s\n", steps, path);
        step = steps;
    }
    {
        FILE *f = fopen("walkthrough.txt", "w");
        if (f != NULL) {
            fprintf(f, "%s\n", path);
            fclose(f);
        }
    }

    /* Replay it along the parent chain, drawing every step. A walkthrough the graph
     * accepts but the screen cannot show is not a walkthrough. */
    game_init(g, s);
    ui->cmd.filled = 0;
    for (step = 0; step <= steps + 1; step++) {
        char name[512];

        ui->typed = 1.0e9;   /* no animation: the frames have to be comparable */
        ui->animate = 0;
        ui->done = 1;
        ui->cmd.filled = 0;
        draw(ui);
        snprintf(name, sizeof name, "%s/frame%02d.ppm", dir, step);
        dump_ppm(name, ui->ctx->pixels, ui->ctx->w, ui->ctx->h);
        if (step > steps) {
            break;
        }
        /* The command stored on a node is the one that reached it, so stepping forward
         * runs the words of the node one level below where we stand. node_at_depth
         * counts back from the winning node, so the level we want comes out as the
         * remaining distance. Getting this backwards runs the last command first,
         * which fails in the starting room and freezes the whole replay there. */
        {
            int at = node_at_depth(g_win, steps - step - 1);

            game_command(g, g_nodes[at].words, (size_t)g_nodes[at].word_len);
            mark_new_fragments(ui, g);
        }
    }
    printf("кадров записано: %d\n", steps + 2);
    if (ui->overfull > 0) {
        printf("кадров не влезло в 480: %d\n", ui->overfull);
    } else {
        printf("все полосы вместили свой текст\n");
    }
    return 0;
}

/* ---------------------------------------------------------- interaction -- */

static int inside(IoRect r, int32_t x, int32_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* The seam a grammar fuzzer needs: read one script and print what the loader made of
 * it, and nothing else. No window, no font, no game, and every line is a fact rather
 * than a pointer, so two runs on two machines print the same thing and a diff between
 * them means something. */
static int probe_load(const char *path) {
    static _Alignas(16) unsigned char mem[1 << 21];
    Arena a;
    Script sc;
    Diagnostic d[64];
    size_t n = 0;
    size_t errors;
    size_t i;
    size_t len = 0;
    char *text = slurp(path, &len);
    int line = 0;
    ScriptStatus st;

    if (text == NULL) {
        printf("read error\n");
        return 2;
    }
    if (arena_init(&a, mem, sizeof mem) != 0) {

        printf("arena error\n");
        return 2;
    }
    st = script_load(&a, &sc, text, len, &line);
    printf("status %s\n", script_status_str(st));
    printf("errline %d\n", line);
    if (st != SCR_OK) {
        free(text);
        return 1;
    }
    printf("rooms %zu\n", sc.room_count);
    printf("rules %zu\n", sc.rule_count);
    printf("flags %zu\n", sc.sym_count);
    errors = script_validate(&sc, d, sizeof d / sizeof d[0], &n);
    printf("errors %zu\n", errors);
    for (i = 0; i < n && i < sizeof d / sizeof d[0]; i++) {
        size_t len2 = 0;
        const char *name = script_sym(&sc, d[i].subject, &len2);

        printf("diag %s %s %d %.*s\n", script_verify_str(d[i].code),
               d[i].severity == SEV_ERROR ? "error" : "warning", d[i].line,
               (int)len2, name);
    }
    free(text);
    return 0;
}

/* Which script a per-game build starts with. `make heart` compiles the name into the
 * binary so that it runs with no argument; the plain quest still asks. */
#ifndef GAME_DEFAULT
#define GAME_DEFAULT "tutorial"
#endif

static const GameDef *pick_game(int argc, char **argv) {
    size_t i;
    /* The first argument that is not a flag is the game. A per-game build carries its
     * own name inside, so `heart --walk out` has to work the same as `quest heart
     * --walk out`: taking argv[1] blindly would read "--walk" as the game. */
    const char *name = NULL;

    for (i = 1; i < (size_t)argc; i++) {
        if (argv[i][0] == '-') {
            /* --walk takes a directory, and that directory is not the game. */
            if (strcmp(argv[i], "--walk") == 0) {
                i++;
            }
            continue;
        }
        name = argv[i];
        break;
    }
    if (strcmp(argc > 1 ? argv[1] : "", "--load") == 0) {
        exit(probe_load(argc >= 3 ? argv[2] : ""));
    }
    if (name == NULL) {
        for (i = 0; i < sizeof k_games / sizeof k_games[0]; i++) {
            if (strcmp(k_games[i].key, GAME_DEFAULT) == 0) {
                return &k_games[i];
            }
        }
        return &k_games[0];
    }
    for (i = 0; i < sizeof k_games / sizeof k_games[0]; i++) {
        if (strcmp(name, k_games[i].key) == 0) {
            return &k_games[i];
        }
    }
    fprintf(stderr, "неизвестная игра \"%s\", известны:", name);
    for (i = 0; i < sizeof k_games / sizeof k_games[0]; i++) {
        fprintf(stderr, " %s", k_games[i].key);
    }
    fprintf(stderr, "\n");
    return NULL;
}

int main(int argc, char **argv) {
    static _Alignas(16) unsigned char arena_mem[1 << 21];
    static uint32_t pixels[GAME_W * GAME_H];
    Arena arena;
    IoCtx ctx;
    Script script;
    Game game;
    TextFont font;
    Ui ui;
    const GameDef *def;
    char *font_text;
    char *script_text;
    size_t font_len = 0, script_len = 0;
    int running = 1;
    int walking;
    int fps_on = 0;
    int audit = 0;
    double last = now_ms();
    double next_frame = last + (1000.0 / 60.0);
    int dirty;
    int32_t seen_w = 0;
    int32_t seen_h = 0;
    double fps_since = last;
    double worst_ms = 0.0;
    unsigned long fps_frames = 0;

    def = pick_game(argc, argv);
    if (def == NULL) {
        return 2;
    }
    if (arena_init(&arena, arena_mem, sizeof arena_mem) != 0) {
        fprintf(stderr, "не удалось создать арену\n");
        return 1;
    }
    font_text = slurp(FONT_PATH, &font_len);
    script_text = slurp(def->path, &script_len);
    if (font_text == NULL || script_text == NULL) {
        fprintf(stderr, "не удалось прочитать %s\n", def->path);
        return 1;
    }
    if (text_font_load(&arena, &font, font_text, font_len, 16) != TEXT_OK) {
        fprintf(stderr, "не удалось загрузить шрифт\n");
        return 1;
    }
    free(font_text);
    {
        int err_line = 0;
        ScriptStatus st = script_load(&arena, &script, script_text, script_len, &err_line);

        if (st != SCR_OK) {
            /* The line number is the difference between a typo and a hunt. */
            fprintf(stderr, "не удалось разобрать сценарий: %s, строка %d\n",
                    script_status_str(st), err_line);
            return 1;
        }
    }
    if (game_init(&game, &script) != GAME_OK) {
        fprintf(stderr, "не удалось начать игру\n");
        return 1;
    }

    if (!io_init(&ctx, pixels, GAME_W, GAME_H)) {
        fprintf(stderr, "не удалось инициализировать буфер\n");
        return 1;
    }
    memset(&ui, 0, sizeof ui);
    ui.last_room = (size_t)-1;   /* so the first frame counts as a room change */
    ui.ctx = &ctx;
    ui.font = &font;
    ui.game = &game;
    ui.script = &script;
    ui.game_title = def->title;
    ui.last_log = game.log_count;
    ui.done = 1;
    /* The walkthrough has its own ui and turns this off there. */
    ui.animate = 1;
    ui.cmd.filled = 0;

    /* The walkthrough draws into the framebuffer and never reads input, so it runs
     * on the test backend: no window, no display, nothing to tear down. */
    walking = (argc >= 4 && strcmp(argv[2], "--walk") == 0);
    {
        /* The same frame report stress prints, so the two can be compared instead of
         * guessed at. Off unless asked for: a game that prints its own rate every
         * second is a game nobody would ship. */
        int a;

        for (a = 1; a < argc; a++) {
            if (strcmp(argv[a], "--fps") == 0) {
                fps_on = 1;
            }
            if (strcmp(argv[a], "--layout") == 0) {
                audit = 1;
            }
        }
    }
    io_set_view(&ctx, GAME_W * SCALE, GAME_H * SCALE);
    if (audit) {
        /* Before the window: the audit draws nothing and needs no display, which is
         * the whole reason it is faster than looking at pictures. */
        return run_layout_audit(&ui, &game, &script);
    }
    if (!io_backend_open(&ctx, walking ? &io_backend_test : io_platform_backend(),
                         walking ? def->path : def->title)) {
        fprintf(stderr, "не удалось открыть окно (задан ли DISPLAY?)\n");
        return 1;
    }
    if (walking) {
        int rc = run_walkthrough(&ui, &game, &script, argv[3]);
        io_backend_close(&ctx);
        return rc;
    }

    /* Redraw only when something can have changed. The screen is static most of the
     * time and repainting it anyway costs a full pass over the window for nothing,
     * which is the difference between a game at rest and a fan running. */
    dirty = 1;
    while (running) {
        IoEvent ev;
        double t0 = now_ms();
        double dt = t0 - last;
        int timeout = (int)(next_frame - t0);
        size_t cmd;
        size_t answer = 0;

        last = t0;
        if (dt > 250.0) {
            dt = 250.0;
        }
        if (timeout < 0) {
            timeout = 0;
        }
        io_poll(&ctx, timeout);
        /* A resize is not an event: the view size simply becomes different. */
        if (ctx.view_w != seen_w || ctx.view_h != seen_h) {
            seen_w = ctx.view_w;
            seen_h = ctx.view_h;
            dirty = 1;
        }

        while (io_next_event(&ctx, &ev)) {
            size_t i;

            /* Any event at all means the screen may have to change, including the ones
             * this game has no use for: a redraw is cheaper than deciding which count. */
            dirty = 1;

            if (ev.kind == IO_EV_QUIT) {
                running = 0;
                break;
            }
            if (ev.kind != IO_EV_POINTER_UP) {
                continue;
            }
            {
                /* A tap anywhere while the answer is still typing finishes it. There
                 * is no keyboard to press, and waiting out a long answer is the one
                 * thing a finger should not have to do. */
                char probe[PARA_MAX];
                size_t here = last_block(&game, 0);
                size_t n = (here != (size_t)-1)
                         ? block_text(&game, here + 1, block_end(&game, here), probe,
                                      sizeof probe)
                         : 0;
                if (n > 0 && ui.typed < (double)n) {
                    ui.typed = (double)n;
                    ui.done = 1;
                    ui.have_last = 0;
                    break;
                }
            }
            for (i = 0; i < ui.hit_n; i++) {
                const Hit *h = &ui.hits[i];

                if (!inside(h->r, ev.x, ev.y)) {
                    continue;
                }
                switch (h->kind) {
                case HIT_SAVE:
                    do_save(&game, def);
                    break;
                case HIT_LOAD:
                    do_load(&ui, &game, def);
                    break;
                case HIT_NEW:
                    /* A new run of the same script with nothing carried over. The save
                     * on disk is left alone on purpose: Загрузить still brings the old
                     * run back, so a misclick costs a walk and not the game. */
                    if (game_init(&game, &script) == GAME_OK) {
                        ui.cmd.filled = 0;
                        ui.last_log = game.log_count;
                        ui.typed = 0.0;
                        ui.scroll = 0.0;
                        ui.scroll_want = 0.0;
                        ui.done = 1;
                        ui.have_last = 0;
                    }
                    break;
                case HIT_EXIT:
                    running = 0;
                    break;
                case HIT_WORD:
                    ui.cmd.slot[ui.cmd.filled++] = h->sym;
                    /* The command is over when no word can follow what is chosen. How
                     * long a command is comes from the script, not from here. */
                    if (game_more(&game, ui.cmd.slot, (size_t)ui.cmd.filled) == 0) {
                        game_command(&game, ui.cmd.slot, (size_t)ui.cmd.filled);
                        /* Whatever the command brought into the room description is
                         * what the player should notice, so it is marked here and
                         * stays marked until they do something else. */
                        mark_new_fragments(&ui, &game);
                        ui.last = ui.cmd;
                        ui.have_last = 1;
                        ui.cmd.filled = 0;
                    }
                    break;
                case HIT_CANCEL:
                    if (ui.cmd.filled > 0) {
                        ui.cmd.filled--;
                    }
                    break;
                default:
                    break;
                }
                break;
            }
        }

        /* The answer is measured after the events, not before: the frame a command
         * lands on is the frame that has to start typing it, and a length read
         * before the command would come back as nothing and mark it finished. */
        if (game.log_count != ui.last_log) {
            ui.last_log = game.log_count;
            ui.typed = 0.0;
            ui.scroll = 0.0;
            ui.scroll_want = 0.0;
            ui.done = 0;
        }
        cmd = last_block(&game, 0);
        if (cmd != (size_t)-1) {
            char probe[PARA_MAX];
            answer = block_text(&game, cmd + 1, block_end(&game, cmd), probe,
                                sizeof probe);
        }
        if (ui.animate) {
            /* A fixed time constant, so the scroll reads the same at any frame rate.
             * Lagging behind the text by a fraction of a line is the point: the block
             * glides instead of jumping. */
            double k = dt / 90.0;

            if (k > 1.0) {
                k = 1.0;
            }
            ui.scroll += (ui.scroll_want - ui.scroll) * k;
            ui.dscroll += (ui.dscroll_want - ui.dscroll) * k;
        }
        if (!ui.done || ui.scroll != ui.scroll_want || ui.dscroll != ui.dscroll_want) {
            dirty = 1;
        }
        if (!ui.done) {
            ui.typed += TYPE_CPS * dt / 1000.0;
            if (ui.typed >= (double)answer) {
                ui.typed = (double)answer;
                ui.done = 1;
                ui.have_last = 0;
            }
        }

        if (running && dirty) {
            double t_draw = now_ms();

            dirty = 0;
            draw(&ui);
            if (ctx.backend != NULL && ctx.backend->present != NULL) {
                ctx.backend->present(ctx.backend->self, &ctx);
            }
            if (fps_on) {
                double cost = now_ms() - t_draw;

                if (cost > worst_ms) {
                    worst_ms = cost;
                }
                fps_frames++;
            }
        }
        /* Counted outside the redraw, so a screen standing still reports zero rather
         * than saying nothing at all. Zero here is the whole point of skipping it. */
        if (fps_on && now_ms() - fps_since >= 1000.0) {
            fprintf(stderr, "%4d x %4d  %6.1f fps   худший кадр %6.1f ms\n",
                    ctx.view_w, ctx.view_h,
                    fps_frames * 1000.0 / (now_ms() - fps_since), worst_ms);
            fps_since = now_ms();
            fps_frames = 0;
            worst_ms = 0.0;
        }
        next_frame += 1000.0 / 60.0;
        if (next_frame < now_ms()) {
            next_frame = now_ms() + (1000.0 / 60.0);
        }
    }
    io_backend_close(&ctx);
    free(script_text);
    return 0;
}