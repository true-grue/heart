#define _POSIX_C_SOURCE 199309L

#include "ui.h"
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
 * Every ui_band is a fixed height in virtual pixels, so the layout is the layout on
 * every platform and at every window size.
 */


/* Band geometry is decided per frame, not fixed here. The system bands grow down from
 * the top, the play bands grow up from the bottom, and the description takes whatever
 * is left between them, so no ui_band is taller than its content and nothing is reserved
 * that goes unused. The answer is the one that gives way when a frame holds more than
 * the canvas has room for: it already scrolls its tail, so shrinking it costs least. */
/* The most words one command can have, and so the most slots the palette needs.
 * Equals RULE_MAX_WORDS: a command the screen cannot show is a command the player
 * cannot type. */

#define TYPE_CPS 45                /* characters a second while the answer types */



typedef struct GameDef {
    const char *key;
    const char *path;
    const char *title;
} GameDef;

static const GameDef k_games[] = {
    { "tutorial", "assets/script/tutorial.script", "Учебный квест" },
    { "field", "assets/script/field.script", "МЕЧ ИЗ ЗАМКА" },
    { "heart", "assets/script/heart.script", "Серое Сердце" }
};

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

/* Index into the log of the last block with the given kind of heading: a room block
 * carries its title, a command block the verb and object.
 *
 * Named for what it returns. It used to be called last_block, which reads like "the last
 * room", and it was compared against g->room, which is a room symbol. Those are different
 * numbers, the comparison was therefore true almost every frame, and the result was a
 * flash of highlight on entering a room that lasted until the next redraw. The name is
 * the only thing that stops it being made again. */

/* Where the block that starts at `from` ends: the next heading, or the log end. */

/* The lines of one block joined into a single paragraph. A room description is
 * written as several says and must not read as several paragraphs, so the lines
 * are glued with a space and wrapped as one piece of prose. */

/* Splits off the first visual row that fits in max_w and reports where the next
 * one starts. The break goes after the last space that fits, so words stay whole;
 * a single word wider than the row overhangs rather than being cut. */

/* Largest codepoint boundary at or before `upto`, so a partially typed answer
 * never ends inside a letter. */

/* ------------------------------------------------------------ drawing -- */

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

/* On the heap, not in the image. It was a static array of 8192 entries, and every entry
 * carries a whole game state, so the program carried fifty five megabytes of queue in
 * its bss for the entire run — including all the runs that never walk anything, which is
 * all of them except one. */
static Node *g_nodes;
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
 * Every width goes through the same ui_chip_w and text_width the drawing uses, so a row
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

/* The widest a row of word chips can get, given the words that could appear in it. */
static int32_t chips_width(const TextFont *f, const Script *s, const Sym *syms,
                           size_t n, const char *trailing) {
    char label[64];
    int32_t x = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t len = ui_chip_label_sym(s, syms[i], label, sizeof label);

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
    /* The floor, not a sample: one row of chips and the shortest answer the engine
     * will settle for, with nothing spare. Asking the layout instead means asking
     * about an empty description, which sizes the ui_band to nothing and then reports
     * that every room overflows a ui_band that in fact scrolls. */
    int32_t cmd_min_h = CMD_PAD + TILE_H + CMD_PAD + CHIP_H + CMD_PAD;
    int32_t desc_floor = GAME_H - CTRL_H - NAME_H - ITEMS_H - cmd_min_h - RESP_MIN_H;
    int32_t fit_desc = (desc_floor - 2 * PAD) / f->line_height;
    int32_t fit_resp = (RESP_MIN_H - 2 * PAD) / f->line_height;
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
         * description is the one ui_band that must never lose text, so it is checked
         * against the smallest ui_band the engine can be forced to give it: one row of
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
                                 ? ui_rows_of(f, room_para, (uint32_t)strlen(room_para),
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
        keep_worst(&w_desc, ui_rows_of(f, para, n, usable), what);

        /* Responses: the longest text any rule of this room can print. */
        for (j = 0; j < r->rule_len; j++) {
            const Rule *ru = &r->rules[j];

            if (ru->act.text_len == 0) {
                continue;
            }
            keep_worst(&w_resp, ui_rows_of(f, ru->act.text, ru->act.text_len, usable), what);
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
            uint32_t len = ui_chip_label_sym(s, (Sym)i, label, sizeof label);

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

    if (rooms_over > 0) {
        printf("\nкомнат, где описание не влезает: %d из %d, худшая — %s, %d строк при %d\n",
               rooms_over, (int)s->room_count, w_over.where, w_over.value, fit_desc);
    } else {
        printf("\nописание влезает в каждой из %d комнат\n", (int)s->room_count);
    }
        /* Width is no longer pass or fail. A row of chips too wide to fit wraps, and
         * the ui_band grows to hold it; the item strip does not wrap and says how many
         * things it is hiding. Each is reported on its own terms. */
        if (w_slots.value > usable - CHIP_ROW_RESERVE ||
            w_pick.value > usable - CHIP_ROW_RESERVE) {
            int32_t worst = (w_pick.value > w_slots.value) ? w_pick.value
                                                              : w_slots.value;
            int32_t row_px = usable - CHIP_ROW_RESERVE;

            printf("\nширокий ряд чипов не дефект: чипы переносятся, полоса растёт\n"
                   "  худшая ширина %d px при доступных %d px, строк чипов: %d\n",
                   worst, row_px, (worst + row_px - 1) / row_px);
        }
        if (w_items.value > usable - CHIP_ROW_RESERVE) {
            printf("\nполоса предметов не переносится, лишнее уходит в «+N» — так задумано\n"
                   "  видно примерно %d чипов из %d\n",
                   (usable - CHIP_ROW_RESERVE) / 64, w_items.value / 64);
        }
        if (w_desc.value > fit_desc) {
            printf("\nописание не влезает, %d строк при %d: хвост уезжает, обрезания нет\n",
                   w_desc.value, fit_desc);
            bad = 1;
        }
        if (w_resp.value > fit_resp) {
            printf("\nответ длиннее полосы, %d строк при %d: хвост уезжает, это по замыслу\n",
                   w_resp.value, fit_resp);
        }
        if (!bad) {
            printf("\nдефектов раскладки нет: ни одна полоса не теряет текст\n");
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

    g_nodes = malloc(WALK_MAX_SEEN * sizeof *g_nodes);
    if (g_nodes == NULL) {
        fprintf(stderr, "не хватило памяти на очередь обхода\n");
        return 1;
    }
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
        /* Two different facts, and they must not be reported as one. Running out of room
         * is not the same as there being nothing to find, and saying "cannot win" after
         * simply giving up is a confident falsehood: it is what made a grown script look
         * broken when the only thing that had happened was that the search got slower
         * than the box it ran in. */
        if (g_nodes_n >= WALK_MAX_SEEN) {
            fprintf(stderr, "победа не найдена: обход дошёл до предела в %zu состояний. "
                            "Это не значит, что её нет — нужен больший предел\n",
                    (size_t)WALK_MAX_SEEN);
        } else {
            fprintf(stderr, "этим скриптом нельзя выиграть: перебраны все %zu состояний\n",
                    g_nodes_n);
        }
        free(g_nodes);
        g_nodes = NULL;
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
            int *idx = malloc(WALK_MAX_SEEN * sizeof *idx);
            int m = 0;

            if (idx == NULL) {
                fprintf(stderr, "не хватило памяти на путь\n");
                free(g_nodes);
                g_nodes = NULL;
                return 1;
            }

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
            free(idx);
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
        ui_draw(ui);
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
            ui_mark_new_fragments(ui, g);
        }
    }
    free(g_nodes);
    g_nodes = NULL;
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
        /* The walkthrough returns from here, so the script text was never reaching the
         * free at the end of main. Four and a half kilobytes, which valgrind pointed at. */
        free(script_text);
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
                size_t here = ui_last_log_index(&game, 0);
                size_t n = (here != (size_t)-1)
                         ? ui_block_text(&game, here + 1, ui_block_end(&game, here), probe,
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
                        ui_mark_new_fragments(&ui, &game);
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
        cmd = ui_last_log_index(&game, 0);
        if (cmd != (size_t)-1) {
            char probe[PARA_MAX];
            answer = ui_block_text(&game, cmd + 1, ui_block_end(&game, cmd), probe,
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
            ui_draw(&ui);
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