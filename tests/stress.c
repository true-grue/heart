#define _POSIX_C_SOURCE 199309L

#include "io.h"
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Demo: pointer-driven window with an animation, kept honest as a stress test.
 *
 * The first version of this demo had three defects worth not repeating. It
 * repainted every frame even when nothing had changed, it used the input poll
 * timeout as its frame clock, and it quit on Escape. Hardware keys are gone now,
 * so the demo answers only to the pointer: it quits on the window close button
 * or on the Quit button drawn inside the canvas.
 *
 * The animation exists to make trouble visible. Motion is integrated against
 * real elapsed time, so a frame the demo fails to deliver shows up as the marker
 * jumping further than the other markers, and the status line reports the cost of
 * the frame that was actually late. */

#define DEMO_W 640          /* the virtual canvas, fixed for the session */
#define DEMO_H 480
#define DEMO_SCALE 2        /* the window opens at this multiple of it */
#define TARGET_FPS 60
#define FRAME_MS (1000.0 / TARGET_FPS)
#define STEP 32             /* simulation runs at a fixed 1000/STEP Hz */

#define FONT_PATH "assets/font/sans.font"

#define PANEL_X 16
#define PANEL_Y 110
#define PANEL_W 608
#define PANEL_H 200

static const char *const kLines[] = {
    "Прыгающий квадрат отскакивает от краёв. Тяните его мышью.",
    "Кадры считаются по времени, а не по числу вызовов:",
    "пропущенный кадр виден как рывок, а не как задержка."
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

/* ---------------------------------------------------------- simulation -- */

/* Position and velocity in 16.16 fixed point. Floating point would be easier and
 * would also make the motion depend on rounding, which is exactly the sort of
 * thing that looks like a rendering bug later. */
typedef struct {
    int32_t x, y;       /* top left, 16.16 */
    int32_t vx, vy;     /* pixels per second, 16.16 */
    IoColor rgb;
} Marker;

#define MARKER_PX 24
#define ONE 65536

static void marker_step(Marker *m, int32_t left, int32_t top, int32_t w, int32_t h,
                        int32_t dt_ms) {
    int32_t dx = (int32_t)(((int64_t)m->vx * dt_ms) / 1000);
    int32_t dy = (int32_t)(((int64_t)m->vy * dt_ms) / 1000);
    int32_t max_x = (left + w - MARKER_PX) * ONE;
    int32_t max_y = (top + h - MARKER_PX) * ONE;

    m->x += dx;
    m->y += dy;
    if (m->x < left * ONE) {
        m->x = left * ONE;
        m->vx = -m->vx;
    } else if (m->x > max_x) {
        m->x = max_x;
        m->vx = -m->vx;
    }
    if (m->y < top * ONE) {
        m->y = top * ONE;
        m->vy = -m->vy;
    } else if (m->y > max_y) {
        m->y = max_y;
        m->vy = -m->vy;
    }
}

static int point_in_rect(int32_t x, int32_t y, IoRect r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* -------------------------------------------------------------- render -- */

static void draw(IoCtx *ctx, const TextFont *font, const Marker *m,
                 unsigned long frame_total, double fps, double worst_ms,
                 unsigned dropped, int dragging, IoRect quit) {
    char status[192];
    int i;
    int32_t y = 30;
    int n;

    io_fill_rect(ctx, (IoRect){ 0, 0, DEMO_W, DEMO_H }, IO_RGB(24, 26, 32));
    io_fill_rect(ctx, (IoRect){ 16, 12, DEMO_W - 32, DEMO_H - 24 }, IO_RGB(32, 35, 43));

    /* The Quit button is drawn as plainly as possible because there is no hover
     * on a touch screen: anything that only appears on pointer entry is invisible
     * to half the targets. */
    io_fill_rect(ctx, quit, IO_RGB(92, 46, 46));
    if (point_in_rect(io_pointer_x(ctx), io_pointer_y(ctx), quit)) {
        io_fill_rect(ctx, (IoRect){ quit.x + 1, quit.y + 1, quit.w - 2, quit.h - 2 },
                     IO_RGB(126, 60, 60));
    }
    n = snprintf(status, sizeof status, "QUIT");
    text_draw(ctx, font, quit.x + (quit.w - 34) / 2, quit.y + 5, status, (uint32_t)n,
              IO_RGB(238, 226, 226));

    for (i = 0; i < (int)(sizeof kLines / sizeof kLines[0]); i++) {
        text_draw(ctx, font, 30, y, kLines[i], (uint32_t)strlen(kLines[i]),
                  IO_RGB(214, 212, 206));
        y += font->line_height;
    }

    io_fill_rect(ctx, (IoRect){ PANEL_X, PANEL_Y, PANEL_W, PANEL_H }, IO_RGB(20, 21, 26));
    io_fill_rect(ctx, (IoRect){ PANEL_X + 5, PANEL_Y + 5, 2, PANEL_H - 10 }, IO_RGB(58, 60, 70));
    io_fill_rect(ctx, (IoRect){ PANEL_X + 5, PANEL_Y + 5, PANEL_W - 10, 2 }, IO_RGB(58, 60, 70));

    /* The clip stack is the part most likely to break silently, so the marker is
     * drawn through it and the frame lines are drawn inside the same region. */
    io_push_clip(ctx, (IoRect){ PANEL_X, PANEL_Y, PANEL_W, PANEL_H });
    {
        int32_t i2;
        for (i2 = PANEL_X + 30; i2 < PANEL_X + PANEL_W; i2 += 30) {
            io_fill_rect(ctx, (IoRect){ i2, PANEL_Y, 1, PANEL_H }, IO_RGB(30, 32, 38));
        }
        io_fill_rect(ctx, (IoRect){ m->x >> 16, m->y >> 16, MARKER_PX, MARKER_PX },
                     m->rgb);
        if (dragging) {
            io_fill_rect(ctx, (IoRect){ m->x >> 16, m->y >> 16, MARKER_PX, 2 },
                         IO_RGB(250, 250, 250));
        }
    }
    io_pop_clip(ctx);

    n = snprintf(status, sizeof status,
                 "кадр %lu   %.1f fps   худший кадр %.1f ms   потеряно событий %u",
                 frame_total, fps, worst_ms, dropped);
    text_draw(ctx, font, 30, PANEL_Y + PANEL_H + 14, status, (uint32_t)n,
              IO_RGB(176, 180, 190));

    if (io_pointer_device(ctx) == IO_DEVICE_MOUSE) {
        int32_t px = io_pointer_x(ctx);
        int32_t py = io_pointer_y(ctx);
        io_fill_rect(ctx, (IoRect){ px, py - 7, 1, 15 }, IO_RGB(120, 200, 160));
        io_fill_rect(ctx, (IoRect){ px - 7, py, 15, 1 }, IO_RGB(120, 200, 160));
    }
}

int main(void) {
    static _Alignas(16) unsigned char arena_mem[1 << 20];
    static uint32_t pixels[DEMO_W * DEMO_H];
    Arena arena;
    IoCtx ctx;
    TextFont font;
    char *font_text;
    size_t font_len = 0;
    const IoRect quit = { 552, 380, 72, 26 };
    const IoRect field = { PANEL_X + 2, PANEL_Y + 2, PANEL_W - 4, PANEL_H - 4 };
    Marker m;
    int running = 1;
    int dragging = 0;
    unsigned long frame_total = 0;
    double last = now_ms();
    double next_frame = last + FRAME_MS;
    double acc = 0.0;
    double t_report = last, worst = 0.0;
    unsigned long frames_since_report = 0;
    double fps_now = 0.0;
    unsigned dropped_at_start;

    if (arena_init(&arena, arena_mem, sizeof arena_mem) != 0) {
        fprintf(stderr, "arena failed\n");
        return 1;
    }
    font_text = slurp(FONT_PATH, &font_len);
    if (font_text == NULL) {
        fprintf(stderr, "cannot read %s\n", FONT_PATH);
        return 1;
    }
    if (text_font_load(&arena, &font, font_text, font_len, 18) != TEXT_OK) {
        fprintf(stderr, "font failed\n");
        return 1;
    }
    free(font_text);
    font_text = NULL;

    if (!io_init(&ctx, pixels, DEMO_W, DEMO_H)) {
        fprintf(stderr, "io_init failed\n");
        return 1;
    }
    io_set_view(&ctx, DEMO_W * DEMO_SCALE, DEMO_H * DEMO_SCALE);
    if (!io_backend_open(&ctx, io_platform_backend(), "quest stress")) {
        fprintf(stderr, "cannot open a window (is DISPLAY set?)\n");
        return 1;
    }
    dropped_at_start = io_dropped_events(&ctx);

    m.x = (PANEL_X + 40) * ONE;
    m.y = (PANEL_Y + 40) * ONE;
    m.vx = 150 * ONE;
    m.vy = 105 * ONE;
    m.rgb = IO_RGB(232, 178, 96);

    while (running) {
        IoEvent ev;
        double t0 = now_ms();
        double frame_ms;
        int timeout = (int)(next_frame - t0);
        uint32_t hsv;

        if (timeout < 0) {
            timeout = 0;
        }
        io_poll(&ctx, timeout);

        while (io_next_event(&ctx, &ev)) {
            switch (ev.kind) {
            case IO_EV_POINTER_DOWN:
                /* Grabbing anywhere in the field keeps the target big; a finger
                 * is not accurate enough for a 24 pixel marker. */
                if (point_in_rect(ev.x, ev.y, field)) {
                    dragging = 1;
                }
                break;
            case IO_EV_POINTER_MOVE:
                if (dragging) {
                    m.x = (ev.x - MARKER_PX / 2) * ONE;
                    m.y = (ev.y - MARKER_PX / 2) * ONE;
                    if (m.x < field.x * ONE) m.x = field.x * ONE;
                    if (m.y < field.y * ONE) m.y = field.y * ONE;
                    if (m.x > (field.x + field.w - MARKER_PX) * ONE) {
                        m.x = (field.x + field.w - MARKER_PX) * ONE;
                    }
                    if (m.y > (field.y + field.h - MARKER_PX) * ONE) {
                        m.y = (field.y + field.h - MARKER_PX) * ONE;
                    }
                }
                break;
            case IO_EV_POINTER_UP:
                /* Buttons act on release, which is what a tap means on a touch
                 * screen: the finger may well land down and slide off. */
                if (dragging) {
                    dragging = 0;
                } else if (point_in_rect(ev.x, ev.y, quit)) {
                    running = 0;
                }
                break;
            case IO_EV_QUIT:
                running = 0;
                break;
            default:
                break;
            }
        }

        /* Fixed timestep against real elapsed time. Without this the animation
         * would silently run at whatever rate the renderer happened to manage,
         * and a dropped frame would look like a smooth but wrong speed. */
        frame_ms = t0 - last;
        last = t0;
        if (frame_ms > 250.0) {
            frame_ms = 250.0;
        }
        if (frame_ms > worst) {
            worst = frame_ms;
        }
        acc += frame_ms;
        while (acc >= STEP) {
            marker_step(&m, field.x, field.y, field.w, field.h, STEP);
            acc -= STEP;
        }
        hsv = (uint32_t)(m.x >> 8) + (uint32_t)(m.y >> 8);
        m.rgb = IO_RGB(200u + (hsv % 40u), 170u, 90u + (hsv % 100u));

        draw(&ctx, &font, &m, frame_total, fps_now, worst,
             io_dropped_events(&ctx) - dropped_at_start, dragging, quit);
        if (ctx.backend != NULL && ctx.backend->present != NULL) {
            ctx.backend->present(ctx.backend->self, &ctx);
        }
        frame_total++;
        frames_since_report++;

        next_frame += FRAME_MS;
        if (next_frame < now_ms()) {
            /* Fell behind: resynchronise rather than accumulate a backlog that
             * would keep every later frame late. */
            next_frame = now_ms() + FRAME_MS;
        }
        if (now_ms() - t_report >= 1000.0) {
            double span = now_ms() - t_report;
            fps_now = frames_since_report * 1000.0 / span;
            fprintf(stderr, "%dx%d  %5.1f fps  худший кадр %6.1f ms  событий потеряно %u\n",
                    ctx.view_w, ctx.view_h, fps_now, worst,
                    io_dropped_events(&ctx) - dropped_at_start);
            t_report = now_ms();
            worst = 0.0;
            frames_since_report = 0;
        }
    }
    io_backend_close(&ctx);
    return 0;
}