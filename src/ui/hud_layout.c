/*
 * hud_layout.c -- the battle screen's regions at a screen size, see
 * tak_hud_layout.h.
 *
 * Above 640x480 the original keeps every piece of its dialog at its own
 * size. The sidebar panel moves to the bottom right corner, the bottom
 * strip moves down and grows to reach the sidebar, and the play area is
 * what is left (legacy:153532-153551, legacy:243509-243519). The strip
 * repeats its art along the extra width and its unit panels centre in
 * it, never left of where they are authored, as measured off the
 * original at 1280x600.
 */

#include "tak_hud_layout.h"

#include <stddef.h>

static HUD_Rect rect(int x, int y, int w, int h) {
    HUD_Rect r = { x, y, w, h };
    return r;
}

static int inside(HUD_Rect outer, HUD_Rect r) {
    return r.x >= outer.x && r.y >= outer.y &&
           r.x + r.w <= outer.x + outer.w &&
           r.y + r.h <= outer.y + outer.h;
}

void HUD_LayoutSourceDefault(HUD_LayoutSource *src) {
    if (!src) return;
    src->unit_menu      = rect(512, 128, 128, 352);
    src->bottom_bar     = rect(0, 431, 512, 49);
    src->info_group     = rect(59, 431, 453, 49);
    src->has_info_group = 1;
}

void HUD_LayoutCanvasSize(HUD_ScaleMode mode, int win_w, int win_h,
                          int *out_w, int *out_h) {
    int w = HUD_AUTHORED_W, h = HUD_AUTHORED_H;
    if (mode == HUD_SCALE_ORIGINAL &&
        win_w >= HUD_AUTHORED_W && win_h >= HUD_AUTHORED_H) {
        w = win_w;
        h = win_h;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void HUD_LayoutCompute(const HUD_LayoutSource *src, int canvas_w, int canvas_h,
                       HUD_Layout *out) {
    if (!out) return;
    HUD_LayoutSource def;
    if (!src) { HUD_LayoutSourceDefault(&def); src = &def; }
    if (canvas_w < HUD_AUTHORED_W) canvas_w = HUD_AUTHORED_W;
    if (canvas_h < HUD_AUTHORED_H) canvas_h = HUD_AUTHORED_H;
    int dx = canvas_w - HUD_AUTHORED_W;
    int dy = canvas_h - HUD_AUTHORED_H;

    out->canvas_w  = canvas_w;
    out->canvas_h  = canvas_h;
    out->side_dx   = dx;
    out->side_dy   = dy;
    out->bottom_dy = dy;

    HUD_Rect um = src->unit_menu, bb = src->bottom_bar;
    out->sidebar = rect(um.x + dx, um.y + dy, um.w, um.h);
    /* The minimap keeps the top of the sidebar column. */
    out->minimap = rect(um.x + dx, 0, HUD_AUTHORED_W - um.x, um.y);
    out->bottom  = rect(bb.x, bb.y + dy, bb.w + dx, bb.h);
    /* The view's last row lies under the strip's first: 48 rows short
     * of the screen, not 49 (legacy:243513-243518). */
    out->play    = rect(0, 0, out->sidebar.x, out->bottom.y + 1);

    out->info_dx = 0;
    if (src->has_info_group && dx > 0) {
        HUD_Rect g = src->info_group;
        int centred = out->bottom.x + (out->bottom.w - g.w) / 2;
        if (centred > g.x) out->info_dx = centred - g.x;
    }
}

HUD_Rect HUD_LayoutPlace(const HUD_LayoutSource *src, const HUD_Layout *lay,
                         HUD_Rect r) {
    if (!src || !lay) return r;
    /* The sidebar column, minimap slot included, keeps the right edge. */
    HUD_Rect column = rect(src->unit_menu.x, 0, HUD_AUTHORED_W - src->unit_menu.x,
                           HUD_AUTHORED_H);
    if (inside(src->unit_menu, r)) {
        r.x += lay->side_dx;
        r.y += lay->side_dy;
        return r;
    }
    if (inside(column, r)) {
        r.x += lay->side_dx;
        return r;
    }
    if (r.x == src->bottom_bar.x && r.y == src->bottom_bar.y &&
        r.w == src->bottom_bar.w && r.h == src->bottom_bar.h)
        return lay->bottom;
    if (inside(src->bottom_bar, r)) {
        if (src->has_info_group && inside(src->info_group, r))
            r.x += lay->info_dx;
        r.y += lay->bottom_dy;
        return r;
    }
    return r;
}

static int same_word(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        if (ca != *b) return 0;
    }
    return *a == *b;
}

HUD_ScaleMode HUD_ScaleModeFromName(const char *name) {
    if (name && same_word(name, "fit")) return HUD_SCALE_FIT;
    return HUD_SCALE_ORIGINAL;
}

HUD_ScaleMode HUD_ScaleModeForSettings(const char *saved, int had_options_file) {
    if (saved && saved[0]) return HUD_ScaleModeFromName(saved);
    return had_options_file ? HUD_SCALE_FIT : HUD_SCALE_ORIGINAL;
}

const char *HUD_ScaleModeName(HUD_ScaleMode mode) {
    return mode == HUD_SCALE_FIT ? "fit" : "original";
}
