#include "test.h"

#include "arena.h"
#include "utf8.h"
#include "io.h"
#include "font.h"
#include "dsl.h"
#include "game.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BYTES 262144
#define SCRIPT_PATH "assets/script/tutorial.script"

static _Alignas(16) unsigned char g_arena_mem[ARENA_BYTES];
static _Alignas(16) unsigned char g_scratch_mem[ARENA_BYTES];

static void test_arena_basic(void) {
    Arena a;
    void *p;
    void *q;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(arena_used(&a), 0);
    CHECK_INT(arena_left(&a), ARENA_BYTES);

    p = arena_alloc(&a, 10, 8);
    CHECK(p != NULL);
    CHECK_INT((size_t)((uintptr_t)p % 8u), 0);
    CHECK_INT(arena_used(&a), 10);

    /* The base is 16-aligned, so the first block starts at offset 0 and the
     * second one has to skip 6 bytes to reach the next 8-aligned address. */
    q = arena_alloc(&a, 10, 8);
    CHECK(q != NULL);
    CHECK(q == (unsigned char *)p + 16);
    CHECK_INT((size_t)((uintptr_t)q % 8u), 0);
    CHECK_INT(arena_used(&a), 26);
    CHECK_INT(arena_last_status(&a), ARENA_OK);
}

static void test_arena_alignment(void) {
    Arena a;
    void *p;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    p = arena_alloc(&a, 1, 1);
    CHECK(p != NULL);

    p = arena_alloc(&a, 3, 16);
    CHECK(p != NULL);
    CHECK_INT((size_t)((uintptr_t)p % 16u), 0);

    p = arena_alloc(&a, 3, 64);
    CHECK(p != NULL);
    CHECK_INT((size_t)((uintptr_t)p % 64u), 0);
}

static void test_arena_oom(void) {
    Arena a;

    CHECK_INT(arena_init(&a, g_arena_mem, 8), 0);
    CHECK(arena_alloc(&a, 16, 8) == NULL);
    CHECK_INT(arena_last_status(&a), ARENA_E_NOMEM);
}

static void test_arena_bad_alignment(void) {
    Arena a;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK(arena_alloc(&a, 4, 3) == NULL);
    CHECK_INT(arena_last_status(&a), ARENA_E_UNALIGNED);
    CHECK(arena_alloc(&a, 4, 0) == NULL);
    CHECK_INT(arena_last_status(&a), ARENA_E_UNALIGNED);
}

static void test_arena_array_overflow(void) {
    Arena a;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK(arena_alloc_array(&a, (size_t)-1, 64, 8) == NULL);
    CHECK_INT(arena_last_status(&a), ARENA_E_NOMEM);
}

static void test_arena_reset(void) {
    Arena a;
    void *p;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    p = arena_alloc(&a, 100, 8);
    CHECK(p != NULL);
    CHECK_INT(arena_used(&a), 100);
    CHECK_INT(arena_peak(&a), 100);

    arena_reset(&a);
    CHECK_INT(arena_used(&a), 0);
    CHECK(arena_last_status(&a) == ARENA_OK);
    /* the arena hands out the same block again */
    CHECK(arena_alloc(&a, 100, 8) == p);
}

static void test_arena_copy(void) {
    static const char src[] = "quest";
    Arena a;
    char *dst;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    dst = arena_copy(&a, src, 5);
    CHECK(dst != NULL);
    CHECK(memcmp(dst, src, 5) == 0);
    CHECK(arena_copy(&a, NULL, 5) == NULL);
    CHECK_INT(arena_last_status(&a), ARENA_E_NULL);
}











static void test_utf8_decode_ascii(void) {
    const uint8_t s[] = { 'A' };
    uint32_t cp = 0;

    CHECK_INT(utf8_decode(s, 1, &cp), 1);
    CHECK_INT(cp, (uint32_t)'A');
}

static void test_utf8_decode_multibyte(void) {
    const uint8_t a[] = { 0xD0u, 0x90u };        /* U+0410 CYRILLIC A */
    const uint8_t b[] = { 0xE2u, 0x84u, 0x96u }; /* U+2116 NUMERO SIGN */
    uint32_t cp = 0;

    CHECK_INT(utf8_decode(a, sizeof a, &cp), 2);
    CHECK_INT(cp, 0x0410u);
    CHECK_INT(utf8_decode(b, sizeof b, &cp), 3);
    CHECK_INT(cp, 0x2116u);
}

static void test_utf8_decode_rejects_invalid(void) {
    const uint8_t continuation[] = { 0x80u };
    const uint8_t overlong[] = { 0xC0u, 0xAFu };
    const uint8_t c1[] = { 0xC1u, 0xBFu };
    const uint8_t surrogate[] = { 0xEDu, 0xA0u, 0x80u };
    const uint8_t too_big[] = { 0xF5u, 0x80u, 0x80u, 0x80u };
    const uint8_t bad_tail[] = { 0xE2u, 0x84u, 0x41u };
    uint32_t cp = 0;

    CHECK_INT(utf8_decode(continuation, sizeof continuation, &cp), 0);
    CHECK_INT(utf8_decode(overlong, sizeof overlong, &cp), 0);
    CHECK_INT(utf8_decode(c1, sizeof c1, &cp), 0);
    CHECK_INT(utf8_decode(surrogate, sizeof surrogate, &cp), 0);
    CHECK_INT(utf8_decode(too_big, sizeof too_big, &cp), 0);
    CHECK_INT(utf8_decode(bad_tail, sizeof bad_tail, &cp), 0);
    /* a truncated sequence must fail too, not read past the end */
    CHECK_INT(utf8_decode(bad_tail, 2, &cp), 0);
    CHECK_INT(utf8_decode(NULL, 4, &cp), 0);
    CHECK_INT(utf8_decode(bad_tail, 0, &cp), 0);
}

static void test_utf8_roundtrip(void) {
    static const uint32_t cps[] = {
        0x41u, 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0x410u,
        0x2116u, 0xFFFDu, 0xFFFFu, 0x10000u, 0x10FFFFu
    };
    size_t i;

    for (i = 0; i < sizeof cps / sizeof cps[0]; i++) {
        uint8_t buf[UTF8_MAX_BYTES];
        uint32_t back = 0;
        size_t n = utf8_encode(cps[i], buf);
        size_t m;

        CHECK(n > 0);
        m = utf8_decode(buf, n, &back);
        CHECK_INT(m, n);
        CHECK_INT(back, cps[i]);
    }
    CHECK_INT(utf8_encode(0xD800u, (uint8_t[UTF8_MAX_BYTES]){0}), 0);
    CHECK_INT(utf8_encode(0x110000u, (uint8_t[UTF8_MAX_BYTES]){0}), 0);
}

static void test_utf8_length_and_offset(void) {
    /* "A" U+0410 U+2116 "B" */
    const uint8_t s[] = { 0x41u, 0xD0u, 0x90u, 0xE2u, 0x84u, 0x96u, 0x42u };

    CHECK_INT(utf8_length(s, sizeof s), 4);
    CHECK_INT(utf8_offset(s, sizeof s, 0), 0);
    CHECK_INT(utf8_offset(s, sizeof s, 1), 1);
    CHECK_INT(utf8_offset(s, sizeof s, 2), 3);
    CHECK_INT(utf8_offset(s, sizeof s, 3), 6);
    CHECK_INT(utf8_offset(s, sizeof s, 99), sizeof s);
}

static void test_utf8_valid(void) {
    const uint8_t good[] = { 0xD0u, 0x90u, 0x41u };
    const uint8_t bad[] = { 0xD0u, 0x41u };

    CHECK_INT(utf8_valid(good, sizeof good), 1);
    CHECK_INT(utf8_valid(bad, sizeof bad), 0);
    CHECK_INT(utf8_valid(good, 0), 1);
}

/* ---------------------------------------------------------------- script -- */

static char *read_file(Arena *a, const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = arena_alloc(a, (size_t)size + 1u, 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[(size_t)size] = '\0';
    if (len != NULL) {
        *len = (size_t)size;
    }
    return buf;
}

static ScriptStatus load_text(Arena *a, Script *s, const char *text) {
    return script_load(a, s, text, strlen(text), NULL);
}

/* Sym 0 is a real interned symbol, so "not found" cannot be 0. */
#define SYM_NONE 0xFFFFFFFFu

/* A command is a run of words now, and every call site would otherwise have to build
 * an array inline. */
static int cmd2(Game *g, const Script *s, const char *a, const char *b);
static int has2(const Game *g, const Script *s, const char *a, const char *b);

static Sym sym_named(const Script *s, const char *name) {
    size_t i;
    for (i = 0; i < s->sym_count; i++) {
        if (script_sym_eq(s, (Sym)i, name)) {
            return (Sym)i;
        }
    }
    return SYM_NONE;
}

static int cmd2(Game *g, const Script *s, const char *a, const char *b) {
    Sym w[2];

    w[0] = sym_named(s, a);
    w[1] = sym_named(s, b);
    return game_command(g, w, 2);
}

static int has2(const Game *g, const Script *s, const char *a, const char *b) {
    Sym w[2];

    w[0] = sym_named(s, a);
    w[1] = sym_named(s, b);
    return game_available(g, w, 2);
}

static const Rule *find_rule(const Script *s, const char *room, const char *verb,
                             const char *object, size_t nth) {
    size_t i;
    Sym rid = 0;
    size_t seen = 0;
    const ScriptRoom *r;

    /* Locate the room by matching its interned id. */
    for (i = 0; i < s->room_count; i++) {
        if (script_sym_eq(s, s->rooms[i].id, room)) {
            rid = s->rooms[i].id;
            break;
        }
    }
    r = script_room_by_id(s, rid);
    if (r == NULL) {
        return NULL;
    }
    for (i = 0; i < r->rule_len; i++) {
        if (script_sym_eq(s, r->rules[i].words[0], verb) &&
            script_sym_eq(s, r->rules[i].words[1], object)) {
            if (seen == nth) {
                return &r->rules[i];
            }
            seen++;
        }
    }
    return NULL;
}

/* Every script the game ships has to load and validate without a single
 * diagnostic; a hand written one that warns is an authoring slip and should not
 * reach a player. Listed by name rather than globbed on purpose: heart was missing
 * from this list for as long as the list existed, and nothing noticed, because a
 * glob is exactly the thing that would have caught it. */
static void test_every_shipped_script_is_clean(void) {
    static const char *const paths[] = {
        "assets/script/tutorial.script",
        "assets/script/field.script",
        "assets/script/heart.script"
    };
    size_t k;

    for (k = 0; k < sizeof paths / sizeof paths[0]; k++) {
        Arena a;
        Script s;
        Diagnostic d[64];
        size_t n = 0;
        size_t errors;
        char *text;
        int line = 0;

        /* read_file allocates from the arena, so the arena has to exist first. */
        CHECK_INT(arena_init(&a, g_arena_mem, ARENA_BYTES), 0);
        text = read_file(&a, paths[k], NULL);
        CHECK(text != NULL);
        if (text == NULL) {
            continue;
        }
        CHECK_INT(script_load(&a, &s, text, strlen(text), &line), SCR_OK);
        CHECK(s.room_count > 0);
        errors = script_validate(&s, d, sizeof d / sizeof d[0], &n);
        CHECK_INT(errors, 0);
        CHECK_INT(n, 0);
        if (n > 0) {
            fprintf(stderr, "%s line %d: %s\n", paths[k], d[0].line,
                    script_verify_str(d[0].code));
        }
    }
}

static void test_script_loads_real_asset(void) {
    Arena a;
    Script s;
    char *text;
    size_t len = 0;
    ScriptStatus st;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK(len > 0);

    st = script_load(&a, &s, text, len, NULL);
    CHECK(st == SCR_OK);
    if (st != SCR_OK) {
        return;
    }
    CHECK_INT(s.room_count, 4);
    CHECK(s.rule_count > 0);
    CHECK(s.sym_count > 0);
}

static void test_script_room_names_and_titles(void) {
    Arena a;
    Script s;
    char *text;
    size_t len = 0;
    size_t n;
    size_t i;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL || script_load(&a, &s, text, len, NULL) != SCR_OK) {
        return;
    }

    CHECK(script_sym_eq(&s, s.rooms[0].id, "офис"));
    CHECK(script_sym_eq(&s, s.rooms[1].id, "холл"));
    CHECK(script_sym_eq(&s, s.rooms[2].id, "библиотека"));
    CHECK(script_sym_eq(&s, s.rooms[3].id, "аудитория"));

    /* Cyrillic identifiers must survive interning byte-exact. */
    CHECK(script_sym(&s, s.rooms[0].id, &n) != NULL);
    CHECK_INT(n, strlen("офис")); /* офис */
    CHECK(memcmp(script_sym(&s, s.rooms[0].id, &n), "офис", n) == 0);

    /* The title is the rest of the room header line. */
    CHECK_INT(s.rooms[0].title_len, strlen("Офис лектора")); /* Офис лектора */
    CHECK(memcmp(s.rooms[0].title, "Офис лектора", s.rooms[0].title_len) == 0);

    /* Description fragments belong to the room that declared them. */
    CHECK_INT(s.rooms[0].frag_len, 4);
    CHECK_INT(s.rooms[1].frag_len, 3);
    for (i = 0; i < s.room_count; i++) {
        CHECK(s.rooms[i].frag_len > 0);
        CHECK(s.rooms[i].rule_len > 0);
        CHECK(s.rooms[i].line > 0);
    }
}

static void test_script_interns_shared_identifiers(void) {
    Arena a;
    Script s;
    char *text;
    size_t len = 0;
    Sym key_hall = 0;
    size_t i;
    int shared = 0;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL || script_load(&a, &s, text, len, NULL) != SCR_OK) {
        return;
    }

    /* "ключ" appears in офис, in холл and in библиотека: one symbol, not three. */
    for (i = 0; i < s.sym_count; i++) {
        if (script_sym_eq(&s, (Sym)i, "ключ")) { /* ключ */
            key_hall = (Sym)i;
        }
    }
    CHECK(key_hall != 0);

    for (i = 0; i < s.rule_count; i++) {
        uint32_t c;
        for (c = 0; c < s.rules[i].guard_len; c++) {
            if (s.rules[i].guard[c].name == key_hall) {
                shared = 1;
            }
        }
    }
    CHECK(shared);
}

static void test_script_rule_order_and_guards(void) {
    Arena a;
    Script s;
    char *text;
    size_t len = 0;
    const Rule *r0;
    const Rule *r1;
    const Rule *r2;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL || script_load(&a, &s, text, len, NULL) != SCR_OK) {
        return;
    }

    r0 = find_rule(&s, "холл", /* холл */
                   "идти",             /* идти */
                   "библиотека", 0);
    r1 = find_rule(&s, "холл", "идти",
                   "библиотека", 1);
    r2 = find_rule(&s, "холл", "идти",
                   "библиотека", 2);
    CHECK(r0 != NULL);
    CHECK(r1 != NULL);
    CHECK(r2 != NULL);
    if (r0 == NULL || r1 == NULL || r2 == NULL) {
        return;
    }
    CHECK(r0->words[0] == r1->words[0] && r0->words[0] == r2->words[0]);
    CHECK(r0->words[1] == r1->words[1] && r0->words[1] == r2->words[1]);
    /* source order must be preserved: this is what makes first-match-wins work */
    CHECK(r0->line < r1->line);
    CHECK(r1->line < r2->line);

    CHECK_INT(r0->act.kind, ACT_END);

    /* The successful variant moves, and now carries its message on the move
     * itself instead of needing a second action. */
    CHECK_INT(r2->act.kind, ACT_GO);
    CHECK(r2->act.text_len > 0);
    CHECK(script_room_by_id(&s, r2->act.target) != NULL);
    CHECK(script_sym_eq(&s, r2->act.target,
                        "библиотека"));
}

static void test_script_effects_and_guards(void) {
    Arena a;
    Script s;
    char *text;
    size_t len = 0;
    const Rule *r;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL || script_load(&a, &s, text, len, NULL) != SCR_OK) {
        return;
    }

    /* взять ключ [-ключ] : [ключ] say ... */
    r = find_rule(&s, "офис", /* офис */
                  "взять", /* взять */
                  "ключ",         /* ключ */
                  0);
    CHECK(r != NULL);
    if (r == NULL) {
        return;
    }
    CHECK_INT(r->guard_len, 1);
    CHECK_INT(r->guard[0].present, 0);
    CHECK_INT(r->effect_len, 1);
    CHECK_INT(r->effects[0].present, 1);
    CHECK(script_sym_eq(&s, r->effects[0].name, "ключ"));
    /* the guard and the effect name the same flag */
    CHECK(r->guard[0].name == r->effects[0].name);
    CHECK_INT(r->act.kind, ACT_SAY);

    /* открыть сейф [записка -_Сейф_открыт] : [_Сейф_открыт] say ... */
    r = find_rule(&s, "офис",
                  "открыть", /* открыть */
                  "сейф",                            /* сейф */
                  1);
    CHECK(r != NULL);
    if (r == NULL) {
        return;
    }
    CHECK_INT(r->guard_len, 2);
    CHECK_INT(r->guard[0].present, 1);
    CHECK_INT(r->guard[1].present, 0);
    CHECK_INT(r->effect_len, 1);
    CHECK(script_sym_eq(&s, r->effects[0].name,
                        "_Сейф_открыт")); /* _Сейф_открыт */
}

static void test_script_validate_real_asset(void) {
    Arena a;
    Script s;
    char *text;
    size_t len = 0;
    Diagnostic diags[16];
    size_t n = 0;
    size_t errors;
    size_t i;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL || script_load(&a, &s, text, len, NULL) != SCR_OK) {
        return;
    }

    errors = script_validate(&s, diags, sizeof diags / sizeof diags[0], &n);
    /* Every flag the sample reads is set somewhere, every go target is a real
     * room, all four rooms are reachable and no rule is shadowed, so a hand
     * written script that produces any diagnostic at all is a defect. */
    CHECK_INT(errors, 0);
    CHECK_INT(n, 0);
    for (i = 0; i < n; i++) {
        CHECK(diags[i].code != V_E_UNDEFINED_FLAG);
        CHECK(diags[i].code != V_W_SHADOWED_RULE);
        CHECK(diags[i].code != V_W_UNRESOLVED_TARGET);
        CHECK(diags[i].code != V_W_UNREACHABLE_ROOM);
    }
}

static void test_script_start_directive(void) {
    static const char text[] =
        "start холл\n"
        "room офис Офис\n"
        ": Где-то.\n"
        "идти холл : go холл\n"
        "room холл Холл\n"
        ": Пусто.\n";
    Arena a;
    Script s;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK(load_text(&a, &s, text) == SCR_OK);
    if (s.room_count == 0) {
        return;
    }
    CHECK_INT(s.have_start_directive, 1);
    CHECK(script_sym_eq(&s, s.start, "холл")); /* холл */
    CHECK(script_room_by_id(&s, s.start) != NULL);
    /* офис is now unreachable, so exactly one warning and no errors */
    {
        Diagnostic d[8];
        size_t n = 0;
        CHECK_INT(script_validate(&s, d, 8, &n), 0);
        CHECK_INT(n, 1);
        if (n == 1) {
            CHECK_INT(d[0].code, V_W_UNREACHABLE_ROOM);
        }
    }
}

static void test_script_detects_undefined_flag(void) {
    static const char text[] =
        "room офис Офис\n"
        ": Где-то.\n"
        "-выдумка: Никогда.\n";
    Arena a;
    Script s;
    Diagnostic d[8];
    size_t n = 0;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK(load_text(&a, &s, text) == SCR_OK);
    if (s.room_count == 0) {
        return;
    }
    CHECK_INT(script_validate(&s, d, 8, &n), 1);
    CHECK(n >= 1);
    if (n >= 1) {
        CHECK_INT(d[0].code, V_E_UNDEFINED_FLAG);
        CHECK_INT(d[0].severity, SEV_ERROR);
        CHECK(d[0].line > 0);
    }
}

static void test_script_detects_shadowed_rule(void) {
    static const char text[] =
        "room офис Офис\n"
        ": Где-то.\n"
        "открыть сейф : Всегда.\n"
        "открыть сейф +ключ : Никогда.\n"
        "взять ключ -ключ : +ключ Взял.\n";
    Arena a;
    Script s;
    Diagnostic d[8];
    size_t n = 0;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK(load_text(&a, &s, text) == SCR_OK);
    if (s.room_count == 0) {
        return;
    }
    CHECK_INT(script_validate(&s, d, 8, &n), 0);
    CHECK_INT(n, 2);
    if (n == 2) {
        CHECK_INT(d[0].code, V_W_SHADOWED_RULE);
        CHECK_INT(d[1].code, V_W_IMPLICIT_START);
    }
}

static void test_script_detects_duplicate_room(void) {
    static const char text[] =
        "room офис Офис\n"
        ": Раз.\n"
        "room офис Офис\n"
        ": Два.\n";
    Arena a;
    Script s;
    Diagnostic d[8];
    size_t n = 0;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK(load_text(&a, &s, text) == SCR_OK);
    if (s.room_count == 0) {
        return;
    }
    CHECK_INT(script_validate(&s, d, 8, &n), 1);
    CHECK(n >= 1);
    if (n >= 1) {
        CHECK_INT(d[0].code, V_E_DUPLICATE_ROOM);
    }
}

/* One test per production of doc/grammar.bnf, named after it. The grammar is written
 * down once, in that file; these are the sentences it makes, so a change to either
 * side that the other does not share fails here instead of in front of a player. */
static void test_grammar_rule(void) {
    static const char one_word[] =
        "room a A\n: ok\nбеги : нет\n";
    static const char four_words[] =
        "room a A\n: ok\nвзять x y z -f : нет\n";
    static const char five_words[] =
        "room a A\n: ok\nвзять x y z w -f : нет\n";
    Arena a;
    Script s;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    /* word* is one or more for a command, so the extremes both have to hold. */
    CHECK_INT(script_load(&a, &s, one_word, sizeof one_word - 1, NULL), SCR_OK);
    CHECK_INT(script_load(&a, &s, four_words, sizeof four_words - 1, NULL), SCR_OK);
    /* RULE_MAX_WORDS is the width of the palette, so a fifth word is a command the
     * player could not type. */
    CHECK_INT(script_load(&a, &s, five_words, sizeof five_words - 1, NULL), SCR_E_SYNTAX);
}

static void test_grammar_rule_with_no_words_is_a_description(void) {
    static const char bare[] = "room a A\n: ok\n";
    static const char signed_head[] = "room a A\n: ok\n-flag +flag2: текст.\n";
    static const char unsigned_word[] = "room a A\n: ok\nflag: текст.\n";
    Arena a;
    Script s;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(script_load(&a, &s, bare, sizeof bare - 1, NULL), SCR_OK);
    CHECK_INT(script_load(&a, &s, signed_head, sizeof signed_head - 1, NULL), SCR_OK);
    /* An unsigned word in the head is a command, so this is a one word command that
     * leads nowhere. It has to load: the format does not forbid it, and a loader that
     * guessed otherwise would be the one thing the grammar does not promise. */
    CHECK_INT(script_load(&a, &s, unsigned_word, sizeof unsigned_word - 1, NULL), SCR_OK);
}

static void test_grammar_action(void) {
    static const char head[] = "start a\nroom a A\nroom b B\n";
    static const char goto_bare[] = "room a A\n: ok\nидти b : go b\n";
    static const char goto_text[] = "room a A\n: ok\nидти b : go b ушли.\n";
    static const char end_bare[] = "room a A\n: ok\nидти b : end\n";
    static const char quote[] = "room a A\n: ok\nидти b : \"нет-а\", — он сказал.\n";
    Arena a;
    Script s;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    /* The text after a cmd is optional. 51 lines of the converted games are a bare
     * "go room", so this is not a corner case. */
    CHECK_INT(script_load(&a, &s, goto_bare, sizeof goto_bare - 1, NULL), SCR_OK);
    CHECK_INT(script_load(&a, &s, goto_text, sizeof goto_text - 1, NULL), SCR_OK);
    CHECK_INT(script_load(&a, &s, end_bare, sizeof end_bare - 1, NULL), SCR_OK);
    /* text is text: a quotation mark is as ordinary at the start of it as a letter. */
    CHECK_INT(script_load(&a, &s, quote, sizeof quote - 1, NULL), SCR_OK);
    (void)head;
}

static void test_script_rejects_bad_syntax(void) {
    static const char no_colon[] = "room a A\n: ok\nвзять x нет\n";
    static const char bad_bracket[] = "room a A\n: ok\nвзять x [-y нет\n";
    static const char tilde_effect[] = "room a A\n: ok\nвзять x : +-y нет\n";
    static const char rule_before_room[] = "взять x : нет\nroom a A\n: ok\n";
    static const char no_rooms[] = "# только комментарий\n";
    /* Five words in the head is one more than the palette can show. */
    static const char wide_command[] = "room a A\n: ok\nвзять x y z w -v нет\n";
    /* A bare word where the items end: the format has nowhere to put it. */
    static const char bare_item[] = "room a A\n: ok\nвзять x -y z : нет\n";
    static const char bare_text[] = "room a A\n: ok\nвзять x : прыгнуть\n";
    Arena a;
    Script s;
    int line = -1;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(script_load(&a, &s, no_colon, sizeof no_colon - 1, &line), SCR_E_SYNTAX);
    /* The loader has to point at the offending line, not just refuse. */
    CHECK(line > 0);
    CHECK_INT(script_load(&a, &s, bad_bracket, sizeof bad_bracket - 1, NULL), SCR_E_SYNTAX);
    CHECK_INT(script_load(&a, &s, tilde_effect, sizeof tilde_effect - 1, NULL), SCR_E_SYNTAX);
    CHECK_INT(script_load(&a, &s, rule_before_room, sizeof rule_before_room - 1, NULL), SCR_E_SYNTAX);
    CHECK_INT(script_load(&a, &s, no_rooms, sizeof no_rooms - 1, NULL), SCR_E_NO_ROOMS);
    /* An unknown word after the colon is prose, not a mistyped action keyword: the
     * format has no action name that could be wrong. */
    CHECK_INT(script_load(&a, &s, bare_text, sizeof bare_text - 1, NULL), SCR_OK);
    /* One word before the colon is a command now, not a description with a
     * condition. A description says +flag or nothing at all. */
    CHECK_INT(script_load(&a, &s, "room a A\n+x: нет\n", sizeof("room a A\n+x: нет\n") - 1,
                         NULL), SCR_OK);
    CHECK_INT(script_load(&a, &s, wide_command, sizeof wide_command - 1, NULL),
              SCR_E_SYNTAX);
    CHECK_INT(script_load(&a, &s, bare_item, sizeof bare_item - 1, NULL), SCR_E_SYNTAX);
    /* Every item carries its sign, and a command may be three words long. */
    CHECK_INT(script_load(&a, &s, "room a A\n: ok\nвзять x y z -a +b : нет\n",
                         sizeof("room a A\n: ok\nвзять x y z -a +b : нет\n") - 1, NULL),
              SCR_OK);
}

static void test_script_rejects_invalid_utf8(void) {
    /* 0xD0 0x28 is a truncated two-byte sequence */
    static const char bad[] = "room \xd0\x28 A\n: ok\n";
    Arena a;
    Script s;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(script_load(&a, &s, bad, sizeof bad - 1, NULL), SCR_E_UTF8);
}

static void test_script_reports_out_of_memory(void) {
    Arena a;
    Arena tiny;
    Script s;
    char *text;
    size_t len = 0;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, SCRIPT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    /* The text lives in the first arena; the load then gets a starved one. */
    CHECK_INT(arena_init(&tiny, g_scratch_mem, 16), 0);
    CHECK_INT(script_load(&tiny, &s, text, len, NULL), SCR_E_MEMORY);
}

/* -------------------------------------------------------------------- io -- */

#define FB_W 16
#define FB_H 12
#define FB_PX (FB_W * FB_H)

static uint32_t g_fb[FB_PX];
static Arena g_arena;

static void fb_reset(IoCtx *ctx) {
    size_t i;
    for (i = 0; i < FB_PX; i++) {
        g_fb[i] = 0x00010101u; /* a colour no test draws */
    }
    CHECK_INT(io_init(ctx, g_fb, FB_W, FB_H), 1);
}

static uint32_t rgb_of(IoColor c) {
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

static int px_is(int32_t x, int32_t y, IoColor c) {
    return g_fb[(size_t)y * FB_W + x] == rgb_of(c);
}

static int count_color(IoColor c) {
    uint32_t want = rgb_of(c);
    size_t i;
    int n = 0;
    for (i = 0; i < FB_PX; i++) {
        if (g_fb[i] == want) {
            n++;
        }
    }
    return n;
}

static IoSeg *edge(IoSeg *out, uint32_t i, IoFixed x0, IoFixed y0, IoFixed x1, IoFixed y1) {
    out[i].x0 = x0; out[i].y0 = y0; out[i].x1 = x1; out[i].y1 = y1;
    return out;
}



/* The loader once stored the trimmed end of a text as its pointer and the correct
 * span as its length, so every say began at its own far end and ran on into the
 * lines that followed. Lengths looked right, the parser was happy, and nothing
 * noticed until the game module actually read the bytes. This pins the bytes. */
static void test_script_text_points_at_the_text(void) {
    Arena a;
    Script s;
    const ScriptRoom *r;
    size_t i;

    CHECK_INT(arena_init(&a, g_arena_mem, ARENA_BYTES), 0);
    CHECK_INT(load_text(&a, &s,
                        "room a A\n"
                        ": первая строка.\n"
                        "+ф: вторая строка.\n"
                        "взять x : взято.\n"), SCR_OK);
    r = script_room_by_id(&s, sym_named(&s, "a"));
    CHECK_INT(r->frag_len, 2);
    CHECK_INT(r->rules[0].act.text_len, strlen("взято."));
    for (i = 0; i < r->frag_len; i++) {
        /* No text may contain a newline: a span that runs past its own line is
         * exactly what the old bug looked like from the outside. */
        CHECK(memchr(r->frags[i].text, '\n', r->frags[i].text_len) == NULL);
        CHECK(memchr(r->rules[0].act.text, '\n',
                     r->rules[0].act.text_len) == NULL);
    }
    CHECK(memcmp(r->frags[0].text, "первая строка.", r->frags[0].text_len) == 0);
    CHECK_INT(r->frags[0].text_len, strlen("первая строка."));
    CHECK(memcmp(r->frags[1].text, "вторая строка.", r->frags[1].text_len) == 0);
    CHECK(memcmp(r->rules[0].act.text, "взято.",
                 r->rules[0].act.text_len) == 0);
}

static void test_script_go_target_resolves(void) {
    Arena a;
    Script s;
    const ScriptRoom *r;

    CHECK_INT(arena_init(&a, g_arena_mem, ARENA_BYTES), 0);
    CHECK_INT(load_text(&a, &s,
                        "room a A\n"
                        "уйти b : go b ушли.\n"
                        "room b B\n"
                        ": B.\n"), SCR_OK);
    r = script_room_by_id(&s, sym_named(&s, "a"));
    CHECK_INT(r->rules[0].act.kind, (int)ACT_GO);
    /* The trailing full stop must not become part of the room name. Before it
     * was a delimiter, "go b." linked to a room called "b." and never resolved. */
    CHECK(script_sym_eq(&s, r->rules[0].act.target, "b"));
    CHECK(script_room_by_id(&s, r->rules[0].act.target) != NULL);
    /* The message rides on the move, which is the whole reason the format has
     * text on a go: it prints before the destination room. */
    CHECK(memcmp(r->rules[0].act.text, "ушли.",
                 r->rules[0].act.text_len) == 0);
}

/* ------------------------------------------------------------- game run -- */

static const char kGameScript[] =
    "start кухня\n"
    "room кухня Кухня\n"
    ": Только кухня, дверь и ключ на полу.\n"
    "-открыта: Дверь закрыта.\n"
    "+открыта: В проём сочится свет.\n"
    "осмотреть пол : Пусто, но чисто.\n"
    "осмотреть дверь : Дверь деревянная.\n"
    "взять ключ -взял : +взял Ключ теперь у меня.\n"
    "взять ключ : Ключ был уже взят.\n"
    "открыть дверь -открыта +взял : +открыта Дверь открыта.\n"
    "закрыть дверь +открыта : Дверь закрыта.\n"
    "уйти кухня -открыта : go спальня\n"
    "room спальня Спальня\n"
    ": Темнота.\n"
    "осмотреть тень : Ничего.\n"
    "спрятаться тень : win Вы победили.\n"
    "подойти к тени : Кто-то шепчет: не оборачивайся.\n"
    "подойти к тени +открыта : Стало тихо.\n";

/* Log lines are (pointer, length) into the script source and are not terminated,
 * so comparing them with strcmp would run off the end of the line. */
static int log_line_is(const Game *g, size_t i, const char *lit) {
    return i < g->log_count && strlen(lit) == g->log_len[i] &&
           memcmp(g->log_text[i], lit, g->log_len[i]) == 0;
}

static int log_has(const Game *g, const char *text) {
    size_t i;
    for (i = 0; i < g->log_count; i++) {
        if (strlen(text) == g->log_len[i] &&
            memcmp(g->log_text[i], text, g->log_len[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The loader must accept this fixture before any of the runtime is exercised, so
 * a grammar change shows up here instead of as a cascade of empty logs. */
static void game_setup(Arena *a, Script *s, Game *g) {
        CHECK_INT(arena_init(a, g_arena_mem, ARENA_BYTES), 0);
    CHECK_INT(script_load(a, s, kGameScript, strlen(kGameScript), NULL), SCR_OK);
    CHECK_INT(game_init(g, s), GAME_OK);
}

/* A bare action is prose to the end of the line, so a word that could have been a
 * second action is simply part of the text. There is no separator to get wrong,
 * which is the reason the format has none. */
static void test_bare_action_runs_to_end_of_line(void) {
    Arena a;
    Script s;
    Game g;
    const ScriptRoom *r;

    CHECK_INT(arena_init(&a, g_arena_mem, ARENA_BYTES), 0);
    CHECK_INT(load_text(&a, &s, "room a A\nпрыгнуть мяч : раз.\n"), SCR_OK);
    r = script_room_by_id(&s, sym_named(&s, "a"));
    CHECK_INT(r->rule_len, 1);
    CHECK_INT(1, 1);
    CHECK_INT((int)r->rules[0].act.kind, (int)ACT_SAY);
    CHECK(memcmp(r->rules[0].act.text, "раз.", r->rules[0].act.text_len) == 0);
    CHECK_INT(game_init(&g, &s), GAME_OK);
    CHECK_INT(cmd2(&g, &s, "прыгнуть", "мяч"), 1);
    CHECK_INT(g.finished, 0);
    CHECK_INT(log_has(&g, "раз."), 1);
}

/* The log is read as blocks: a heading and the paragraph under it. A room
 * description is written as several says and has to arrive as one paragraph, and a
 * command has to be named, or the screen shows prose with no idea what caused it.
 * This is the structure the drawing code walks, so it is worth pinning down. */
static void test_game_log_is_blocks_with_headings(void) {
    Arena a;
    Script s;
    Game g;
    size_t heads = 0;
    size_t i;

    game_setup(&a, &s, &g);
    /* The start room: heading, then one line per description fragment. */
    CHECK_INT(g.log_count, 3);
    CHECK_INT(g.log_headed[0], 1);
    CHECK_INT(g.log_headed[1], 0);
    CHECK_INT(g.log_headed[2], 0);
    CHECK(log_line_is(&g, 0, "Кухня"));
    CHECK(log_line_is(&g, 1, "Только кухня, дверь и ключ на полу."));
    CHECK(log_line_is(&g, 2, "Дверь закрыта."));

    /* A command opens a block named after itself, before it says anything. */
    CHECK_INT(cmd2(&g, &s, "осмотреть", "пол"), 1);
    CHECK_INT(g.log_count, 5);
    CHECK_INT(g.log_headed[3], 1);
    CHECK_INT(g.log_head[3], NULL);
    CHECK_INT(g.log_word_len[3], 2);
    CHECK(script_sym_eq(&s, g.log_words[3][0], "осмотреть"));
    CHECK(script_sym_eq(&s, g.log_words[3][1], "пол"));
    CHECK(log_line_is(&g, 4, "Пусто, но чисто."));

    /* Going somewhere opens a room block after the command block. */
    CHECK_INT(cmd2(&g, &s, "уйти", "кухня"), 1);
    CHECK_INT(g.log_count, 8);
    CHECK_INT(g.log_headed[6], 1);
    CHECK(log_line_is(&g, 6, "Спальня"));
    CHECK(log_line_is(&g, 7, "Темнота."));

    for (i = 0; i < g.log_count; i++) {
        heads += g.log_headed[i];
    }
    CHECK_INT(heads, 4);
}

static void test_game_enters_start_room(void) {
    Arena a;
    Script s;
    Game g;

    game_setup(&a, &s, &g);
    /* Title first, then every fragment whose guard holds, in source order. */
    CHECK_INT(g.log_count, 3);
    CHECK(log_line_is(&g, 0, "Кухня"));
    CHECK(log_line_is(&g, 1, "Только кухня, дверь и ключ на полу."));
    CHECK(log_line_is(&g, 2, "Дверь закрыта."));
    CHECK_INT(log_has(&g, "В проём сочится свет."), 0);
}

static void test_game_palette_follows_guards(void) {
    Arena a;
    Script s;
    Game g;
    Sym verbs[8];
    Sym objects[8];

    game_setup(&a, &s, &g);
    /* Source order, and only rules whose guard holds. "открыть" needs the key
     * already in hand and "закрыть" needs the door already open, so neither is
     * offered yet: the palette is the guard, with no separate availability
     * concept to keep in step with it. */
    /* One call, asked once per slot: the first ask has an empty prefix and yields the
     * verbs, the next asks what may follow a verb and yields the objects. */
    CHECK_INT(game_next(&g, NULL, 0, verbs, 8), 3);
    CHECK(script_sym_eq(&s, verbs[0], "осмотреть"));
    CHECK(script_sym_eq(&s, verbs[1], "взять"));
    CHECK(script_sym_eq(&s, verbs[2], "уйти"));

    {
        Sym prefix[1];

        prefix[0] = sym_named(&s, "взять");
        CHECK_INT(game_next(&g, prefix, 1, objects, 8), 1);
        CHECK(script_sym_eq(&s, objects[0], "ключ"));
        /* Two objects under one verb, in the order the room declares them. */
        prefix[0] = sym_named(&s, "осмотреть");
        CHECK_INT(game_next(&g, prefix, 1, objects, 8), 2);
        CHECK(script_sym_eq(&s, objects[0], "пол"));
        CHECK(script_sym_eq(&s, objects[1], "дверь"));
        /* Asking what may follow a whole command is not an answer: the palette uses it
         * to know the sentence is finished. */
        CHECK_INT(game_more(&g, prefix, 2), 0);
    }
}

/* An effect carries a sign like a guard does. -flag after the colon takes the flag
 * away, which is how a command drops what the player was holding. The strip along the
 * bottom shows set flags, so this is also what makes a thing leave it. */
static void test_game_effect_can_clear_a_flag(void) {
    static const char text[] =
        "start кухня\n"
        "room кухня Кухня\n"
        ": Только кухня.\n"
        "-нож: На столе лежит нож.\n"
        "+нож: Нож при вас.\n"
        "взять нож -нож : +нож Взяли нож.\n"
        "выбросить нож +нож : -нож Нож остался на полу.\n";
    Arena a;
    Script s;
    Game g;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(script_load(&a, &s, text, sizeof text - 1, NULL), SCR_OK);
    CHECK_INT(game_init(&g, &s), GAME_OK);
    CHECK_INT(game_flag_on(&g, 0), 0);

    CHECK_INT(cmd2(&g, &s, "взять", "нож"), 1);
    CHECK_INT(game_flag_on(&g, 0), 1);
    /* The guard on the description now fails and the other one prints instead, which
     * is the visible half of the same change. */
    CHECK_INT(log_has(&g, "Нож при вас."), 0);

    CHECK_INT(cmd2(&g, &s, "выбросить", "нож"), 1);
    CHECK_INT(game_flag_on(&g, 0), 0);
}

/* The description on screen is built from the room's fragments against the flags as
 * they are now. Reading it out of the log instead showed the room as it was on entry,
 * so a fragment that had appeared or vanished after a command stayed put. */
static void test_game_room_text_follows_flags(void) {
    static const char text[] =
        "start кухня\n"
        "room кухня Кухня\n"
        ": Только кухня.\n"
        "-нож: На столе лежит нож.\n"
        "+нож: Нож при вас.\n"
        "взять нож -нож : +нож Взяли нож.\n";
    char buf[256];
    Arena a;
    Script s;
    Game g;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(script_load(&a, &s, text, sizeof text - 1, NULL), SCR_OK);
    CHECK_INT(game_init(&g, &s), GAME_OK);

    CHECK_INT(game_room_text(&g, buf, sizeof buf) > 0, 1);
    CHECK_INT(strstr(buf, "На столе лежит нож.") != NULL, 1);
    CHECK_INT(strstr(buf, "Нож при вас.") == NULL, 1);

    /* The log is not rewritten: it keeps what was said, and the screen is what is
     * true. Those are different things the moment a flag moves. */
    CHECK_INT(cmd2(&g, &s, "взять", "нож"), 1);
    CHECK_INT(log_has(&g, "На столе лежит нож."), 1);

    CHECK_INT(game_room_text(&g, buf, sizeof buf) > 0, 1);
    CHECK(strstr(buf, "На столе лежит нож.") == NULL);
    CHECK_INT(strstr(buf, "Нож при вас.") != NULL, 1);
}

static void test_game_first_matching_rule_wins(void) {
    Arena a;
    Script s;
    Game g;
    Sym words[2];

    game_setup(&a, &s, &g);

    words[0] = sym_named(&s, "взять");
    words[1] = sym_named(&s, "ключ");

    /* The guarded variant comes first in the source and its guard holds. */
    CHECK_INT(game_command(&g, words, 2), 1);
    CHECK_INT(log_has(&g, "Ключ теперь у меня."), 1);
    CHECK_INT(log_has(&g, "Ключ был уже взят."), 0);

    /* Now its guard fails, so the later unconditional rule is the one that runs.
     * This ordering is the whole reason commands are first-match-wins. */
    CHECK_INT(game_command(&g, words, 2), 1);
    CHECK_INT(log_has(&g, "Ключ был уже взят."), 1);
}

static void test_game_effects_run_before_actions(void) {
    Arena a;
    Script s;
    Game g;
    Sym verbs[8];

    game_setup(&a, &s, &g);
    /* With the key in hand the door can be opened. */
    CHECK_INT(cmd2(&g, &s, "взять", "ключ"), 1);
    CHECK_INT(has2(&g, &s, "открыть", "дверь"), 1);
    CHECK_INT(has2(&g, &s, "закрыть", "дверь"), 0);

    CHECK_INT(cmd2(&g, &s, "открыть", "дверь"), 1);
    CHECK_INT(log_has(&g, "Дверь открыта."), 1);
    /* The effect landed before the action, so the palette has already changed. */
    CHECK_INT(has2(&g, &s, "закрыть", "дверь"), 1);
    /* Leaving needed the door shut, and it no longer is. */
    CHECK_INT(has2(&g, &s, "уйти", "кухня"), 0);
    /* Opening the door puts "открыть" and "уйти" out of reach again, so the
     * list swaps them for "закрыть" and stays the same size. */
    CHECK_INT(game_next(&g, NULL, 0, verbs, 8), 3);
    CHECK(script_sym_eq(&s, verbs[2], "закрыть"));
}

static void test_game_go_enters_the_target_room(void) {
    Arena a;
    Script s;
    Game g;

    game_setup(&a, &s, &g);
    CHECK_INT(cmd2(&g, &s, "уйти", "кухня"), 1);
    /* "go" prints the destination exactly as entering a room does. */
    CHECK(script_sym_eq(&s, g.room, "спальня"));
    CHECK_INT(log_has(&g, "Спальня"), 1);
    CHECK_INT(log_has(&g, "Темнота."), 1);
}

static void test_game_unknown_pair_does_nothing(void) {
    Arena a;
    Script s;
    Game g;
    size_t before;

    game_setup(&a, &s, &g);
    before = g.log_count;
    CHECK_INT(cmd2(&g, &s, "осмотреть", "стена"), 0);
    CHECK_INT(g.log_count, before);
}

static void test_game_terminal_action_stops_the_game(void) {
    Arena a;
    Script s;
    Game g;
    Sym verbs[8];

    game_setup(&a, &s, &g);
    CHECK_INT(cmd2(&g, &s, "уйти", "кухня"), 1);
    CHECK_INT(cmd2(&g, &s, "спрятаться", "тень"), 1);
    CHECK_INT(g.finished, 1);
    CHECK_INT(log_has(&g, "Вы победили."), 1);
    /* Nothing is offered and nothing runs once the game is over. */
    CHECK_INT(game_next(&g, NULL, 0, verbs, 8), 0);
    CHECK_INT(cmd2(&g, &s, "осмотреть", "тень"), 0);
}

static void test_game_log_drops_the_oldest_line(void) {
    Arena a;
    Script s;
    Game g;
    int i;

    game_setup(&a, &s, &g);
    for (i = 0; i < GAME_MAX_LOG + 20; i++) {
        game_say(&g, "x", 1);
    }
    CHECK_INT(g.log_count, GAME_MAX_LOG);
    /* The start room's title was pushed out long ago. */
    CHECK_INT(log_has(&g, "Кухня"), 0);
}

static void test_io_event_queue_survives_poll(void) {
    IoCtx ctx;
    IoEvent ev;
    IoEvent in;

    CHECK_INT(arena_init(&g_arena, g_arena_mem, ARENA_BYTES), 0);
    fb_reset(&ctx);
    CHECK_INT(io_backend_open(&ctx, &io_backend_test, "t"), 1);

    memset(&in, 0, sizeof in);
    in.kind = IO_EV_POINTER_DOWN;
    in.button = IO_BUTTON_LEFT;
    in.device = IO_DEVICE_MOUSE;
    in.x = 3;
    in.y = 4;
    io_post_event(&ctx, &in);

    /* io_poll refreshes the held state, which used to consume the queue and
     * starve the application, so the same event must still be readable. */
    io_poll(&ctx, 0);
    CHECK_INT(io_button_held(&ctx, IO_BUTTON_LEFT), 1);
    CHECK_INT(io_next_event(&ctx, &ev), 1);
    CHECK_INT(ev.kind, IO_EV_POINTER_DOWN);
    CHECK_INT(ev.button, IO_BUTTON_LEFT);
    CHECK_INT(io_next_event(&ctx, &ev), 0);

    in.kind = IO_EV_POINTER_UP;
    io_post_event(&ctx, &in);
    io_poll(&ctx, 0);
    CHECK_INT(io_button_held(&ctx, IO_BUTTON_LEFT), 0);
    io_backend_close(&ctx);
}

static void test_io_pointer_converts_to_virtual(void) {
    IoCtx ctx;
    IoEvent in;
    IoEvent ev;

    fb_reset(&ctx); /* 16 x 12 canvas */
    /* A window of 32 x 24 is exactly twice the canvas, so the mapping is easy to
     * state by hand: no bars, and every coordinate lands on an even pixel. */
    io_set_view(&ctx, 32, 24);
    /* The canvas rectangle is not stored anywhere, only worked out when it is needed,
     * so it is checked the only way it can be observed: a window pixel at the corner
     * and one past the far edge have to land on canvas 0,0 and 16,12. */
    {
        int32_t vx = -1;
        int32_t vy = -1;

        io_to_virtual(&ctx, 0, 0, &vx, &vy);
        CHECK_INT(vx, 0);
        CHECK_INT(vy, 0);
        io_to_virtual(&ctx, 32, 24, &vx, &vy);
        CHECK_INT(vx, 16);
        CHECK_INT(vy, 12);
    }

    memset(&in, 0, sizeof in);
    in.kind = IO_EV_POINTER_MOVE;
    in.x = 10;
    in.y = 8;
    io_post_event(&ctx, &in);
    io_poll(&ctx, 0);

    /* The queued event is rewritten, so the application never sees window pixels
     * even though the backend is the one that produced them. */
    CHECK_INT(io_next_event(&ctx, &ev), 1);
    CHECK_INT(ev.x, 5);
    CHECK_INT(ev.y, 4);
    CHECK_INT(io_pointer_x(&ctx), 5);
    CHECK_INT(io_pointer_y(&ctx), 4);
}

static void test_io_pointer_agrees_with_scaling(void) {
    IoCtx ctx;
    uint32_t out[20 * 20];
    int32_t vx, vy;
    int wx = -1, wy = -1;
    int x, y;

    fb_reset(&ctx); /* 16 x 12, ratio 4:3 */
    /* A square window forces letterbox bars, so the canvas no longer starts at
     * the window origin and the mapping is not a plain division. */
    io_set_view(&ctx, 20, 20);
    /* Bars top and bottom: the canvas starts two pixels down and is fifteen tall, so
     * the top bar maps to negative canvas and the bottom bar past the end. */
    {
        int32_t vx = -1;
        int32_t vy = -1;

        io_to_virtual(&ctx, 0, 0, &vx, &vy);
        CHECK_INT(vx, 0);
        CHECK(vy < 0);
        io_to_virtual(&ctx, 0, 19, &vx, &vy);
        CHECK(vy >= 12);
    }

    /* Paint one recognisable pixel, scale it into the window, then convert that
     * window pixel back. If the two halves of the letterbox arithmetic ever
     * disagree, a tap lands next to the thing it hit, which is the single most
     * annoying bug this layer can have. */
    for (y = 0; y < ctx.h; y++) {
        for (x = 0; x < ctx.w; x++) {
            ctx.pixels[(size_t)y * ctx.w + x] = 0x00010101u;
        }
    }
    ctx.pixels[(size_t)6 * ctx.w + 8] = 0x00ABCDEFu;
    io_scale_canvas(&ctx, out, 20, 20);

    /* Found by how far the pixel is from the background, not by exact equality: at a
     * fractional scale the scaler averages, so a lone pixel comes out blended with its
     * neighbours and never keeps its own colour. The point of the test is that the
     * window position maps back to the canvas pixel that was drawn, and that still has
     * a single answer even though the colour does not survive intact. */
    {
        uint32_t best = 0;

        for (y = 0; y < 20; y++) {
            for (x = 0; x < 20; x++) {
                uint32_t p = out[(size_t)y * 20 + x];
                uint32_t d = (p > 0x00010101u ? p - 0x00010101u : 0x00010101u - p);

                if (d > best) {
                    best = d;
                    wx = x;
                    wy = y;
                }
            }
        }
    }
    CHECK_INT(wy >= 0, 1);
    io_to_virtual(&ctx, wx, wy, &vx, &vy);
    CHECK_INT(vx, 8);
    CHECK_INT(vy, 6);
}

/* A whole-number scale must stay an exact doubling.
 *
 * The scaler averages, because at a fractional scale nearest neighbour has to stretch
 * some source pixels and drop others and a sixteen pixel font comes out with letters cut
 * in half. But averaging must not touch the case where there is nothing to average: at
 * twice the size every destination pixel covers exactly one source pixel, and if that
 * stops being true the whole interface goes soft at the size it is normally opened. */
static void test_io_integer_scale_is_exact(void) {
    IoCtx ctx;
    uint32_t pixels[4 * 4];
    uint32_t out[8 * 8];
    int32_t x, y;

    io_init(&ctx, pixels, 4, 4);
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            ctx.pixels[(size_t)y * 4 + x] = 0x00112233u + (uint32_t)(y * 4 + x) * 0x00010101u;
        }
    }
    memset(out, 0, sizeof out);
    io_scale_canvas(&ctx, out, 8, 8);

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            uint32_t want = ctx.pixels[(size_t)(y / 2) * 4 + (x / 2)];

            CHECK_INT(out[(size_t)y * 8 + x], (int32_t)want);
        }
    }
}

static void test_io_drag_origin_is_kept(void) {
    IoCtx ctx;
    IoEvent in;

    fb_reset(&ctx);
    memset(&in, 0, sizeof in);
    in.kind = IO_EV_POINTER_DOWN;
    in.button = IO_BUTTON_LEFT;
    in.x = 7;
    in.y = 2;
    io_post_event(&ctx, &in);
    io_poll(&ctx, 0);

    in.kind = IO_EV_POINTER_MOVE;
    in.x = 1;
    in.y = 1;
    io_post_event(&ctx, &in);
    io_poll(&ctx, 0);

    /* Moving away from the press point must not move the press point, which is
     * the whole of what tells a tap from a drag. */
    CHECK_INT(io_drag_x(&ctx), 7);
    CHECK_INT(io_drag_y(&ctx), 2);
    CHECK_INT(io_pointer_x(&ctx), 1);
}

static void test_io_event_ring_counts_overflow(void) {
    IoCtx ctx;
    IoEvent in;
    int i;

    fb_reset(&ctx);
    memset(&in, 0, sizeof in);
    in.kind = IO_EV_POINTER_MOVE;
    for (i = 0; i < IO_EVENT_MAX + 10; i++) {
        io_post_event(&ctx, &in);
    }
    /* the loss is counted rather than silent */
    CHECK_INT(io_dropped_events(&ctx), 10);
}

static void test_io_scale_preserves_aspect(void) {
    IoCtx ctx;
    uint32_t out[64 * 64];
    int32_t x, y;
    int bars;

    fb_reset(&ctx); /* 16 x 12 canvas, so 4:3 */
    /* paint the whole canvas so every filled pixel is distinguishable */
    for (y = 0; y < ctx.h; y++) {
        for (x = 0; x < ctx.w; x++) {
            ctx.pixels[(size_t)y * ctx.w + x] = 0x00FF00u;
        }
    }

    /* 1:1 is a copy */
    memset(out, 0, sizeof out);
    io_scale_canvas(&ctx, out, 16, 12);
    CHECK_INT(out[0], 0x00FF00u);
    CHECK_INT(out[16 * 12 - 1], 0x00FF00u);

    /* 2x in both directions fills the target exactly */
    for (y = 0; y < 64 * 64; y++) {
        out[y] = 0;
    }
    io_scale_canvas(&ctx, out, 32, 24);
    CHECK_INT(out[0], 0x00FF00u);
    CHECK_INT(out[32 * 24 - 1], 0x00FF00u);

    /* a wider target gets vertical bars and no stretching: the green area is
     * centred and the columns at the edges stay black */
    for (y = 0; y < 64 * 64; y++) {
        out[y] = 0;
    }
    io_scale_canvas(&ctx, out, 64, 24);
    CHECK_INT(out[64 * 12 + 0], 0);          /* left bar */
    CHECK_INT(out[64 * 12 + 32], 0x00FF00u); /* centred */
    CHECK_INT(out[64 * 12 + 63], 0);         /* right bar */
    bars = 0;
    for (x = 0; x < 64; x++) {
        if (out[64 * 12 + x] == 0) {
            bars++;
        }
    }
    CHECK_INT(bars, 32);
}

static void test_io_view_reports_window_size(void) {
    IoCtx ctx;
    fb_reset(&ctx);
    CHECK_INT(ctx.view_w, FB_W);
    CHECK_INT(ctx.view_h, FB_H);
    io_set_view(&ctx, 1920, 1080);
    CHECK_INT(ctx.view_w, 1920);
    CHECK_INT(ctx.view_h, 1080);
    /* the virtual canvas never moves */
    CHECK_INT(ctx.w, FB_W);
    CHECK_INT(ctx.h, FB_H);
    io_set_view(&ctx, 0, 100);
    CHECK_INT(ctx.view_w, 1920);
}

/* Reconstructs how much of the fill landed on each pixel, in 1/256 units.
 * fb_reset leaves the background at 1, so a pixel painted with cov of white
 * holds 1 + 254*cov/256. Summing it measures the covered area, which is what a
 * rasteriser actually gets right, rather than how many whole pixels it hit. */
static int total_coverage(void) {
    size_t i;
    int total = 0;

    for (i = 0; i < FB_PX; i++) {
        int v = (int)((g_fb[i] >> 16) & 0xFFu);
        if (v > 1) {
            total += ((v - 1) * 256 + 127) / 254;
        }
    }
    return total;
}

/* Pixels that are neither the background nor the fill: the proof that the
 * edge was antialiased instead of snapped to whole pixels. */
static int count_partial(IoColor fill) {
    size_t i;
    uint32_t want = rgb_of(fill);
    int n = 0;

    for (i = 0; i < FB_PX; i++) {
        if (g_fb[i] != 0x00010101u && g_fb[i] != want) {
            n++;
        }
    }
    return n;
}

static void test_io_init_validates(void) {
    IoCtx ctx;
    CHECK_INT(io_init(NULL, g_fb, FB_W, FB_H), 0);
    CHECK_INT(io_init(&ctx, NULL, FB_W, FB_H), 0);
    CHECK_INT(io_init(&ctx, g_fb, 0, FB_H), 0);
    CHECK_INT(io_init(&ctx, g_fb, FB_W, -1), 0);
}

static void test_io_rect_fills_exact_pixels(void) {
    IoCtx ctx;
    IoColor red = IO_RGB(255, 0, 0);

    fb_reset(&ctx);
    CHECK_INT(io_fill_rect(&ctx, (IoRect){ 2, 1, 4, 3 }, red), 1);

    CHECK(px_is(2, 1, red));
    CHECK(px_is(5, 3, red));
    CHECK_INT(count_color(red), 12);
    CHECK(!px_is(1, 1, red));
    CHECK(!px_is(6, 1, red));
    CHECK(!px_is(2, 0, red));
    CHECK(!px_is(2, 4, red));
}

static void test_io_rect_is_clipped_to_screen(void) {
    IoCtx ctx;
    IoColor blue = IO_RGB(0, 0, 255);

    fb_reset(&ctx);
    /* deliberately straddles the right and bottom edges */
    CHECK_INT(io_fill_rect(&ctx, (IoRect){ FB_W - 2, FB_H - 2, 10, 10 }, blue), 1);
    CHECK_INT(count_color(blue), 4);
}

static void test_io_clip_restricts_and_intersects(void) {
    IoCtx ctx;
    IoColor green = IO_RGB(0, 255, 0);

    fb_reset(&ctx);
    io_push_clip(&ctx, (IoRect){ 1, 1, 3, 2 });
    CHECK(io_clip(&ctx).x == 1);
    CHECK(io_clip(&ctx).w == 3);
    CHECK_INT(io_fill_rect(&ctx, (IoRect){ 0, 0, FB_W, FB_H }, green), 1);
    CHECK_INT(count_color(green), 6);
    io_pop_clip(&ctx);
    CHECK(io_clip(&ctx).w == FB_W);

    /* a nested clip cannot escape its parent */
    fb_reset(&ctx);
    io_push_clip(&ctx, (IoRect){ 4, 4, 6, 6 });
    io_push_clip(&ctx, (IoRect){ 0, 0, 16, 16 });
    CHECK(io_clip(&ctx).x == 4);
    CHECK(io_clip(&ctx).w == 6);
    CHECK_INT(io_fill_rect(&ctx, (IoRect){ 0, 0, FB_W, FB_H }, green), 1);
    CHECK_INT(count_color(green), 36);
    io_pop_clip(&ctx);
    io_pop_clip(&ctx);
}

static void test_io_triangle_fills_span(void) {
    IoCtx ctx;
    IoSeg e[3];
    IoColor white = IO_RGB(255, 255, 255);

    fb_reset(&ctx);
    /* right triangle: (0,0) (6,0) (0,6) */
    edge(e, 0, io_fx(0), io_fx(0), io_fx(6), io_fx(0));
    edge(e, 1, io_fx(6), io_fx(0), io_fx(0), io_fx(6));
    edge(e, 2, io_fx(0), io_fx(6), io_fx(0), io_fx(0));
    CHECK_INT(io_fill_poly(&ctx, e, 3, 0, 0, white), 1);

    /* The covered area equals the geometric area, 6*6/2 = 18 pixels. */
    CHECK(total_coverage() > 18 * 256 - 128);
    CHECK(total_coverage() < 18 * 256 + 128);
    CHECK(px_is(0, 0, white));
    CHECK(!px_is(5, 5, white));
    /* the hypotenuse is antialiased, so pixels along it are neither colour */
    CHECK(count_partial(white) > 4);
}

static void test_io_winding_makes_hole_without_contours(void) {
    IoCtx ctx;
    IoSeg e[8];
    IoColor yellow = IO_RGB(255, 255, 0);

    fb_reset(&ctx);
    /* Outer square clockwise, inner square the other way, as one flat edge list.
     * Nothing in the API knows these two squares are separate shapes. */
    edge(e, 0, io_fx(0), io_fx(0), io_fx(8), io_fx(0));
    edge(e, 1, io_fx(8), io_fx(0), io_fx(8), io_fx(8));
    edge(e, 2, io_fx(8), io_fx(8), io_fx(0), io_fx(8));
    edge(e, 3, io_fx(0), io_fx(8), io_fx(0), io_fx(0));
    edge(e, 4, io_fx(2), io_fx(2), io_fx(2), io_fx(6));
    edge(e, 5, io_fx(2), io_fx(6), io_fx(6), io_fx(6));
    edge(e, 6, io_fx(6), io_fx(6), io_fx(6), io_fx(2));
    edge(e, 7, io_fx(6), io_fx(2), io_fx(2), io_fx(2));

    CHECK_INT(io_fill_poly(&ctx, e, 8, 0, 0, yellow), 1);
    CHECK_INT(count_color(yellow), 64 - 16);
    /* row 3 is filled at x=0,1 and x=6,7 and hollow in between */
    CHECK(px_is(1, 1, yellow));
    CHECK(px_is(1, 3, yellow));
    CHECK(!px_is(3, 3, yellow));
    CHECK(!px_is(4, 3, yellow));
    CHECK(px_is(7, 3, yellow));
}

static void test_io_subpixel_shape_still_draws(void) {
    IoCtx ctx;
    IoSeg e[4];
    IoColor red = IO_RGB(255, 0, 0);

    /* A quarter-pixel tall band contains no pixel centre at all. Without the
     * thin-band rule it would vanish and flicker as it moves. */
    fb_reset(&ctx);
    edge(e, 0, 0, 0, io_fx(4), 0);
    edge(e, 1, io_fx(4), 0, io_fx(4), IO_FX_ONE / 4);
    edge(e, 2, io_fx(4), IO_FX_ONE / 4, 0, IO_FX_ONE / 4);
    edge(e, 3, 0, IO_FX_ONE / 4, 0, 0);
    CHECK_INT(io_fill_poly(&ctx, e, 4, 0, 0, red), 1);
    /* A band thinner than one pixel is deliberately drawn solid rather than
     * faint: it must not flicker out of existence as it moves, and a hairline
     * asked for as a hairline should not come out grey. */
    CHECK(total_coverage() >= 4 * 256 - 16);
    CHECK(px_is(0, 0, red));
    CHECK(px_is(3, 0, red));

    /* a quarter-pixel wide band claims one pixel column */
    fb_reset(&ctx);
    edge(e, 0, io_fx(2), io_fx(2), io_fx(2) + IO_FX_ONE / 4, io_fx(2));
    edge(e, 1, io_fx(2) + IO_FX_ONE / 4, io_fx(2), io_fx(2) + IO_FX_ONE / 4, io_fx(5));
    edge(e, 2, io_fx(2) + IO_FX_ONE / 4, io_fx(5), io_fx(2), io_fx(5));
    edge(e, 3, io_fx(2), io_fx(5), io_fx(2), io_fx(2));
    CHECK_INT(io_fill_poly(&ctx, e, 4, 0, 0, red), 1);
    /* The other way round a quarter-pixel wide column stays at a quarter
     * opacity, and that is correct: horizontal coverage is analytic, so it
     * cannot flicker the way a sub-sample-tall shape would. */
    CHECK(total_coverage() > 192 - 24);
    CHECK(total_coverage() < 192 + 24);
    CHECK_INT(count_color(red), 0);
}

static void test_io_rejects_oversized_polygon_without_drawing_part(void) {
    IoCtx ctx;
    IoSeg *e;
    IoColor red = IO_RGB(255, 0, 0);
    uint32_t n = (uint32_t)IO_POLY_MAX_CROSS + 8u;
    uint32_t i;

    CHECK_INT(arena_init(&g_arena, g_arena_mem, ARENA_BYTES), 0);
    e = arena_alloc_array(&g_arena, n, sizeof(IoSeg), ARENA_DEFAULT_ALIGN);
    CHECK(e != NULL);
    if (e == NULL) {
        return;
    }
    fb_reset(&ctx);
    /* every edge spans the full height, so each scanline crosses all of them */
    for (i = 0; i < n; i++) {
        edge(e, i, io_fx((int32_t)(i % FB_W)), 0, io_fx((int32_t)(i % FB_W)), io_fx(FB_H));
    }
    /* rejected outright: a partial shape would be a silent artefact */
    CHECK_INT(io_fill_poly(&ctx, e, n, 0, 0, red), 0);
    CHECK_INT(count_color(red), 0);
}

static void test_io_backend_test_is_inert(void) {
    IoCtx ctx;
    fb_reset(&ctx);
    CHECK_INT(io_backend_open(&ctx, &io_backend_test, "t"), 1);
    CHECK(ctx.backend == &io_backend_test);
    io_backend_close(&ctx);
    CHECK(ctx.backend == NULL);
}


/* ------------------------------------------------------------------ text -- */

#define FONT_PATH "assets/font/sans.font"

/* Every load starts from a fresh arena: the file plus the expanded segments are
 * around 150 KB, so loading twice into one arena would not fit. */
static TextFont load_font(Arena *a, int32_t px) {
    TextFont f;
    char *text;
    size_t len = 0;

    CHECK_INT(arena_init(a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(a, FONT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL) {
        memset(&f, 0, sizeof f);
        return f;
    }
    CHECK(text_font_load(a, &f, text, len, px) == TEXT_OK);
    return f;
}

static void test_text_font_loads(void) {
    Arena a;
    TextFont f;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    f = load_font(&a, 16);
    CHECK_INT(f.count, 197);
    /* The em is whatever the typeface says it is. It used to be pinned to DejaVu's
     * 2048, which made this a detector of one font rather than a check of the
     * loader, and the first other font broke six more assertions with it. */
    CHECK(f.upem > 0);
    CHECK_INT(f.px_size, 16);
    CHECK(f.ascent > 0);
    CHECK(f.descent > 0);
    CHECK(f.line_height >= f.ascent + f.descent);
    /* 16px over a 2048 upem: the ascenders come out lower than the nominal size */
    CHECK(f.ascent < 16);

    /* Every visible glyph must have an outline. DejaVu builds many Cyrillic
     * letters as composites, and a pen that ignores those ships blank letters
     * that look like a renderer bug instead of a baking one. */
    {
        size_t i;
        int drawn = 0;
        for (i = 0; i < f.count; i++) {
            if (f.glyphs[i].codepoint != 0x20u && f.glyphs[i].codepoint != 0xA0u) {
                CHECK(f.glyphs[i].seg_count > 0);
                drawn++;
            }
        }
        CHECK(drawn > 150);
    }
}

static void test_text_glyph_lookup(void) {
    Arena a;
    TextFont f;
    TextFont f2;
    const TextGlyph *g;
    size_t i;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    f = load_font(&a, 16);
    f2 = load_font(&a, 32);

    /* the file is sorted, so the binary search is only valid if that holds */
    for (i = 1; i < f.count; i++) {
        CHECK(f.glyphs[i - 1].codepoint < f.glyphs[i].codepoint);
    }
    CHECK_INT(f.glyphs[0].codepoint, 32);

    g = text_glyph(&f, 'A');
    CHECK(g != NULL);
    if (g != NULL) {
        CHECK_INT(g->codepoint, 65);
        CHECK(g->seg_count > 0);
        CHECK(g->segs != NULL);
    }
    /* Cyrillic A U+0410 */
    g = text_glyph(&f, 0x0410u);
    CHECK(g != NULL);
    if (g != NULL) {
        /* The advance is kept in em units, so it is the same at every size and the
         * scaling happens where the string is measured. */
        CHECK(g->advance > 0);
        CHECK_INT(text_glyph(&f2, 0x0410u)->advance, g->advance);
    }
    /* space has an advance but no outline */
    g = text_glyph(&f, ' ');
    CHECK(g != NULL);
    if (g != NULL) {
        CHECK_INT(g->seg_count, 0);
        CHECK(g->advance > 0);
    }
    CHECK(text_glyph(&f, 0x4E2Du) == NULL); /* CJK, not in the baked set */
}

static void test_text_advances_scale(void) {
    Arena a;
    TextFont small;
    TextFont large;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    small = load_font(&a, 16);
    large = load_font(&a, 32);

    /* Twice the pixels is twice the width, for whatever font is loaded. */
    CHECK(text_width(&small, "A", 1) > 0);
    CHECK_INT(text_width(&large, "A", 1), 2 * text_width(&small, "A", 1));
    CHECK(text_width(&small, "AAA", 3) > 2 * text_width(&small, "A", 1));
    CHECK(text_width(&small, "A", 1) < text_width(&small, "AA", 2));
}

static void test_text_missing_glyph_still_advances(void) {
    Arena a;
    TextFont f;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    f = load_font(&a, 16);
    /* a stray byte must not stall the line, so the fallback is one em */
    CHECK_INT(text_width(&f, "\xff\xfe", 2), 32);
    CHECK(text_width(&f, "A", 1) < text_width(&f, "A\xff", 2));
}

static void test_text_draws_above_baseline(void) {
    IoCtx ctx;
    Arena a;
    TextFont f;
    char *text;
    size_t len = 0;
    int32_t pen;
    int drawn;
    int below;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    text = read_file(&a, FONT_PATH, &len);
    CHECK(text != NULL);
    if (text == NULL || text_font_load(&a, &f, text, len, 24) != TEXT_OK) {
        return;
    }
    fb_reset(&ctx);
    pen = text_draw(&ctx, &f, 1, 10, "A", 1, IO_RGB(255, 255, 255));

    /* The pen ends up at the x it started at plus the width of the string, which is
     * the whole contract and says nothing about which font produced it. */
    CHECK_INT(pen, 1 + text_width(&f, "A", 1));
    /* The outline of a capital letter sits above the baseline, so with y down
     * the pixels must land in rows above the baseline and nowhere below it.
     * The framebuffer is only 12 rows tall, so the glyph clips at the top. */
    drawn = 0;
    below = 0;
    {
        int32_t row;
        int32_t col;
        for (row = 0; row < FB_H; row++) {
            for (col = 0; col < FB_W; col++) {
                /* antialiased text never lands on the exact fill colour, so any
                 * pixel that changed counts */
                if (g_fb[(size_t)row * FB_W + col] != 0x00010101u) {
                    if (row < 10) {
                        drawn++;
                    } else {
                        below++;
                    }
                }
            }
        }
    }
    CHECK(drawn > 0);
    CHECK_INT(below, 0);
}

static void test_text_runs_advance_the_pen(void) {
    IoCtx ctx;
    Arena a;
    TextFont f;
    int32_t x1;
    int32_t x2;

    f = load_font(&a, 16);
    fb_reset(&ctx);
    x1 = text_draw(&ctx, &f, 0, 10, "AB", 2, IO_RGB(255, 0, 0));
    x2 = text_draw(&ctx, &f, 0, 10, "A", 1, IO_RGB(255, 0, 0));
    CHECK_INT(x1, x2 + text_width(&f, "A", 1));
    CHECK_INT(x1, text_width(&f, "AB", 2));
}

static void test_text_glyph_coordinates_are_scaled(void) {
    Arena a;
    TextFont f;
    const TextGlyph *g;
    uint32_t i;
    int64_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;

    f = load_font(&a, 16);
    g = text_glyph(&f, 'A');
    CHECK(g != NULL);
    if (g == NULL) {
        return;
    }
    /* Coordinates are 16.16 pixels. A capital A at 16px is about 11px tall and
     * 9px wide, so anything a few 1/65536 pixels wide would mean the mapping is
     * off by the fixed-point factor. */
    for (i = 0; i < g->seg_count; i++) {
        const IoSeg *e = &g->segs[i];
        if (e->x0 < xmin) xmin = e->x0;
        if (e->x0 > xmax) xmax = e->x0;
        if (e->x1 < xmin) xmin = e->x1;
        if (e->x1 > xmax) xmax = e->x1;
        if (e->y0 < ymin) ymin = e->y0;
        if (e->y0 > ymax) ymax = e->y0;
        if (e->y1 < ymin) ymin = e->y1;
        if (e->y1 > ymax) ymax = e->y1;
    }
    CHECK(xmin >= 0);
    CHECK(xmax < (16 * IO_FX_ONE));
    CHECK(xmax > (2 * IO_FX_ONE));
    /* cap height 0.73em is about 11.7px at 16px */
    CHECK(ymin < -(2 * IO_FX_ONE));
    CHECK(ymin > -(20 * IO_FX_ONE));
    /* nothing below the baseline, so after the y flip the largest y is 0 */
    CHECK(ymax <= 0);
}

static void test_text_rejects_bad_font(void) {
    static const char version[] = "font 2\nupem 1000\nasc 800\ndesc -200\nline 1000\n"
                                  "glyph 65 600 0\n";
    static const char no_glyphs[] = "font 1\nupem 1000\nasc 800\ndesc -200\nline 1000\n";
    static const char no_upem[] = "font 1\nasc 800\nglyph 65 600 0\n";
    static const char truncated[] = "font 1\nupem 1000\nglyph 65 600 3\n  1 2 3 4\n";
    Arena a;
    TextFont f;

    CHECK_INT(arena_init(&a, g_arena_mem, sizeof g_arena_mem), 0);
    CHECK_INT(text_font_load(&a, &f, version, sizeof version - 1, 16), TEXT_E_VERSION);
    CHECK_INT(text_font_load(&a, &f, no_glyphs, sizeof no_glyphs - 1, 16), TEXT_E_SYNTAX);
    CHECK_INT(text_font_load(&a, &f, no_upem, sizeof no_upem - 1, 16), TEXT_E_SYNTAX);
    CHECK_INT(text_font_load(&a, &f, truncated, sizeof truncated - 1, 16), TEXT_E_SYNTAX);
    CHECK_INT(text_font_load(&a, &f, "upem 1000\n", 11, 0), TEXT_E_SYNTAX);
}

const test_case test_cases[] = {
    TEST_CASE(test_arena_basic),
    TEST_CASE(test_arena_alignment),
    TEST_CASE(test_arena_oom),
    TEST_CASE(test_arena_bad_alignment),
    TEST_CASE(test_arena_array_overflow),
    TEST_CASE(test_arena_reset),
    TEST_CASE(test_arena_copy),
    TEST_CASE(test_utf8_decode_ascii),
    TEST_CASE(test_utf8_decode_multibyte),
    TEST_CASE(test_utf8_decode_rejects_invalid),
    TEST_CASE(test_utf8_roundtrip),
    TEST_CASE(test_utf8_length_and_offset),
    TEST_CASE(test_utf8_valid),
    TEST_CASE(test_every_shipped_script_is_clean),
    TEST_CASE(test_script_loads_real_asset),
    TEST_CASE(test_script_room_names_and_titles),
    TEST_CASE(test_script_interns_shared_identifiers),
    TEST_CASE(test_script_rule_order_and_guards),
    TEST_CASE(test_script_effects_and_guards),
    TEST_CASE(test_script_validate_real_asset),
    TEST_CASE(test_script_start_directive),
    TEST_CASE(test_script_detects_undefined_flag),
    TEST_CASE(test_script_detects_shadowed_rule),
    TEST_CASE(test_script_detects_duplicate_room),
    TEST_CASE(test_grammar_rule),
    TEST_CASE(test_grammar_rule_with_no_words_is_a_description),
    TEST_CASE(test_grammar_action),
    TEST_CASE(test_script_rejects_bad_syntax),
    TEST_CASE(test_script_rejects_invalid_utf8),
    TEST_CASE(test_script_reports_out_of_memory),
    TEST_CASE(test_script_text_points_at_the_text),
    TEST_CASE(test_script_go_target_resolves),
    TEST_CASE(test_bare_action_runs_to_end_of_line),
    TEST_CASE(test_game_log_is_blocks_with_headings),
    TEST_CASE(test_game_enters_start_room),
    TEST_CASE(test_game_palette_follows_guards),
    TEST_CASE(test_game_effect_can_clear_a_flag),
    TEST_CASE(test_game_room_text_follows_flags),
    TEST_CASE(test_game_first_matching_rule_wins),
    TEST_CASE(test_game_effects_run_before_actions),
    TEST_CASE(test_game_go_enters_the_target_room),
    TEST_CASE(test_game_unknown_pair_does_nothing),
    TEST_CASE(test_game_terminal_action_stops_the_game),
    TEST_CASE(test_game_log_drops_the_oldest_line),
    TEST_CASE(test_io_event_queue_survives_poll),
    TEST_CASE(test_io_pointer_converts_to_virtual),
    TEST_CASE(test_io_pointer_agrees_with_scaling),
    TEST_CASE(test_io_integer_scale_is_exact),
    TEST_CASE(test_io_drag_origin_is_kept),
    TEST_CASE(test_io_event_ring_counts_overflow),
    TEST_CASE(test_io_scale_preserves_aspect),
    TEST_CASE(test_io_view_reports_window_size),
    TEST_CASE(test_io_init_validates),
    TEST_CASE(test_io_rect_fills_exact_pixels),
    TEST_CASE(test_io_rect_is_clipped_to_screen),
    TEST_CASE(test_io_clip_restricts_and_intersects),
    TEST_CASE(test_io_triangle_fills_span),
    TEST_CASE(test_io_winding_makes_hole_without_contours),
    TEST_CASE(test_io_subpixel_shape_still_draws),
    TEST_CASE(test_io_rejects_oversized_polygon_without_drawing_part),
    TEST_CASE(test_io_backend_test_is_inert),
    TEST_CASE(test_text_font_loads),
    TEST_CASE(test_text_glyph_lookup),
    TEST_CASE(test_text_advances_scale),
    TEST_CASE(test_text_missing_glyph_still_advances),
    TEST_CASE(test_text_draws_above_baseline),
    TEST_CASE(test_text_runs_advance_the_pen),
    TEST_CASE(test_text_glyph_coordinates_are_scaled),
    TEST_CASE(test_text_rejects_bad_font)
};

const size_t test_case_count = sizeof test_cases / sizeof test_cases[0];