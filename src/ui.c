#include "ui.h"

#include "utf8.h"

#include <stddef.h>
#include <string.h>

/* Shortens the longest names until the row fits, cutting at a space where it can, and
 * leaves a dot as the mark.
 *
 * It lives outside the drawing because a rule about what may be hidden from the player
 * is exactly the kind of rule that needs a test, and inside draw() there is no way to
 * reach it. Returns nothing: the labels are shortened in place. */
void items_fit(const TextFont *f, char label[][LABEL_MAX], uint32_t *len, size_t n) {
        /* Shorten the longest name until the row fits. Longest first is what makes it
         * fair: every name loses about the same rather than one losing all of its own.
         * Each round cuts one character off the longest and marks it with a dot. */
    size_t rounds;
    size_t k;

    for (rounds = 0;; rounds++) {
            int32_t total = 0;
            int32_t worst = -1;
            size_t worst_chars = 0;

            for (k = 0; k < n; k++) {
                size_t chars = utf8_length((const uint8_t *)label[k], len[k]);

                total += (int32_t)text_width(f, label[k], len[k]) + 2 * CHIP_PAD_X +
                         CHIP_GAP;
                if (chars > worst_chars && chars > ITEM_MIN_CHARS) {
                    worst_chars = chars;
                    worst = (int32_t)k;
                }
            }
            if (n == 0 || total - CHIP_GAP <= GAME_W - 2 * MARGIN_X || worst < 0) {
                break;
            }
            {
                /* Two characters go, and the dot stands in for one of them: a name that
                 * loses a character and gains a dot has not got shorter, and a loop
                 * that waits for the row to fit waits forever.
                 *
                 * The cut prefers the space, because a name is words and not a string:
                 * «спички отогреты» shortened to «спички о.» says less than «спички.»,
                 * and the second is shorter besides. The space is dropped rather than
                 * kept, so a dot never ends up stranded after one. */
                size_t keep = worst_chars - 2;
                size_t cut = utf8_offset((const uint8_t *)label[worst], len[worst], keep);
                size_t back = cut;
                size_t chars_left = keep;

                while (back > 0 && label[worst][back - 1] != ' ' &&
                       chars_left > ITEM_MIN_CHARS) {
                    back--;
                    chars_left--;
                }
                if (back > 0 && label[worst][back - 1] == ' ' &&
                    chars_left > ITEM_MIN_CHARS) {
                    cut = back - 1;
                }
                label[worst][cut++] = '.';
                label[worst][cut] = '\0';
                len[worst] = (uint32_t)cut;
            }
            if (++rounds > ITEM_MAX * 64) {
                /* Belt and braces. Every round shortens the longest name by one
                 * character, so this cannot be reached; a game hanging on a strip of
                 * carried things is far worse than a strip that does not quite fit. */
                break;
            }
        }

}

void ui_add_hit(Ui *ui, IoRect r, int kind, Sym sym) {
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
void ui_text_at(IoCtx *c, const TextFont *f, int32_t x, int32_t top,
                    const char *t, uint32_t len, IoColor ink) {
    text_draw(c, f, x, top + f->ascent, t, len, ink);
}

/* Identifiers use underscores where a name has two words. The chip is the only
 * place that turns them back into spaces: that is a display concern. */
/* The same for a bare script, so the auditor measures labels without inventing a Ui
 * just to borrow this. One implementation, two callers. */
uint32_t ui_chip_label_sym(const Script *s, Sym sym, char *out, size_t cap) {
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

int32_t ui_rows_of(const TextFont *f, const char *t, uint32_t len,
                     int32_t width);

uint32_t ui_chip_label(const Ui *ui, Sym sym, char *out, size_t cap) {
    return ui_chip_label_sym(ui->script, sym, out, cap);
}

int32_t ui_chip_w(const TextFont *f, const char *label, uint32_t len) {
    return text_width(f, label, len) + 2 * CHIP_PAD_X;
}

/* One label, one length, taken from the literal itself. Measuring and drawing with
 * two separately typed numbers is how a button ends up reading "Сохр". */
/* Draws one row of word chips and returns how many fitted. Anything past the edge is
 * replaced by a "+N" chip rather than being drawn where the player cannot click it.
 * Room for that chip is reserved from the first chip on, otherwise the row fills the
 * width completely and there is nowhere left to admit the loss. */
int32_t ui_draw_button(IoCtx *c, Ui *ui, const TextFont *f, int32_t right,
                           int32_t top, int32_t h, const char *label, size_t cap,
                           IoColor bg, int kind) {
    uint32_t len = (uint32_t)(cap - 1);
    int32_t w = ui_chip_w(f, label, len);
    int32_t x = right - w;
    IoRect r = { x, top, w, h };

    io_fill_rect(c, r, bg);
    ui_add_hit(ui, r, kind, 0);
    ui_text_at(c, f, x + CHIP_PAD_X, top + (h - f->line_height) / 2, label, len, C_INK);
    return x - CHIP_GAP;
}

int32_t ui_draw_tile(IoCtx *c, const TextFont *f, int32_t x, int32_t y,
                         const char *label, uint32_t len, int filled) {
    int32_t w = text_width(f, label, len) + 2 * CHIP_PAD_X;
    IoRect r = { x, y, w, TILE_H };

    io_fill_rect(c, r, filled ? C_CHIP_ON : C_FIELD);
    io_fill_rect(c, (IoRect){ r.x, r.y, r.w, 1 }, filled ? C_CHIP_ON : C_RULE);
    io_fill_rect(c, (IoRect){ r.x, r.y + r.h - 1, r.w, 1 }, C_RULE);
    if (len > 0) {
        ui_text_at(c, f, r.x + CHIP_PAD_X, r.y + (TILE_H - f->line_height) / 2,
                label, len, filled ? C_INK : C_DIM);
    }
    return x + w + CHIP_GAP;
}

/* How many rows of chips these words need at this width, measured with the same
 * arithmetic the drawing uses so the band is never sized for one arrangement and drawn
 * for another. The reserve at the right end is what leaves room for a "+N". */
/* Does the next chip start a new row? One predicate, because measuring and drawing
 * must agree and there is no test in the world that catches them disagreeing: the band
 * would simply be the wrong height, and the screen would look plausible. */
int ui_chip_wraps(int32_t x, int32_t w) {
    return x > 0 && x + CHIP_GAP + w > GAME_W - MARGIN_X - CHIP_ROW_RESERVE;
}

int32_t ui_chip_rows_needed(const TextFont *f, Ui *ui, const Sym *syms, size_t n,
                                const char *trailing) {
    int32_t x = 0;
    int32_t rows = 1;
    char label[LABEL_MAX];
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t len = ui_chip_label(ui, syms[i], label, sizeof label);
        int32_t w = ui_chip_w(f, label, len);

        if (ui_chip_wraps(x, w)) {
            rows++;
            x = 0;
        }
        x += w + CHIP_GAP;
    }
    if (trailing != NULL) {
        int32_t w = ui_chip_w(f, trailing, (uint32_t)strlen(trailing));

        if (ui_chip_wraps(x, w)) {
            rows++;
        }
    }
    return rows;
}

/* Draws the words and the trailing chip as one flow that wraps, rather than as two
 * things each remembering a position. That is what stops "назад" landing on the first
 * word: it is placed after the last word drawn, on whichever row that turns out to be. */
int32_t ui_draw_chip_rows(IoCtx *c, Ui *ui, const TextFont *f, int32_t y,
                              const Sym *syms, size_t n, int kind,
                              const char *trailing) {
    int32_t x = MARGIN_X;
    int32_t rows = 1;
    char label[LABEL_MAX];
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t len = ui_chip_label(ui, syms[i], label, sizeof label);
        int32_t w = ui_chip_w(f, label, len);

        if (ui_chip_wraps(x, w)) {
            rows++;
            x = MARGIN_X;
            y += CHIP_H + CHIP_GAP;
        }
        {
            IoRect r = { x, y, w, TILE_H };

            io_fill_rect(c, r, C_CHIP);
            io_fill_rect(c, (IoRect){ r.x, r.y, r.w, 1 }, C_RULE);
            ui_text_at(c, f, r.x + CHIP_PAD_X, r.y + (TILE_H - f->line_height) / 2, label,
                    len, C_INK);
            ui_add_hit(ui, r, kind, syms[i]);
        }
        x += w + CHIP_GAP;
    }
    if (trailing != NULL) {
        uint32_t len = (uint32_t)strlen(trailing);
        int32_t w = ui_chip_w(f, trailing, len);

        if (ui_chip_wraps(x, w)) {
            rows++;
            x = MARGIN_X;
            y += CHIP_H + CHIP_GAP;
        }
        {
            IoRect r = { x, y, w, TILE_H };

            io_fill_rect(c, r, C_CHIP);
            ui_text_at(c, f, r.x + CHIP_PAD_X, r.y + (TILE_H - f->line_height) / 2, trailing,
                    len, C_DIM);
            ui_add_hit(ui, r, HIT_CANCEL, 0);
        }
    }
    return rows;
}

/* The words on offer now. One list, read by both the measurement and the drawing.
 * A word nothing can follow is a dead end rather than an action, so it is filtered
 * here: the source lists every verb the room mentions, and this band is the place that
 * promises the player a choice is possible. */
size_t ui_gather_pick(Ui *ui, Sym *choices, size_t cap) {
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
