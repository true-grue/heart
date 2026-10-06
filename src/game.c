#include "game.h"

#include <string.h>

/* Linear scan: the flag set is fixed once the script is loaded and is small. */

static int flag_slot(const Game *g, Sym name) {
    size_t k;
    for (k = 0; k < g->flag_count; k++) {
        if (g->flag_name[k] == name) {
            return (int)k;
        }
    }
    return -1;
}

static int flag_intern(Game *g, Sym name) {
    int k = flag_slot(g, name);
    if (k >= 0) {
        return 0;
    }
    if (g->flag_count >= GAME_MAX_FLAGS) {
        return -1;
    }
    g->flag_name[g->flag_count] = name;
    g->flag_present[g->flag_count] = 0;
    g->flag_count++;
    return 1;
}

static int flag_is(const Game *g, Sym name) {
    int k = flag_slot(g, name);
    return (k >= 0) ? g->flag_present[k] : 0;
}

/* One function, not a setter plus a clearer: dropping what a command held is as
 * ordinary as picking it up. */
static void flag_apply(Game *g, Sym name, int present) {
    int k = flag_slot(g, name);
    if (k >= 0) {
        g->flag_present[k] = (uint8_t)present;
    }
}

/* An empty guard holds; a flag the script never sets reads absent, same thing. */
static int cond_holds(const Game *g, const Cond *c, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        int want = c[i].present ? 1 : 0;
        if (flag_is(g, c[i].name) != want) {
            return 0;
        }
    }
    return 1;
}

static int rule_guard_holds(const Game *g, const Rule *ru) {
    if (ru->guard == NULL || ru->guard_len == 0) {
        return 1;
    }
    return cond_holds(g, ru->guard, ru->guard_len);
}

void game_say(Game *g, const char *text, uint32_t len) {
    if (len == 0) {
        return;
    }
    if (g->log_count == GAME_MAX_LOG) {
        /* Drop the oldest line rather than refuse to speak: only the tail is ever
         * on screen. */
        memmove(g->log_text, g->log_text + 1, (GAME_MAX_LOG - 1) * sizeof g->log_text[0]);
        memmove(g->log_len, g->log_len + 1, (GAME_MAX_LOG - 1) * sizeof g->log_len[0]);
        memmove(g->log_headed, g->log_headed + 1, (GAME_MAX_LOG - 1) * sizeof g->log_headed[0]);
        memmove(g->log_head, g->log_head + 1, (GAME_MAX_LOG - 1) * sizeof g->log_head[0]);
        memmove(g->log_words, g->log_words + 1, (GAME_MAX_LOG - 1) * sizeof g->log_words[0]);
        memmove(g->log_word_len, g->log_word_len + 1, (GAME_MAX_LOG - 1) * sizeof g->log_word_len[0]);
        g->log_count--;
    }
    g->log_text[g->log_count] = text;
    g->log_len[g->log_count] = len;
    g->log_headed[g->log_count] = 0;
    g->log_head[g->log_count] = NULL;
    g->log_words[g->log_count] = NULL;
    g->log_word_len[g->log_count] = 0;
    g->log_count++;
}

/* The heading occupies a log line with no text, so block boundaries live in the
 * same array and cannot drift when the log is trimmed. */
void game_head(Game *g, const char *title, uint32_t title_len, const Sym *words,
               uint32_t words_len) {
    /* Not via game_say: it ignores an empty line, so the heading would land on the
     * previous one. */
    if (g->log_count == GAME_MAX_LOG) {
        memmove(g->log_text, g->log_text + 1, (GAME_MAX_LOG - 1) * sizeof g->log_text[0]);
        memmove(g->log_len, g->log_len + 1, (GAME_MAX_LOG - 1) * sizeof g->log_len[0]);
        memmove(g->log_headed, g->log_headed + 1, (GAME_MAX_LOG - 1) * sizeof g->log_headed[0]);
        memmove(g->log_head, g->log_head + 1, (GAME_MAX_LOG - 1) * sizeof g->log_head[0]);
        memmove(g->log_words, g->log_words + 1, (GAME_MAX_LOG - 1) * sizeof g->log_words[0]);
        memmove(g->log_word_len, g->log_word_len + 1, (GAME_MAX_LOG - 1) * sizeof g->log_word_len[0]);
        g->log_count--;
    }
    g->log_text[g->log_count] = title;
    g->log_len[g->log_count] = title_len;
    g->log_headed[g->log_count] = 1;
    g->log_head[g->log_count] = title;
    /* Zero words means the heading is a room, not something the player did. */
    g->log_words[g->log_count] = (words_len > 0) ? words : NULL;
    g->log_word_len[g->log_count] = words_len;
    g->log_count++;
}

/* Every fragment whose guard holds, joined by a space: what is true now, not what
 * was said on entry. A flag moving is how a fragment appears or vanishes, so reading
 * the log for the screen would show a room that no longer exists.
 *
 * The spans variant also reports where each fragment landed, so the caller can point
 * out the one the player just caused; a flat string throws that away. */
size_t game_room_text_spans(const Game *g, char *out, size_t cap, FragSpan *spans,
                            size_t span_cap, size_t *span_n) {
    const ScriptRoom *r = script_room_by_id(g->script, g->room);
    size_t n = 0;
    size_t sn = 0;
    uint32_t i;

    /* The count is reported, never left to the caller to guess: guessing reads uninitialised
     * stack past what was written, zeroes on one toolchain and not on another. */
    if (span_n != NULL) {
        *span_n = 0;
    }
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (r == NULL) {
        return 0;
    }
    for (i = 0; i < r->frag_len; i++) {
        const Frag *f = &r->frags[i];
        size_t k;
        size_t start;
        size_t take;

        if (!cond_holds(g, f->guard, f->guard_len)) {
            continue;
        }
        if (n > 0 && n + 1 < cap) {
            out[n++] = ' ';
        }
        start = n;
        take = f->text_len;
        if (take > cap - 1 - n) {
            take = cap - 1 - n;
        }
        for (k = 0; k < take; k++) {
            out[n + k] = f->text[k];
        }
        n += take;
        if (spans != NULL && sn < span_cap && take > 0) {
            spans[sn].off = (uint32_t)start;
            spans[sn].len = (uint32_t)take;
            spans[sn].line = f->line;
            sn++;
        }
    }
    out[n] = '\0';
    if (span_n != NULL) {
        *span_n = sn;
    }
    return n;
}

size_t game_room_text(const Game *g, char *out, size_t cap) {
    return game_room_text_spans(g, out, cap, NULL, 0, NULL);
}

void game_enter(Game *g, Sym room) {
    const ScriptRoom *r = script_room_by_id(g->script, room);
    uint32_t i;

    if (r == NULL) {
        return;
    }
    g->room = room;
    game_head(g, r->title, r->title_len, NULL, 0);
    /* All fragments in source order; first-match-wins would make conditional
     * fragments unreachable. */
    for (i = 0; i < r->frag_len; i++) {
        const Frag *f = &r->frags[i];
        if (f->guard != NULL && f->guard_len > 0 && !cond_holds(g, f->guard, f->guard_len)) {
            continue;
        }
        game_say(g, f->text, f->text_len);
    }
}

static int words_match(const Sym *rule_words, uint32_t rule_len, const Sym *words,
                       size_t words_len) {
    size_t i;

    if (rule_len != words_len) {
        return 0;
    }
    for (i = 0; i < words_len; i++) {
        if (rule_words[i] != words[i]) {
            return 0;
        }
    }
    return 1;
}

static const Rule *find_rule(const Game *g, const Sym *words, size_t words_len) {
    const ScriptRoom *r = script_room_by_id(g->script, g->room);
    uint32_t i;

    if (g->finished || r == NULL) {
        return NULL;
    }
    for (i = 0; i < r->rule_len; i++) {
        const Rule *ru = &r->rules[i];
        if (words_match(ru->words, ru->word_len, words, words_len) &&
            rule_guard_holds(g, ru)) {
            return ru;
        }
    }
    return NULL;
}

void game_save(const Game *g, GameState *st) {
    st->room = g->room;
    st->flag_count = g->flag_count;
    memcpy(st->flag_present, g->flag_present, sizeof st->flag_present);
    st->log_count = g->log_count;
    st->finished = g->finished;
    st->won = g->won;
}

void game_restore(Game *g, const GameState *st) {
    g->room = st->room;
    g->flag_count = st->flag_count;
    memcpy(g->flag_present, st->flag_present, sizeof g->flag_present);
    g->log_count = st->log_count;
    g->finished = st->finished;
    g->won = st->won;
}

int game_available(const Game *g, const Sym *words, size_t words_len) {
    return find_rule(g, words, words_len) != NULL;
}

const Rule *game_rule_for(const Game *g, const Sym *words, size_t words_len) {
    return find_rule(g, words, words_len);
}

int game_command(Game *g, const Sym *words, size_t words_len) {
    const Rule *ru = find_rule(g, words, words_len);
    uint32_t i;

    if (ru == NULL) {
        return 0;
    }
    game_head(g, NULL, 0, ru->words, ru->word_len);

    /* Effects run before the action, so the rule's text and the room a go leads
     * into already see the new flags. */
    for (i = 0; i < ru->effect_len; i++) {
        flag_apply(g, ru->effects[i].name, ru->effects[i].present);
    }
    switch (ru->act.kind) {
    case ACT_SAY:
        game_say(g, ru->act.text, ru->act.text_len);
        break;
    case ACT_GO:
        /* The text of a go prints first, which is why the format allows text on a go. */
        if (ru->act.text_len > 0) {
            game_say(g, ru->act.text, ru->act.text_len);
        }
        game_enter(g, ru->act.target);
        break;
    case ACT_END:
        game_say(g, ru->act.text, ru->act.text_len);
        g->finished = 1;
        break;
    case ACT_WIN:
        game_say(g, ru->act.text, ru->act.text_len);
        g->finished = 1;
        g->won = 1;
        break;
    default:
        break;
    }
    return 1;
}

/* The words that may come after a prefix, in the order the room declares them: the
 * source order is what the author chose to read first. */
size_t game_next(const Game *g, const Sym *prefix, size_t prefix_len, Sym *out,
                 size_t cap) {
    const ScriptRoom *r = script_room_by_id(g->script, g->room);
    size_t n = 0;
    uint32_t i, j;

    if (g->finished || r == NULL) {
        return 0;
    }
    for (i = 0; i < r->rule_len && n < cap; i++) {
        const Rule *ru = &r->rules[i];
        Sym word;

        if (!rule_guard_holds(g, ru) || ru->word_len <= prefix_len) {
            continue;
        }
        for (j = 0; j < prefix_len; j++) {
            if (ru->words[j] != prefix[j]) {
                break;
            }
        }
        if (j != prefix_len) {
            continue;
        }
        word = ru->words[prefix_len];
        for (j = 0; j < n; j++) {
            if (out[j] == word) {
                break;
            }
        }
        if (j == n) {
            out[n++] = word;
        }
    }
    return n;
}

/* Whether any word can follow this prefix, i.e. whether the command is finished.
 * Not answerable by listing into a zero-length array: empty and full look the same. */
size_t game_flag_count(const Game *g) {
    return g->flag_count;
}

const char *game_flag_name(const Game *g, size_t i, size_t *len) {
    return script_sym(g->script, g->flag_name[i], len);
}

int game_flag_on(const Game *g, size_t i) {
    return g->flag_present[i] != 0;
}

int game_more(const Game *g, const Sym *prefix, size_t prefix_len) {
    const ScriptRoom *r = script_room_by_id(g->script, g->room);
    uint32_t i;
    size_t j;

    if (g->finished || r == NULL) {
        return 0;
    }
    for (i = 0; i < r->rule_len; i++) {
        const Rule *ru = &r->rules[i];

        if (!rule_guard_holds(g, ru) || ru->word_len <= prefix_len) {
            continue;
        }
        for (j = 0; j < prefix_len; j++) {
            if (ru->words[j] != prefix[j]) {
                break;
            }
        }
        if (j == prefix_len) {
            return 1;
        }
    }
    return 0;
}

GameStatus game_init(Game *g, const Script *s) {
    size_t i;
    uint32_t f, r;

    if (g == NULL || s == NULL || s->room_count == 0) {
        return GAME_E_NO_ROOM;
    }
    memset(g, 0, sizeof *g);
    g->script = s;

    /* Every flag the script mentions gets a slot, so a guard never meets an unknown
     * name. Plain indexing, not the loader's at(): every index sits inside a loop
     * bounded by its own length, so &arr[0] of a NULL array is never formed. */
    for (i = 0; i < s->room_count; i++) {
        const ScriptRoom *room = &s->rooms[i];
        for (f = 0; f < room->frag_len; f++) {
            const Frag *fr = &room->frags[f];
            uint32_t c;
            for (c = 0; c < fr->guard_len; c++) {
                if (flag_intern(g, fr->guard[c].name) < 0) {
                    return GAME_E_TOO_MANY_FLAGS;
                }
            }
        }
        for (r = 0; r < room->rule_len; r++) {
            const Rule *ru = &room->rules[r];
            uint32_t c;
            for (c = 0; c < ru->guard_len; c++) {
                if (flag_intern(g, ru->guard[c].name) < 0) {
                    return GAME_E_TOO_MANY_FLAGS;
                }
            }
            for (c = 0; c < ru->effect_len; c++) {
                if (flag_intern(g, ru->effects[c].name) < 0) {
                    return GAME_E_TOO_MANY_FLAGS;
                }
            }
        }
    }

    g->room = s->have_start_directive ? s->start : s->rooms[0].id;
    game_enter(g, g->room);
    return GAME_OK;
}