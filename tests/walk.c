#include "walk.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The queue is also a set, and the set has to be reached by key. Without it the only
 * way to ask whether a state is already in the graph is to walk every state in it, so
 * a lookup costs the size of the search — heart spent 0.4 s inside that loop before
 * reaching the limit. Open addressing, one slot per entry twice over: the table is
 * sized once for the queue it serves and never grows. */
#define WALK_SLOT_N 16384u /* power of two, twice WALK_MAX_SEEN */
static int32_t *g_slot;    /* index into g_nodes, or -1 for an empty slot */

/* FNV-1a over exactly what same_state compares: room, finished and the first
 * flag_count flags. log_count and won are absent in both — the log is output rather
 * than state, and a state that has already won never enters the graph. The hash only
 * has to spread the keys; same_state decides identity, so a collision costs a probe
 * and nothing else. */
static uint64_t state_hash(const GameState *st) {
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    h = (h ^ (uint64_t)st->room) * 1099511628211ULL;
    h = (h ^ (uint64_t)st->finished) * 1099511628211ULL;
    for (i = 0; i < st->flag_count; i++) {
        h = (h ^ (uint64_t)st->flag_present[i]) * 1099511628211ULL;
    }
    return h;
}

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
    size_t i = (size_t)state_hash(st) & (WALK_SLOT_N - 1u);

    /* The whole queue, not just the expanded part. A state is room plus flags and that
     * is the whole of what the future depends on, so a second route to it cannot offer
     * anything the first did not. */
    while (g_slot[i] >= 0) {
        if (same_state(&g_nodes[g_slot[i]].st, st)) {
            return 1;
        }
        i = (i + 1u) & (WALK_SLOT_N - 1u);
    }
    return 0;
}

/* Puts a state where the lookup above stopped: nothing is ever removed, so a key's
 * probe sequence never changes underneath it. The winning state is not remembered —
 * it is queued without its `st` and the search ends on that same turn, so nothing
 * would ever read it back. */
static void remember(size_t idx) {
    size_t i = (size_t)state_hash(&g_nodes[idx].st) & (WALK_SLOT_N - 1u);

    while (g_slot[i] >= 0) {
        i = (i + 1u) & (WALK_SLOT_N - 1u);
    }
    g_slot[i] = (int32_t)idx;
}

static void nodes_free(void) {
    free(g_slot);
    g_slot = NULL;
    free(g_nodes);
    g_nodes = NULL;
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
            remember(g_nodes_n - 1);
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

int walk_layout_audit(Ui *ui, Game *g, const Script *s,
                       const char *const *titles, size_t titles_n) {
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
        /* The widest title among the games that ship. Passed in rather than reached
         * for: the registry belongs to the application, and a file that searches the
         * state graph has no business knowing which games exist. */
        for (i = 0; i < titles_n; i++) {
            int32_t tw = (int32_t)text_width(f, titles[i],
                                             (uint32_t)strlen(titles[i]));

            keep_worst(&w_ctrl, bw + MARGIN_X + tw, titles[i]);
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

int walk_run(Ui *ui, Game *g, const Script *s, const char *dir) {
    char path[1024];
    GameState start;
    int step;
    int steps = 0;

    g_nodes = malloc(WALK_MAX_SEEN * sizeof *g_nodes);
    g_slot = malloc(WALK_SLOT_N * sizeof *g_slot);
    if (g_nodes == NULL || g_slot == NULL) {
        fprintf(stderr, "не хватило памяти на очередь обхода\n");
        free(g_nodes);
        free(g_slot);
        g_nodes = NULL;
        g_slot = NULL;
        return 1;
    }
    for (step = 0; step < (int)WALK_SLOT_N; step++) {
        g_slot[step] = -1;
    }
    g_nodes_n = 0;
    g_head = 0;
    g_win = -1;
    game_save(g, &start);
    g_nodes[0].st = start;
    g_nodes[0].parent = -1;
    g_nodes[0].word_len = 0;
    g_nodes_n = 1;
    remember(0);

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
            fprintf(stderr, "победа не найдена: обход дошёл до предела в %lu состояний. "
                            "Это не значит, что её нет — нужен больший предел\n",
                    (unsigned long)WALK_MAX_SEEN);
        } else {
            fprintf(stderr, "этим скриптом нельзя выиграть: перебраны все %lu состояний\n",
                    (unsigned long)g_nodes_n);
        }
        nodes_free();
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
                nodes_free();
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
    nodes_free();
    printf("кадров записано: %d\n", steps + 2);
    if (ui->overfull > 0) {
        printf("кадров не влезло в 480: %d\n", ui->overfull);
    } else {
        printf("все полосы вместили свой текст\n");
    }
    return 0;
}
