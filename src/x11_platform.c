#include "io.h"

#ifdef IO_X11

#include "utf8.h"

#include <poll.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

/* X11 window backend. Fixed size and not resizable: the game renders at one
 * logical scale and lets the display enlarge the result, so resize and DPI
 * handling never reach the drawing code. */

typedef struct X11State {
    Display *dpy;
    Window win;
    GC gc;
    XImage *img;
    uint32_t *stage;   /* window sized: scaled canvas in 0xRRGGBB, then swapped */
    size_t stage_cap;
    int32_t w, h;      /* window size in physical pixels */
    int32_t img_w, img_h;  /* size the XImage was built for */
    Atom wm_delete;
} X11State;

static X11State g_x11;

/* Sleeps until X has something to say, so the frame loop costs nothing while
 * the window is idle. */
static void x11_wait(void *self, int timeout_ms) {
    X11State *st = (X11State *)self;
    struct pollfd pfd;

    if (st->dpy == NULL || XPending(st->dpy) > 0) {
        return;
    }
    pfd.fd = ConnectionNumber(st->dpy);
    pfd.events = POLLIN;
    pfd.revents = 0;
    poll(&pfd, 1, timeout_ms);
}

static int x11_pump(void *self, IoCtx *ctx) {
    X11State *st = (X11State *)self;

    while (XPending(st->dpy) > 0) {
        XEvent xe;
        IoEvent ev;

        XNextEvent(st->dpy, &xe);
        memset(&ev, 0, sizeof ev);
        ev.x = (int32_t)xe.xbutton.x;
        ev.y = (int32_t)xe.xbutton.y;

        switch (xe.type) {
        case ButtonPress:
        case ButtonRelease:
        case MotionNotify:
            /* Buttons 4 to 7 are the scroll wheel in X11, and buttons above 8
             * are back and forward. There is no wheel event yet, so letting
             * them through would hand the application invented buttons. */
            if (xe.type != MotionNotify &&
                (xe.xbutton.button < 1 || xe.xbutton.button > 3)) {
                break;
            }
            if (xe.type == ButtonPress) {
                ev.kind = IO_EV_POINTER_DOWN;
            } else if (xe.type == ButtonRelease) {
                ev.kind = IO_EV_POINTER_UP;
            } else {
                ev.kind = IO_EV_POINTER_MOVE;
                ev.button = 0;
            }
            if (ev.kind != IO_EV_POINTER_MOVE) {
                ev.button = (uint8_t)(xe.xbutton.button - 1u);
            }
            ev.device = IO_DEVICE_MOUSE;
            io_post_event(ctx, &ev);
            break;
        case ConfigureNotify:
            /* The window is resizable, so the canvas stays virtual and only the
             * view size changes. Nothing in the drawing code hears about it. */
            if (xe.xconfigure.width != st->w || xe.xconfigure.height != st->h) {
                st->w = xe.xconfigure.width;
                st->h = xe.xconfigure.height;
                io_set_view(ctx, st->w, st->h);
            }
            break;
        case ClientMessage:
            if ((Atom)xe.xclient.data.l[0] == st->wm_delete) {
                ev.kind = IO_EV_QUIT;
                io_post_event(ctx, &ev);
            }
            break;
        default:
            break;
        }
    }
    return 1;
}

static int x11_open(void *self, const char *title, int32_t w, int32_t h) {
    X11State *st = (X11State *)self;
    XSetWindowAttributes attr;
    XSizeHints hints;
    XWMHints *wmh;
    XClassHint *cls;
    long mask;

    st->dpy = XOpenDisplay(NULL);
    if (st->dpy == NULL) {
        return 0;
    }
    /* io_backend_open passes the view size, so the window opens at the size the
     * caller asked for; the canvas it shows is ctx->w by ctx->h. */
    st->w = w;
    st->h = h;

    memset(&attr, 0, sizeof attr);
    attr.background_pixel = BlackPixel(st->dpy, DefaultScreen(st->dpy));
    /* No keyboard mask: hardware keys are not an input path for this game. */
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | StructureNotifyMask;
    mask = CWBackPixel | CWEventMask;
    st->win = XCreateWindow(st->dpy, RootWindow(st->dpy, DefaultScreen(st->dpy)), 0, 0,
                            (unsigned)w, (unsigned)h, 0, CopyFromParent, InputOutput,
                            CopyFromParent, CWBackPixel | CWEventMask, &attr);
    (void)mask;

    /* Resizable: a minimum keeps the window from becoming useless, but nothing
     * pins it, and ConfigureNotify only ever changes the view size. */
    memset(&hints, 0, sizeof hints);
    hints.flags = PMinSize;
    hints.min_width = 160;
    hints.min_height = 90;
    XSetWMNormalHints(st->dpy, st->win, &hints);

    wmh = XAllocWMHints();
    wmh->flags = InputHint | StateHint;
    wmh->input = False;
    wmh->initial_state = NormalState;
    XSetWMHints(st->dpy, st->win, wmh);
    XFree(wmh);

    cls = XAllocClassHint();
    cls->res_name = (char *)"quest";
    cls->res_class = (char *)"Quest";
    XSetClassHint(st->dpy, st->win, cls);
    XFree(cls);

    if (title != NULL) {
        XStoreName(st->dpy, st->win, title);
    }
    st->wm_delete = XInternAtom(st->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(st->dpy, st->win, &st->wm_delete, 1);

    st->gc = XCreateGC(st->dpy, st->win, 0, NULL);
    st->stage = NULL;
    st->stage_cap = 0;
    st->img = NULL;
    st->img_w = 0;
    st->img_h = 0;
    XMapWindow(st->dpy, st->win);
    XFlush(st->dpy);
    return 1;
}

static void x11_present(void *self, IoCtx *ctx) {
    X11State *st = (X11State *)self;
    size_t n = (size_t)st->w * (size_t)st->h;

    /* The XImage points at our staging buffer, so the image must be destroyed
     * before that buffer is freed. Doing it the other way round leaves Xlib
     * holding a dangling pointer, which is exactly the kind of thing that only
     * shows up as an ASan DEADLYSIGNAL on someone else's machine. */
    if (st->img != NULL && (st->img_w != st->w || st->img_h != st->h)) {
        st->img->data = NULL;
        XDestroyImage(st->img);
        st->img = NULL;
        st->img_w = 0;
        st->img_h = 0;
    }
    if (st->stage_cap < n) {
        free(st->stage);
        st->stage = (uint32_t *)malloc(n * sizeof(uint32_t));
        st->stage_cap = (st->stage != NULL) ? n : 0;
        if (st->stage == NULL) {
            return;
        }
    }
    if (st->img == NULL) {
        st->img_w = st->w;
        st->img_h = st->h;
        st->img = XCreateImage(st->dpy, DefaultVisual(st->dpy, DefaultScreen(st->dpy)),
                               DefaultDepth(st->dpy, DefaultScreen(st->dpy)), ZPixmap, 0,
                               (char *)st->stage, (unsigned)st->w, (unsigned)st->h, 32, 0);
        if (st->img == NULL) {
            return;
        }
        st->img->bytes_per_line = (int)(st->w * 4);
    }
    /* The virtual canvas is scaled to the window here, and only here. */
    io_scale_canvas(ctx, st->stage, st->w, st->h);
    /* No channel swap: a 0xRRGGBB word in little endian memory is already the
     * bytes B,G,R,0 that a depth 24 XImage wants. Swapping here used to cost a
     * full pass over the window and exchanged red with blue at the same time,
     * which no amount of white-on-grey text would have revealed. */
    XPutImage(st->dpy, st->win, st->gc, st->img, 0, 0, 0, 0,
              (unsigned)st->w, (unsigned)st->h);
    XFlush(st->dpy);
}

static void x11_close(void *self) {
    X11State *st = (X11State *)self;

    if (st->dpy == NULL) {
        return;
    }
    if (st->img != NULL) {
        st->img->data = NULL;
        XDestroyImage(st->img);
        st->img = NULL;
    }
    free(st->stage);
    st->stage = NULL;
    st->stage_cap = 0;
    st->img_w = 0;
    st->img_h = 0;
    XFreeGC(st->dpy, st->gc);
    XDestroyWindow(st->dpy, st->win);
    XCloseDisplay(st->dpy);
    st->dpy = NULL;
}

static const IoBackend x11_backend = {
    &g_x11,
    x11_open,
    x11_present,
    x11_wait,
    x11_pump,
    x11_close
};

/* The platform entry point for this build. With no DISPLAY there is no window to
 * hand back, and reporting that beats dying inside XOpenDisplay. */
const IoBackend *io_platform_backend(void) {
    if (getenv("DISPLAY") == NULL) {
        return NULL;
    }
    return &x11_backend;
}

#endif /* IO_X11 */
