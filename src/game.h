#ifndef GAME_GAME_H
#define GAME_GAME_H

#include "arena.h"
#include "dsl.h"

#include <stddef.h>
#include <stdint.h>

/* Runtime for the script DSL.
 *
 * The player has no keyboard, so commands are never typed. They are chosen by
 * pointing, which means the interpreter's input is a (verb, object) pair rather
 * than a line of text, and it means the available commands have to be derived
 * from the rules rather than from a parser.
 *
 * Nothing here allocates. The log and the flag table are fixed arrays inside the
 * Game, so the whole runtime is one object the caller owns.
 */

#define GAME_MAX_FLAGS 128
#define GAME_MAX_LOG 256

typedef enum GameStatus {
    GAME_OK = 0,
    GAME_E_TOO_MANY_FLAGS,
    GAME_E_NO_ROOM,
    GAME_E_LOG_FULL
} GameStatus;

typedef struct Game {
    const Script *script;

    Sym room;                     /* where the player stands */
    Sym flag_name[GAME_MAX_FLAGS];
    uint8_t flag_present[GAME_MAX_FLAGS];
    size_t flag_count;

    /* Output, oldest first, as blocks. A block opens with something to show as a
     * heading: a room title, or the command the player chose. The lines that follow
     * belong to that heading and are drawn as one paragraph, so the separate says a
     * room description is written as do not read as separate paragraphs.
     *
     * The texts point into the script source buffer, so nothing is copied and
     * nothing is freed. */
    const char *log_text[GAME_MAX_LOG];
    uint32_t log_len[GAME_MAX_LOG];
    uint8_t log_headed[GAME_MAX_LOG];    /* this line opens a block */
    const char *log_head[GAME_MAX_LOG];  /* room title; NULL for a command block */
    /* The command behind a heading, or NULL when the heading is a room. Points into
     * the rule, so it must not outlive the script. */
    const Sym *log_words[GAME_MAX_LOG];
    uint8_t log_word_len[GAME_MAX_LOG];
    size_t log_count;

    int finished;                 /* an end or win action has run */
    int won;                      /* it was a win, not a loss */
} Game;

/* Enough state to put the game back where it was. A walkthrough has to try a
 * command and then take it back, and the log is only append-only output, so
 * rewinding it to a count is enough. */
typedef struct GameState {
    Sym room;
    uint8_t flag_present[GAME_MAX_FLAGS];
    size_t flag_count;
    size_t log_count;
    int finished;
    int won;
} GameState;

void game_save(const Game *g, GameState *st);
void game_restore(Game *g, const GameState *st);

/* Collects every flag the script mentions and moves to the start room. */
GameStatus game_init(Game *g, const Script *s);

/* Appends one line to the current block. Once the log is full the oldest line is
 * dropped, because a player cannot read a line that has scrolled away anyway. */
void game_say(Game *g, const char *text, uint32_t len);

/* Opens a block: a heading followed by lines. A room block heads with the room
 * title, a command block with the verb and object the player chose. The length is
 * passed because the title points into the script source and is not terminated. */
void game_head(Game *g, const char *title, uint32_t title_len, const Sym *words,
               uint32_t words_len);

/* Enters a room: its title, then every fragment whose guard holds, in source
 * order. All matching fragments print, because first-match-wins here would make
 * every conditional fragment unreachable. */
/* The current room's description as it stands now, not as it was written into the log
 * on entry. Writes at most cap bytes including the terminator and returns the length
 * written. */
/* One fragment's place in the assembled description. "line" is the fragment's line in
 * the script, which is the only stable name it has. */
typedef struct FragSpan {
    uint32_t off;
    uint32_t len;
    int line;
} FragSpan;

size_t game_room_text(const Game *g, char *out, size_t cap);
/* span_n receives how many spans were written. It is never optional in practice:
 * without it the caller has to find the end of the array by inspecting entries that
 * were never written, which is reading uninitialised memory. */
size_t game_room_text_spans(const Game *g, char *out, size_t cap, FragSpan *spans,
                            size_t span_cap, size_t *span_n);

void game_enter(Game *g, Sym room);

/* Runs the first rule of the current room that matches (verb, object) and whose
 * guard holds. Effects run before the first action. Returns 1 when a rule ran. */
int game_command(Game *g, const Sym *words, size_t words_len);

/* The palette is derived from the guards: a command is offered only when some
 * rule for it would actually run. This is what replaces a "you cannot do that
 * yet" message, which the format cannot express. */
/* The words that may follow prefix, for the slot now being filled. The first slot
 * asks with prefix_len 0, which is where the verbs come from. */
size_t game_next(const Game *g, const Sym *prefix, size_t prefix_len, Sym *out,
                 size_t cap);

/* The flags a player is carrying, for the strip along the bottom. Index i names the
 * flag; the caller decides what to do with it. */
size_t game_flag_count(const Game *g);
const char *game_flag_name(const Game *g, size_t i, size_t *len);
int game_flag_on(const Game *g, size_t i);

/* Whether the command is still unfinished, i.e. some word can follow the prefix. */
int game_more(const Game *g, const Sym *prefix, size_t prefix_len);

/* True when some rule for this pair would run right now. */
int game_available(const Game *g, const Sym *words, size_t words_len);

/* The rule a command would run: the first one in the current room whose words match
 * and whose guard holds, or NULL. The walk needs it because two endings can share
 * their words — only the guard tells them apart — and it has to name the ending a
 * state reached rather than count states that look alike. */
const Rule *game_rule_for(const Game *g, const Sym *words, size_t words_len);

#endif