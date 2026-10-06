#ifndef CORE_CUR_H
#define CORE_CUR_H

#include <stddef.h>
#include <string.h>

/* A cursor over the lines of a text buffer, and the one way to step it.
 *
 * It was written twice, once in the script loader and once in the font loader, byte for byte
 * the same twenty three lines under different names, and the type was duplicated too. Both
 * read line based text that comes from outside the engine, so they have to agree on what a
 * line is, and agreeing by copy is how they came to disagree once: one was fixed to handle
 * CRLF and the other was not. */

typedef struct Cur {
    const char *p;
    const char *end;
    int line;
} Cur;

/* Starts a cursor over a buffer. Written here because setting up a walk over text was also
 * repeated: a field added to Cur would otherwise have to be added at every call site, and one
 * of them would be forgotten. */
static inline void cur_init(Cur *c, const char *text, size_t len) {
    c->p = text;
    c->end = text + len;
    c->line = 0;
}

/* Advances to the next line that carries something, skipping blanks and comments. Returns 0
 * at the end of the buffer. */
static inline int cur_next_line(Cur *c, const char **ls, size_t *llen) {
    while (c->p < c->end) {
        const char *start = c->p;
        const char *nl = (const char *)memchr(start, '\n', (size_t)(c->end - start));
        const char *stop = (nl != NULL) ? nl : c->end;

        c->line++;
        if (stop > start && stop[-1] == '\r') {
            stop--;
        }
        c->p = (nl != NULL) ? nl + 1 : c->end;

        if (stop == start || *start == '#') {
            continue;
        }
        *ls = start;
        *llen = (size_t)(stop - start);
        return 1;
    }
    return 0;
}

#endif
