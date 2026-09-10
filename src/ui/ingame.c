/*
 * ingame.c -- In-game screen (GAMESTATE_IN_GAME).
 *
 * Phase B scope: renders terrain at the GameWorld's current camera
 * position. ESC exits to menu. No simulation, no HUD, no mouse scroll
 * yet (step 9 adds camera input).
 */

#include "tak_ingame.h"
#include "tak_gameloop.h"
#include "tak_world.h"
#include "tak_ai.h"
#include "tak_terrain.h"
#include "tak_unit.h"
#include "tak_minimap.h"
#include "tak_camera.h"
#include "tak_ui.h"
#include "tak_debug_panel.h"
#include "tak_hud.h"
#include "tak_fog.h"
#include "tak_font.h"
#include "tak_hud_text.h"
#include "tak_game_sound.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int initialized;
    /* Edge-detected key state for the M4 debug hotkeys. SDL's
     * GetKeyboardState gives us "currently down"; we want "just
     * pressed this frame" so a single tap doesn't fire 60 times.
     * One byte per scancode we care about, snapshotted last frame. */
    uint8_t prev_keys[SDL_NUM_SCANCODES];
    uint8_t prev_left;
    uint8_t prev_right;
    /* Marquee drag-select state. Tracking starts on a left press over
     * the game world; it becomes an active marquee once the cursor
     * moves past a small threshold (so plain clicks stay clicks). The
     * anchor is stored in WORLD space so camera scroll mid-drag keeps
     * the box pinned to the terrain, like legacy. */
    uint8_t drag_tracking;
    uint8_t drag_active;
    int     drag_start_wx, drag_start_wy;     /* window coords, threshold test */
    int32_t drag_world_x, drag_world_y;       /* world-space anchor corner */
    Font *end_font;
    HUDText *end_text;
} ig;

/* Order-ack voice: legacy Unit_PlayOrderAck (legacy:221247)
 * plays the selected unit's soundclass action on every player-issued
 * order — attack/guard/patrol/Move/select, anything else "default".
 * We voice the first selected unit, panned from its world position. */
static void ig_play_order_ack(const GameWorld *world, const char *action) {
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    if (n <= 0 || !world) return;
    const UnitDef *def = Units_GetSelectedDef();
    if (!def || !def->soundcategory[0]) return;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    int h = sel[0];
    if (h < 0 || h >= count) return;
    GameSound_UnitAction(def->soundcategory, action, 0x7f,
                         units[h].world_x, units[h].world_y,
                         world->cam_x, world->cam_y,
                         world->viewport_w, world->viewport_h);
}

static int unit_is_commander_def(const UnitDef *def) {
    if (!def) return 0;
    if (strstr(def->category, "Monarch")) return 1;
    if (strstr(def->unitname, "KING")) return 1;
    if (strstr(def->unitname, "QUEEN")) return 1;
    return 0;
}

static int player_team_id(const GameWorld *world, int player_id) {
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    const PlayerSlot *slot = &world->cfg.players[player_id - 1];
    if (slot->kind == TAK_SLOT_CLOSED) return 0;
    return slot->team > 0 ? slot->team : player_id;
}

static int player_slot_active(const GameWorld *world, int player_id) {
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    return world->cfg.players[player_id - 1].kind != TAK_SLOT_CLOSED;
}

static void InGame_EvaluateSkirmishRules(GameWorld *world) {
    if (!world || world->skirmish_game_over) return;
    if (world->mission.objective_count > 0 ||
        world->mission.placement_count > 0) {
        return;
    }

    /* Legacy grace: no elimination verdicts in the first 30s
     * (legacy:240040 — 900 ticks @30Hz = 1800 @our 60Hz). */
    if (world->skirmish_elapsed_ticks < 1800) return;

    int slot_active[TAK_MAX_PLAYERS + 1] = { 0 };
    int player_alive[TAK_MAX_PLAYERS + 1] = { 0 };
    int monarch_alive[TAK_MAX_PLAYERS + 1] = { 0 };
    int active_slot_count = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        slot_active[p] = player_slot_active(world, p);
        if (slot_active[p]) active_slot_count++;
    }
    if (active_slot_count <= 1) return;

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        /* A monarch riding a transport is alive — counting only
         * ACTIVE ruled a boarded king dead (false Defeat). */
        if (u->alive != UNIT_ALIVE_ACTIVE &&
            u->alive != UNIT_ALIVE_TRANSPORTED) continue;
        if (u->player_id < 1 || u->player_id > TAK_MAX_PLAYERS) continue;
        if (!slot_active[u->player_id]) continue;
        const UnitDef *def = Units_GetDef((int)u->def_idx);
        player_alive[u->player_id] = 1;
        if (unit_is_commander_def(def)) {
            monarch_alive[u->player_id] = 1;
        }
    }

    int alive_team_seen[TAK_MAX_PLAYERS + 1] = { 0 };
    int alive_team_count = 0;
    int winner_team = 0;
    int eliminated_local = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        if (!slot_active[p]) continue;
        int alive = world->cfg.monarch_expendable
                  ? player_alive[p]
                  : monarch_alive[p];
        if (p == 1 && !alive) eliminated_local = 1;
        if (!alive) continue;
        int team = player_team_id(world, p);
        if (team <= 0) continue;
        if (!alive_team_seen[team]) {
            alive_team_seen[team] = 1;
            alive_team_count++;
            winner_team = team;
        }
    }

    if (alive_team_count <= 1) {
        world->skirmish_game_over = 1;
        world->skirmish_winner_team = (alive_team_count == 1) ? winner_team : 0;
        int local_team = player_team_id(world, 1);
        if (alive_team_count == 1 && winner_team == local_team && !eliminated_local) {
            world->skirmish_local_result = 1;
            strncpy(world->skirmish_end_reason, "Victory",
                    sizeof(world->skirmish_end_reason) - 1);
        } else if (alive_team_count == 0) {
            world->skirmish_local_result = 0;
            strncpy(world->skirmish_end_reason, "Draw",
                    sizeof(world->skirmish_end_reason) - 1);
        } else {
            world->skirmish_local_result = -1;
            strncpy(world->skirmish_end_reason, "Defeat",
                    sizeof(world->skirmish_end_reason) - 1);
        }
        fprintf(stderr, "Skirmish ended: %s winner_team=%d local=%d\n",
                world->skirmish_end_reason,
                world->skirmish_winner_team,
                world->skirmish_local_result);
    }
}

static void InGame_EvaluateMissionObjectives(GameWorld *world) {
    int unit_count = 0;
    const Unit *units;
    MissionUnitSnapshot *snapshots;
    int satisfied = 0;

    if (!world || world->mission.objective_count <= 0) return;
    units = Units_GetActive(&unit_count);
    if (!units || unit_count <= 0) return;

    snapshots = (MissionUnitSnapshot *)malloc(
        (size_t)unit_count * sizeof(MissionUnitSnapshot));
    if (!snapshots) return;

    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        const UnitDef *def = Units_GetDef((int)u->def_idx);
        memset(&snapshots[i], 0, sizeof(snapshots[i]));
        if (def) {
            strncpy(snapshots[i].unitname, def->unitname,
                    sizeof(snapshots[i].unitname) - 1);
        }
        snapshots[i].player = (int)u->player_id;
        snapshots[i].x = u->world_x / 16;
        snapshots[i].z = u->world_y / 16;
        snapshots[i].alive = (u->alive == 1);
        snapshots[i].mobile = (def && def->max_velocity > 0.0f) ? 1 : 0;
        snapshots[i].commander = unit_is_commander_def(def);
    }

    for (int i = 0; i < world->mission.objective_count; i++) {
        if (Mission_ObjectiveSatisfied(&world->mission.objectives[i],
                                       snapshots, unit_count, 1,
                                       world->mission_elapsed_seconds)) {
            satisfied++;
        }
    }
    world->mission_objectives_satisfied = satisfied;
    world->mission_victory =
        Mission_AllObjectivesSatisfied(&world->mission, snapshots,
                                       unit_count, 1,
                                       world->mission_elapsed_seconds);
    free(snapshots);
}

/* Per-subsystem sim timing (ms, cumulative): 0 ai, 1 engines,
 * 2 economy, 3 fog. Read+reset by the perf probe. */
double g_sim_prof_ms[4];

static double prof_now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
}

static void InGame_SimulationStep(GameWorld *world) {
    if (!world || !world->loaded) return;
    if (world->skirmish_game_over) return;

    /* Current prototype sim systems still live in render/ui modules.
     * Keep the fixed-step boundary here until those systems move under
     * src/game/SimulationState. Nothing gameplay-owned should tick from
     * raw frame_dt. */
    double t0 = prof_now_ms();
    TAK_AI_TickSkirmish(world);
    double t1 = prof_now_ms();
    Units_TickEngines();
    double t2 = prof_now_ms();
    Economy_Tick(&world->economy);
    double t3 = prof_now_ms();
    g_sim_prof_ms[0] += t1 - t0;
    g_sim_prof_ms[1] += t2 - t1;
    g_sim_prof_ms[2] += t3 - t2;
    /* Fog recompute is the hottest sim pass (per-cell LOS raycasts per
     * unit — 95% of the browser perf trace). Staggered 5Hz per player
     * is visually identical to per-tick. */
    {
        uint32_t t = (uint32_t)(world->skirmish_elapsed_ticks
                              + world->mission_elapsed_ticks);
        double f0 = prof_now_ms();
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
            if (world->cfg.players[p - 1].kind == TAK_SLOT_CLOSED) continue;
            if (((t + (uint32_t)p) % 12u) != 0u) continue;
            Fog_Update(world, p);
        }
        g_sim_prof_ms[3] += prof_now_ms() - f0;
    }
    if (world->mission.objective_count > 0) {
        world->mission_elapsed_ticks++;
        world->mission_elapsed_seconds = world->mission_elapsed_ticks / 60;
        InGame_EvaluateMissionObjectives(world);
    } else {
        world->skirmish_elapsed_ticks++;
        InGame_EvaluateSkirmishRules(world);
    }
}

void InGame_DebugRunSimTicks(int ticks) {
    GameWorld *world = World_Get();
    for (int i = 0; i < ticks; i++) {
        InGame_SimulationStep(world);
    }
}

int InGame_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&ig, 0, sizeof(ig));

    GameWorld *world = World_Get();
    if (!world || !world->loaded) {
        fprintf(stderr, "InGame_Init: no loaded world; bailing to menu\n");
        return -1;
    }
    fprintf(stderr, "InGame_Init: world=%dx%d tiles (%dx%d px), cam=(%d, %d)\n",
            world->tnt.width_tiles, world->tnt.height_tiles,
            world->map_pixels_w, world->map_pixels_h,
            world->cam_x, world->cam_y);

    /* Non-fatal: if minimap fails to init (no overview image on this
     * map, or GPU upload fails), the screen still runs. */
    (void)Minimap_Init(platform);

    /* Debug overlay (TAK_DEBUG only — release builds get inline no-ops). */
    DebugPanel_Init(platform);

    ig.end_font = Font_Load("data/anims/font12", UI_RGBAFormat());
    if (ig.end_font) {
        ig.end_text = HUDText_Load(platform, ig.end_font);
    }

    ig.initialized = 1;
    return 0;
}

/* Marquee rectangle. Legacy draws a plain white outline box (four
 * quads off the solid-white texture, caller alpha — the legacy reference
 * :125401-125540); one SDL outline rect matches that look. */
static void InGame_DrawMarquee(TAK_Platform *platform,
                               const GameWorld *world) {
    if (!ig.drag_active || !platform || !platform->renderer || !world) return;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    int ax = (int)(ig.drag_world_x - world->cam_x);
    int ay = (int)(ig.drag_world_y - world->cam_y);
    SDL_Rect box = { ax < mx ? ax : mx, ay < my ? ay : my,
                     abs(mx - ax) + 1, abs(my - ay) + 1 };
    SDL_Renderer *r = platform->renderer;
    SDL_BlendMode prev_blend;
    SDL_GetRenderDrawBlendMode(r, &prev_blend);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 220);
    SDL_RenderDrawRect(r, &box);
    SDL_SetRenderDrawBlendMode(r, prev_blend);
}

static void InGame_DrawSkirmishEndOverlay(TAK_Platform *platform,
                                          const GameWorld *world) {
    if (!platform || !platform->renderer || !world || !world->skirmish_game_over) return;

    SDL_Renderer *r = platform->renderer;
    SDL_BlendMode prev_blend;
    SDL_GetRenderDrawBlendMode(r, &prev_blend);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_Rect shade = {0, 0, platform->window_w, platform->window_h};
    SDL_RenderFillRect(r, &shade);

    SDL_Rect panel = {
        (platform->window_w - 360) / 2,
        (platform->window_h - 126) / 2,
        360,
        126
    };
    SDL_SetRenderDrawColor(r, 28, 24, 18, 230);
    SDL_RenderFillRect(r, &panel);
    SDL_SetRenderDrawColor(r, 210, 190, 130, 255);
    SDL_RenderDrawRect(r, &panel);

    if (ig.end_text) {
        const char *title = world->skirmish_end_reason[0]
                          ? world->skirmish_end_reason
                          : "Battle Complete";
        char detail[96];
        const char *hint = "Press Esc to return to menu";
        SDL_Color title_col = {255, 232, 150, 255};
        SDL_Color text_col = {235, 225, 200, 255};

        snprintf(detail, sizeof(detail), "Winning team: %d",
                 world->skirmish_winner_team);

        int title_w = HUDText_Measure(ig.end_text, title);
        int detail_w = HUDText_Measure(ig.end_text, detail);
        int hint_w = HUDText_Measure(ig.end_text, hint);
        HUDText_DrawString(platform, ig.end_text,
                           panel.x + (panel.w - title_w) / 2,
                           panel.y + 24, title, title_col);
        HUDText_DrawString(platform, ig.end_text,
                           panel.x + (panel.w - detail_w) / 2,
                           panel.y + 58, detail, text_col);
        HUDText_DrawString(platform, ig.end_text,
                           panel.x + (panel.w - hint_w) / 2,
                           panel.y + 86, hint, text_col);
    }

    SDL_SetRenderDrawBlendMode(r, prev_blend);
}

int InGame_Tick(TAK_Platform *platform, Timer *timer) {
    if (!ig.initialized) return GAMESTATE_MENU;
    if (!timer) return GAMESTATE_MENU;

    GameWorld *world = World_Get();
    if (!world || !world->loaded) return GAMESTATE_MENU;

    SDL_Surface *off = UI_Offscreen();
    /* Clear the UI canvas to fully transparent every frame. Anything
     * drawn onto it (debug panel, future HUD) overlays the 3D scene
     * via alpha compositing in TAK_Platform_Present. Without this,
     * old pixels from previous frames stick around — most visibly as
     * "ghost" trails when the debug panel is dragged. */
    if (off) SDL_FillRect(off, NULL, SDL_MapRGBA(off->format, 0, 0, 0, 0));

    world->viewport_w = platform->window_w;
    world->viewport_h = platform->window_h;

    /* HUD reserves the right sidebar + bottom strip; viewport_w/h
     * shrink to the visible game area for camera bounds + math. */
    HUD_Init(platform, world);

    while (Timer_ConsumeTick(timer)) {
        InGame_SimulationStep(world);
    }

    /* Clip the world to the play area. Legacy draws terrain and scene
     * objects into the viewport and then paints the sidebar and bottom
     * frames over the top every frame (:210228-210273). Without the clip
     * a unit standing at the map edge draws across the sidebar. */
    SDL_Rect world_clip;
    int have_clip = HUD_GetViewportRect(platform, &world_clip);
    if (have_clip) SDL_RenderSetClipRect(platform->renderer, &world_clip);
    Terrain_Render(world, platform);
    Fog_RenderOverlay(world, platform);
    Units_Render(world, platform);
    if (have_clip) SDL_RenderSetClipRect(platform->renderer, NULL);

    HUD_Draw(platform, world);
    Minimap_Draw(platform);
    InGame_DrawMarquee(platform, world);
    InGame_DrawSkirmishEndOverlay(platform, world);

    /* Custom cursor when a command mode is active and the mouse is
     * over the game viewport. Hide the OS cursor so only ours
     * shows; restore otherwise. Reads mouse state inline since the
     * input handling block hasn't run yet. */
    {
        int mx = 0, my = 0;
        if (platform->has_focus) SDL_GetMouseState(&mx, &my);
        int over_world = platform->has_focus &&
                         !HUD_HitTest(mx, my, platform);
        int drew_cursor = 0;
        if (over_world && HUD_GetCommandMode() != 0) {
            HUD_DrawCommandCursor(platform, mx, my);
            drew_cursor = 1;
        } else if (over_world) {
            /* Context-sensitive default cursor (legacy manual §IV.2):
             * select hand over any unit, attack cursor over an enemy
             * when something is selected, normal pointer otherwise. */
            int n_sel = 0;
            Units_GetSelection(&n_sel);
            int hover = Units_PickAt(world->cam_x + mx,
                                     world->cam_y + my, 48);
            int cur_id = HUD_CUR_NORMAL;
            if (hover >= 0) {
                if (g_units_get_player(hover) == 1 &&
                    Units_IsUnderConstruction(hover) &&
                    Units_SelectionHasBuilder()) {
                    cur_id = HUD_CMD_HEAL;   /* resume-build cursor */
                } else if (g_units_get_player(hover) != 1 && n_sel > 0) {
                    cur_id = HUD_CMD_ATTACK;
                } else {
                    cur_id = HUD_CUR_SELECT;
                }
            }
            drew_cursor = HUD_DrawCursorById(platform, cur_id, mx, my);
        }
        SDL_ShowCursor(drew_cursor ? SDL_DISABLE : SDL_ENABLE);
    }

    /* Debug overlay paints onto UI canvas before UI_Present uploads. */
    DebugPanel_TickFPS((float)Timer_GetFrameDT(timer));
    DebugPanel_Draw();

    UI_Present(platform);

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_ESCAPE]) {
        return GAMESTATE_MENU;
    }

    /* M4 debug hotkeys (PHASE_C_3DO.md §6 M4): spawn a monarch with
     * '3', tune TA_SCALE with '-'/'=', tune TAN_TILT with '['/']'.
     * Edge-detected so a held key doesn't repeat. Logging both values
     * on every tweak makes R3 tuning straightforward — read off the
     * numbers when the model looks right and pin them in units.c.
     *
     * Spawn coordinate: camera-center in world space. Drops the
     * monarch into the middle of the visible viewport so you can
     * actually see it without scrolling. */
#define IG_PRESSED(sc) (keys[sc] && !ig.prev_keys[sc])
    /* Digit keys are gameplay (control groups); the digit debug
     * hotkeys below require Alt so the two don't collide. */
    int ig_alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    if (ig_alt && IG_PRESSED(SDL_SCANCODE_3)) {
        int32_t wx = world->cam_x + world->viewport_w / 2;
        int32_t wy = world->cam_y + world->viewport_h / 2;
        Units_DebugSpawnMonarch("ARA", wx, wy);
    }
    /* Phase D M3 debug: 'H' rotates each alive unit's head piece by
     * 30° per press (5461 in COB fixed-point: 65536 = 360°). Visual
     * proof that the per-piece transform pipeline works. */
    if (IG_PRESSED(SDL_SCANCODE_H)) {
        Units_DebugRotateHead(5461);
    }
    /* Phase D M5 debug: 'L' (locomotion) invokes the unit's walk
     * script, animating legs (and other body parts that walk drives).
     * Each press spawns a fresh walk thread; unit accumulates multiple
     * cycling threads until its 16 slots fill. (W is camera scroll.) */
    if (IG_PRESSED(SDL_SCANCODE_L)) {
        Units_DebugInvokeScript("walk");
    }
    /* Phase D M5b: 'V' bumps unit velocity by 4*65536 (4 unit/sec).
     * MoveWatcher and walk scripts can poll GET-UNIT-VALUE(5) to drive
     * leg-cycle timing. */
    if (IG_PRESSED(SDL_SCANCODE_V)) {
        Units_DebugBumpVelocity(4 * 65536);
    }
    /* Phase D M6: 'K' kills the first alive unit — its Killed script
     * runs (typically EXPLODE pieces); on completion the unit is
     * despawned. */
    if (IG_PRESSED(SDL_SCANCODE_K)) {
        Units_DebugKillFirst();
    }
    /* Toggle per-unit health bars on/off (manual §IV.2: '~' key). */
    if (IG_PRESSED(SDL_SCANCODE_GRAVE)) {
        Units_ToggleHealthBars();
        fprintf(stderr, "Health bars: %s\n",
                Units_GetHealthBarsOn() ? "ON" : "OFF");
    }
    /* Sprint 1: 'E' spawns an enemy monarch close to camera center —
     * within ARAKING's 232-pixel sight radius so auto-acquire fires
     * immediately. */
    if (IG_PRESSED(SDL_SCANCODE_E)) {
        int32_t wx = world->cam_x + world->viewport_w / 2 + 120;
        int32_t wy = world->cam_y + world->viewport_h / 2 + 120;
        Units_DebugSpawnEnemy(wx, wy);
    }
    /* M8 stress test: replace the active array with a grid of N monarchs
     * of player 1's faction. '4' = 500, '5' = 2000 (the engine's cap). */
    if (ig_alt && (IG_PRESSED(SDL_SCANCODE_4) || IG_PRESSED(SDL_SCANCODE_5))) {
        static const char *side_prefixes[] = { "ARA", "TAR", "VER", "ZON" };
        int side = world->cfg.players[0].side;
        const char *prefix = (side >= 0 && side < 4) ? side_prefixes[side] : "ARA";
        int color = world->cfg.players[0].color;
        int32_t cx = world->cam_x + world->viewport_w / 2;
        int32_t cy = world->cam_y + world->viewport_h / 2;
        int n = IG_PRESSED(SDL_SCANCODE_4) ? 500 : 2000;
        Units_DebugSpawnGrid(n, prefix, 1, color, cx, cy, 64);
    }
    if (IG_PRESSED(SDL_SCANCODE_MINUS)) {
        Units_SetTAScale(Units_GetTAScale() * 0.85f);
        fprintf(stderr, "TA_SCALE=%.6f  TAN_TILT=%.4f\n",
                (double)Units_GetTAScale(), (double)Units_GetTanTilt());
    }
    if (IG_PRESSED(SDL_SCANCODE_EQUALS)) {
        Units_SetTAScale(Units_GetTAScale() / 0.85f);
        fprintf(stderr, "TA_SCALE=%.6f  TAN_TILT=%.4f\n",
                (double)Units_GetTAScale(), (double)Units_GetTanTilt());
    }
    if (IG_PRESSED(SDL_SCANCODE_LEFTBRACKET)) {
        Units_SetTanTilt(Units_GetTanTilt() - 0.05f);
        fprintf(stderr, "TA_SCALE=%.6f  TAN_TILT=%.4f\n",
                (double)Units_GetTAScale(), (double)Units_GetTanTilt());
    }
    if (IG_PRESSED(SDL_SCANCODE_RIGHTBRACKET)) {
        Units_SetTanTilt(Units_GetTanTilt() + 0.05f);
        fprintf(stderr, "TA_SCALE=%.6f  TAN_TILT=%.4f\n",
                (double)Units_GetTAScale(), (double)Units_GetTanTilt());
    }
    /* Control groups: Ctrl+digit assigns the current selection to a
     * group, plain digit recalls it (legacy squad hotkeys). */
    {
        static const SDL_Scancode ig_digits[10] = {
            SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
            SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
            SDL_SCANCODE_8, SDL_SCANCODE_9
        };
        int ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
        if (!ig_alt) {
            for (int d = 0; d < 10; d++) {
                if (!IG_PRESSED(ig_digits[d])) continue;
                if (ctrl) {
                    Units_AssignControlGroup(d);
                    fprintf(stderr, "Control group %d assigned\n", d);
                } else {
                    int n = Units_RecallControlGroup(d);
                    fprintf(stderr, "Control group %d recalled (%d units)\n",
                            d, n);
                }
            }
        }
    }
#undef IG_PRESSED


    /* Camera scroll. Speed, boost, and edge-margin come from the
     * module-global CameraConfig (tak_camera.h) — a future options UI
     * tweaks that struct and all of this automatically follows.
     * Keyboard and mouse-edge contributions add to each other, so
     * holding A at the left edge scrolls at 2× speed. */
    const CameraConfig *cc = Camera_GetConfig();
    float scroll_px = cc->scroll_px_per_sec * (float)Timer_GetFrameDT(timer);
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
        scroll_px *= cc->boost_multiplier;
    int32_t dx = 0, dy = 0;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) dx -= (int32_t)scroll_px;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) dx += (int32_t)scroll_px;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) dy -= (int32_t)scroll_px;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) dy += (int32_t)scroll_px;

    /* Mouse input is consumed in priority order: debug panel → minimap
     * → edge scroll. The first one to claim the click wins; later
     * handlers see "no click" so they don't double-react. */
    int mm_active = 0;
    int dp_active = 0;
    int wx = 0, wy = 0;
    uint32_t buttons = 0;
    int left = 0, right = 0;
    if (platform->has_focus) {
        buttons = SDL_GetMouseState(&wx, &wy);
        left  = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))  != 0;
        right = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
        if (DebugPanel_HandleInput(platform, wx, wy, left)) {
            dp_active = 1;
            dx = dy = 0;     /* panel wins over WASD this frame */
        }
    }

    /* Edge-detect mouse buttons so click handlers fire once per
     * press, not every frame the button is held. */
    int left_pressed  = (left  && !ig.prev_left);
    int left_released = (!left && ig.prev_left);
    int right_pressed = (right && !ig.prev_right);
    ig.prev_left  = (uint8_t)left;
    ig.prev_right = (uint8_t)right;
    int shift_held = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];

    /* HUD click dispatch first — sidebar action buttons set/clear
     * the command mode. If the click hit a button we consume it
     * and skip world-click handling. */
    if (platform->has_focus && !dp_active && left_pressed &&
        HUD_HitTest(wx, wy, platform))
    {
        if (HUD_HandleSidebarClick(wx, wy, platform)) {
            /* Click consumed by HUD — skip world handler this frame. */
        }
    }
    if (platform->has_focus && !dp_active && right_pressed &&
        HUD_HitTest(wx, wy, platform))
    {
        HUD_HandleSidebarRightClick(wx, wy, platform);
    }

    /* Game-world click handling — TAK left-click mouse interface
     * (manual §IV.2 default):
     *   - Pending command mode: left-click executes it (Move/Attack/etc.)
     *   - Otherwise: left-click on friendly = select, on enemy = attack,
     *     on terrain = move (legacy default-cursor behaviour).
     *   - Right-click: cancel command mode if active, else deselect. */
    if (platform->has_focus && !dp_active && !HUD_HitTest(wx, wy, platform)) {
        int32_t world_click_x = world->cam_x + wx;
        int32_t world_click_y = world->cam_y + wy;
        int cmd = HUD_GetCommandMode();

        /* Press arms drag-tracking; movement past a 4px threshold
         * upgrades it to a marquee; release resolves to either a
         * marquee select or the single-click dispatch below. The
         * anchor lives in world space so camera scroll mid-drag
         * keeps the box pinned to the terrain. */
        if (left_pressed) {
            ig.drag_tracking = 1;
            ig.drag_active   = 0;
            ig.drag_start_wx = wx;
            ig.drag_start_wy = wy;
            ig.drag_world_x  = world_click_x;
            ig.drag_world_y  = world_click_y;
        }
        if (ig.drag_tracking && left &&
            (abs(wx - ig.drag_start_wx) > 4 || abs(wy - ig.drag_start_wy) > 4)) {
            ig.drag_active = 1;
        }
        if (left_released && ig.drag_tracking && ig.drag_active) {
            int n = Units_SelectInRect(ig.drag_world_x, ig.drag_world_y,
                                       world_click_x, world_click_y,
                                       shift_held);
            if (n > 0) ig_play_order_ack(world, "select");
            fprintf(stderr, "Marquee select: %d units\n", n);
            ig.drag_tracking = 0;
            ig.drag_active   = 0;
        } else if (left_released && ig.drag_tracking) {
            ig.drag_tracking = 0;
            int hit = Units_PickAt(world_click_x, world_click_y, 48);
            int n_sel = 0;
            Units_GetSelection(&n_sel);
            if (HUD_IsTargetingMode(cmd)) {
                /* Pending targeting command: world-click executes it
                 * and clears the mode. Mirrors legacy
                 * Selection_IssueAttackOrder dispatch which routes by
                 * the registered hotkey + click target. */
                switch (cmd) {
                    case HUD_CMD_MOVE:
                        Units_CommandMoveSelected(world_click_x, world_click_y);
                        break;
                    case HUD_CMD_PATROL:
                        Units_CommandPatrolSelected(world_click_x, world_click_y);
                        break;
                    case HUD_CMD_ATTACK:
                        if (hit >= 0) Units_CommandAttackSelected(hit);
                        else Units_CommandAttackGroundSelected(world_click_x,
                                                              world_click_y);
                        break;
                    case HUD_CMD_HEAL:
                        if (hit >= 0) Units_CommandRepairSelected(hit);
                        break;
                    case HUD_CMD_LOAD:
                        if (hit >= 0) Units_CommandLoadSelected(hit);
                        break;
                    case HUD_CMD_CLEAR:
                        /* Sweep cursor: legacy's CLEAR order resolves on
                         * the map cell, so a tree/rock/rubble under the
                         * click is the target and a live unit is not
                         * (legacy:187127-187207). Try the feature first
                         * and keep the unit form as our fallback. */
                        if (Units_CommandReclaimFeatureSelected(
                                world_click_x, world_click_y) == 0 &&
                            hit >= 0) {
                            Units_CommandReclaimSelected(hit);
                        }
                        break;
                    case HUD_CMD_GUARD:
                        if (hit >= 0) Units_CommandGuardSelected(hit);
                        break;
                    case HUD_CMD_UNLOAD:
                        Units_CommandUnloadSelected(world_click_x, world_click_y);
                        break;
                    case HUD_CMD_W_SPECIAL:
                        /* Special-weapon shot: switch the selected
                         * unit's active weapon to slot 2 then issue
                         * an attack at the click target. The combat
                         * code reads weapon_slot and will fire the
                         * Special weapon next tick. */
                        Units_CommandSetWeaponSlotSelected(2);
                        if (hit >= 0) {
                            Units_CommandAttackSelected(hit);
                        } else {
                            Units_CommandMoveSelected(world_click_x, world_click_y);
                        }
                        break;
                    case HUD_CMD_PLACE_BUILD: {
                        /* Building placement: spawn the building at
                         * 1 HP and issue the selected builder a BUILD
                         * order. The TickCombat MOVING handler walks
                         * the builder to the site and adds HP per
                         * tick (1.0/workertime of max — matches legacy
                         * the legacy reference ~162905). */
                        int bdef = HUD_GetBuildPlacementDefIdx();
                        if (bdef >= 0) {
                            /* Same cell snap the ghost drew at, so the
                             * building lands where the preview was
                             * (legacy:184168). */
                            int32_t bx = world_click_x, by = world_click_y;
                            Units_SnapBuildSite(bdef, &bx, &by);
                            int new_handle = Units_BeginBuilding(bdef, bx, by);
                            if (new_handle >= 0) {
                                fprintf(stderr,
                                  "Build: started def=%d at (%d,%d) handle=%d\n",
                                  bdef, bx, by, new_handle);
                            } else {
                                fprintf(stderr, "Build: BeginBuilding failed (no builder selected?)\n");
                            }
                        }
                        break;
                    }
                }
                {
                    const char *ack = "default";
                    switch (cmd) {
                        case HUD_CMD_MOVE:   ack = "Move";   break;
                        case HUD_CMD_ATTACK: ack = "attack"; break;
                        case HUD_CMD_PATROL: ack = "patrol"; break;
                        case HUD_CMD_GUARD:  ack = "guard";  break;
                        default: break;
                    }
                    ig_play_order_ack(world, ack);
                }
                HUD_ClearCommandMode();
            } else if (hit >= 0 && g_units_get_player(hit) == 1 &&
                       Units_IsUnderConstruction(hit) &&
                       Units_SelectionHasBuilder() && !shift_held) {
                /* Builder + nanoframe click = resume (legacy HelpBuild). */
                Units_CommandRepairSelected(hit);
                ig_play_order_ack(world, "default");
            } else if (hit >= 0 && g_units_get_player(hit) == 1) {
                /* Friendly unit click: replace selection; shift-click
                 * toggles the unit in/out of the selection. */
                if (shift_held) Units_SelectToggle(hit);
                else            Units_SelectSingle(hit);
                ig_play_order_ack(world, "select");
                fprintf(stderr, "Selected unit %d\n", hit);
            } else if (n_sel > 0) {
                if (hit >= 0) {
                    Units_CommandAttackSelected(hit);
                    ig_play_order_ack(world, "attack");
                    fprintf(stderr, "Attack -> unit %d\n", hit);
                } else {
                    Units_CommandMoveSelected(world_click_x, world_click_y);
                    ig_play_order_ack(world, "Move");
                    fprintf(stderr, "Move -> (%d,%d)\n",
                            world_click_x, world_click_y);
                }
            }
        }
        if (right_pressed) {
            if (HUD_GetCommandMode() != 0) {
                HUD_ClearCommandMode();
            } else {
                Units_SelectSingle(-1);
            }
        }
    }

    /* A release anywhere else (over the HUD, after focus loss) cancels
     * an in-flight drag so it can't fire later with stale coords. */
    if (left_released) {
        ig.drag_tracking = 0;
        ig.drag_active   = 0;
    }

    /* Minimap click/drag: if the user is holding left-mouse over the
     * minimap, jump the camera to the clicked point. This runs before
     * edge-scroll so that clicking near the window edge (on the
     * minimap itself, which sits in the top-right corner) doesn't
     * also trigger edge-scroll. When minimap consumes the input we
     * skip edge-scroll for the frame. */
    if (!dp_active && platform->has_focus) {
        int32_t cam_x = 0, cam_y = 0;
        if (Minimap_HandleInput(platform, wx, wy, left, &cam_x, &cam_y)) {
            /* Legacy: with units selected, minimap left-click is a MOVE
             * order to that spot; camera-jump only with no selection. */
            int n_sel = 0;
            Units_GetSelection(&n_sel);
            if (n_sel > 0) {
                if (left_pressed) {
                    Units_CommandMoveSelected(
                        cam_x + world->viewport_w / 2,
                        cam_y + world->viewport_h / 2);
                    ig_play_order_ack(world, "Move");
                }
            } else {
                world->cam_x = cam_x;
                world->cam_y = cam_y;
            }
            dx = dy = 0;     /* minimap wins over WASD this frame */
            mm_active = 1;
        }
    }

    /* Mouse-edge scroll. Only fires when window has focus — otherwise
     * the camera drifts while the user is Alt-Tabbed to another app.
     * Mouse state is in WINDOW coords (not canvas), which is right:
     * we're reacting to the physical window edge. A zero margin
     * disables edge scroll entirely (useful on multi-monitor setups
     * where the player wants cursor-over-edge without the camera
     * following). */
    if (!dp_active && !mm_active && platform->has_focus && cc->edge_scroll_margin_px > 0) {
        int wx = 0, wy = 0;
        SDL_GetMouseState(&wx, &wy);
        const int edge = cc->edge_scroll_margin_px;
        if (wx >= 0 && wx < edge)                        dx -= (int32_t)scroll_px;
        if (wx >= platform->window_w - edge)             dx += (int32_t)scroll_px;
        if (wy >= 0 && wy < edge)                        dy -= (int32_t)scroll_px;
        if (wy >= platform->window_h - edge)             dy += (int32_t)scroll_px;
    }
    if (dx || dy) {
        int32_t new_x = world->cam_x + dx;
        int32_t new_y = world->cam_y + dy;
        int32_t max_x = world->map_pixels_w - world->viewport_w;
        int32_t max_y = world->map_pixels_h - world->viewport_h;
        if (new_x < 0) new_x = 0; else if (new_x > max_x) new_x = max_x;
        if (new_y < 0) new_y = 0; else if (new_y > max_y) new_y = max_y;
        world->cam_x = new_x;
        world->cam_y = new_y;
    }

    /* Snapshot for next frame's edge-detect on debug hotkeys. */
    memcpy(ig.prev_keys, keys, sizeof(ig.prev_keys));

    return GAMESTATE_IN_GAME;
}

void InGame_Shutdown(void) {
    /* Nothing transient yet. GameWorld teardown is main.c's responsibility
     * via World_End() — that outlives this screen and Phase D's pause
     * menu will want to re-enter InGame without rebuilding the world. */
    DebugPanel_Shutdown();
    if (ig.end_text) {
        HUDText_Free(NULL, ig.end_text);
        ig.end_text = NULL;
    }
    if (ig.end_font) {
        Font_Free(ig.end_font);
        ig.end_font = NULL;
    }
    memset(&ig, 0, sizeof(ig));
}
