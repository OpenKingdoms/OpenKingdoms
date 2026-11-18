#ifndef TAK_HUD_CURSOR_H
#define TAK_HUD_CURSOR_H

/* The battle cursor shows the order the next click gives (manual section
 * IV.2). The pick reads only these facts, so it runs without a world. */
typedef struct HUDCursorFacts {
    int armed;          /* the HUD_CMD_* waiting for its click, 0 for none */
    int own_selected;   /* units of the player's own are selected */
    int own_walker;     /* one of them can move */
    int unit;           /* a unit is under the pointer */
    int selectable;     /* it can be selected: not a frame */
    int enemy;          /* it belongs to an enemy */
    int mend;           /* the selection can heal it or help build it */
    int weapons;        /* selected units with a weapon in hand */
    int weapons_take;   /* of those, the ones whose weapon can take it */
    int load_refused;   /* the one selected transport cannot carry it */
    int raise;          /* a body the selection can raise lies there */
    int sweep;          /* something the selection can sweep lies there */
} HUDCursorFacts;

/* A HUD_CMD_* or HUD_CUR_* id for HUD_DrawCursorById. */
int HUD_PickCursor(const HUDCursorFacts *f);

/* The same over a minimap or Tab map point. With nothing of yours
 * selected a press there only looks (D-037), so it is the pointer. */
int HUD_PickMapCursor(const HUDCursorFacts *f);

#endif /* TAK_HUD_CURSOR_H */
