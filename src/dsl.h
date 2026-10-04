#ifndef DSL_DSL_H
#define DSL_DSL_H

#include "arena.h"

#include <stddef.h>
#include <stdint.h>

/* Index into Script.symtab. Identifiers are interned at load time and compared
 * byte-wise, so the script can hold non-ASCII names without decoding. */
typedef uint32_t Sym;

typedef struct SymEntry {
    uint32_t off;
    uint32_t len;
} SymEntry;

typedef enum ActKind {
    ACT_SAY = 0,
    ACT_GO,
    ACT_END,
    ACT_WIN
} ActKind;

typedef struct Act {
    ActKind kind;
    Sym target;          /* room for ACT_GO */
    const char *text;    /* into the source buffer */
    uint32_t text_len;
} Act;

/* One guard condition or one effect. Guards and effects share one Cond array in
 * the arena, each run contiguous, so a Rule needs two pointers into it. */
typedef struct Cond {
    Sym name;
    uint8_t present;     /* 1 = must be present or gets set, 0 = must be absent */
} Cond;

/* One line of a room description. Every fragment whose guard holds is printed,
 * in source order. */
typedef struct Frag {
    Cond *guard;         /* NULL when the fragment is unconditional */
    uint32_t guard_len;
    const char *text;
    uint32_t text_len;
    int line;
} Frag;

/* A command is however many words the script needs, not a fixed pair. Four is the
 * whole width of the command palette, so the format and the screen cannot drift
 * apart without one of them being wrong. */
#define RULE_MAX_WORDS 4

/* A player command. Within a room the first rule whose guard holds wins.
 *
 * One action, not a list. The format has no separator between actions, so a rule
 * that did two things would have to guess where the first stopped; keeping a
 * length that is always one is the shape the format actually has. */
typedef struct Rule {
    Sym words[RULE_MAX_WORDS];
    uint32_t word_len;
    Cond *guard;
    uint32_t guard_len;
    Cond *effects;
    uint32_t effect_len;
    Act act;
    int line;
} Rule;

typedef struct Room {
    Sym id;
    const char *title;
    uint32_t title_len;
    Frag *frags;
    uint32_t frag_len;
    Rule *rules;
    uint32_t rule_len;
    int line;
} ScriptRoom;

typedef struct Script {
    Arena *arena;
    ScriptRoom *rooms;
    size_t room_count;
    Rule *rules;          /* flat, in source order */
    size_t rule_count;
    SymEntry *symtab;
    size_t sym_count;
    char *pool;           /* identifier bytes */
    size_t pool_len;
    Sym start;
    int have_start_directive;
} Script;

typedef enum ScriptStatus {
    SCR_OK = 0,
    SCR_E_UTF8,
    SCR_E_SYNTAX,
    SCR_E_NO_ROOMS,
    SCR_E_MEMORY
} ScriptStatus;

typedef enum Severity {
    SEV_ERROR = 0,
    SEV_WARNING
} Severity;

typedef enum VerifyCode {
    V_OK = 0,
    V_E_UNDEFINED_FLAG,
    V_E_DUPLICATE_ROOM,
    V_W_SHADOWED_RULE,
    V_W_UNRESOLVED_TARGET,
    V_W_UNREACHABLE_ROOM,
    V_W_IMPLICIT_START
} VerifyCode;

typedef struct Diagnostic {
    Severity severity;
    VerifyCode code;
    int line;
    Sym subject;
} Diagnostic;

/* Parses text into structures allocated from the arena. text stays owned by the
 * caller: Frag.text and Act.text point into it, so it must outlive the Script.
 * On a parse error the line number is stored in *err_line when not NULL. */
ScriptStatus script_load(Arena *a, Script *out, const char *text, size_t len,
                         int *err_line);

/* Checks content integrity. Writes up to cap diagnostics, sets *out_n to the
 * number produced (which may exceed cap), and returns the SEV_ERROR count.
 * Uses the script arena for scratch space. */
size_t script_validate(const Script *s, Diagnostic *out, size_t cap, size_t *out_n);

const char *script_status_str(ScriptStatus st);
const char *script_verify_str(VerifyCode code);

/* The symbol for a name, or SYM_NONE. Saving and loading need to turn a name that
 * was written to a file back into the symbol it was interned as. */
#define SYM_NONE 0xFFFFFFFFu
Sym script_sym_lookup(const Script *s, const char *name, size_t len);

const char *script_sym(const Script *s, Sym sym, size_t *len);
int script_sym_eq(const Script *s, Sym sym, const char *name);

const ScriptRoom *script_room_by_id(const Script *s, Sym id);

#endif