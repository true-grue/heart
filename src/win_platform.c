#include "io.h"

#ifdef IO_WIN

#include "utf8.h"

/* The pointer messages that carry touch are declared only under WINVER >= 0x0602, and
 * whether _WIN32_WINNT alone is enough to raise WINVER differs between MinGW versions:
 * it built here and failed to build elsewhere with the same source. So both are asked
 * for, before windows.h, because afterwards it is too late. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WINVER
#define WINVER 0x0602
#endif

#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM, which splits the sign correctly */

/* And spelled out as well, so the file does not depend on which of those two the SDK
 * decided to honour. These are fixed ABI values and cannot drift. */
#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#endif
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN 0x0246
#endif
#ifndef WM_POINTERUP
#define WM_POINTERUP 0x0247
#endif
#ifndef PT_TOUCH
#define PT_TOUCH 0x00000002
#endif

/* Win32 window backend. The canvas stays virtual, as everywhere: this scales it to the
 * window, and a resize changes only the window size.
 *
 * No keyboard path, the same as everywhere else in the project. */

typedef struct WinState {
    HWND hwnd;
    IoCtx *ctx;          /* set by present and pump; the wndproc reads it */
    HDC dc;              /* the window */
    HDC mem;             /* memory DC holding the DIB the canvas is drawn into */
    HBITMAP dib;
    HGDIOBJ old;         /* what SelectObject returned, so it can be put back */
    uint32_t *bits;      /* the DIB pixels: io_scale_canvas writes here directly */
    int32_t w, h;        /* client size in physical pixels */
    int32_t dib_w, dib_h;/* size the DIB was built for */
    int quit;
} WinState;

static WinState g_win;

static const wchar_t k_class[] = L"QuestWindow";

/* A 32-bit BI_RGB DIB wants B,G,R,A bytes in memory, and io.c produces a 0xRRGGBB word,
 * which on a little endian machine is exactly B,G,R,0. So the canvas goes to the window
 * with no channel swap, and the alpha byte that comes along is ignored by GDI. */
static void win_make_dib(WinState *st) {
    BITMAPINFO info;

    if (st->dib != NULL) {
        SelectObject(st->mem, st->old);
        DeleteObject(st->dib);
        st->dib = NULL;
        st->bits = NULL;
    }
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = st->w;
    /* A negative height makes it top-down, so row zero in memory is the top row and
     * the canvas needs no flip on the way out. */
    info.bmiHeader.biHeight = -st->h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    st->dib = CreateDIBSection(st->dc, &info, DIB_RGB_COLORS, (void **)&st->bits,
                               NULL, 0);
    if (st->dib == NULL) {
        return;
    }
    st->old = SelectObject(st->mem, st->dib);
    st->dib_w = st->w;
    st->dib_h = st->h;
}

static void win_blit(WinState *st) {
    if (st->bits == NULL) {
        return;
    }
    BitBlt(st->dc, 0, 0, st->w, st->h, st->mem, 0, 0, SRCCOPY);
}

static void win_push(WinState *st) {
    PAINTSTRUCT ps;
    HDC dst;

    if (st->bits == NULL) {
        return;
    }
    dst = BeginPaint(st->hwnd, &ps);
    BitBlt(dst, 0, 0, st->w, st->h, st->mem, 0, 0, SRCCOPY);
    EndPaint(st->hwnd, &ps);
}

/* One place builds every pointer event, because the mouse and the touch path differ
 * only in the message and the device tag. */
static void win_pointer(WinState *st, IoEventKind kind, LPARAM lp, int device) {
    IoEvent ev;

    if (st->ctx == NULL) {
        return;
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    /* GET_X_LPARAM splits the sign for coordinates past 32767; a plain LOWORD would
     * make the right and bottom edges of a large window negative. */
    ev.x = (int32_t)GET_X_LPARAM(lp);
    ev.y = (int32_t)GET_Y_LPARAM(lp);
    ev.button = 0;   /* one button is the whole vocabulary: there is no wheel */
    ev.device = (IoDevice)device;
    io_post_event(st->ctx, &ev);
}

static LRESULT CALLBACK win_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinState *st = (WinState *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (st == NULL) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_SIZE:
        st->w = (int32_t)LOWORD(lp);
        st->h = (int32_t)HIWORD(lp);
        if (st->w > 0 && st->h > 0 &&
            (st->w != st->dib_w || st->h != st->dib_h)) {
            win_make_dib(st);
        }
        return 0;

    case WM_PAINT:
        win_push(st);
        return 0;

    /* Painting the background ourselves is what stops the window flashing white
     * between frames, and there is nothing else that belongs there. */
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        win_pointer(st, IO_EV_POINTER_DOWN, lp, IO_DEVICE_MOUSE);
        return 0;
    case WM_LBUTTONUP:
        win_pointer(st, IO_EV_POINTER_UP, lp, IO_DEVICE_MOUSE);
        return 0;
    case WM_MOUSEMOVE:
        win_pointer(st, IO_EV_POINTER_MOVE, lp, IO_DEVICE_MOUSE);
        return 0;

    /* Touch arrives as pointer messages with the kind in wParam. There is no
     * WM_POINTERMOVE: a pointer that moves says WM_POINTERUPDATE, and that is the only
     * one of the three that carries a position. The device tag is the single thing
     * downstream that tells a tap from a click, so it has to be set here. */
    case WM_POINTERDOWN:
    case WM_POINTERUP:
    case WM_POINTERUPDATE:
        if (wp == PT_TOUCH) {
            IoEventKind kind = (msg == WM_POINTERDOWN) ? IO_EV_POINTER_DOWN
                              : (msg == WM_POINTERUP) ? IO_EV_POINTER_UP
                                                      : IO_EV_POINTER_MOVE;
            win_pointer(st, kind, lp, IO_DEVICE_TOUCH);
            return 0;
        }
        break;

    case WM_CLOSE:
    case WM_DESTROY:
        if (st->ctx != NULL) {
            IoEvent ev;

            memset(&ev, 0, sizeof ev);
            ev.kind = IO_EV_QUIT;
            io_post_event(st->ctx, &ev);
        }
        st->quit = 1;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int win_open(void *self, const char *title, int32_t w, int32_t h) {
    WinState *st = (WinState *)self;
    WNDCLASSEXW wc;
    RECT r;

    (void)title;
    st->w = w;
    st->h = h;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = win_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    /* IDC_ARROW is a MAKEINTRESOURCE for the A entry point; the wide call needs it
     * widened by hand. */
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = k_class;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }

    st->hwnd = CreateWindowExW(0, k_class, L"quest", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, (int)w, (int)h, NULL,
                               NULL, wc.hInstance, NULL);
    if (st->hwnd == NULL) {
        return 0;
    }
    SetWindowLongPtrW(st->hwnd, GWLP_USERDATA, (LONG_PTR)st);

    /* io_backend_open is given the outer size, and only the client area shows the
     * canvas, so measure it rather than trusting what was asked for. */
    GetClientRect(st->hwnd, &r);
    st->w = (int32_t)(r.right - r.left);
    st->h = (int32_t)(r.bottom - r.top);

    st->dc = GetDC(st->hwnd);
    if (st->dc == NULL) {
        return 0;
    }
    st->mem = CreateCompatibleDC(st->dc);
    if (st->mem == NULL) {
        return 0;
    }
    win_make_dib(st);
    ShowWindow(st->hwnd, SW_SHOW);
    UpdateWindow(st->hwnd);
    return 1;
}

static void win_present(void *self, IoCtx *ctx) {
    WinState *st = (WinState *)self;

    st->ctx = ctx;
    if (st->w <= 0 || st->h <= 0 || st->bits == NULL) {
        return;
    }
    /* The virtual canvas is scaled to the window here, and only here. */
    io_scale_canvas(ctx, st->bits, st->w, st->h);
    win_blit(st);
    InvalidateRect(st->hwnd, NULL, FALSE);
}

/* Sleeps until Windows has a message, so a frame loop costs nothing while the window is
 * idle. Same reason the X11 backend waits on the socket. */
static void win_wait(void *self, int timeout_ms) {
    MSG msg;

    (void)self;
    if (MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)timeout_ms, QS_ALLINPUT) !=
        WAIT_OBJECT_0) {
        return;
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static int win_pump(void *self, IoCtx *ctx) {
    WinState *st = (WinState *)self;
    MSG msg;

    st->ctx = ctx;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !st->quit;
}

static void win_close(void *self) {
    WinState *st = (WinState *)self;

    if (st->dib != NULL && st->mem != NULL) {
        /* Put the memory DC's original bitmap back before deleting ours, or GDI keeps
         * a dangling selection and every later draw on that DC is undefined. */
        SelectObject(st->mem, st->old);
        DeleteObject(st->dib);
        st->dib = NULL;
        st->bits = NULL;
    }
    if (st->mem != NULL) {
        DeleteDC(st->mem);
        st->mem = NULL;
    }
    if (st->dc != NULL && st->hwnd != NULL) {
        ReleaseDC(st->hwnd, st->dc);
        st->dc = NULL;
    }
    if (st->hwnd != NULL) {
        DestroyWindow(st->hwnd);
        st->hwnd = NULL;
    }
}

const IoBackend io_backend_win = {
    &g_win,
    win_open,
    win_present,
    win_wait,
    win_pump,
    win_close
};

/* The platform entry point. Unconditional inside this file's own guard, for the same
 * reason as the other platform files: the build chose this one, so nothing here has to
 * know which other platform files exist in order not to collide with them. */
const IoBackend *io_platform_backend(void) {
    return &io_backend_win;
}
#endif
