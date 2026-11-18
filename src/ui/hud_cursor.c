#include "tak_hud_cursor.h"
#include "tak_hud.h"

/* An armed order shows its own cursor, with three exceptions. */
static int hud_armed_cursor(const HUDCursorFacts *f) {
    /* The sweep raises a body the selection can raise. */
    if (f->armed == HUD_CMD_CLEAR && f->raise) return HUD_CUR_REVIVE;
    /* An enemy no weapon in hand can take (legacy:186260). */
    if (f->armed == HUD_CMD_ATTACK && f->unit && f->enemy &&
        f->weapons > 0 && f->weapons_take == 0)
        return HUD_CUR_TOOFAR;
    /* A unit the transport cannot take (legacy:186387-186402). */
    if (f->armed == HUD_CMD_LOAD && f->unit && f->load_refused)
        return HUD_CUR_NORMAL;
    return f->armed;
}

/* With nothing armed: heal over a unit the selection can mend
 * (legacy:186541-186545), attack or too far over an enemy
 * (legacy:186135-186330), the select hand over any other unit, then on
 * the ground revive, sweep (legacy:186587-186599) and move for a
 * selection that can move (legacy:186603). */
int HUD_PickCursor(const HUDCursorFacts *f) {
    if (!f) return HUD_CUR_NORMAL;
    if (f->armed != HUD_CMD_NONE) return hud_armed_cursor(f);
    if (f->unit) {
        if (f->mend) return HUD_CMD_HEAL;
        /* A frame nobody can help is its ground (legacy:186553). */
        if (f->selectable || f->enemy) {
            if (!f->enemy || !f->own_selected) return HUD_CUR_SELECT;
            if (f->weapons_take > 0) return HUD_CMD_ATTACK;
            if (f->weapons > 0) return HUD_CUR_TOOFAR;
            /* Nobody armed: the click walks to the ground there. */
        }
    }
    if (f->raise) return HUD_CUR_REVIVE;
    if (f->sweep) return HUD_CMD_CLEAR;
    return f->own_walker ? HUD_CMD_MOVE : HUD_CUR_NORMAL;
}

int HUD_PickMapCursor(const HUDCursorFacts *f) {
    if (!f || !f->own_selected) return HUD_CUR_NORMAL;
    return HUD_PickCursor(f);
}
