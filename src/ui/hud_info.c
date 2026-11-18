/*
 * hud_info.c -- what the bottom strip's selected-unit panel describes.
 */

#include "tak_hud_info.h"

#include <stddef.h>

HUDInfoPick HUD_PickInfo(int hover_unit, int hover_feature,
                         const int *sel, int sel_count) {
    HUDInfoPick p = { HUD_INFO_NONE, -1 };
    if (hover_unit >= 0) {
        p.kind = HUD_INFO_UNIT;
        p.index = hover_unit;
    } else if (hover_feature >= 0) {
        p.kind = HUD_INFO_FEATURE;
        p.index = hover_feature;
    } else if (sel && sel_count == 1 && sel[0] >= 0) {
        p.kind = HUD_INFO_UNIT;
        p.index = sel[0];
    }
    return p;
}
