#ifndef IO_IO_H
#define IO_IO_H

#include "arena.h"

#include <stddef.h>
#include <stdint.h>

/* Software framebuffer with one drawing primitive: filled polygons.
 *
 * All drawing happens in virtual coordinates: the canvas is a fixed size chosen
 * by the game and never changes, whatever the window happens to be. The window
 * size only decides how the canvas is scaled on the way to the screen, so the
 * same game state produces the same pixels on every platform and at every
 * window size, which is also what makes golden images exact.
 *
 * Coordinates are 16.16 fixed point, rasterisation is integer only, and edges are
 * sampled at pixel centres, so a shape produces the same pixels on every machine
 * and compiler. There is no antialiasing: edges are hard, and a shape never
 * disappears, so any sub-pixel feature still claims one pixel.
 *
 * Coordinates are screen space, origin top-left, y down, so pixels[y * w + x]
 * needs no flipping.
 */

typedef int32_t IoFixed;

#define IO_FX_ONE 65536
#define IO_FX_HALF 32768

static inline IoFixed io_fx(int32_t v) {
    return (IoFixed)(v * IO_FX_ONE);
}

typedef struct { int32_t x, y, w, h; } IoRect;
typedef struct { uint8_t r, g, b; } IoColor;

#define IO_RGB(r, g, b) ((IoColor){ (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) })

/* One edge of a polygon. The winding sign comes from the direction: y1 > y0 adds,
 * y1 < y0 subtracts. Holes therefore need no notion of contour grouping, which is
 * why the rasteriser takes a flat list of edges. */
typedef struct {
    IoFixed x0, y0, x1, y1;
} IoSeg;

/* Maximum edges crossing one scanline. A polygon crossing more on any row is
 * rejected outright rather than drawn truncated. */
#define IO_POLY_MAX_CROSS 256

/* Antialiasing. Each output row is sampled this many times vertically and the
 * horizontal coverage of each span is exact, which is what makes 16px text
 * legible: without it the stems of a Cyrillic letter are one pixel and either
 * vanish or read as a smear. Axis-aligned polygons with whole pixel corners skip
 * all of this and stay exact, so panels and rules remain crisp. */
#define IO_AA_SAMPLES 1
#define IO_AA_STEP (IO_FX_ONE / (2 * IO_AA_SAMPLES))
#define IO_COV_ONE 256u

/* Coverage accumulator for one row. A canvas wider than this gets no
 * antialiasing rather than a wrapped buffer. */
#define IO_COVER_MAX 2048

#define IO_CLIP_MAX 8
#define IO_EVENT_MAX 64

/* Pointer only. Hardware keyboard support was dropped by the owner: on the web
 * and on touch devices it means virtual keyboards and IME per platform, and
 * none of that is worth carrying when the interface is driven by pointing. Any
 * text entry the game needs is drawn by us and tapped, which behaves the same
 * on all three targets. */
typedef enum IoEventKind {
    IO_EV_NONE = 0,
    IO_EV_POINTER_DOWN,
    IO_EV_POINTER_UP,
    IO_EV_POINTER_MOVE,
    IO_EV_QUIT
} IoEventKind;

typedef enum IoDevice {
    IO_DEVICE_MOUSE = 0,
    IO_DEVICE_TOUCH
} IoDevice;

#define IO_BUTTON_LEFT 0u
#define IO_BUTTON_RIGHT 1u
#define IO_BUTTON_MIDDLE 2u

typedef struct IoEvent {
    IoEventKind kind;
    IoDevice device;
    uint8_t button;
    int32_t x, y;     /* always virtual coordinates, whatever the window does */
} IoEvent;

typedef struct IoCtx IoCtx;
typedef struct IoBackend IoBackend;

struct IoCtx {
    uint32_t *pixels;          /* the virtual canvas, 0x00RRGGBB, caller owned */
    int32_t w, h;              /* virtual size, fixed for the session */
    int32_t view_w, view_h;    /* actual window size in physical pixels */


    IoRect clip[IO_CLIP_MAX];
    int clip_n;

    const IoBackend *backend;

    IoEvent ring[IO_EVENT_MAX];
    int ring_n;            /* appended */
    int ring_read;         /* consumed; io_poll looks without consuming */
    unsigned dropped;      /* events discarded because the ring was full */
    uint8_t button_down[3];
    IoDevice pointer_device;
    int32_t pointer_x, pointer_y;
    int32_t pointer_down_x, pointer_down_y;  /* where the press started, for drags */

    /* Scratch for one scanline's crossings, so drawing never allocates and never
     * touches the caller's edges. */
    IoFixed cx[IO_POLY_MAX_CROSS];
    int8_t cdir[IO_POLY_MAX_CROSS];
    uint16_t rowcov[IO_COVER_MAX];
};

/* pixels must hold the virtual canvas, w * h entries, and outlive the context. */
int io_init(IoCtx *ctx, uint32_t *pixels, int32_t w, int32_t h);

/* The window size changed. Backends report it; nothing else has to care. */
void io_set_view(IoCtx *ctx, int32_t view_w, int32_t view_h);

/* Converts a point in window pixels into virtual coordinates. Backends report
 * window pixels, and io_poll does the conversion, so everything above this layer
 * sees one coordinate system only. */
void io_to_virtual(const IoCtx *ctx, int32_t win_x, int32_t win_y,
                   int32_t *out_x, int32_t *out_y);

/* Nearest-neighbour blit of the virtual canvas into a window-sized buffer of
 * 0xRRGGBB pixels. The single place scaling exists, so every backend agrees on
 * the result and none of them grows its own copy of the arithmetic. */
void io_scale_canvas(const IoCtx *ctx, uint32_t *dst, int32_t dst_w, int32_t dst_h);

/* The edges are in the caller's own space; dx and dy move them on screen. Text
 * needs that to place a glyph without copying its edges.
 * Returns 1 when the polygon was rasterised, 0 when a scanline had more
 * crossings than the scratch holds. */
int io_fill_poly(IoCtx *ctx, const IoSeg *edges, uint32_t n, IoFixed dx, IoFixed dy,
                 IoColor color);

/* A rectangle is four edges, so this is a convenience and not a second primitive. */
int io_fill_rect(IoCtx *ctx, IoRect r, IoColor color);

void io_push_clip(IoCtx *ctx, IoRect r);
void io_pop_clip(IoCtx *ctx);
IoRect io_clip(const IoCtx *ctx);

/* The whole platform surface: five functions. A backend opens a window, blits
 * the framebuffer, waits, drains native events and closes. It never touches the
 * pixels themselves.
 *
 * Which backend exists is decided by the build, not by the caller: the file for
 * the target platform defines io_platform_backend() and nothing above this line
 * learns which platform it got. A new platform is a new file and one line in the
 * Makefile, with no edit to the header and none to the application. */
struct IoBackend {
    void *self;
    int  (*open)(void *self, const char *title, int32_t w, int32_t h);
    void (*present)(void *self, IoCtx *ctx);
    /* Blocks until at least one event is queued, or the timeout expires. A loop
     * that only polls would burn a whole core and flood the display. */
    void (*wait)(void *self, int timeout_ms);
    /* Drains native events into the ring via io_post_event. */
    int  (*pump)(void *self, IoCtx *ctx);
    void (*close)(void *self);
};

int io_backend_open(IoCtx *ctx, const IoBackend *backend, const char *title);
void io_backend_close(IoCtx *ctx);

/* Waits for events, then drains the backend queue and refreshes the held-key
 * and mouse state. */
void io_poll(IoCtx *ctx, int timeout_ms);

/* Backends call this; tests inject events directly. A full ring drops the new
 * event and counts it, so the loss is visible instead of silent. */
void io_post_event(IoCtx *ctx, const IoEvent *ev);
int  io_next_event(IoCtx *ctx, IoEvent *out);
unsigned io_dropped_events(const IoCtx *ctx);

int  io_button_held(const IoCtx *ctx, uint8_t button);
int32_t io_pointer_x(const IoCtx *ctx);
int32_t io_pointer_y(const IoCtx *ctx);
IoDevice io_pointer_device(const IoCtx *ctx);
int32_t io_drag_x(const IoCtx *ctx);
int32_t io_drag_y(const IoCtx *ctx);

/* The one platform entry point. Returns NULL when the target has no window to
 * open, which the caller reports like any other failure. */
const IoBackend *io_platform_backend(void);

/* The headless backend, for the tests and the walkthrough. It is named here, and no
 * platform is, because it is not a platform: every build has it, whatever the target
 * is. Its own file decides whether it is also the platform, and only when the build
 * says there is no windowing target at all. */
extern const IoBackend io_backend_test;

#endif