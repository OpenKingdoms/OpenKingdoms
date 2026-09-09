/*
 * debug_panel.c — Phase C debug overlay.
 *
 * One screen-module implementation of an immediate-mode tweak panel.
 * Self-contained: holds its own font, position, drag state, FPS
 * smoothing, and the row layout for current debug entries.
 *
 * Compiled only when TAK_DEBUG=1 (set by CMakeLists.txt under the
 * Debug config). Release builds get the no-op inline stubs from
 * tak_debug_panel.h, so call sites don't need #ifdefs.
 *
 * Design points worth knowing:
 *
 *   • Drawing target is UI_Offscreen() (the 640×480 canvas that
 *     UI_Present composites on top of the 3D scene). That gives us
 *     proper layering for free — terrain and units render first to
 *     plat->renderer; this panel paints to the canvas; UI_Present
 *     uploads the canvas as the final pass.
 *
 *   • Drag math is in canvas coords, not window coords. Mouse comes
 *     in as window coords from SDL_GetMouseState; we map through
 *     TAK_Platform_MapMouseToCanvas to get into canvas space, where
 *     the panel rect lives.
 *
 *   • Text uses the existing GAF font system (Font_DrawString). We
 *     load a single small font at init.
 *
 *   • Buttons are clickable rectangles drawn with SDL_FillRect onto
 *     the canvas surface. Hit-testing happens in canvas coords.
 *     Edge-detected so a held click doesn't repeat (cf. ingame.c's
 *     prev_keys pattern from M4).
 *
 *   • Adding new rows: extend draw_panel + handle_input, declaring
 *     a static SDL_Rect for each new button so hit-test and draw
 *     stay in sync. Long-term we could thread this through a row-
 *     descriptor array, but the explicit code is fine while the
 *     panel is small.
 */

#include "tak_debug_panel.h"

#ifdef TAK_DEBUG

#include "tak_ui.h"
#include "tak_font.h"
#include "tak_unit.h"
#include "tak_tex_atlas.h"
#include "tak_world.h"
#include "tak_memory.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* Canvas-space layout. The panel starts here on first frame and gets
 * dragged anywhere within (0..640, 0..480). */
#define DP_INITIAL_X     8
#define DP_INITIAL_Y     8
#define DP_WIDTH         220
#define DP_TITLE_H       12
#define DP_ROW_H         12
#define DP_PAD           4
#define DP_BUTTON_W      18

typedef struct {
    int x, y, w, h;
    int dragging;
    int drag_off_x, drag_off_y;

    /* Edge-detected button state (left mouse button). */
    int prev_left_down;

    /* Smoothed FPS readout. */
    float fps_accum_dt;
    int   fps_accum_frames;
    float fps_display;

    Font *font;
    int   initialized;
} DebugPanelState;

static DebugPanelState dp;

/* Compute the panel's height each frame from the row count, so the
 * background rectangle stretches to fit content. Update this when
 * adding rows. */
static int dp_row_count(void) {
    /* TA_SCALE, TAN_TILT, Heading, FPS, BFCull-On, BFCull-Invert,
     * LightLevel, ReloadTextures, Spawn ARA/ZON/VER/TAR, Clear. 13 total. */
    return 13;
}

static int dp_panel_h(void) {
    return DP_TITLE_H + DP_PAD + dp_row_count() * DP_ROW_H + DP_PAD;
}

/* ── Init / Shutdown ───────────────────────────────────────────── */

int DebugPanel_Init(TAK_Platform *plat) {
    (void)plat;
    memset(&dp, 0, sizeof(dp));
    dp.x = DP_INITIAL_X;
    dp.y = DP_INITIAL_Y;
    dp.w = DP_WIDTH;
    dp.h = dp_panel_h();

    /* Same font Battle Setup uses; small, fits a 12px row. */
    dp.font = Font_Load("data/fonts/b_times new roman (100)", UI_RGBAFormat());
    if (!dp.font) {
        fprintf(stderr, "DebugPanel_Init: font load failed; panel disabled\n");
        return -1;
    }
    dp.initialized = 1;
    return 0;
}

void DebugPanel_Shutdown(void) {
    if (dp.font) { Font_Free(dp.font); dp.font = NULL; }
    memset(&dp, 0, sizeof(dp));
}

/* ── Tick: FPS smoothing ───────────────────────────────────────── */

void DebugPanel_TickFPS(float frame_dt) {
    if (!dp.initialized) return;
    dp.fps_accum_dt += frame_dt;
    dp.fps_accum_frames++;
    if (dp.fps_accum_dt >= 0.5f) {
        dp.fps_display = (float)dp.fps_accum_frames / dp.fps_accum_dt;
        dp.fps_accum_dt = 0.0f;
        dp.fps_accum_frames = 0;
    }
}

/* ── Hit-testing helpers ──────────────────────────────────────── */

static int point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* Each row's local rects, relative to panel-top-left. Centralised so
 * hit-test and draw can't disagree. */
typedef struct {
    SDL_Rect minus;       /* "-" button */
    SDL_Rect plus;        /* "+" button */
    SDL_Rect action;      /* full-row action button (for Spawn etc.) */
    int      has_pm;      /* row has +/- buttons */
    int      has_action;  /* row is a single-action button */
} RowLayout;

static RowLayout dp_row_layout(int row_index) {
    RowLayout r;
    memset(&r, 0, sizeof(r));
    int row_y = DP_TITLE_H + DP_PAD + row_index * DP_ROW_H;
    /* +/- buttons sit on the right edge of the panel for value rows. */
    r.minus.x  = DP_WIDTH - DP_PAD - 2 * DP_BUTTON_W - 2;
    r.minus.y  = row_y;
    r.minus.w  = DP_BUTTON_W;
    r.minus.h  = DP_ROW_H - 1;
    r.plus.x   = DP_WIDTH - DP_PAD - DP_BUTTON_W;
    r.plus.y   = row_y;
    r.plus.w   = DP_BUTTON_W;
    r.plus.h   = DP_ROW_H - 1;
    /* Single-action button spans most of the row. */
    r.action.x = DP_PAD;
    r.action.y = row_y;
    r.action.w = DP_WIDTH - 2 * DP_PAD;
    r.action.h = DP_ROW_H - 1;
    return r;
}

/* Spawn a monarch at the camera-center world coord. Used by the
 * panel's spawn buttons. */
static void spawn_at_camera_center(const char *side) {
    GameWorld *world = World_Get();
    if (!world) return;
    int32_t wx = world->cam_x + world->viewport_w / 2;
    int32_t wy = world->cam_y + world->viewport_h / 2;
    Units_DebugSpawnMonarch(side, wx, wy);
}

/* ── Input handler ──────────────────────────────────────────────── */

int DebugPanel_HandleInput(TAK_Platform *plat,
                           int window_mouse_x, int window_mouse_y,
                           int left_button_down)
{
    if (!dp.initialized) return 0;

    int cx = -1, cy = -1;
    int in_canvas = TAK_Platform_MapMouseToCanvas(plat,
        window_mouse_x, window_mouse_y, &cx, &cy);

    int left_pressed  = left_button_down && !dp.prev_left_down;
    int left_released = !left_button_down && dp.prev_left_down;
    int consumed = 0;

    /* Title-bar drag start: click in the panel's title strip. */
    if (in_canvas && left_pressed && !dp.dragging &&
        point_in_rect(cx, cy, dp.x, dp.y, dp.w, DP_TITLE_H))
    {
        dp.dragging   = 1;
        dp.drag_off_x = cx - dp.x;
        dp.drag_off_y = cy - dp.y;
        consumed = 1;
    }

    if (dp.dragging) {
        if (in_canvas) {
            dp.x = cx - dp.drag_off_x;
            dp.y = cy - dp.drag_off_y;
            /* Clamp inside canvas. */
            if (dp.x < 0) dp.x = 0;
            if (dp.y < 0) dp.y = 0;
            if (dp.x + dp.w > 640) dp.x = 640 - dp.w;
            if (dp.y + dp_panel_h() > 480) dp.y = 480 - dp_panel_h();
        }
        if (left_released) dp.dragging = 0;
        consumed = 1;
    }

    /* Button clicks (only on press, only when not dragging). */
    if (in_canvas && left_pressed && !dp.dragging) {
        int local_x = cx - dp.x;
        int local_y = cy - dp.y;

        /* Row 0: TA_SCALE */
        RowLayout r = dp_row_layout(0);
        if (point_in_rect(local_x, local_y, r.minus.x, r.minus.y, r.minus.w, r.minus.h)) {
            Units_SetTAScale(Units_GetTAScale() * 0.85f); consumed = 1;
        } else if (point_in_rect(local_x, local_y, r.plus.x, r.plus.y, r.plus.w, r.plus.h)) {
            Units_SetTAScale(Units_GetTAScale() / 0.85f); consumed = 1;
        }

        /* Row 1: TAN_TILT */
        r = dp_row_layout(1);
        if (point_in_rect(local_x, local_y, r.minus.x, r.minus.y, r.minus.w, r.minus.h)) {
            Units_SetTanTilt(Units_GetTanTilt() - 0.05f); consumed = 1;
        } else if (point_in_rect(local_x, local_y, r.plus.x, r.plus.y, r.plus.w, r.plus.h)) {
            Units_SetTanTilt(Units_GetTanTilt() + 0.05f); consumed = 1;
        }

        /* Row 2: Heading — rotate all alive units. ±π/8 (22.5°) per
         * click is small enough to refine, big enough to spin around
         * in ~16 clicks. */
        r = dp_row_layout(2);
        const float ROT_STEP = 0.39269908f;     /* π/8 */
        if (point_in_rect(local_x, local_y, r.minus.x, r.minus.y, r.minus.w, r.minus.h)) {
            Units_DebugRotateAll(-ROT_STEP); consumed = 1;
        } else if (point_in_rect(local_x, local_y, r.plus.x, r.plus.y, r.plus.w, r.plus.h)) {
            Units_DebugRotateAll(+ROT_STEP); consumed = 1;
        }

        /* Row 3: FPS readout — no buttons. */

        /* Row 4: Backface cull on/off toggle. */
        r = dp_row_layout(4);
        if (point_in_rect(local_x, local_y, r.action.x, r.action.y, r.action.w, r.action.h)) {
            Units_SetBackfaceCullOn(!Units_GetBackfaceCullOn()); consumed = 1;
        }
        /* Row 5: Backface cull invert. */
        r = dp_row_layout(5);
        if (point_in_rect(local_x, local_y, r.action.x, r.action.y, r.action.w, r.action.h)) {
            Units_SetBackfaceCullInvert(!Units_GetBackfaceCullInvert()); consumed = 1;
        }
        /* Row 6: Light level (0..31). +/- button row. Note the change
         * doesn't take effect until the next skirmish load — the
         * shade table is applied at atlas-build time. Print the new
         * value so the user can re-launch with stderr awareness. */
        r = dp_row_layout(6);
        if (point_in_rect(local_x, local_y, r.minus.x, r.minus.y, r.minus.w, r.minus.h)) {
            int lvl = TexAtlas_GetLightLevel();
            TexAtlas_SetLightLevel(lvl - 1);
            fprintf(stderr, "Light level set to %d (re-load skirmish to apply)\n",
                    TexAtlas_GetLightLevel());
            consumed = 1;
        } else if (point_in_rect(local_x, local_y, r.plus.x, r.plus.y, r.plus.w, r.plus.h)) {
            int lvl = TexAtlas_GetLightLevel();
            TexAtlas_SetLightLevel(lvl + 1);
            fprintf(stderr, "Light level set to %d (re-load skirmish to apply)\n",
                    TexAtlas_GetLightLevel());
            consumed = 1;
        }

        /* Row 7: Reload textures (drops + rebuilds all atlases at the
         * current light level, then re-bakes monarch meshes). Active
         * units may be stale until cleared & respawned. */
        r = dp_row_layout(7);
        if (point_in_rect(local_x, local_y, r.action.x, r.action.y, r.action.w, r.action.h)) {
            Units_ClearInstances();
            Units_DropAllMeshCaches();
            TexAtlas_Reload(plat);
            Units_BakeMonarchMeshes();
            fprintf(stderr, "TexAtlas reloaded at light=%d\n",
                    TexAtlas_GetLightLevel());
            consumed = 1;
        }

        /* Rows 8-11: Spawn buttons (ARA, ZON, VER, TAR). */
        const char *sides[4] = { "ARA", "ZON", "VER", "TAR" };
        for (int s = 0; s < 4; s++) {
            r = dp_row_layout(8 + s);
            if (point_in_rect(local_x, local_y, r.action.x, r.action.y, r.action.w, r.action.h)) {
                spawn_at_camera_center(sides[s]); consumed = 1;
            }
        }

        /* Row 12: Clear units. */
        r = dp_row_layout(12);
        if (point_in_rect(local_x, local_y, r.action.x, r.action.y, r.action.w, r.action.h)) {
            Units_ClearInstances(); consumed = 1;
        }
    }

    dp.prev_left_down = left_button_down;
    return consumed;
}

/* ── Drawing ───────────────────────────────────────────────────── */

static void fill(SDL_Surface *off, int x, int y, int w, int h, uint32_t rgba) {
    SDL_Rect r = { x, y, w, h };
    SDL_FillRect(off, &r, rgba);
}

static uint32_t mk_rgba(SDL_Surface *off, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return SDL_MapRGBA(off->format, r, g, b, a);
}

static void draw_button(SDL_Surface *off, int x, int y, int w, int h,
                        uint32_t fill_rgba, uint32_t border_rgba,
                        const char *label)
{
    fill(off, x, y, w, h, fill_rgba);
    /* 1-px border (four strips). */
    fill(off, x, y, w, 1, border_rgba);
    fill(off, x, y + h - 1, w, 1, border_rgba);
    fill(off, x, y, 1, h, border_rgba);
    fill(off, x + w - 1, y, 1, h, border_rgba);
    if (label) {
        int tw = Font_MeasureString(dp.font, label);
        int tx = x + (w - tw) / 2;
        int ty = y + (h - Font_LineHeight(dp.font)) / 2;
        Font_DrawString(dp.font, off, tx, ty, label);
    }
}

void DebugPanel_Draw(void) {
    if (!dp.initialized) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;

    dp.h = dp_panel_h();

    uint32_t bg     = mk_rgba(off,  20,  20,  30, 220);
    uint32_t title  = mk_rgba(off,  60,  80, 110, 255);
    uint32_t border = mk_rgba(off, 120, 140, 170, 255);
    uint32_t btn_m  = mk_rgba(off,  90,  50,  50, 255);
    uint32_t btn_p  = mk_rgba(off,  50,  90,  50, 255);
    uint32_t btn_a  = mk_rgba(off,  60,  60,  90, 255);
    uint32_t btn_b  = mk_rgba(off, 130, 130, 160, 255);

    /* Background + title bar. */
    fill(off, dp.x, dp.y, dp.w, dp.h, bg);
    fill(off, dp.x, dp.y, dp.w, DP_TITLE_H, title);
    /* Outer border. */
    fill(off, dp.x, dp.y, dp.w, 1, border);
    fill(off, dp.x, dp.y + dp.h - 1, dp.w, 1, border);
    fill(off, dp.x, dp.y, 1, dp.h, border);
    fill(off, dp.x + dp.w - 1, dp.y, 1, dp.h, border);

    Font_DrawString(dp.font, off, dp.x + DP_PAD, dp.y, "DEBUG  (drag here)");

    char buf[96];

    /* Row 0: TA_SCALE */
    {
        RowLayout r = dp_row_layout(0);
        snprintf(buf, sizeof(buf), "TA_SCALE %.6f", (double)Units_GetTAScale());
        Font_DrawString(dp.font, off, dp.x + DP_PAD, dp.y + r.minus.y, buf);
        draw_button(off, dp.x + r.minus.x, dp.y + r.minus.y, r.minus.w, r.minus.h, btn_m, btn_b, "-");
        draw_button(off, dp.x + r.plus.x,  dp.y + r.plus.y,  r.plus.w,  r.plus.h,  btn_p, btn_b, "+");
    }
    /* Row 1: TAN_TILT */
    {
        RowLayout r = dp_row_layout(1);
        snprintf(buf, sizeof(buf), "TAN_TILT %.4f", (double)Units_GetTanTilt());
        Font_DrawString(dp.font, off, dp.x + DP_PAD, dp.y + r.minus.y, buf);
        draw_button(off, dp.x + r.minus.x, dp.y + r.minus.y, r.minus.w, r.minus.h, btn_m, btn_b, "-");
        draw_button(off, dp.x + r.plus.x,  dp.y + r.plus.y,  r.plus.w,  r.plus.h,  btn_p, btn_b, "+");
    }
    /* Row 2: Heading — show degrees of unit 0 if any spawned. */
    {
        RowLayout r = dp_row_layout(2);
        int n;
        const Unit *us = Units_GetActive(&n);
        float deg = (n > 0 && us[0].alive) ? us[0].heading * 57.2957795f : 0.0f;
        snprintf(buf, sizeof(buf), "Heading %.0f deg", (double)deg);
        Font_DrawString(dp.font, off, dp.x + DP_PAD, dp.y + r.minus.y, buf);
        draw_button(off, dp.x + r.minus.x, dp.y + r.minus.y, r.minus.w, r.minus.h, btn_m, btn_b, "<");
        draw_button(off, dp.x + r.plus.x,  dp.y + r.plus.y,  r.plus.w,  r.plus.h,  btn_p, btn_b, ">");
    }
    /* Row 3: FPS readout */
    {
        RowLayout r = dp_row_layout(3);
        snprintf(buf, sizeof(buf), "FPS %.1f", (double)dp.fps_display);
        Font_DrawString(dp.font, off, dp.x + DP_PAD, dp.y + r.minus.y, buf);
    }
    /* Row 4: Backface culling on/off */
    {
        RowLayout r = dp_row_layout(4);
        snprintf(buf, sizeof(buf), "BFCull: %s",
                 Units_GetBackfaceCullOn() ? "ON" : "OFF");
        draw_button(off, dp.x + r.action.x, dp.y + r.action.y,
                    r.action.w, r.action.h, btn_a, btn_b, buf);
    }
    /* Row 5: Backface cull invert */
    {
        RowLayout r = dp_row_layout(5);
        snprintf(buf, sizeof(buf), "BFCull invert: %s",
                 Units_GetBackfaceCullInvert() ? "ON" : "OFF");
        draw_button(off, dp.x + r.action.x, dp.y + r.action.y,
                    r.action.w, r.action.h, btn_a, btn_b, buf);
    }
    /* Row 6: Light level slider (-/+) */
    {
        RowLayout r = dp_row_layout(6);
        snprintf(buf, sizeof(buf), "Light: %d", TexAtlas_GetLightLevel());
        Font_DrawString(dp.font, off, dp.x + DP_PAD, dp.y + r.minus.y, buf);
        draw_button(off, dp.x + r.minus.x, dp.y + r.minus.y, r.minus.w, r.minus.h, btn_m, btn_b, "-");
        draw_button(off, dp.x + r.plus.x,  dp.y + r.plus.y,  r.plus.w,  r.plus.h,  btn_p, btn_b, "+");
    }
    /* Row 7: Reload textures */
    {
        RowLayout r = dp_row_layout(7);
        draw_button(off, dp.x + r.action.x, dp.y + r.action.y,
                    r.action.w, r.action.h,
                    mk_rgba(off, 80, 110, 80, 255), btn_b, "Reload textures (apply Light)");
    }
    /* Rows 8-11: Spawn ARA/ZON/VER/TAR */
    {
        const char *labels[4] = { "Spawn ARA Monarch", "Spawn ZON Monarch",
                                   "Spawn VER Monarch", "Spawn TAR Monarch" };
        for (int s = 0; s < 4; s++) {
            RowLayout r = dp_row_layout(8 + s);
            draw_button(off, dp.x + r.action.x, dp.y + r.action.y,
                        r.action.w, r.action.h, btn_a, btn_b, labels[s]);
        }
    }
    /* Row 12: Clear */
    {
        RowLayout r = dp_row_layout(12);
        draw_button(off, dp.x + r.action.x, dp.y + r.action.y,
                    r.action.w, r.action.h,
                    mk_rgba(off, 90, 60, 60, 255), btn_b, "Clear units");
    }
}

#endif /* TAK_DEBUG */
