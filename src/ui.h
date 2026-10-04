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

#define CMD_SLOTS RULE_MAX_WORDS
#define MAX_HITS 48
#define FRAG_MAX 64

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

#endif
