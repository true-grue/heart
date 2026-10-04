#include "io.h"

#include <string.h>

/* ------------------------------------------------------------------ init -- */

static IoRect letterbox(int32_t dst_w, int32_t dst_h, int32_t cw, int32_t ch);

int io_init(IoCtx *ctx, uint32_t *pixels, int32_t w, int32_t h) {
    IoRect full;

    if (ctx == NULL || pixels == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->pixels = pixels;
    ctx->w = w;
    ctx->h = h;
    full.x = 0;
    full.y = 0;
    full.w = w;
    full.h = h;
    ctx->clip[0] = full;
    ctx->clip_n = 1;
    ctx->view_w = w;
    ctx->view_h = h;
    /* A one to one mapping until a backend reports a real window, and it comes out of
     * this on its own: the canvas matches the window because the two numbers are equal,
     * not because anything was cached while they were. */
    return 1;
}

IoRect io_clip(const IoCtx *ctx) {
    return ctx->clip[ctx->clip_n - 1];
}

void io_push_clip(IoCtx *ctx, IoRect r) {
    IoRect cur = io_clip(ctx);
    int32_t right;
    int32_t bottom;

    if (ctx->clip_n >= IO_CLIP_MAX) {
        return;
    }
    /* A nested clip intersects its parent, so a child cannot draw outside it. */
    right = (r.x + r.w < cur.x + cur.w) ? (r.x + r.w) : (cur.x + cur.w);
    bottom = (r.y + r.h < cur.y + cur.h) ? (r.y + r.h) : (cur.y + cur.h);
    r.x = (r.x > cur.x) ? r.x : cur.x;
    r.y = (r.y > cur.y) ? r.y : cur.y;
    r.w = (right > r.x) ? (right - r.x) : 0;
    r.h = (bottom > r.y) ? (bottom - r.y) : 0;
    ctx->clip[ctx->clip_n++] = r;
}

void io_pop_clip(IoCtx *ctx) {
    if (ctx->clip_n > 1) {
        ctx->clip_n--;
    }
}

/* Where the canvas sits inside the window. Aspect ratio is preserved and the
 * surplus becomes bars. Stretching would defeat the point of virtual
 * coordinates: the game's proportions would end up depending on the window
 * shape, so it would look different on every platform and every monitor.
 *
 * Scaling and pointer conversion must agree exactly, or a tap lands somewhere
 * other than where it was drawn, so both go through this one function. */
static IoRect letterbox(int32_t dst_w, int32_t dst_h, int32_t cw, int32_t ch) {
    IoRect r;
    int32_t vw, vh;

    if ((int64_t)dst_w * ch > (int64_t)dst_h * cw) {
        /* The window is relatively wider, so the height binds. */
        vh = dst_h;
        vw = (int32_t)(((int64_t)dst_h * cw) / ch);
    } else {
        vw = dst_w;
        vh = (int32_t)(((int64_t)dst_w * ch) / cw);
    }
    if (vw < 1) {
        vw = 1;
    }
    if (vh < 1) {
        vh = 1;
    }
    /* Rounding must never produce a rectangle larger than the window, or the
     * centred offset goes negative and the writes leave the buffer. */
    if (vw > dst_w) {
        vw = dst_w;
    }
    if (vh > dst_h) {
        vh = dst_h;
    }
    r.x = (dst_w - vw) / 2;
    r.y = (dst_h - vh) / 2;
    r.w = vw;
    r.h = vh;
    return r;
}

void io_set_view(IoCtx *ctx, int32_t view_w, int32_t view_h) {
    if (ctx == NULL || view_w <= 0 || view_h <= 0) {
        return;
    }
    /* Only the size is recorded. The canvas rectangle inside the window is worked out
     * where it is needed, because a copy of it is a second answer to the same question
     * and the two answers drift the moment one place is updated and the other is not. */
    ctx->view_w = view_w;
    ctx->view_h = view_h;
}

void io_to_virtual(const IoCtx *ctx, int32_t win_x, int32_t win_y,
                   int32_t *out_x, int32_t *out_y) {
    IoRect v;

    /* The outputs are written on every path: callers pass locals straight into
     * state, and a partial write would leave them indeterminate. */
    if (out_x != NULL) {
        *out_x = 0;
    }
    if (out_y != NULL) {
        *out_y = 0;
    }
    if (ctx == NULL) {
        return;
    }
    v = letterbox(ctx->view_w, ctx->view_h, ctx->w, ctx->h);
    if (v.w <= 0 || v.h <= 0) {
        return;
    }
    /* The canvas origin sits at the view rectangle origin, so the offset is
     * subtracted before scaling and not added afterwards. Adding it back put
     * every tap inside the letterbox bars up to a full bar width off. */
    if (out_x != NULL) {
        *out_x = (int32_t)(((int64_t)(win_x - v.x) * ctx->w) / v.w);
    }
    if (out_y != NULL) {
        *out_y = (int32_t)(((int64_t)(win_y - v.y) * ctx->h) / v.h);
    }
}

void io_scale_canvas(const IoCtx *ctx, uint32_t *dst, int32_t dst_w, int32_t dst_h) {
    int32_t vw, vh, ox, oy, x, y, sy, yerr;
    int32_t last_sy;
    int64_t step_x;
    IoRect view;

    if (ctx == NULL || dst == NULL || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    view = letterbox(dst_w, dst_h, ctx->w, ctx->h);
    vw = view.w;
    vh = view.h;
    ox = view.x;
    oy = view.y;
    last_sy = ctx->h - 1;

    /* Bars only need clearing when there are any, which is the uncommon case
     * for a window that matches the canvas aspect. */
    if (vw != dst_w || vh != dst_h) {
        for (y = 0; y < dst_h; y++) {
            memset(dst + (size_t)y * (size_t)dst_w, 0, (size_t)dst_w * sizeof(uint32_t));
        }
    }

    /* Every destination pixel is the average of the source pixels that fall into it.
     *
     * Nearest neighbour at a whole number is an exact doubling, and that case is left
     * exactly as it was: at the size the window is normally opened, nothing softens and
     * the font stays crisp.
     *
     * At a fractional scale nearest neighbour has to either stretch some source pixels
     * over two destinations or skip them entirely, and it cannot do both consistently.
     * The result is that some rows of pixels are drawn twice as tall as their
     * neighbours and some are missing outright, which on a sixteen pixel font reads as
     * letters cut in half. That is not a defect of the font and not of Windows; it was
     * measured here at 1.9281 when the window was created from its outer size. Averaging
     * cannot drop anything, so the letters stay whole at whatever size the window is
     * dragged to.
     *
     * Stepping is done with fixed point accumulators rather than a division per pixel,
     * for the reason the comment above used to carry: the obvious form measured at 25 ms
     * for a 1920x1080 window.
     */
    /* One division per axis for the whole frame, not one per pixel: the per pixel form
     * is what used to cost 25 ms at 1920x1080. */
    step_x = ((int64_t)ctx->w << 16) / vw;
    sy = 0;
    yerr = 0;
    for (y = 0; y < vh; y++) {
        uint32_t *out = dst + (size_t)(y + oy) * (size_t)dst_w + (size_t)ox;
        int64_t x0 = 0;
        int64_t x1 = step_x;

        for (x = 0; x < vw; x++) {
            int32_t s0 = (int32_t)(x0 >> 16);
            int32_t s1 = (int32_t)((x1 + 0xFFFF) >> 16);
            uint32_t acc[3] = { 0, 0, 0 };
            uint32_t n = 0;
            int32_t ry;
            int32_t rx;

            if (s1 <= s0) {
                s1 = s0 + 1;
            }
            if (s1 > ctx->w) {
                s1 = ctx->w;
            }
            for (ry = sy; ry <= sy + (yerr / vh) && ry < ctx->h; ry++) {
                const uint32_t *row = ctx->pixels + (size_t)ry * (size_t)ctx->w;

                for (rx = s0; rx < s1; rx++) {
                    uint32_t p = row[rx];

                    acc[0] += (p >> 16) & 0xFFu;
                    acc[1] += (p >> 8) & 0xFFu;
                    acc[2] += p & 0xFFu;
                    n++;
                }
            }
            if (n == 0) {
                out[x] = ctx->pixels[(size_t)sy * (size_t)ctx->w + (size_t)s0];
            } else {
                out[x] = ((((acc[0] + n / 2) / n) & 0xFFu) << 16) |
                         ((((acc[1] + n / 2) / n) & 0xFFu) << 8) |
                         (((acc[2] + n / 2) / n) & 0xFFu);
            }
            x0 += step_x;
            x1 += step_x;
        }
        yerr += ctx->h;
        while (yerr >= vh && sy < last_sy) {
            yerr -= vh;
            sy++;
        }
        if (sy > last_sy) {
            sy = last_sy;
        }
    }
}

/* --------------------------------------------------------- fixed point -- */

/* Division truncates toward zero, so floor and ceil are spelled out rather than
 * relying on the sign of a right shift. */
static int32_t fx_floor(IoFixed v) {
    int32_t q = v / IO_FX_ONE;
    if (v % IO_FX_ONE < 0) {
        q--;
    }
    return q;
}

static int32_t fx_ceil(IoFixed v) {
    int32_t q = v / IO_FX_ONE;
    if (v % IO_FX_ONE > 0) {
        q++;
    }
    return q;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        return lo;
    }
    return (v > hi) ? hi : v;
}

static void put_px(IoCtx *ctx, int32_t x, int32_t y, IoColor c) {
    ctx->pixels[(size_t)y * (size_t)ctx->w + (size_t)x] =
        ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

/* --------------------------------------------------------- scanline fill -- */

static int crosses(const IoSeg *e, IoFixed yc) {
    /* The half-open test makes a vertex count once per scanline, not twice. */
    return (e->y0 <= yc && e->y1 > yc) || (e->y1 <= yc && e->y0 > yc);
}

static int count_crossings(const IoSeg *edges, uint32_t n, IoFixed yc) {
    uint32_t i;
    int c = 0;

    for (i = 0; i < n; i++) {
        if (edges[i].y0 != edges[i].y1 && crosses(&edges[i], yc)) {
            c++;
        }
    }
    return c;
}

static void fill_span(IoCtx *ctx, int32_t row, IoFixed xs, IoFixed xe, IoColor color) {
    IoRect clip = io_clip(ctx);
    int32_t x0 = fx_ceil(xs - IO_FX_HALF);
    int32_t x1 = fx_ceil(xe - IO_FX_HALF) - 1;

    /* Without antialiasing a sub-pixel span would vanish and flicker as it
     * moves, so any span claims at least one pixel. */
    if (x1 < x0) {
        x0 = fx_floor(xs + (xe - xs) / 2);
        x1 = x0;
    }
    x0 = clamp_i32(x0, clip.x, clip.x + clip.w - 1);
    x1 = clamp_i32(x1, clip.x, clip.x + clip.w - 1);
    while (x0 <= x1) {
        put_px(ctx, x0, row, color);
        x0++;
    }
}

/* Every edge axis aligned and every corner a whole pixel: such a polygon needs
 * no coverage and can be filled exactly. */
static int poly_is_crisp(const IoSeg *edges, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        const IoSeg *e = &edges[i];
        if (e->x0 != e->x1 && e->y0 != e->y1) {
            return 0;
        }
        if ((e->x0 % IO_FX_ONE) || (e->y0 % IO_FX_ONE) ||
            (e->x1 % IO_FX_ONE) || (e->y1 % IO_FX_ONE)) {
            return 0;
        }
    }
    return 1;
}

/* Computes the rows a polygon touches, in screen space, and the sample
 * position to use when the shape is too thin to hold a pixel centre. */
static void poly_rows(IoCtx *ctx, const IoSeg *edges, uint32_t n, IoFixed dy,
                       int32_t *row_lo, int32_t *row_hi, int *thin, IoFixed *mid) {
    const IoSeg *e0 = &edges[0];
    IoFixed ymin = e0->y0 + dy;
    IoFixed ymax = e0->y1 + dy;
    IoRect clip = io_clip(ctx);
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (edges[i].y0 + dy < ymin) ymin = edges[i].y0 + dy;
        if (edges[i].y1 + dy < ymin) ymin = edges[i].y1 + dy;
        if (edges[i].y0 + dy > ymax) ymax = edges[i].y0 + dy;
        if (edges[i].y1 + dy > ymax) ymax = edges[i].y1 + dy;
    }

    *row_lo = fx_ceil(ymin - IO_FX_HALF);
    *row_hi = fx_ceil(ymax - IO_FX_HALF) - 1;
    *thin = 0;
    *mid = 0;
    if (*row_hi < *row_lo) {
        /* Too thin to contain a pixel centre anywhere: sample the middle, so the
         * feature claims a row instead of flickering out of existence. */
        *thin = 1;
        *mid = (ymin + ymax) / 2;
        *row_lo = fx_floor(*mid);
        *row_hi = *row_lo;
    }
    *row_lo = clamp_i32(*row_lo, clip.y, clip.y + clip.h - 1);
    *row_hi = clamp_i32(*row_hi, clip.y, clip.y + clip.h - 1);
}

/* Gathers and sorts the crossings of one sample line. Returns how many. */
static int gather_crossings(IoCtx *ctx, const IoSeg *edges, uint32_t n, IoFixed yc) {
    uint32_t i;
    int nc = 0;
    int j;

    for (i = 0; i < n && nc < IO_POLY_MAX_CROSS; i++) {
        const IoSeg *e = &edges[i];
        if (e->y0 == e->y1 || !crosses(e, yc)) {
            continue;
        }
        ctx->cx[nc] = e->x0 + (IoFixed)(((int64_t)(e->x1 - e->x0) *
                                         (int64_t)(yc - e->y0)) / (int64_t)(e->y1 - e->y0));
        ctx->cdir[nc] = (e->y1 > e->y0) ? (int8_t)1 : (int8_t)-1;
        nc++;
    }
    /* Insertion sort: a sample line crosses only a handful of edges. */
    for (i = 1; i < (uint32_t)nc; i++) {
        IoFixed kx = ctx->cx[i];
        int8_t kd = ctx->cdir[i];
        j = (int)i - 1;
        while (j >= 0 && ctx->cx[j] > kx) {
            ctx->cx[j + 1] = ctx->cx[j];
            ctx->cdir[j + 1] = ctx->cdir[j];
            j--;
        }
        ctx->cx[j + 1] = kx;
        ctx->cdir[j + 1] = kd;
    }
    return nc;
}

static int fill_exact(IoCtx *ctx, const IoSeg *edges, uint32_t n, IoFixed dx, IoFixed dy,
                      IoColor color, int32_t row_lo, int32_t row_hi, int thin, IoFixed mid) {
    int32_t row;

    for (row = row_lo; row <= row_hi; row++) {
        IoFixed yc = (thin ? mid : (IoFixed)row * IO_FX_ONE + IO_FX_HALF) - dy;
        int nc = gather_crossings(ctx, edges, n, yc);
        int winding = 0;
        int j;
        IoFixed span_start = 0;

        if (nc == 0) {
            continue;
        }
        for (j = 0; j < nc; j++) {
            int before = winding;
            winding += ctx->cdir[j];
            if (before == 0 && winding != 0) {
                span_start = ctx->cx[j] + dx;
            } else if (before != 0 && winding == 0) {
                fill_span(ctx, row, span_start, ctx->cx[j] + dx, color);
            }
        }
    }
    return 1;
}

static void blend_px(IoCtx *ctx, int32_t x, int32_t y, IoColor c, uint32_t cov) {
    uint32_t inv = IO_COV_ONE - cov;
    uint32_t d = ctx->pixels[(size_t)y * (size_t)ctx->w + (size_t)x];
    uint32_t r = ((uint32_t)c.r * cov + ((d >> 16) & 0xFFu) * inv) >> 8;
    uint32_t g = ((uint32_t)c.g * cov + ((d >> 8) & 0xFFu) * inv) >> 8;
    uint32_t b = ((uint32_t)c.b * cov + (d & 0xFFu) * inv) >> 8;

    ctx->pixels[(size_t)y * (size_t)ctx->w + (size_t)x] = (r << 16) | (g << 8) | b;
}

/* Adds a span's horizontal coverage. A pixel touched by two of the vertical
 * samples accumulates twice, which is what makes a diagonal edge read as grey
 * rather than as a staircase. */
static void add_span(IoCtx *ctx, int32_t x_lo, int32_t x_hi, IoFixed xs, IoFixed xe) {
    IoFixed x = xs;
    int32_t px = fx_floor(xs);

    if (px < x_lo) {
        px = x_lo;
    }
    while (x < xe && px <= x_hi) {
        IoFixed left = (x > (IoFixed)px * IO_FX_ONE) ? x : (IoFixed)px * IO_FX_ONE;
        IoFixed right = ((IoFixed)(px + 1) * IO_FX_ONE < xe) ? (IoFixed)(px + 1) * IO_FX_ONE : xe;
        uint32_t add = (uint32_t)(((right - left) * (IO_COV_ONE / IO_AA_SAMPLES)) >> 16);
        uint32_t v = (uint32_t)ctx->rowcov[px] + add;

        ctx->rowcov[px] = (uint16_t)(v > IO_COV_ONE ? IO_COV_ONE : v);
        x = right;
        px++;
    }
}

static int fill_covered(IoCtx *ctx, const IoSeg *edges, uint32_t n, IoFixed dx, IoFixed dy,
                        IoColor color, int32_t row_lo, int32_t row_hi, int thin, IoFixed mid) {
    IoRect clip = io_clip(ctx);
    int32_t row, x;
    int s;
    IoFixed clip_l = (IoFixed)clip.x * IO_FX_ONE;
    IoFixed clip_r = (IoFixed)(clip.x + clip.w) * IO_FX_ONE;

    for (row = row_lo; row <= row_hi; row++) {
        memset(ctx->rowcov + clip.x, 0, (size_t)clip.w * sizeof(uint16_t));
        for (s = 0; s < IO_AA_SAMPLES; s++) {
            /* Fixed eighths of the row. Spread over the covered slice instead,
             * which looks tempting, but then all four samples land inside a thin
             * shape and it comes out opaque, which is the opposite of what the
             * antialiasing is for. Thin shapes are handled by the row fallback in
             * poly_rows, which is the right trade for a one pixel rule: you asked
             * for a hairline, you get a solid hairline rather than a grey one. */
            IoFixed yc = (thin ? mid
                               : ((IoFixed)row * IO_FX_ONE +
                                  (IoFixed)(2 * s + 1) * IO_AA_STEP)) - dy;
            int nc = gather_crossings(ctx, edges, n, yc);
            int winding = 0;
            int j;
            IoFixed span_start = 0;

            for (j = 0; j < nc; j++) {
                int before = winding;
                winding += ctx->cdir[j];
                if (before == 0 && winding != 0) {
                    span_start = ctx->cx[j] + dx;
                } else if (before != 0 && winding == 0) {
                    IoFixed xs = span_start;
                    IoFixed xe = ctx->cx[j] + dx;

                    if (xs < clip_l) xs = clip_l;
                    if (xe > clip_r) xe = clip_r;
                    if (xs < xe) {
                        add_span(ctx, clip.x, clip.x + clip.w - 1, xs, xe);
                    }
                }
            }
        }
        for (x = clip.x; x < clip.x + clip.w; x++) {
            if (ctx->rowcov[x] != 0) {
                blend_px(ctx, x, row, color, ctx->rowcov[x]);
            }
        }
    }
    return 1;
}

int io_fill_poly(IoCtx *ctx, const IoSeg *edges, uint32_t n, IoFixed dx, IoFixed dy,
                 IoColor color) {
    int32_t row_lo, row_hi, row;
    int thin;
    IoFixed mid;

    if (ctx == NULL || edges == NULL || n == 0) {
        return 0;
    }
    poly_rows(ctx, edges, n, dy, &row_lo, &row_hi, &thin, &mid);

    /* Budget check before anything is drawn, so an oversized polygon is never
     * left half painted. The extremes are enough: a sample line between them
     * crosses no more edges than they do. */
    if (poly_is_crisp(edges, n) || ctx->w > IO_COVER_MAX) {
        for (row = row_lo; row <= row_hi; row++) {
            IoFixed yc = (thin ? mid : (IoFixed)row * IO_FX_ONE + IO_FX_HALF) - dy;
            if (count_crossings(edges, n, yc) > IO_POLY_MAX_CROSS) {
                return 0;
            }
        }
    } else {
        for (row = row_lo; row <= row_hi; row++) {
            IoFixed base = (IoFixed)row * IO_FX_ONE;
            if (count_crossings(edges, n, base - dy) > IO_POLY_MAX_CROSS ||
                count_crossings(edges, n, base + IO_FX_ONE - dy) > IO_POLY_MAX_CROSS) {
                return 0;
            }
        }
    }

    if (poly_is_crisp(edges, n) || ctx->w > IO_COVER_MAX) {
        return fill_exact(ctx, edges, n, dx, dy, color, row_lo, row_hi, thin, mid);
    }
    return fill_covered(ctx, edges, n, dx, dy, color, row_lo, row_hi, thin, mid);
}

int io_fill_rect(IoCtx *ctx, IoRect r, IoColor color) {
    IoSeg e[4];
    IoFixed x0 = io_fx(r.x);
    IoFixed y0 = io_fx(r.y);
    IoFixed x1 = io_fx(r.x + r.w);
    IoFixed y1 = io_fx(r.y + r.h);

    e[0].x0 = x0; e[0].y0 = y0; e[0].x1 = x1; e[0].y1 = y0;
    e[1].x0 = x1; e[1].y0 = y0; e[1].x1 = x1; e[1].y1 = y1;
    e[2].x0 = x1; e[2].y0 = y1; e[2].x1 = x0; e[2].y1 = y1;
    e[3].x0 = x0; e[3].y0 = y1; e[3].x1 = x0; e[3].y1 = y0;
    return io_fill_poly(ctx, e, 4, 0, 0, color);
}

/* ----------------------------------------------------------------- input -- */

void io_post_event(IoCtx *ctx, const IoEvent *ev) {
    if (ctx == NULL || ev == NULL || ev->kind == IO_EV_NONE) {
        return;
    }
    if (ctx->ring_n >= IO_EVENT_MAX) {
        /* Dropping silently would hide lost input, so it is counted. */
        ctx->dropped++;
        return;
    }
    ctx->ring[ctx->ring_n++] = *ev;
}

int io_next_event(IoCtx *ctx, IoEvent *out) {
    if (ctx == NULL || out == NULL || ctx->ring_read >= ctx->ring_n) {
        return 0;
    }
    *out = ctx->ring[ctx->ring_read++];
    if (ctx->ring_read == ctx->ring_n) {
        ctx->ring_read = 0;
        ctx->ring_n = 0;
    }
    return 1;
}

unsigned io_dropped_events(const IoCtx *ctx) {
    return (ctx == NULL) ? 0u : ctx->dropped;
}

int io_button_held(const IoCtx *ctx, uint8_t button) {
    if (ctx == NULL || button > IO_BUTTON_MIDDLE) {
        return 0;
    }
    return ctx->button_down[button] != 0;
}

int32_t io_pointer_x(const IoCtx *ctx) {
    return (ctx == NULL) ? 0 : ctx->pointer_x;
}

int32_t io_pointer_y(const IoCtx *ctx) {
    return (ctx == NULL) ? 0 : ctx->pointer_y;
}

IoDevice io_pointer_device(const IoCtx *ctx) {
    return (ctx == NULL) ? IO_DEVICE_MOUSE : ctx->pointer_device;
}

int32_t io_drag_x(const IoCtx *ctx) {
    return (ctx == NULL) ? 0 : ctx->pointer_down_x;
}

int32_t io_drag_y(const IoCtx *ctx) {
    return (ctx == NULL) ? 0 : ctx->pointer_down_y;
}

void io_poll(IoCtx *ctx, int timeout_ms) {
    IoEvent ev;
    int i;

    if (ctx == NULL) {
        return;
    }
    if (ctx->backend != NULL && ctx->backend->wait != NULL) {
        ctx->backend->wait(ctx->backend->self, timeout_ms);
    }
    if (ctx->backend != NULL && ctx->backend->pump != NULL) {
        ctx->backend->pump(ctx->backend->self, ctx);
    }
    /* Read the pending events without consuming them: the application reads the
     * same queue through io_next_event, and consuming it here would starve it. */
    for (i = ctx->ring_read; i < ctx->ring_n; i++) {
        IoEvent *q = &ctx->ring[i];

        if (q->kind == IO_EV_POINTER_DOWN || q->kind == IO_EV_POINTER_UP ||
            q->kind == IO_EV_POINTER_MOVE) {
            /* Backends report window pixels, and converting here means nothing
             * above this layer ever has to know the window exists. The queued
             * event is rewritten in place, so the application, the accessors and
             * the drag origin all speak virtual coordinates and only the backend
             * speaks window ones. */
            io_to_virtual(ctx, q->x, q->y, &q->x, &q->y);
        }
        ev = *q;
        switch (ev.kind) {
        case IO_EV_POINTER_DOWN:
            if (ev.button <= IO_BUTTON_MIDDLE) {
                ctx->button_down[ev.button] = 1;
            }
            /* Remembered so a drag can be told from a tap on a moving target. */
            ctx->pointer_down_x = ev.x;
            ctx->pointer_down_y = ev.y;
            ctx->pointer_device = ev.device;
            break;
        case IO_EV_POINTER_UP:
            if (ev.button <= IO_BUTTON_MIDDLE) {
                ctx->button_down[ev.button] = 0;
            }
            ctx->pointer_device = ev.device;
            break;
        case IO_EV_POINTER_MOVE:
            ctx->pointer_device = ev.device;
            break;
        default:
            break;
        }
        if (ev.kind == IO_EV_POINTER_DOWN || ev.kind == IO_EV_POINTER_UP ||
            ev.kind == IO_EV_POINTER_MOVE) {
            ctx->pointer_x = ev.x;
            ctx->pointer_y = ev.y;
        }
    }
}

/* --------------------------------------------------------------- backend -- */

int io_backend_open(IoCtx *ctx, const IoBackend *backend, const char *title) {
    int rc;

    if (ctx == NULL || backend == NULL || backend->open == NULL) {
        return 0;
    }
    /* The window is sized from the view, not from the virtual canvas. */
    rc = backend->open(backend->self, title, ctx->view_w, ctx->view_h);
    if (rc != 0) {
        ctx->backend = backend;
    }
    return rc;
}

void io_backend_close(IoCtx *ctx) {
    if (ctx->backend != NULL && ctx->backend->close != NULL) {
        ctx->backend->close(ctx->backend->self);
    }
    ctx->backend = NULL;
}
