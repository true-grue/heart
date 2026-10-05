#ifndef CORE_WALK_H
#define CORE_WALK_H

#include "ui.h"

/* Both run on the test backend: no window, no events, only the framebuffer. Neither one
 * knows the rules of any particular script: the walkthrough asks the interface the same
 * questions a player asks, so it cannot claim a route the game would not offer.
 *
 * It lives beside main and not inside it because it draws nothing itself: it plays a
 * route and writes frames. A file that renders the game and a file that searches it are
 * two things, and the search was sitting in the middle of the first. */

/* Plays the shortest route through the state graph, drawing every step on the way. */
int walk_run(Ui *ui, Game *g, const Script *s, const char *dir);

/* Measures the layout from the script alone, without opening anything. */
int walk_layout_audit(Ui *ui, Game *g, const Script *s,
                       const char *const *titles, size_t titles_n);

#endif
