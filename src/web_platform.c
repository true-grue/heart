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
 * One space, not two: every coordinate the backend reports and every size it asks the
 * view to be is in canvas backing pixels, that is CSS pixels times devicePixelRatio.
 * The same factor that sizes the canvas multiplies pointer coordinates, and the view
 * is set to the backing size, so io_poll's conversion and the drawing scale agree by
 * construction rather than by being kept in step. */

typedef struct WebState {
    int32_t w, h;      /* backing store size, which is also what the view is set to */
    double dpr;        /* canvas pixels per CSS pixel */
    uint32_t *stage;   /* window sized, 0xRRGGBB, filled by io_scale_canvas */
    size_t stage_cap;
} WebState;

/* Native events are queued here and drained by pump, rather than posted from the
 * callbacks directly: io_backend_open has not handed the context to the backend yet,
 * so at the moment a callback can first fire there may be nowhere to post to. */
#define WEB_QUEUE 64

static WebState g_web;
static IoEvent g_queue[WEB_QUEUE];
static unsigned g_queue_n;

/* Creates or reuses the canvas emscripten's shell page already contains, sizes it to
 * the backing store, and keeps the browser from turning a drag into a scroll. */
EM_JS(int, web_canvas_open, (int w, int h, double dpr), {
    var c = document.getElementById("canvas");
    if (!c) {
        c = document.createElement("canvas");
        c.id = "canvas";
        document.body.appendChild(c);
    }
    c.width = w;
    c.height = h;
    c.style.width = (w / dpr) + "px";
    c.style.height = (h / dpr) + "px";
    c.style.touchAction = "none";
    c.style.display = "block";
    c.style.margin = "0 auto";
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

/* The canvas is sized on open and again on a resize. Both go through here so the
 * backing store and the view cannot be set to different numbers. */
static void web_resize(int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    g_web.w = w;
    g_web.h = h;
    web_canvas_open(w, h, g_web.dpr);
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
static EM_BOOL web_resize_cb(int type, const EmscriptenUiEvent *ev, void *user) {
    (void)type;
    (void)user;
    web_resize((int32_t)((double)ev->windowInnerWidth * g_web.dpr),
               (int32_t)((double)ev->windowInnerHeight * g_web.dpr));
    return true;
}

static int web_open(void *self, const char *title, int32_t w, int32_t h) {
    WebState *st = (WebState *)self;
    const char *canvas = "#canvas";

    (void)title;
    st->dpr = emscripten_get_device_pixel_ratio();
    if (st->dpr <= 0.0) {
        st->dpr = 1.0;
    }
    st->w = 0;
    st->h = 0;
    st->stage = NULL;
    st->stage_cap = 0;
    g_queue_n = 0;

    /* io_backend_open passes the view size, so the window opens at the size the caller
     * asked for; the canvas it shows is the virtual canvas scaled into the backing
     * store, which is that size in device pixels. */
    web_resize((int32_t)((double)w * st->dpr), (int32_t)((double)h * st->dpr));

    emscripten_set_mousedown_callback(canvas, NULL, 0, web_mouse_cb);
    emscripten_set_mouseup_callback(canvas, NULL, 0, web_mouse_cb);
    emscripten_set_mousemove_callback(canvas, NULL, 0, web_mouse_cb);
    emscripten_set_touchstart_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_touchend_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_touchcancel_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_touchmove_callback(canvas, NULL, 0, web_touch_cb);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 0,
                                   web_resize_cb);
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
    unsigned i;

    (void)self;
    /* The view is in backing pixels and the application set it to the size it asked
     * for. Setting it here, before the first event is read, is what makes a click and
     * a drawn pixel the same place when devicePixelRatio is not 1. */
    io_set_view(ctx, g_web.w, g_web.h);
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
