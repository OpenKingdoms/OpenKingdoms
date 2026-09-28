/*
 * capture.c -- the fixed battle capture's flags, fog dump and scout
 * walk, see tak_capture.h.
 */
#include "tak_capture.h"
#include "tak_command_emit.h"
#include "tak_commands.h"
#include "tak_fog.h"
#include "tak_net_match.h"
#include "tak_unit.h"
#include "tak_util.h"
#include "tak_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Capture_ArgsInit(TAK_CaptureArgs *a) {
    memset(a, 0, sizeof *a);
    a->los = -1;
}

static int why_is(char *why, size_t cap, const char *fmt, const char *arg) {
    if (why && cap) snprintf(why, cap, fmt, arg ? arg : "");
    return -1;
}

int Capture_TakeArg(TAK_CaptureArgs *a, int argc, char **argv, int *i,
                    char *why, size_t why_cap) {
    if (!a || !argv || !i || *i >= argc) return 0;
    const char *f = argv[*i];
    int has_value = *i + 1 < argc;
    const char *v = has_value ? argv[*i + 1] : NULL;
    if (strcmp(f, "--scout") == 0) { a->scout = 1; return 1; }
    if (strcmp(f, "--map") != 0 && strcmp(f, "--seed") != 0 &&
        strcmp(f, "--los") != 0 && strcmp(f, "--fog-dump") != 0)
        return 0;
    if (!has_value || !v[0] || (v[0] == '-' && v[1] == '-'))
        return why_is(why, why_cap, "%s needs a value", f);
    if (strcmp(f, "--map") == 0) {
        a->map = v;
    } else if (strcmp(f, "--seed") == 0) {
        /* 64 bits wide, since a long is 32 on Windows and would wrap. */
        char *end = NULL;
        unsigned long long n = strtoull(v, &end, 10);
        if (!end || *end || v[0] == '-' || n > 0xFFFFFFFFull)
            return why_is(why, why_cap, "--seed takes a whole number from 0 to 4294967295, not \"%s\"", v);
        a->seed = (uint32_t)n;
        a->has_seed = 1;
    } else if (strcmp(f, "--los") == 0) {
        if (tak_stricmp(v, "on") == 0) a->los = 1;
        else if (tak_stricmp(v, "off") == 0) a->los = 0;
        else return why_is(why, why_cap, "--los takes on or off, not \"%s\"", v);
    } else {
        a->fog_dump = v;
    }
    (*i)++;
    return 1;
}

int Capture_Check(const TAK_CaptureArgs *a, int skirmish, int screenshot,
                  char *why, size_t why_cap) {
    if (!a) return 0;
    int any = a->map || a->has_seed || a->los >= 0 || a->scout;
    if (any && !skirmish)
        return why_is(why, why_cap, "%s", "--map, --seed, --los and --scout need --skirmish");
    if ((a->scout || a->fog_dump) && !screenshot)
        return why_is(why, why_cap, "%s", "--scout and --fog-dump need --screenshot");
    return 0;
}

int Capture_FogCell(const GameWorld *w, int cx, int cy) {
    if (!w) return TAK_FOG_UNEXPLORED;
    int st = Fog_StateAt(w, cx * 16 + 8, cy * 16 + 8);
    if (st == TAK_FOG_EXPLORED && !w->cfg.line_of_sight) st = TAK_FOG_VISIBLE;
    return st;
}

int Capture_WriteFog(const GameWorld *w, const char *path) {
    if (!w || !path) return -1;
    int cw = w->map_pixels_w / 16, ch = w->map_pixels_h / 16;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++)
            fputc(Capture_FogCell(w, cx, cy), f);
    int ok = ferror(f) == 0;
    fclose(f);
    return ok ? 0 : -1;
}

static struct {
    int     stage, handle;
    int32_t home_x, home_y, far_x, far_y;
} g_scout;

void Capture_ScoutReset(void) { memset(&g_scout, 0, sizeof g_scout); }

int Capture_ScoutTick(GameWorld *w, uint32_t tick, uint32_t turn_back_tick) {
    /* A capture is a single-player thing: in a match the orders would
     * go to every other player. */
    if (!w || TAK_Match_IsLive()) return 0;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (g_scout.stage == 0) {
        for (int i = 0; i < count; i++) {
            const UnitDef *d = Units_GetDef(units[i].def_idx);
            if (units[i].alive != UNIT_ALIVE_ACTIVE || !d || !d->commander) continue;
            if (units[i].player_id != Units_LocalPlayer()) continue;
            g_scout.handle = i;
            g_scout.home_x = units[i].world_x;
            g_scout.home_y = units[i].world_y;
            float dx = (float)(w->map_pixels_w / 2 - units[i].world_x);
            float dy = (float)(w->map_pixels_h / 2 - units[i].world_y);
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1.0f) len = 1.0f;
            g_scout.far_x = units[i].world_x + (int32_t)(dx / len * 34.0f * 16.0f);
            g_scout.far_y = units[i].world_y + (int32_t)(dy / len * 34.0f * 16.0f);
            TAK_Cmd_EmitUnit(TAK_CMD_MOVE, i, g_scout.far_x, g_scout.far_y, -1, 0, 0);
            g_scout.stage = 1;
            return 1;
        }
        return 0;
    }
    /* The view holds the middle of the walk, kept on the map. */
    int32_t cam_x = (g_scout.home_x + g_scout.far_x) / 2 - w->viewport_w / 2;
    int32_t cam_y = (g_scout.home_y + g_scout.far_y) / 2 - w->viewport_h / 2;
    int32_t max_x = w->map_pixels_w - w->viewport_w, max_y = w->map_pixels_h - w->viewport_h;
    if (cam_x > max_x) cam_x = max_x;
    if (cam_y > max_y) cam_y = max_y;
    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    w->cam_x = cam_x;
    w->cam_y = cam_y;
    if (g_scout.stage == 1 && g_scout.handle < count) {
        const Unit *u = &units[g_scout.handle];
        int32_t dx = u->world_x - g_scout.far_x, dy = u->world_y - g_scout.far_y;
        if ((int64_t)dx * dx + (int64_t)dy * dy <= 48 * 48 || tick >= turn_back_tick) {
            TAK_Cmd_EmitUnit(TAK_CMD_MOVE, g_scout.handle, g_scout.home_x,
                             g_scout.home_y, -1, 0, 0);
            g_scout.stage = 2;
            return 1;
        }
    }
    return 0;
}
