#include "io.h"

#ifdef IO_WEB

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/html5.h>

/* Browser backend. The frame loop in the application is a while loop that blocks in
 * wait(), and a browser has no way to block: control must return to the event loop or
 * no input event ever arrives again. emscripten_sleep() is that return, and it exists
 * only under ASYNCIFY, which is why the Makefile names it for this build and nothing
 * else. main() is not touched, and it is not supposed to be.
 *
 * One space, not two, and which space depends on the path: in release the view is the
 * virtual canvas and the browser enlarges it, under the test key the view is the pinned
 * resolution and the browser does nothing at all. Either way this factor turns a browser
 * coordinate in CSS pixels into a backing pixel, and the view is set to the backing size,
 * so io_poll's conversion and the drawing scale agree by construction. */

typedef struct WebState {
    int32_t w, h;      /* backing store size, which is also what the view is set to */
    int32_t vw, vh;    /* virtual canvas, from the context; the backing store in release */
    int32_t fixed_w, fixed_h;  /* test key asked for a resolution: browser scales nothing */
    int32_t win_w, win_h;      /* window in CSS pixels, for the letterbox */
    int32_t css_w, css_h;      /* box the canvas is shown in, to notice a change */
    int settle;                /* frames left to keep remeasuring the window */
    double dpr;        /* backing pixels per CSS pixel */
    uint32_t *stage;   /* backing sized, 0xRRGGBB, filled by io_scale_canvas */
    size_t stage_cap;
} WebState;

/* Native events are queued here and drained by pump, rather than posted from the
 * callbacks directly: io_backend_open has not handed the context to the backend yet,
 * so at the moment a callback can first fire there may be nowhere to post to. */
#define WEB_QUEUE 64

static WebState g_web;
static IoEvent g_queue[WEB_QUEUE];
static unsigned g_queue_n;

/* Creates or reuses the canvas emscripten's shell page already contains, sizes the
 * backing store and the box the browser shows it in, and keeps the browser from turning
 * a drag into a scroll.
 *
 * image-rendering is pixelated because the game smooths nothing anywhere else: letting the
 * browser interpolate would be the one place a pixel got blurred. */
EM_JS(int, web_canvas_open, (int w, int h, int css_w, int css_h, int crisp), {
    var c = document.getElementById("canvas");
    if (!c) {
        c = document.createElement("canvas");
        c.id = "canvas";
        document.body.appendChild(c);
    }
    /* Assigning width or height clears the canvas, and the application only redraws when
     * the view changes, which on a resize it does not: the backing store is the same
     * 640x480 it always is. Setting them again anyway left a black rectangle on screen. */
    if (c.width !== w) {
        c.width = w;
    }
    if (c.height !== h) {
        c.height = h;
    }
    c.style.width = css_w + "px";
    c.style.height = css_h + "px";
    /* Nearest neighbour while the factor is whole, which is every case but a window
       smaller than the canvas; there the canvas is shrinking and dropping every other
       pixel would lose more than smoothing costs. */
    c.style.imageRendering = crisp ? "pixelated" : "auto";
    /* Centred here rather than by the page's layout: a whole-number scale leaves bars of
     * up to a whole row and column, and they have to end up the same on both sides. Fullscreen
     * is the document and not the canvas, so the page layout is what holds in both modes.
     *
     * The layout viewport, not window.innerWidth, and that is not fussiness: the size the
     * canvas was fitted to comes from documentElement.clientHeight, and the two are the
     * same number on a desktop and different on a phone, where innerHeight counts the
     * address bar that slides away. Fitting to one and centring in the other left the
     * field sitting off centre after a fullscreen toggle, and only on Android. */
    var pw = document.documentElement.clientWidth;
    var ph = document.documentElement.clientHeight;
    c.style.position = "absolute";
    c.style.left = Math.max(0, Math.floor((pw - css_w) / 2)) + "px";
    c.style.top = Math.max(0, Math.floor((ph - css_h) / 2)) + "px";
    /* none, so the only gestures that reach this game are the game's own. A browser free
     * to pinch and pan the page underneath a canvas sized in whole multiples turns the
     * scale into whatever the last gesture left, which is the one thing the integer
     * multiplier exists to prevent. */
    c.style.display = "block";
    c.style.touchAction = "none";
    var s = document.getElementById("quest_web_css");
    if (!s) {
        s = document.createElement("style");
        s.id = "quest_web_css";
        s.textContent = "html,body{overflow:hidden;background:#000;margin:0;}";
        document.head.appendChild(s);
    }
    Module.questCtx = c.getContext("2d", { alpha: false });
    return 1;
})

/* The test key: ?canvas=1280x960 pins the backing store and the box it is shown in to the
 * same numbers, so the browser scales nothing and what the page shows is what the native
 * build draws at that resolution. Absent, the release path runs and the browser enlarges
 * the canvas itself. */
EM_JS(int, web_key_size, (int which), {
    var m = /[?&]canvas=([0-9]+)x([0-9]+)/.exec(location.search);
    if (!m) {
        return 0;
    }
    var v = parseInt(m[which === 0 ? 1 : 2], 10);
    return v > 0 ? v : 0;
})

/* The window in CSS pixels: what the release path fits the canvas into. Two functions and
 * not one with int* out-parameters, because those do not survive the trip into the
 * browser: the pointer arrives as the number it was pointing at and the write lands
 * somewhere in the heap. Measured, not reasoned about: it returned 0 and 216900000 where
 * the window was 1280x817, and the canvas was then sized from a number that was never a
 * size. It reads the layout viewport, not window.innerWidth, because a two finger zoom
 * changes the visual viewport and would otherwise come back as a resize and move the
 * scale while the fingers are still down. */
EM_JS(int, web_window_w, (void), { return document.documentElement.clientWidth | 0; })
EM_JS(int, web_window_h, (void), { return document.documentElement.clientHeight | 0; })

/* Canvas ImageData is RGBA, a 0xRRGGBB word in little endian memory is B,G,R,0, and
 * alpha is opaque everywhere. Byte order is the platform's business: the framebuffer is
 * only read here, never written. */
EM_JS(void, web_blit, (int w, int h, void *ptr), {
    var src = ptr;
    /* The buffer is in scope here as HEAPU8; Module.HEAPU8 is a compatibility
     * property that this build does not set, and reading it yields undefined. */
    var u8 = HEAPU8;
    var n = w * h;
    var d = new Uint8ClampedArray(n * 4);
    for (var i = 0, j = 0; i < n; i++, src += 4, j += 4) {
        d[j] = u8[src + 2];
        d[j + 1] = u8[src + 1];
        d[j + 2] = u8[src];
        d[j + 3] = 255;
    }
    Module.questCtx.putImageData(new ImageData(d, w, h), 0, 0);
})

/* The canvas is sized on open and again on a resize, and both go through here so the
 * backing store, the box it is shown in and the view cannot end up as three different
 * numbers.
 *
 * Release: the backing store is the virtual canvas and the box is that canvas times a
 * whole number. The browser does the enlarging, so nothing in the heap depends on how big
 * the screen is and a large window cannot ask for memory the page does not have. The
 * factor is a whole number because a fractional one would make the browser interpolate
 * and put unequal pixels side by side, which is the one thing this game never does to a
 * pixel. The bars are the page background, and they are centred rather than pushed to the
 * top, because a whole-number factor leaves up to a whole row of them.
 *
 * Test key: both are the requested resolution, so the browser does nothing at all. */
static void web_resize(void) {
    WebState *st = &g_web;
    int32_t css_w, css_h;
    int crisp = 1;

    if (st->vw <= 0 || st->vh <= 0) {
        return;
    }
    if (st->win_w <= 0 || st->win_h <= 0) {
        /* The window could not be measured, so the size the caller asked for is not used
         * as a stand-in: that is GAME_W*SCALE, which is 1280x960 and hangs off a window
         * smaller than itself. One to one is the only fallback that cannot be cropped. */
        st->win_w = st->vw;
        st->win_h = st->vh;
    }
    if (st->fixed_w > 0 && st->fixed_h > 0) {
        css_w = st->fixed_w;
        css_h = st->fixed_h;
        st->w = st->fixed_w;
        st->h = st->fixed_h;
    } else {
        int32_t k = st->win_w / st->vw;
        int32_t kh = st->win_h / st->vh;

        if (kh < k) {
            k = kh;
        }
        if (k >= 1) {
            css_w = st->vw * k;
            css_h = st->vh * k;
        } else {
            /* The window is smaller than the canvas, which is every phone held upright.
               No whole number fits here, so the canvas takes the fraction that does, and
               nearest neighbour is switched off: dropping every other pixel while
               shrinking loses more than the smoothing it costs. */
            double fx = (double)st->win_w / (double)st->vw;
            double fy = (double)st->win_h / (double)st->vh;
            double f = (fx < fy) ? fx : fy;

            css_w = (int32_t)((double)st->vw * f + 0.5);
            css_h = (int32_t)((double)st->vh * f + 0.5);
            crisp = 0;
        }
        st->w = st->vw;
        st->h = st->vh;
    }
    if (css_w == st->css_w && css_h == st->css_h) {
        return;   /* nothing moved, and touching the DOM every frame would be waste */
    }
    st->css_w = css_w;
    st->css_h = css_h;
    st->dpr = (double)st->w / (double)css_w;
    web_canvas_open(st->w, st->h, css_w, css_h, crisp);
}

/* Browser coordinates are CSS pixels from the top left of the canvas; the view is in
 * backing pixels. */
static void web_queue(int kind, double cx, double cy, IoDevice device, uint8_t button) {
    IoEvent *ev;

    if (g_queue_n >= WEB_QUEUE) {
        return;
    }
    ev = &g_queue[g_queue_n++];
    memset(ev, 0, sizeof *ev);
    ev->kind = (uint8_t)kind;
    ev->x = (int32_t)(cx * g_web.dpr);
    ev->y = (int32_t)(cy * g_web.dpr);
    ev->device = device;
    ev->button = button;
}

static EM_BOOL web_mouse_cb(int type, const EmscriptenMouseEvent *ev, void *user) {
    int kind;

    (void)user;
    switch (type) {
    case EMSCRIPTEN_EVENT_MOUSEDOWN: kind = IO_EV_POINTER_DOWN; break;
    case EMSCRIPTEN_EVENT_MOUSEUP:   kind = IO_EV_POINTER_UP;   break;
    default:                         kind = IO_EV_POINTER_MOVE; break;
    }
    if (kind == IO_EV_POINTER_MOVE && ev->buttons == 0) {
        /* A move with nothing held is a hover, and this game has no hover state. */
        return true;
    }
    web_queue(kind, (double)ev->targetX, (double)ev->targetY, IO_DEVICE_MOUSE,
              (uint8_t)(kind == IO_EV_POINTER_MOVE ? 0 : (ev->button - 1)));
    return true;
}

/* Touch carries several contacts; only the first is used, which matches an interface
 * that already treats the pointer as a single one. */
static EM_BOOL web_touch_cb(int type, const EmscriptenTouchEvent *ev, void *user) {
    int kind;

    (void)user;
    if (ev->numTouches < 1) {
        return true;
    }
    switch (type) {
    case EMSCRIPTEN_EVENT_TOUCHSTART:              kind = IO_EV_POINTER_DOWN; break;
    case EMSCRIPTEN_EVENT_TOUCHEND:
    case EMSCRIPTEN_EVENT_TOUCHCANCEL:             kind = IO_EV_POINTER_UP;   break;
    default:                                       kind = IO_EV_POINTER_MOVE; break;
    }
    web_queue(kind, (double)ev->touches[0].targetX,
              (double)ev->touches[0].targetY, IO_DEVICE_TOUCH, 0);
    return true;
}

/* The window is the browser window, so a resize is the page becoming a different
 * size. The canvas fills it and the view follows; nothing else hears about it. */
#define WEB_SETTLE 8      /* frames of remeasuring after the window moves */

static EM_BOOL web_resize_cb(int type, const EmscriptenUiEvent *ev, void *user) {
    (void)type;
    (void)user;
    if (ev->windowInnerWidth > 0 && ev->windowInnerHeight > 0) {
        g_web.win_w = (int32_t)ev->windowInnerWidth;
        g_web.win_h = (int32_t)ev->windowInnerHeight;
    }
    g_web.settle = WEB_SETTLE;
    web_resize();
    return true;
}

/* A phone turning its screen says which way it is turning and nothing about how big it
 * will be, so this only starts the remeasuring; the size arrives a frame later. */
static EM_BOOL web_orientation_cb(int type, const EmscriptenOrientationChangeEvent *ev,
                                  void *user) {
    (void)type;
    (void)ev;
    (void)user;
    g_web.settle = WEB_SETTLE;
    web_resize();
    return true;
}

static int web_open(void *self, const char *title, int32_t w, int32_t h) {
    WebState *st = (WebState *)self;
    const char *canvas = "#canvas";

    (void)title;
    (void)w;
    (void)h;
    st->stage = NULL;
    st->stage_cap = 0;
    g_queue_n = 0;

    st->fixed_w = web_key_size(0);
    st->fixed_h = web_key_size(1);
    /* Nothing is taken from the size the caller asked for. That number is GAME_W*SCALE,
     * and its only meaning here was how big a window to open; the window belongs to the
     * browser, and measuring it is the only thing that decides how many times the canvas
     * is enlarged. Left at zero so that a failure to measure falls back to one to one
     * rather than to a window-sized box on a window that is not that big. */
    st->win_w = 0;
    st->win_h = 0;
    st->settle = WEB_SETTLE;

    /* The canvas cannot be sized here: what it is sized from is the virtual canvas, and
     * the only place that is known is the context, which the backend does not get until
     * the first pump. The window is measured there. */

    emscripten_set_mousedown_callback(canvas, NULL, 0, web_mouse_cb);
    emscripten_set_mouseup_callback(canvas, NULL, 0, web_mouse_cb);
    emscripten_set_mousemove_callback(canvas, NULL, 0, web_mouse_cb);
    emscripten_set_touchstart_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_touchend_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_touchcancel_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_touchmove_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0,
                                   web_resize_cb);
    emscripten_set_orientationchange_callback(NULL, 0, web_orientation_cb);
    return 1;
}

static void web_present(void *self, IoCtx *ctx) {
    WebState *st = (WebState *)self;
    size_t n = (size_t)st->w * (size_t)st->h;

    if (n == 0) {
        return;
    }
    if (st->stage_cap < n) {
        free(st->stage);
        st->stage = (uint32_t *)malloc(n * sizeof(uint32_t));
        st->stage_cap = (st->stage != NULL) ? n : 0;
        if (st->stage == NULL) {
            return;
        }
    }
    /* The virtual canvas is scaled to the backing store here, and only here. */
    io_scale_canvas(ctx, st->stage, st->w, st->h);
    web_blit(st->w, st->h, st->stage);
}

/* Nothing can sleep on a browser thread without handing the event loop back, so this
 * is the one place in the platform where the call does not return immediately.
 * Returning instead would turn every wait into a spin that never lets input in. */
static void web_wait(void *self, int timeout_ms) {
    (void)self;
    emscripten_sleep(timeout_ms > 0 ? timeout_ms : 1);
}

static int web_pump(void *self, IoCtx *ctx) {
    WebState *st = (WebState *)self;
    unsigned i;

    /* The window is remeasured for a few frames after every resize and after every
     * orientation change, because neither reports a size that can be used straight away:
     * a phone turning its screen announces the size it is leaving, and nothing fires a
     * second resize to say the new one. The canvas was therefore sized for the old
     * orientation and hung off the screen. Re-reading settles it within a frame or two,
     * and the counter stops the measuring rather than leaving it running forever. */
    if (st->settle > 0) {
        int ww = web_window_w();
        int wh = web_window_h();

        if (ww > 0 && wh > 0) {
            if (ww != st->win_w || wh != st->win_h) {
                st->win_w = ww;
                st->win_h = wh;
                st->settle = WEB_SETTLE;   /* still moving: go on watching */
            }
            st->settle--;
        }
    }
    /* The virtual canvas is the one thing needed to size the canvas, and only the context
     * knows it, so the first pump is where the canvas gets its size. Doing it here rather
     * than in open also puts io_set_view before the first event is read, which is what
     * makes a click and a drawn pixel the same place. */
    st->vw = ctx->w;
    st->vh = ctx->h;
    web_resize();
    io_set_view(ctx, st->w, st->h);
    for (i = 0; i < g_queue_n; i++) {
        io_post_event(ctx, &g_queue[i]);
    }
    g_queue_n = 0;
    return 1;
}

static void web_close(void *self) {
    WebState *st = (WebState *)self;

    free(st->stage);
    st->stage = NULL;
    st->stage_cap = 0;
    g_queue_n = 0;
}

static const IoBackend web_backend = {
    &g_web,
    web_open,
    web_present,
    web_wait,
    web_pump,
    web_close
};

const IoBackend *io_platform_backend(void) {
    return &web_backend;
}

#endif /* IO_WEB */
