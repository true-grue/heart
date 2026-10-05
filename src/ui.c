#include "ui.h"

#include "utf8.h"

#include <stddef.h>
#include <string.h>

/* Shortens the longest names until the row fits, cutting at a space where it can, and
 * leaves a dot as the mark.
 *
 * It lives outside the drawing because a rule about what may be hidden from the player
 * is exactly the kind of rule that needs a test, and inside draw() there is no way to
 * reach it. Returns nothing: the labels are shortened in place. */
void items_fit(const TextFont *f, char label[][LABEL_MAX], uint32_t *len, size_t n) {
        /* Shorten the longest name until the row fits. Longest first is what makes it
         * fair: every name loses about the same rather than one losing all of its own.
         * Each round cuts one character off the longest and marks it with a dot. */
    size_t rounds;
    size_t k;

    for (rounds = 0;; rounds++) {
            int32_t total = 0;
            int32_t worst = -1;
            size_t worst_chars = 0;

            for (k = 0; k < n; k++) {
                size_t chars = utf8_length((const uint8_t *)label[k], len[k]);

                total += (int32_t)text_width(f, label[k], len[k]) + 2 * CHIP_PAD_X +
                         CHIP_GAP;
                if (chars > worst_chars && chars > ITEM_MIN_CHARS) {
                    worst_chars = chars;
                    worst = (int32_t)k;
                }
            }
            if (n == 0 || total - CHIP_GAP <= GAME_W - 2 * MARGIN_X || worst < 0) {
                break;
            }
            {
                /* Two characters go, and the dot stands in for one of them: a name that
                 * loses a character and gains a dot has not got shorter, and a loop
                 * that waits for the row to fit waits forever.
                 *
                 * The cut prefers the space, because a name is words and not a string:
                 * «спички отогреты» shortened to «спички о.» says less than «спички.»,
                 * and the second is shorter besides. The space is dropped rather than
                 * kept, so a dot never ends up stranded after one. */
                size_t keep = worst_chars - 2;
                size_t cut = utf8_offset((const uint8_t *)label[worst], len[worst], keep);
                size_t back = cut;
                size_t chars_left = keep;

                while (back > 0 && label[worst][back - 1] != ' ' &&
                       chars_left > ITEM_MIN_CHARS) {
                    back--;
                    chars_left--;
                }
                if (back > 0 && label[worst][back - 1] == ' ' &&
                    chars_left > ITEM_MIN_CHARS) {
                    cut = back - 1;
                }
                label[worst][cut++] = '.';
                label[worst][cut] = '\0';
                len[worst] = (uint32_t)cut;
            }
            if (++rounds > ITEM_MAX * 64) {
                /* Belt and braces. Every round shortens the longest name by one
                 * character, so this cannot be reached; a game hanging on a strip of
                 * carried things is far worse than a strip that does not quite fit. */
                break;
            }
        }

}
