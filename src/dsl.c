#include "dsl.h"

#include "cur.h"
#include "utf8.h"

#include <string.h>

/* ------------------------------------------------------------------ sink -- */

/* Pass 1 has every array NULL so PUSH only counts; pass 2 fills. */
typedef struct Sink {
    ScriptRoom *rooms;
    size_t rooms_n;
    size_t *room_frag_start;
    size_t *room_rule_start;
    Rule *rules;
    size_t rules_n;
    Frag *frags;
    size_t frags_n;
    Cond *conds;
    size_t conds_n;
    Sym *targets;
    size_t targets_n;
} Sink;

#define PUSH(arr, count, value)          \
    do {                                 \
        if ((arr) != NULL) {             \
            (arr)[(count)] = (value);    \
        }                                \
        (count)++;                       \
    } while (0)

/* &arr[i] on a NULL array is UB and the counting pass has none; UBSan flags it. */
static void *at(void *arr, size_t index, size_t elem_size) {
    if (arr == NULL) {
        return NULL;
    }
    return (unsigned char *)arr + index * elem_size;
}

typedef struct Parse {
    int counting;
    Sink sink;
    SymEntry *symtab;
    size_t symtab_n;
    size_t symtab_cap;
    char *pool;
    size_t pool_n;
    size_t pool_cap;
    size_t occ_n;
    Sym start;
    int have_start;
} Parse;

/* --------------------------------------------------------------- cursor --- */

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *trim_end(const char *p, const char *end) {
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    return end;
}

/* lit must be a literal at the call site: as a function parameter it decays to a
 * pointer, so sizeof would yield the pointer size. */
#define WORD_IS(p, n, lit) \
    ((n) == sizeof(lit) - 1 && memcmp((p), (lit), sizeof(lit) - 1) == 0)

/* Returns the end, not the start: callers compute trim_end(p, end) - p, or every
 * text lands at the far side of itself. */

static const char *eat_kw(const char *p, const char *end, const char *kw, size_t n) {
    if ((size_t)(end - p) < n || memcmp(p, kw, n) != 0) {
        return NULL;
    }
    if (p + n == end) {
        return p + n;
    }
    if (p[n] != ' ' && p[n] != '\t') {
        return NULL;
    }
    return skip_ws(p + n + 1, end);
}

/* A word is a letter, a digit or an underscore; bytes over 0x7F count as letters,
 * since the scripts are Russian and utf8_valid already rejected the rest.
 *
 * The period is a delimiter by the owner's decision, so a room reference at the end
 * of a sentence links to the room. It costs nothing on the text side: say, end and
 * win take the rest of the line, not one word. */
static int is_word_byte(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch >= 0x80;
}

/* Reads one word, advancing p past it, NULL if there is none: that is also how the
 * caller detects a sign. */
static const char *read_word(const char *p, const char *end, const char **ws, size_t *wl) {
    const char *w;

    p = skip_ws(p, end);
    w = p;
    while (p < end && is_word_byte((unsigned char)*p)) {
        p++;
    }
    *wl = (size_t)(p - w);
    *ws = w;
    return (*wl == 0) ? NULL : p;
}

/* ----------------------------------------------------------------- syms --- */

static Sym intern(Parse *P, const char *s, size_t len) {
    size_t i;

    if (P->counting) {
        P->occ_n++;
        return 0;
    }
    for (i = 0; i < P->symtab_n; i++) {
        if (P->symtab[i].len == len && memcmp(P->pool + P->symtab[i].off, s, len) == 0) {
            return (Sym)i;
        }
    }
    if (P->symtab_n >= P->symtab_cap || len > P->pool_cap - P->pool_n) {
        return 0;
    }
    memcpy(P->pool + P->pool_n, s, len);
    P->symtab[P->symtab_n].off = (uint32_t)P->pool_n;
    P->symtab[P->symtab_n].len = (uint32_t)len;
    P->pool_n += len;
    return (Sym)P->symtab_n++;
}

/* --------------------------------------------------------------- flags -- */

/* A flag list is exactly one token: "a", "-a" or "a,-b". No spaces around the commas,
 * so a description's condition stays a single word and cannot be read as a command.
 *
 * Every item carries its sign and both signs are read on both sides: the side of the
 * colon already says guard or effect. Guards and effects share one Cond array, so the
 * caller notes the seam and slices the two runs apart. */
static const char *parse_items(Parse *P, const char *p, const char *end, int effects,
                               size_t *out_n) {
    size_t n = 0;

    for (;;) {
        Cond c;
        const char *w;
        size_t wl;

        p = skip_ws(p, end);
        if (p >= end || (*p != '+' && *p != '-')) {
            break;
        }
        (void)effects;
        c.present = (*p == '+') ? 1u : 0u;
        p++;
        if (read_word(p, end, &w, &wl) == NULL) {
            return NULL;
        }
        c.name = intern(P, w, wl);
        PUSH(P->sink.conds, P->sink.conds_n, c);
        n++;
        p = w + wl;
    }
    *out_n = n;
    return p;
}

/* --------------------------------------------------------------- actions -- */

/* The consequence is the whole tail of a line, with no separator between actions on
 * purpose: a command does one thing, so a print cannot swallow what follows it. */
static const char *parse_action(Parse *P, const char *p, const char *end, Act *out,
                                size_t *effect_len) {
    memset(out, 0, sizeof *out);
    *effect_len = 0;
    p = skip_ws(p, end);

    /* Leading effects: "+a -b text". Either sign starts the list; parse_items
     * consumes the signs itself. */
    if (p < end && (*p == '+' || *p == '-')) {
        const char *q = parse_items(P, p, end, 1, effect_len);

        if (q == NULL) {
            return NULL;
        }
        p = skip_ws(q, end);
    }

    /* The effects do not swallow the action: they come before it, so one line can
     * clear a flag and end the game at once. */

    if ((size_t)(end - p) >= 2 && p[0] == 'g' && p[1] == 'o' &&
        (p + 2 == end || p[2] == ' ' || p[2] == '\t')) {
        const char *w;
        size_t wl;
        const char *q = read_word(skip_ws(p + 2, end), end, &w, &wl);

        if (q == NULL) {
            return NULL;
        }
        out->kind = ACT_GO;
        out->target = intern(P, w, wl);
        PUSH(P->sink.targets, P->sink.targets_n, out->target);
        p = skip_ws(q, end);
    } else if ((size_t)(end - p) >= 3 && p[0] == 'e' && p[1] == 'n' && p[2] == 'd' &&
               (p + 3 == end || p[3] == ' ' || p[3] == '\t')) {
        out->kind = ACT_END;
        p = skip_ws(p + 3, end);
    } else if ((size_t)(end - p) >= 3 && p[0] == 'w' && p[1] == 'i' && p[2] == 'n' &&
               (p + 3 == end || p[3] == ' ' || p[3] == '\t')) {
        out->kind = ACT_WIN;
        p = skip_ws(p + 3, end);
    } else {
        /* The text is the rest of the line whatever it starts with; only a leading end or win
         * is a keyword, and only when it stands alone. */
        out->kind = ACT_SAY;
    }
    out->text = p;
    out->text_len = (uint32_t)(trim_end(p, end) - p);
    return end;
}

/* ----------------------------------------------------------------- lines -- */

/* A room's range is known only once the next header appears or the file ends. */
static void close_room(Sink *s) {
    size_t idx;

    if (s->rooms == NULL || s->rooms_n == 0) {
        return;
    }
    idx = s->rooms_n - 1;
    s->rooms[idx].frag_len = (uint32_t)(s->frags_n - s->room_frag_start[idx]);
    s->rooms[idx].rule_len = (uint32_t)(s->rules_n - s->room_rule_start[idx]);
}

/* One line, one shape: condition, colon, consequence. Up to one word before the
 * colon describes the room, two or three is a command. */
static ScriptStatus parse_line_body(Parse *P, const char *s, const char *end,
                                    int line, int is_command) {
    const char *colon = (const char *)memchr(s, ':', (size_t)(end - s));
    const char *head_end;
    const char *w;
    size_t wl;
    const char *p;
    size_t cond_start = P->sink.conds_n;
    size_t cond_len = 0;
    size_t eff_start;
    size_t effect_len = 0;

    if (colon == NULL) {
        return SCR_E_SYNTAX;
    }
    head_end = colon;

    if (!is_command) {
        /* "flags : text" */
        const char *text;
        Frag f;

        p = skip_ws(s, head_end);
        if (p != head_end) {
            /* Anything left between the returned position and the colon is a word
             * the format has nowhere to put. */
            p = parse_items(P, p, head_end, 0, &cond_len);
            if (p == NULL) {
                return SCR_E_SYNTAX;
            }
            p = skip_ws(p, head_end);
            if (p != head_end) {
                return SCR_E_SYNTAX;
            }
        }
        text = skip_ws(colon + 1, end);
        if (text >= end || trim_end(text, end) == text) {
            return SCR_E_SYNTAX;
        }
        memset(&f, 0, sizeof f);
        f.guard = (cond_len > 0) ? (Cond *)at(P->sink.conds, cond_start, sizeof(Cond)) : NULL;
        f.guard_len = (uint32_t)cond_len;
        f.text = text;
        f.text_len = (uint32_t)(trim_end(text, end) - text);
        f.line = line;
        PUSH(P->sink.frags, P->sink.frags_n, f);
        return SCR_OK;
    }

    {
        Rule r;

        /* words [conditions] : [effects] action. The words are greedy, stopping at the first
         * sign or the colon, so how long a command is comes from the script. */
        p = skip_ws(s, head_end);
        r.word_len = 0;
        while (p < head_end && *p != '+' && *p != '-') {
            if (read_word(p, head_end, &w, &wl) == NULL) {
                return SCR_E_SYNTAX;
            }
            if (r.word_len == RULE_MAX_WORDS) {
                return SCR_E_SYNTAX; /* a wider command than the palette can show */
            }
            r.words[r.word_len++] = intern(P, w, wl);
            p = skip_ws(w + wl, head_end);
        }
        if (r.word_len == 0) {
            return SCR_E_SYNTAX;
        }
        p = parse_items(P, p, head_end, 0, &cond_len);
        if (p == NULL) {
            return SCR_E_SYNTAX;
        }
        p = skip_ws(p, head_end);
        if (p != head_end) {
            return SCR_E_SYNTAX;
        }
        eff_start = P->sink.conds_n;

        /* p is still on the colon, a delimiter, so handing it over would read an empty word. */
        p = parse_action(P, colon + 1, end, &r.act, &effect_len);
        if (p == NULL) {
            return SCR_E_SYNTAX;
        }
        r.guard = (cond_len > 0) ? (Cond *)at(P->sink.conds, cond_start, sizeof(Cond)) : NULL;
        r.guard_len = (uint32_t)cond_len;
        r.effects = (effect_len > 0) ? (Cond *)at(P->sink.conds, eff_start, sizeof(Cond)) : NULL;
        r.effect_len = (uint32_t)effect_len;
        r.line = line;
        PUSH(P->sink.rules, P->sink.rules_n, r);
        return SCR_OK;
    }
}

static ScriptStatus parse_line(Parse *P, const char *s, size_t n, int *in_room, int line) {
    const char *end = s + n;
    const char *p = skip_ws(s, end);
    const char *w;
    size_t wl;
    const char *after;

    after = eat_kw(p, end, "start", sizeof("start") - 1);
    if (after != NULL) {
        if (read_word(after, end, &w, &wl) == NULL) {
            return SCR_E_SYNTAX;
        }
        P->start = intern(P, w, wl);
        P->have_start = 1;
        return SCR_OK;
    }
    after = eat_kw(p, end, "room", sizeof("room") - 1);
    if (after != NULL) {
        size_t frag_start = P->sink.frags_n;
        size_t rule_start = P->sink.rules_n;
        const char *title_start;
        const char *title_end;
        ScriptRoom r;

        close_room(&P->sink);
        memset(&r, 0, sizeof r);
        if (read_word(after, end, &w, &wl) == NULL) {
            return SCR_E_SYNTAX;
        }
        r.id = intern(P, w, wl);
        title_start = skip_ws(w + wl, end);
        title_end = trim_end(title_start, end);
        r.title = title_start;
        r.title_len = (uint32_t)(title_end - title_start);
        r.frags = (Frag *)at(P->sink.frags, frag_start, sizeof(Frag));
        r.rules = (Rule *)at(P->sink.rules, rule_start, sizeof(Rule));
        r.line = line;
        PUSH(P->sink.rooms, P->sink.rooms_n, r);
        if (P->sink.room_frag_start != NULL) {
            P->sink.room_frag_start[P->sink.rooms_n - 1] = frag_start;
            P->sink.room_rule_start[P->sink.rooms_n - 1] = rule_start;
        }
        *in_room = 1;
        return SCR_OK;
    }
    /* A description or a command must belong to a room. */
    if (!*in_room) {
        return SCR_E_SYNTAX;
    }
    {
        /* A bare word before the colon makes this a command; words inside a condition list
         * are signed, so they do not count. */
        const char *colon = (const char *)memchr(s, ':', (size_t)(end - s));
        const char *head_end = (colon != NULL) ? colon : end;
        const char *q = s;
        int words = 0;

        while (q < head_end && read_word(q, head_end, &w, &wl) != NULL) {
            words++;
            q = skip_ws(q + wl, head_end);
        }
        return parse_line_body(P, s, end, line, words >= 1);
    }
}

static ScriptStatus run_pass(Parse *P, const char *text, size_t len, int *err_line) {
    Cur c;
    const char *s;
    size_t n;
    int in_room = 0;

    cur_init(&c, text, len);

    while (cur_next_line(&c, &s, &n)) {
        ScriptStatus st = parse_line(P, s, n, &in_room, c.line);
        if (st != SCR_OK) {
            if (err_line != NULL) {
                *err_line = c.line;
            }
            return st;
        }
    }
    close_room(&P->sink);
    return SCR_OK;
}

/* ------------------------------------------------------------------ load -- */

ScriptStatus script_load(Arena *a, Script *out, const char *text, size_t len,
                         int *err_line) {
    Parse P;
    Sink S;
    ScriptStatus st;
    size_t frags_n;
    size_t conds_n;
    size_t targets_n;
    size_t occ_n;

    if (err_line != NULL) {
        *err_line = 0;
    }
    if (a == NULL || out == NULL || text == NULL) {
        return SCR_E_SYNTAX;
    }
    if (!utf8_valid((const uint8_t *)text, len)) {
        return SCR_E_UTF8;
    }

    memset(out, 0, sizeof *out);
    out->arena = a;

    memset(&P, 0, sizeof P);
    P.counting = 1;
    st = run_pass(&P, text, len, err_line);
    if (st != SCR_OK) {
        return st;
    }
    if (P.sink.rooms_n == 0) {
        return SCR_E_NO_ROOMS;
    }

    frags_n = P.sink.frags_n;
    conds_n = P.sink.conds_n;
    targets_n = P.sink.targets_n;
    occ_n = P.occ_n;

    /* Pass 2 arrays. S is a fresh sink so the counting totals are not wiped. */
    memset(&S, 0, sizeof S);
    S.rooms = arena_alloc_array(a, P.sink.rooms_n, sizeof(ScriptRoom), ARENA_DEFAULT_ALIGN);
    S.rules = arena_alloc_array(a, P.sink.rules_n, sizeof(Rule), ARENA_DEFAULT_ALIGN);
    S.frags = arena_alloc_array(a, frags_n, sizeof(Frag), ARENA_DEFAULT_ALIGN);
    S.conds = arena_alloc_array(a, conds_n, sizeof(Cond), ARENA_DEFAULT_ALIGN);
    S.targets = arena_alloc_array(a, targets_n, sizeof(Sym), ARENA_DEFAULT_ALIGN);
    S.room_frag_start = arena_alloc_array(a, P.sink.rooms_n, sizeof(size_t), ARENA_DEFAULT_ALIGN);
    S.room_rule_start = arena_alloc_array(a, P.sink.rooms_n, sizeof(size_t), ARENA_DEFAULT_ALIGN);
    if (S.rooms == NULL || S.rules == NULL || S.frags == NULL ||
        S.conds == NULL || S.targets == NULL || S.room_frag_start == NULL ||
        S.room_rule_start == NULL) {
        return SCR_E_MEMORY;
    }
    P.symtab = arena_alloc_array(a, occ_n, sizeof(SymEntry), ARENA_DEFAULT_ALIGN);
    P.pool = arena_alloc(a, len, 1);
    if (P.symtab == NULL || P.pool == NULL) {
        return SCR_E_MEMORY;
    }

    P.sink = S;
    P.counting = 0;
    P.symtab_cap = occ_n;
    P.pool_cap = len;

    st = run_pass(&P, text, len, err_line);
    if (st != SCR_OK) {
        return st;
    }

    out->rooms = S.rooms;
    out->rule_count = P.sink.rules_n;
    out->room_count = P.sink.rooms_n;
    out->rules = S.rules;
    out->symtab = P.symtab;
    out->sym_count = P.symtab_n;
    out->pool = P.pool;
    out->pool_len = P.pool_n;
    out->start = P.have_start ? P.start : out->rooms[0].id;
    out->have_start_directive = P.have_start;
    return SCR_OK;
}

/* --------------------------------------------------------------- queries -- */

Sym script_sym_lookup(const Script *s, const char *name, size_t len) {
    size_t i;

    for (i = 0; i < s->sym_count; i++) {
        if (s->symtab[i].len == len &&
            memcmp(s->pool + s->symtab[i].off, name, len) == 0) {
            return (Sym)i;
        }
    }
    return SYM_NONE;
}

const char *script_sym(const Script *s, Sym sym, size_t *len) {
    if (sym >= s->sym_count) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    if (len != NULL) {
        *len = s->symtab[sym].len;
    }
    return s->pool + s->symtab[sym].off;
}

int script_sym_eq(const Script *s, Sym sym, const char *name) {
    size_t n;
    size_t i;
    const char *p = script_sym(s, sym, &n);

    if (p == NULL || name == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (name[i] == '\0' || name[i] != p[i]) {
            return 0;
        }
    }
    return name[i] == '\0';
}

const ScriptRoom *script_room_by_id(const Script *s, Sym id) {
    size_t i;
    for (i = 0; i < s->room_count; i++) {
        if (s->rooms[i].id == id) {
            return &s->rooms[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- validation -- */

static int flag_ever_set(const Script *s, Sym sym) {
    size_t i;
    uint32_t j;

    for (i = 0; i < s->rule_count; i++) {
        for (j = 0; j < s->rules[i].effect_len; j++) {
            if (s->rules[i].effects[j].name == sym) {
                return 1;
            }
        }
    }
    return 0;
}

static int cond_has(const Cond *list, uint32_t n, Sym sym, uint8_t present) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (list[i].name == sym && list[i].present == present) {
            return 1;
        }
    }
    return 0;
}

/* True when every condition of a is also required by b; an unconditional a
 * implies everything. */
static int guard_implies(const Rule *a, const Rule *b) {
    uint32_t i;

    if (a->guard == NULL || a->guard_len == 0) {
        return 1;
    }
    if (b->guard == NULL) {
        return 0;
    }
    for (i = 0; i < a->guard_len; i++) {
        if (!cond_has(b->guard, b->guard_len, a->guard[i].name, a->guard[i].present)) {
            return 0;
        }
    }
    return 1;
}

/* Commands are however long the script makes them, so the old verb/object pair
 * compare is gone. */
static int same_command(const Rule *a, const Rule *b) {
    uint32_t i;

    if (a->word_len != b->word_len) {
        return 0;
    }
    for (i = 0; i < a->word_len; i++) {
        if (a->words[i] != b->words[i]) {
            return 0;
        }
    }
    return 1;
}

static void mark_reachable(const Script *s, unsigned char *reach) {
    size_t i;
    int changed = 1;

    for (i = 0; i < s->room_count; i++) {
        reach[i] = (unsigned char)(s->rooms[i].id == s->start ? 1 : 0);
    }
    while (changed) {
        changed = 0;
        for (i = 0; i < s->room_count; i++) {
            uint32_t r;
            if (!reach[i]) {
                continue;
            }
            for (r = 0; r < s->rooms[i].rule_len; r++) {
                const Rule *rule = &s->rooms[i].rules[r];
                size_t j;
                if (rule->act.kind != ACT_GO) {
                    continue;
                }
                for (j = 0; j < s->room_count; j++) {
                    if (!reach[j] && s->rooms[j].id == rule->act.target) {
                        reach[j] = 1;
                        changed = 1;
                    }
                }
            }
        }
    }
}

#define EMIT(sev, code_, line_, subj)                              \
    do {                                                           \
        if (n < cap) {                                             \
            out[n].severity = (sev);                               \
            out[n].code = (code_);                                 \
            out[n].line = (line_);                                 \
            out[n].subject = (subj);                               \
        }                                                          \
        n++;                                                       \
        if ((sev) == SEV_ERROR) {                                  \
            errors++;                                              \
        }                                                          \
    } while (0)

size_t script_validate(const Script *s, Diagnostic *out, size_t cap, size_t *out_n) {
    size_t n = 0;
    size_t errors = 0;
    size_t i;
    size_t j;
    size_t k;
    unsigned char *reach;

    for (i = 0; i < s->room_count; i++) {
        for (j = 0; j < i; j++) {
            if (s->rooms[i].id == s->rooms[j].id) {
                EMIT(SEV_ERROR, V_E_DUPLICATE_ROOM, s->rooms[i].line, s->rooms[i].id);
            }
        }
    }

    /* A flag used in a guard but never set is a typo: the branch can never be taken. */
    for (i = 0; i < s->room_count; i++) {
        const ScriptRoom *room = &s->rooms[i];
        for (j = 0; j < room->frag_len; j++) {
            uint32_t c;
            for (c = 0; c < room->frags[j].guard_len; c++) {
                if (!flag_ever_set(s, room->frags[j].guard[c].name)) {
                    EMIT(SEV_ERROR, V_E_UNDEFINED_FLAG, room->frags[j].line,
                         room->frags[j].guard[c].name);
                }
            }
        }
        for (j = 0; j < room->rule_len; j++) {
            uint32_t c;
            for (c = 0; c < room->rules[j].guard_len; c++) {
                if (!flag_ever_set(s, room->rules[j].guard[c].name)) {
                    EMIT(SEV_ERROR, V_E_UNDEFINED_FLAG, room->rules[j].line,
                         room->rules[j].guard[c].name);
                }
            }
        }
    }

    for (i = 0; i < s->room_count; i++) {
        const ScriptRoom *room = &s->rooms[i];
        for (j = 0; j < room->rule_len; j++) {
            for (k = j + 1; k < room->rule_len; k++) {
                const Rule *a = &room->rules[j];
                const Rule *b = &room->rules[k];
                if (same_command(a, b) && guard_implies(a, b)) {
                    EMIT(SEV_WARNING, V_W_SHADOWED_RULE, b->line, b->words[0]);
                }
            }
        }
    }

    for (i = 0; i < s->rule_count; i++) {
        if (s->rules[i].act.kind == ACT_GO &&
            script_room_by_id(s, s->rules[i].act.target) == NULL) {
            EMIT(SEV_WARNING, V_W_UNRESOLVED_TARGET, s->rules[i].line,
                 s->rules[i].act.target);
        }
    }

    reach = arena_alloc_array(s->arena, s->room_count, 1, 1);
    if (reach != NULL) {
        mark_reachable(s, reach);
        for (i = 0; i < s->room_count; i++) {
            if (!reach[i]) {
                EMIT(SEV_WARNING, V_W_UNREACHABLE_ROOM, s->rooms[i].line, s->rooms[i].id);
            }
        }
    }

    if (!s->have_start_directive && s->room_count > 0) {
        EMIT(SEV_WARNING, V_W_IMPLICIT_START, s->rooms[0].line, s->rooms[0].id);
    }

    if (out_n != NULL) {
        *out_n = n;
    }
    return errors;
}

#undef EMIT

const char *script_status_str(ScriptStatus st) {
    switch (st) {
    case SCR_OK:         return "ok";
    case SCR_E_UTF8:     return "file is not valid UTF-8";
    case SCR_E_SYNTAX:   return "malformed script";
    case SCR_E_NO_ROOMS: return "script declares no rooms";
    case SCR_E_MEMORY:   return "not enough arena memory";
    default:             return "unknown error";
    }
}

const char *script_verify_str(VerifyCode code) {
    switch (code) {
    case V_OK:                  return "ok";
    case V_E_UNDEFINED_FLAG:    return "flag is used but never set";
    case V_E_DUPLICATE_ROOM:    return "room id declared twice";
    case V_W_SHADOWED_RULE:     return "rule can never fire, an earlier rule always matches";
    case V_W_UNRESOLVED_TARGET: return "go target is not a declared room";
    case V_W_UNREACHABLE_ROOM:  return "room is unreachable from the start";
    case V_W_IMPLICIT_START:    return "no start directive, first room used";
    default:                    return "unknown check";
    }
}