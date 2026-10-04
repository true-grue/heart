#ifndef CORE_WALK_H
#define CORE_WALK_H

#include "ui.h"

/* Both run on the test backend: no window, no events, only the framebuffer. Neither one
 * knows the rules of any particular script: the walkthrough asks the interface the same
 * questions a player asks, so it cannot claim a route the game would not offer. */

/* Plays the shortest route through the state graph, drawing every step on the way. */
int walk_run(Ui *ui, Game *g, const Script *s, const char *dir);

/* Measures the layout from the script alone, without opening anything. */
int walk_layout_audit(Ui *ui, Game *g, const Script *s);

#endif
