/*
 * camera.c -- camera scroll/input config storage.
 *
 * Keeps a single module-global CameraConfig so the in-game screen
 * reads consistent tuning values and a future options UI can mutate
 * them from anywhere. The config is data-only; actual scroll math
 * lives in ingame.c (reads via Camera_GetConfig).
 */

#include "tak_camera.h"
#include <string.h>

static CameraConfig g_cfg;

void Camera_ResetDefaults(void) {
    /* 1200 px/s feels comparable to the original game at 1× scroll.
     * Shift×3 (= 3600) covers big maps in a few seconds. Edge margin
     * of 16 window pixels matches typical RTS convention — big enough
     * to be hit deliberately, small enough not to fire accidentally. */
    g_cfg.scroll_px_per_sec     = 1200.0f;
    g_cfg.boost_multiplier      = 3.0f;
    g_cfg.edge_scroll_margin_px = 16;
}

const CameraConfig *Camera_GetConfig(void) {
    return &g_cfg;
}

void Camera_SetConfig(const CameraConfig *cfg) {
    if (!cfg) return;
    g_cfg = *cfg;
}
