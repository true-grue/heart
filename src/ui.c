#include "ui.h"

#include "utf8.h"

#include <stddef.h>
#include <string.h>

/* Kept out of draw() so it can be tested. */
void items_fit(const TextFont *f, char label[][LABEL_MAX], uint32_t *len, size_t n) {
    /* Longest first: every name loses about the same, not one all of its own. */
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
                /* Two go, the dot counting as one, or the loop waits forever: a name that
                 * loses a character and gains a dot is no shorter. Prefer the space, and
                 * drop it, so no dot is stranded after one. */
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
                /* Unreachable; hanging here is worse than a strip that does not fit. */
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

/* text_draw takes a baseline, not the top of the line; a top coordinate
 * draws the text off the top of the band, so every caller goes through here. */
void ui_text_at(IoCtx *c, const TextFont *f, int32_t x, int32_t top,
                    const char *t, uint32_t len, IoColor ink) {
    text_draw(c, f, x, top + f->ascent, t, len, ink);
}

/* Identifiers use underscores where a name has two words; the chip is the only
 * place that turns them back into spaces. Shared with the auditor, which has no Ui. */
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

/* One label, one length, taken from the literal itself: two separately
 * typed numbers is how a button ends up reading "Сохр". */
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

/* Row count and wrap test share one predicate: they must agree, and nothing
 * catches disagreement but a plausible-looking band of the wrong height. */
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

/* One wrapping flow: the trailing chip goes after the last word drawn, which is
 * what stops "назад" landing on the first word. */
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

/* The words on offer, read by both the measurement and the drawing. A word
 * nothing can follow is a dead end, not an action, so it is filtered out here. */
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

/* Wraps `t` downwards from `top`; the clip stack, not measurement, cuts overflow. */
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

/* The last `fit` rows from the top of the ui_band: on overflow the tail is what
 * matters, and the end position is returned so a caret cannot drift onto another row. */
double ui_text_tail(IoCtx *c, const TextFont *f, int32_t top, int fit, int32_t width,
                        const char *full, uint32_t len, uint32_t vis, IoColor ink,
                        double off, const Span *hl, size_t hl_n, IoColor hot,
                        int32_t *end_x, int32_t *end_y) {
    const char *base = full;
    Rows r;
    uint32_t left = len;
    size_t k;

    /* Rows are cut from the WHOLE paragraph, not the revealed prefix: on the prefix a
     * word that does not fit jumps to the next line mid-reveal and reads as typed twice. */
    r.at[0] = 0;
    r.n = 0;
    while (left > 0 && r.n < ROWS_MAX) {
        uint32_t rest = 0;
        (void)ui_wrap_row(f, full, left, width, &rest);
        /* Only "no progress" ends the split. Comparing against r.at[r.n], an absolute
         * offset that grows every row, looks like a guard and is not: two neighbouring
         * rows of equal byte length drop the rest of the paragraph. */
        if (rest == 0) {
            break;
        }
        left -= rest;
        full += rest;
        r.n++;
        r.at[r.n] = r.at[r.n - 1] + rest;
    }
    {
        /* The block slides by a pixel offset, not whole rows; skip is a floor so
         * the row crossing the top edge is drawn half out rather than not at all. */
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
            /* Pieces, so a highlight can change colour without re-laying the row.
             * Splits fall on fragment boundaries, so no piece starts mid-character. */
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
                    /* Each piece starts where the last one ended; all at the left
                     * margin the row piles into itself and reads as garbled text. */
                    text_draw(c, f, PAD + text_width(f, base + from, p - from), y,
                              base + p, seg - p, col);
                    p = seg;
                }
            }
            if (to == vis) {
                /* Caret goes after the last character arrived, end of the row the
                 * reveal stops in. */
                *end_x = PAD + text_width(f, base + from, to - from);
                *end_y = y;
            }
        }
        return want;
    }
}



/* Fragments the last command brought in. Once per action, not per frame: the
 * highlight sits until the player acts again. */
void ui_mark_new_fragments(Ui *ui, const Game *g) {
    char buf[PARA_MAX];
    FragSpan sp[FRAG_MAX];
    size_t n = 0;
    size_t i;
    size_t j;

    /* A command that walks into another room did not bring that room into being:
     * its fragments are new to the player and none of it is news, so nothing is marked. */
    if (g->room != ui->last_room) {
        /* Baseline deliberately not recorded here: this runs the instant the command
         * returns, and an empty one makes the next action paint the whole room.
         * draw() takes it lazily, once the room is already on screen. */
        ui->last_room = g->room;
        ui->frag_line_n = 0;
        ui->frag_hot_n = 0;
        return;
    }
    ui->frag_hot_n = 0;
    /* Use the returned count: entries nobody wrote are uninitialised stack, and they
     * differ between toolchains, so Linux and Windows marked different fragment counts. */
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
            ui->frag_line[ui->frag_line_n++] = sp[i].line;
            if (ui->frag_hot_n < FRAG_MAX) {
                ui->frag_hot[ui->frag_hot_n++] = sp[i].line;
            }
        }
    }
}

/* Entering a room is not news about it, so its fragments are recorded as shown:
 * without this the first action inside turns the whole description colour. */
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

/* Band geometry for this frame: system bands grow down from the top, play bands up from
 * the bottom, the description takes the rest, and the answer gives way as it scrolls. */

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

    /* While the answer is still being written the band offers nothing, so no chip row. */
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

    /* The description is exactly its own text. Taking the remainder is not generous:
     * when answer and command band outgrew the canvas it went negative and clipped. */
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

    /* The answer gives way; the description does not. */
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
        /* Genuinely overfull: reported, so the walkthrough can name the room. */
        L.overfull = 1;
        L.resp_h = L.cmd_y - L.desc_y - L.desc_h;
        if (L.resp_h < 0) {
            L.resp_h = 0;
        }
        L.resp_y = L.cmd_y - L.resp_h;
    }
    return L;
}

/* The chosen words as slots; one function for both rows, formerly two copies of a loop. */
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
        /* While the answer is still being written the command that caused it stays in
         * full: clearing the row throws away the object, the part least likely remembered. */
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

/* A save is the room and the flags, nothing else; the log is display, rebuilt by
 * re-entering the room -- smaller than writing it out, and the printed fragments follow the
 * flags just restored. One file per game. */

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

void ui_draw(Ui *ui) {
    IoCtx *c = ui->ctx;
    const TextFont *f = ui->font;
    const Game *g = ui->game;
    char para[PARA_MAX];
    char dpara[PARA_MAX];
    char dpara2[PARA_MAX];
    size_t room = ui_last_log_index(g, 1);
    size_t cmd = ui_last_log_index(g, 0);
    size_t answer_n;
    size_t desc_n;
    size_t i;
    Layout L;

    ui->hit_n = 0;

    /* Before anything is drawn: the description reads the marks, so a reset landing below
     * it leaves the entering frame in the previous room's colours. And g->room is a room
     * symbol while ui_last_log_index returns a log index: comparing them is always true. */
    if ((size_t)g->room != ui->last_room) {
        ui->last_room = (size_t)g->room;
        ui->dscroll = 0.0;
        ui->dscroll_want = 0.0;
    }
    /* Invariant, not a reaction: whatever the command did on the way here, the fragments
     * on screen were already there and nothing lights up on account of arriving. */
    if (ui->frag_line_n == 0) {
        ui_note_room_fragments(ui, g);
    }

    io_fill_rect(c, (IoRect){ 0, 0, GAME_W, GAME_H }, C_BG);

    /* Bands first, contents after: geometry comes from the text this frame will show. */
    answer_n = (cmd != (size_t)-1)
                   ? ui_block_text(g, cmd + 1, ui_block_end(g, cmd), para, sizeof para)
                   : 0;
    desc_n = game_room_text(g, dpara, sizeof dpara);
    L = ui_compute_layout(ui, dpara, (uint32_t)desc_n, para, (uint32_t)answer_n);

    ui_band(c, L.ctrl_y, L.ctrl_h, C_BAND);
    ui_band(c, L.name_y, L.name_h, C_BAND);
    ui_band(c, L.desc_y, L.desc_h, C_FIELD);
    ui_band(c, L.cmd_y, L.cmd_h, C_BAND);
    ui_band(c, L.items_y, L.items_h, C_BAND);
    ui_band(c, L.resp_y, L.resp_h, C_FIELD);

    ui_text_at(c, f, MARGIN_X, L.ctrl_y + (L.ctrl_h - f->line_height) / 2, ui->game_title,
            (uint32_t)strlen(ui->game_title), C_DIM);
    {
        static const char new_label[] = "Новая игра";
        static const char save_label[] = "Сохранить";
        static const char load_label[] = "Загрузить";
        static const char exit_label[] = "Выход";
        int32_t right = GAME_W - MARGIN_X;

        /* Laid out right to left, so the way out stays in the corner a hand goes to. */
        right = ui_draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, exit_label,
                            sizeof exit_label, C_QUIT, HIT_EXIT);
        right = ui_draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, load_label,
                            sizeof load_label, C_CHIP, HIT_LOAD);
        right = ui_draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, save_label,
                            sizeof save_label, C_CHIP, HIT_SAVE);
        (void)ui_draw_button(c, ui, f, right, L.ctrl_y + 2, BTN_H, new_label,
                          sizeof new_label, C_CHIP, HIT_NEW);
    }

    if (room != (size_t)-1) {
        ui_text_at(c, f, MARGIN_X, L.name_y + (L.name_h - f->line_height) / 2, g->log_head[room],
                g->log_len[room], C_NAME);
    }

    /* Description band: one paragraph, built from the room's fragments against the flags
     * as they are now, not read out of the log -- that showed the room as it was on entry. */
    io_push_clip(c, (IoRect){ 0, L.desc_y, GAME_W, L.desc_h });
    if (desc_n > 0) {
        Span hl[FRAG_MAX];
        FragSpan sp[FRAG_MAX];
        size_t hl_n = 0;
        size_t nsp = 0;
        size_t si;
        size_t k;

        /* Fragments the last command brought, as offsets into this frame's text. Rebuilt
         * per frame: the offsets move when the text is assembled differently. */
        if (game_room_text_spans(g, dpara2, sizeof dpara2, sp, FRAG_MAX, &nsp) ==
            desc_n) {
            for (si = 0; si < nsp; si++) {
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
        ui->dscroll_want = ui_text_tail(c, f, L.desc_y + PAD, fit, GAME_W - 2 * MARGIN_X,
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
    /* A new room resets the scroll, keyed on the room so loading resets it too. */

    (void)ui_layout_commands(ui, dpara, (uint32_t)desc_n, para, (uint32_t)answer_n);

    /* The strip of what the player is carrying. A leading underscore means state, not a
     * thing, and it is the only marker the format has. No "+N": how many things he is not
     * looking at is the least useful number on screen, so names shorten longest-first. */
    {
        int32_t x = MARGIN_X;
        char label[ITEM_MAX][LABEL_MAX];
        uint32_t len[ITEM_MAX];
        size_t count = 0;
        size_t k;

        for (i = 0; i < game_flag_count(g) && count < ITEM_MAX; i++) {
            size_t l2 = 0;
            const char *name;

            if (!game_flag_on(g, i)) {
                continue;
            }
            name = game_flag_name(g, i, &l2);
            if (l2 == 0 || name[0] == '_') {
                continue;
            }
            for (k = 0; k < l2 && k < LABEL_MAX - 2; k++) {
                label[count][k] = (name[k] == '_') ? ' ' : name[k];
            }
            label[count][k] = '\0';
            len[count] = (uint32_t)k;
            count++;
        }

        items_fit(f, label, len, count);

        for (k = 0; k < count; k++) {
            int32_t w = (int32_t)text_width(f, label[k], len[k]) + 2 * CHIP_PAD_X;

            io_fill_rect(c, (IoRect){ x, L.items_y + 3, w, L.items_h - 6 }, C_CHIP);
            ui_text_at(c, f, x + CHIP_PAD_X,
                    L.items_y + 3 + (L.items_h - 6 - f->line_height) / 2,
                    label[k], len[k], C_DIM);
            x += w + CHIP_GAP;
        }
        if (count == 0) {
            /* Nothing carried, not "nothing drawn": shortened names still show. */
            static const char empty[] = "пусто";

            ui_text_at(c, f, x, L.items_y + (L.items_h - f->line_height) / 2, empty,
                    (uint32_t)(sizeof empty - 1), C_DIM);
        }
    }

    io_push_clip(c, (IoRect){ 0, L.resp_y, GAME_W, L.resp_h });
    if (cmd != (size_t)-1) {
        size_t n = answer_n;
        size_t vis = (ui->typed >= (double)n) ? n : ui_utf8_floor(para, n, (size_t)ui->typed);

        int fit = (L.resp_h - 2 * PAD) / f->line_height;
        int32_t end_x = MARGIN_X;
        int32_t end_y = L.resp_y + PAD;
        int32_t top = L.resp_y + PAD;

        /* Bottom anchored: slack reads as a gap under the description, not a hole above. */
        {
            int32_t rows = ui_rows_of(f, para, (uint32_t)n, GAME_W - 2 * MARGIN_X);

            if (rows < fit) {
                top = L.resp_y + L.resp_h - PAD - rows * f->line_height;
                if (top < L.resp_y + PAD) {
                    top = L.resp_y + PAD;
                }
                fit = rows;
            }
        }

        ui->scroll_want = ui_text_tail(c, f, top, fit, GAME_W - 2 * MARGIN_X, para,
                                    (uint32_t)n, (uint32_t)vis, C_INK, ui->scroll, NULL, 0,
                                    C_INK, &end_x, &end_y);
        if (!ui->animate) {
            /* The walkthrough must land on the tail, or the frame it writes is not the
             * frame the player would have seen. */
            ui->scroll = ui->scroll_want;
        }
        if (vis < n) {
            /* The caret moves with the text, directly after the last letter that
             * arrived; a fixed spot would read as revealed rather than written. */
            io_fill_rect(c, (IoRect){ end_x + 2, end_y - f->ascent + 3, 7,
                                      f->ascent - 3 }, C_CARET);
        }
    } else {
        /* No verb/object wording: a command is however many words the script gave it, so
         * the old hint naming two slots was wrong for everything else. */
        static const char hint[] = "Выберите действие.";
        ui_text_top(c, f, L.resp_y + PAD, GAME_W - 2 * MARGIN_X, hint,
                 (uint32_t)(sizeof hint - 1), C_DIM);
    }
    io_pop_clip(c);
}

size_t ui_utf8_floor(const char *t, size_t len, size_t upto) {
    while (upto > 0 && upto < len &&
           ((unsigned char)t[upto] & 0xC0u) == 0x80u) {
        upto--;
    }
    return upto;
}

size_t ui_block_text(const Game *g, size_t from, size_t to, char *out, size_t cap) {
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

size_t ui_block_end(const Game *g, size_t from) {
    size_t i;

    for (i = from + 1; i < g->log_count; i++) {
        if (g->log_headed[i]) {
            return i;
        }
    }
    return g->log_count;
}

size_t ui_last_log_index(const Game *g, int want_room) {
    size_t i;

    for (i = g->log_count; i > 0; i--) {
        if (g->log_headed[i - 1] && ((g->log_head[i - 1] != NULL) == want_room)) {
            return i - 1;
        }
    }
    return (size_t)-1;
}
