#ifndef TAK_KEYBINDS_VIEW_H
#define TAK_KEYBINDS_VIEW_H

#include "tak_keybinds.h"

/*
 * What the key binds screen shows, apart from how it draws it: the
 * groups as the screen names them, the list's scroll, a key's name cut
 * to its cell, and the line under the list. No art and no font, so a
 * test reaches it without the game's data.
 */

/* The table's Camera group is 19 actions, so the screen shows its eight
 * bookmark keys as a group of their own. */
#define KEYVIEW_GROUPS   7
#define KEYVIEW_MAX_ROWS 16

const char *KeyView_GroupLabel(int group);
/* The actions of a group in the order shown. Returns how many. */
int KeyView_GroupActions(int group, KeyAction *out, int cap);
int KeyView_GroupOf(KeyAction a);

/* A list of `rows` with `visible` showing: the first row kept in range,
 * where the thumb sits along `travel` pixels, and the first row for a
 * thumb dragged to `offset`. */
int KeyView_ClampScroll(int scroll, int rows, int visible);
int KeyView_ThumbOffset(int scroll, int rows, int visible, int travel);
int KeyView_ScrollAt(int offset, int rows, int visible, int travel);

/* A chord's name no wider than `max_w` as `measure` counts it. The
 * modifiers go to one letter first, "C+S+F12", and then the end goes. */
typedef int (*KeyViewMeasure)(void *ctx, const char *text);
char *KeyView_ChordFit(KeyChord chord, int max_w, KeyViewMeasure measure,
                       void *ctx, char *buf, int cap);

/* "3 keys override Grid", or what the scheme is with none. */
char *KeyView_Status(char *buf, int cap);

/* What a browser does with a key before the game hears of it: 0 for
 * nothing, 1 when it may act on the key as well, 2 when it keeps the
 * key for itself. A guide for the player, since browsers differ. The
 * note is "" for 0. */
int KeyView_BrowserRisk(KeyChord chord);
const char *KeyView_BrowserNote(KeyChord chord);

/* 1 in the browser build, where the screen shows those notes. */
int  KeyView_InBrowser(void);

/* Why Keybinds_Capture refused the key that went down this frame. */
const char *KeyView_RefusalNote(const uint8_t *keys, const uint8_t *prev);

#endif /* TAK_KEYBINDS_VIEW_H */
