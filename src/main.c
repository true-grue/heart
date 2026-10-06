/* Must precede every include: a feature-test macro consulted after <stdio.h> is no
 * macro at all. musl declares clock_gettime either way and hides the breakage. */
#define _POSIX_C_SOURCE 199309L

#include "walk.h"

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
 * The command is built from slots filled left to right; the empty one is being filled
 * and its choices sit on the row below. The slot count is an upper bound, not the shape:
 * nothing here assumes two.
 */


/* Band geometry is decided per frame, not fixed here: system bands grow down from the
 * top, play bands up from the bottom, the description takes the rest, and the answer is
 * the one that gives way, because it already scrolls its tail. */
/* Equals RULE_MAX_WORDS: a command the screen cannot show is a command the player
 * cannot type. */

#define TYPE_CPS 45



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

/* Index into the log of the last block of the given heading kind. Named for what it
 * returns: it used to read like "the last room" and was compared against g->room, a room
 * symbol -- two different numbers, so the comparison was true almost every frame and
 * entering a room flashed a highlight until the next redraw. */


/* One block joined into a single paragraph: a room description is written as several
 * says and must not read as several, so lines are glued with a space. */

/* Break goes after the last space that fits, so words stay whole; a word wider than
 * the row overhangs rather than being cut. */

/* Largest codepoint boundary at or before `upto`, so typing never ends mid-letter. */

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

/* Machine-written format: a line that does not fit is corrupt, and is skipped. */
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
    /* Clear the log before entering, so the room is the only thing on screen and
     * entering prints the title and the fragments the restored flags call for. */
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



/* ---------------------------------------------------------- interaction -- */

static int inside(IoRect r, int32_t x, int32_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* The grammar fuzzer's seam: read one script, print what the loader made of it. Every
 * line is a fact rather than a pointer, so two machines agree and a diff means something. */
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
        free(text);
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
    printf("rooms %lu\n", (unsigned long)sc.room_count);
    printf("rules %lu\n", (unsigned long)sc.rule_count);
    printf("flags %lu\n", (unsigned long)sc.sym_count);
    errors = script_validate(&sc, d, sizeof d / sizeof d[0], &n);
    printf("errors %lu\n", (unsigned long)errors);
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

/* A per-game build compiles its name in and runs with no argument; plain quest asks. */
#ifndef GAME_DEFAULT
#define GAME_DEFAULT "tutorial"
#endif

static const GameDef *pick_game(int argc, char **argv) {
    size_t i;
    /* First argument that is not a flag is the game: a per-game build carries its own
     * name, so `heart --walk out` must work; argv[1] blindly would read "--walk". */
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
    int walking = 0;
    const char *walk_dir = NULL;
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

    /* The walkthrough draws into the framebuffer and never reads input, so it runs on the
     * test backend: no window, no display. The flag is searched for, not read at argv[2] --
     * `heart --walk out` is three arguments, and insisting on four opened a window on the
     * one build where nobody has a display. */
    {
        int a;
        for (a = 1; a < argc; a++) {
            if (strcmp(argv[a], "--walk") == 0 && a + 1 < argc) {
                walking = 1;
                walk_dir = argv[a + 1];
                break;
            }
        }
    }
    {
        /* Same frame report stress prints, so the two can be compared. Off unless
         * asked: a game printing its own rate every second is a game nobody ships. */
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
        /* Before the window: the audit draws nothing and needs no display. */
    {
        static const char *titles[16];
        size_t tn = 0;
        size_t ti;
        int rc;

        for (ti = 0; ti < sizeof k_games / sizeof k_games[0] && ti < 16; ti++) {
            titles[ti] = k_games[ti].title;
            tn++;
        }
        rc = walk_layout_audit(&ui, &game, &script, titles, tn);
        return rc;
    }
    }
    if (!io_backend_open(&ctx, walking ? &io_backend_test : io_platform_backend(),
                         walking ? def->path : def->title)) {
        fprintf(stderr, "не удалось открыть окно (задан ли DISPLAY?)\n");
        return 1;
    }
    if (walking) {
        int rc = walk_run(&ui, &game, &script, walk_dir);

        io_backend_close(&ctx);
        /* The walkthrough returns from here, so script_text never reached the free at the
         * end of main -- valgrind pointed at those 4.5 kB. */
        free(script_text);
        return rc;
    }

    /* Redraw only when something can have changed: a static screen repainted anyway
     * costs a full pass over the window, which is the difference between a game at rest
     * and a fan running. */
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

            /* Any event at all, including the unused ones: a redraw is cheaper than
             * deciding which events count. */
            dirty = 1;

            if (ev.kind == IO_EV_QUIT) {
                running = 0;
                break;
            }
            if (ev.kind != IO_EV_POINTER_UP) {
                continue;
            }
            {
                /* A tap finishes a still-typing answer: there is no keyboard to press,
                 * and waiting one out is the one thing a finger should not have to do. */
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
                    /* The save on disk is left alone on purpose: Загрузить still brings
                     * the old run back, so a misclick costs a walk, not the game. */
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
                    /* Over when no word can follow; length comes from the script. */
                    if (game_more(&game, ui.cmd.slot, (size_t)ui.cmd.filled) == 0) {
                        game_command(&game, ui.cmd.slot, (size_t)ui.cmd.filled);
                        /* Mark what the command brought in; it stays marked until the
                         * player does something else. */
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

        /* Measured after the events, not before: the frame a command lands on is the one
         * that has to start typing it, and a length read earlier comes back as nothing. */
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
            /* Fixed time constant, so scroll reads the same at any frame rate; a
             * fraction of a line of lag is the point, the block glides. */
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
            /* Fixed rate, and a late frame must not catch up by printing the rest: the
             * step is capped at 40 ms, well under the loop's 250 ms dt cap, so one frame
             * cannot print more than a word and a half whatever the machine was doing. */
            double step = (dt > 40.0) ? 40.0 : dt;

            ui.typed += TYPE_CPS * step / 1000.0;
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
        /* Counted outside the redraw, so a still screen reports zero instead of nothing. */
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