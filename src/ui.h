#ifndef CORE_UI_H
#define CORE_UI_H

#include "dsl.h"
#include "font.h"
#include "game.h"
#include "io.h"

#include <stddef.h>

/* The whole of the interface's state, in one type, in a header.
 *
 * It lived in main.c, which meant nothing else could name it: a function that takes a
 * frame had to be declared against an incomplete `struct Ui`, so every such function had
 * to live in main.c too. That is the whole reason main.c was 2286 lines: not because the
 * drawing needed the room, but because the state had exactly one owner and the owner was
 * the wrong file.
 *
 * Moving the type out first is deliberate. Moving the code first does not compile, and
 * moving both at once gives no way to tell a mistake in the move from a mistake in the
 * code. */

#define GAME_W 640
#define GAME_H 480
#define SCALE 2

#define FONT_PATH "assets/font/sans.font"

#define MARGIN_X 16
#define PAD 10                     /* text inset inside a band */
#define BTN_H 26                   /* a button in the control band */
#define CHIP_H 44                  /* a verb or object chip */
#define CHIP_GAP 4
#define CHIP_PAD_X 6
/* Kept free at the right end of every chip row so the "and N more" chip has somewhere
 * to go. Seven verbs is the widest row in the shipped games and it fits with sixteen
 * pixels to spare, but a row one word longer must degrade honestly rather than run off
 * the canvas: a chip drawn past the edge is also a chip the pointer cannot reach. */
#define CHIP_ROW_RESERVE 56
/* Kept free at the right end of the item strip so the "and N more" chip always has
 * somewhere to go. Four glyphs is the widest it ever gets: a plus and two digits. */
#define COUNTER_RESERVE 60

#define ROWS_MAX 32
#define LABEL_MAX 48

#define HIT_SAVE 0
#define HIT_LOAD 1
#define HIT_EXIT 2
#define HIT_WORD 3
#define HIT_CANCEL 4
#define HIT_NEW 5

#define C_BG IO_RGB(20, 22, 27)
#define C_BAND IO_RGB(29, 32, 39)
#define C_FIELD IO_RGB(20, 21, 26)
#define C_INK IO_RGB(224, 222, 216)
#define C_DIM IO_RGB(150, 154, 164)
#define C_NAME IO_RGB(238, 226, 196)
#define C_CHIP IO_RGB(48, 52, 62)
#define C_CHIP_ON IO_RGB(72, 102, 86)
#define C_QUIT IO_RGB(96, 48, 48)
#define C_RULE IO_RGB(56, 58, 68)
#define C_CARET IO_RGB(226, 200, 140)
/* What the eye is meant to catch: text that turned up because of what the player just
 * did. Warm against the cold ink, and dark enough to keep reading at length. */
#define C_HOT IO_RGB(255, 196, 108)

/* A run of the description to draw in the accent colour. Byte offsets into the text the
 * band is drawing, which is what lets a highlight survive rewrapping: the run is
 * marked in the source text and the row drawer finds it wherever it lands. */
typedef struct Span {
    uint32_t off;
    uint32_t len;
} Span;

#define CTRL_H 30
#define NAME_H 32
#define ITEMS_H 26                   /* the strip of carried things */
#define CMD_PAD 8
#define DESC_MIN_H 56                /* below this the description stops being read */
#define RESP_MIN_H 60
#define TILE_H 36                     /* one slot in the command line */
#define PARA_MAX 4096
#define MAX_CHOICES 16

#define CMD_SLOTS RULE_MAX_WORDS
#define MAX_HITS 48
#define FRAG_MAX 64

/* The carried strip: how many things it can hold, and the shortest name it will shorten a
 * name to. A name of one character plus the dot is all it will leave, because a dot on
 * its own says nothing about what was carried. */
#define ITEM_MAX 32
#define ITEM_MIN_CHARS 2

typedef struct Hit {
    IoRect r;
    int kind;
    Sym sym;
} Hit;

/* The command under construction: the slots already chosen, left to right. The
 * format has a verb and an object today; CMD_SLOTS leaves the sentence able to
 * grow without the band learning a new shape. */
typedef struct Command {
    Sym slot[CMD_SLOTS];
    int filled;
} Command;

typedef struct Ui {
    IoCtx *ctx;
    const TextFont *font;
    const Game *game;
    const Script *script;
    const char *game_title;

    Command cmd;
    Command last;        /* the command just run, held on screen while it answers */
    int have_last;

    Hit hits[MAX_HITS];
    size_t hit_n;

    size_t last_log;      /* to notice a new answer and restart the typing */
    double typed;         /* characters revealed so far */
    /* How far the answer band has scrolled, in pixels, and where it is going. Rows
     * would jump a whole line every time the text grew past the bottom, and that jump
     * is the blink. */
    double scroll;
    double scroll_want;
    /* The description scrolls its tail too, for the same reason the answer does: a room
     * can say more than fits, and cutting it off is worse than moving it. */
    double dscroll;
    double dscroll_want;
    int animate;          /* off for the walkthrough: the frames must be comparable */
    int overfull;         /* frames whose bands could not hold their own text */
    size_t last_room;      /* to notice a new room and start its scroll over */
    /* Fragments of this room already on screen, and those that arrived with the last
     * command. Keyed on the fragment's line in the script, the only stable name one
     * has. */
    int frag_line[FRAG_MAX];
    size_t frag_line_n;
    int frag_hot[FRAG_MAX];
    size_t frag_hot_n;
    int done;             /* the answer is fully revealed */
} Ui;

/* The few things a second file needs in order to look at a frame the way the drawing
 * does. Exported as functions and not as data, so this header stays the only place that
 * says how a chip is measured, and a second copy of the arithmetic cannot appear. */
uint32_t ui_wrap_row(const TextFont *f, const char *t, uint32_t len, int32_t max_w,
                     uint32_t *rest);
uint32_t ui_chip_label_sym(const Script *s, Sym sym, char *out, size_t cap);
uint32_t ui_chip_label(const Ui *ui, Sym sym, char *out, size_t cap);
int32_t ui_chip_w(const TextFont *f, const char *label, uint32_t len);
int32_t ui_rows_of(const TextFont *f, const char *t, uint32_t len, int32_t width);
void ui_mark_new_fragments(Ui *ui, const Game *g);

/* Shortens carried item names until the row fits. Exported so the tests can reach it:
 * inside the drawing there is no way to. */
void items_fit(const TextFont *f, char label[][LABEL_MAX], uint32_t *len, size_t n);

#endif
