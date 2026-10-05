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

void ui_band(IoCtx *c, int32_t y, int32_t h, IoColor bg) {
    io_fill_rect(c, (IoRect){ 0, y, GAME_W, h }, bg);
    io_fill_rect(c, (IoRect){ 0, y + h - 1, GAME_W, 1 }, C_RULE);
}

typedef struct Rows {
    uint32_t at[ROWS_MAX + 1];
    size_t n;
} Rows;

/* Wraps `t` and draws it from `top` downwards. Anything past the ui_band is clipped by
 * the clip stack rather than measured away. */
void ui_text_top(IoCtx *c, const TextFont *f, int32_t top, int32_t width,
                     const char *t, uint32_t len, IoColor ink) {
    Rows r;
    uint32_t left = len;
    size_t k;

    r.at[0] = 0;
    r.n = 0;
    while (left > 0 && r.n < ROWS_MAX) {
        uint32_t rest = 0;
        (void)ui_wrap_row(f, t, left, width, &rest);
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

/* The last `fit` rows, laid out from the top of the ui_band downwards. When there is
 * more text than fits, the tail is what matters: the newest sentence is the one
 * being read, and a ui_band that shows the first rows instead hides the end of the
 * answer under the fold.
 *
 * Reports where the text ended, so a caret can sit immediately after the last
 * letter. Working that out here rather than in the caller keeps the two from
 * drifting apart: a caret placed by its own arithmetic ends up on some other row. */
double ui_text_tail(IoCtx *c, const TextFont *f, int32_t top, int fit, int32_t width,
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
        (void)ui_wrap_row(f, full, left, width, &rest);
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



/* Works out which fragments the last command brought in, so the description can point
 * at them. Called once per action, not per frame: the highlight is meant to sit there
 * until the player does something else. */
void ui_mark_new_fragments(Ui *ui, const Game *g) {
    char buf[PARA_MAX];
    FragSpan sp[FRAG_MAX];
    size_t n = 0;
    size_t i;
    size_t j;

    /* A command that walks into another room did not bring that room into being. Every
     * fragment in it is new to the player and none of it is news, so the baseline is
     * recorded and nothing is marked. Without this the whole new room lights up. */
    if (g->room != ui->last_room) {
        /* Deliberately not recording the baseline here. This runs the instant the
         * command returns, and whether the room's text can be assembled yet is a
         * matter of timing; recording an empty baseline means the next action inside
         * that room finds every fragment new and paints the whole room. The baseline is
         * taken lazily by draw, on a frame where the room is already on screen. */
        ui->last_room = g->room;
        ui->frag_line_n = 0;
        ui->frag_hot_n = 0;
        return;
    }
    ui->frag_hot_n = 0;
    /* The count comes back from the call. Looking for the end of the array by
     * inspecting entries nobody wrote is reading uninitialised stack, and what is
     * there differs between toolchains: the same source then marks a different number
     * of fragments on Linux and on Windows, which is exactly what it did. */
    if (game_room_text_spans(g, buf, sizeof buf, sp, FRAG_MAX, &n) == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
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
void ui_note_room_fragments(Ui *ui, const Game *g) {
    char buf[PARA_MAX];
    FragSpan sp[FRAG_MAX];
    size_t n = 0;
    size_t i;

    ui->frag_line_n = 0;
    ui->frag_hot_n = 0;
    if (game_room_text_spans(g, buf, sizeof buf, sp, FRAG_MAX, &n) == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (ui->frag_line_n < FRAG_MAX) {
            ui->frag_line[ui->frag_line_n++] = sp[i].line;
        }
    }
}

/* Band geometry for this frame.
 *
 * The system bands grow down from the top, the play bands grow up from the bottom, and
 * the description takes what is left in between: no empty space, and no ui_band taller than
 * its content. The answer is the one that gives way, because it already scrolls. */

Layout ui_compute_layout(Ui *ui, const char *desc, uint32_t desc_len,
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

    /* While the answer is still being written, the ui_band keeps the command that caused it
     * and offers nothing, so it needs no row of choices yet. */
    if (!ui->done && ui->have_last) {
        L.chip_rows = 0;
    } else {
        n = ui_gather_pick(ui, choices, MAX_CHOICES);
        if (ui->cmd.filled > 0) {
            trailing = "назад";
        }
        L.chip_rows = ui_chip_rows_needed(f, ui, choices, n, trailing);
    }
    L.cmd_h = CMD_PAD + TILE_H + CMD_PAD + L.chip_rows * CHIP_H +
              (L.chip_rows > 0 ? (L.chip_rows - 1) * CHIP_GAP : 0) + CMD_PAD;
    if (L.chip_rows == 0) {
        L.cmd_h = CMD_PAD + TILE_H + CMD_PAD;
    }
    L.cmd_y = yb - L.cmd_h;
    yb = L.cmd_y;
    /* Absolute, not offsets inside the ui_band: the draw calls take canvas coordinates. */
    L.tile_y = L.cmd_y + CMD_PAD;
    L.pick_y = L.cmd_y + CMD_PAD + TILE_H + CMD_PAD;

    /* The description is exactly its own text. It used to take the remainder, which
     * sounds generous and is not: when the answer and the command ui_band together wanted
     * more than the canvas had, the remainder went negative and the description was
     * quietly clipped, with only the first lines showing. A ui_band that must not lose
     * text gets the height of that text. */
    (void)desc_len;
    desc_n = (desc != NULL && desc[0] != '\0')
                 ? ui_rows_of(f, desc, (uint32_t)strlen(desc), GAME_W - 2 * MARGIN_X)
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
     * its tail, so a short ui_band costs the reader a scroll and a clipped description
     * costs them the room. */
    rows = (ans_n > 0) ? ui_rows_of(f, ans, ans_n, GAME_W - 2 * MARGIN_X) : 1;
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

/* The chosen words as slots. One function for both rows, because they were two copies
 * of the same loop differing only in which array they read: the command being typed and
 * the command that produced the answer still being written. A second copy is a second
 * place to forget the label, the padding or the hit rectangle. */
int32_t ui_draw_slots(Ui *ui, IoCtx *c, const TextFont *f, int32_t x, int32_t y,
                          const Sym *slot, size_t filled) {
    char label[LABEL_MAX];
    size_t i;

    for (i = 0; i < filled; i++) {
        uint32_t len = ui_chip_label(ui, slot[i], label, sizeof label);

        x = ui_draw_tile(c, f, x, y, label, len, 1);
    }
    return x;
}

Layout ui_layout_commands(Ui *ui, const char *desc, uint32_t desc_len,
                             const char *ans, uint32_t ans_n) {
    IoCtx *c = ui->ctx;
    const TextFont *f = ui->font;
    Sym choices[MAX_CHOICES];
    int32_t x = MARGIN_X;
    size_t n = 0;
    Layout L = ui_compute_layout(ui, desc, desc_len, ans, ans_n);

    if (!ui->done && ui->have_last) {
        /* While the answer is still being written, the command that caused it stays in
         * full. Clearing the row the moment the command runs throws away the object,
         * which is the part the player is least likely to remember. */
        (void)ui_draw_slots(ui, c, f, x, L.tile_y, ui->last.slot,
                         (size_t)ui->last.filled);
        return L;
    }
    x = ui_draw_slots(ui, c, f, x, L.tile_y, ui->cmd.slot, (size_t)ui->cmd.filled);
    {
        static const char pick[] = "?";

        x = ui_draw_tile(c, f, x, L.tile_y, pick, (uint32_t)(sizeof pick - 1), 0);
    }
    (void)x;

    n = ui_gather_pick(ui, choices, MAX_CHOICES);
    if (n == 0) {
        static const char none[] = "здесь нечего делать";

        ui_text_at(c, f, MARGIN_X, L.pick_y + (TILE_H - f->line_height) / 2, none,
                (uint32_t)(sizeof none - 1), C_DIM);
    }
    (void)ui_draw_chip_rows(c, ui, f, L.pick_y, choices, n, HIT_WORD,
                         (ui->cmd.filled > 0) ? "назад" : NULL);
    return L;
}

/* ------------------------------------------------------------ saves -- */

/* A save is the room and the flags, and nothing else. The log is display: it is
 * rebuilt by entering the room again, which is both smaller than writing it out
 * and more correct, because the fragments that print are chosen by the flags that
 * were just restored. One file per game, in the working directory. */

int32_t ui_rows_of(const TextFont *f, const char *t, uint32_t len, int32_t width) {
    uint32_t left = len;
    int32_t rows = 0;

    while (left > 0 && rows < 256) {
        uint32_t rest = 0;
        (void)ui_wrap_row(f, t, left, width, &rest);
        if (rest == 0) {
            break;
        }
        left -= rest;
        t += rest;
        rows++;
    }
    return rows;
}

uint32_t ui_wrap_row(const TextFont *f, const char *t, uint32_t len,
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
