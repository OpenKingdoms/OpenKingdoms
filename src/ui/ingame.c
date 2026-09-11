/*
 * ingame.c -- In-game screen (GAMESTATE_IN_GAME).
 *
 * Phase B scope: renders terrain at the GameWorld's current camera
 * position. ESC exits to menu. No simulation, no HUD, no mouse scroll
 * yet (step 9 adds camera input).
 */

#include "tak_ingame.h"
#include "tak_settings.h"
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
#include "tak_command_emit.h"
#include "tak_command_queue.h"
#include "tak_fog.h"
#include "tak_font.h"
#include "tak_hud_text.h"
#include "tak_game_sound.h"
#include "tak_ambient.h"
#include "tak_end_screen.h"
#include "tak_ingame_menu.h"
#include "tak_chat.h"
#include "tak_gui.h"
#include "tak_blit.h"
#include "tak_perf_probe.h"
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
    /* Load kept armed by an order given with Shift held, until Shift
     * is let go (legacy:243768-243771). */
    uint8_t load_shift_hold;
    /* A dialog is up and the clock has stopped. */
    uint8_t paused;
    /* The banner: the label of victorytext.gui / defeattext.gui in
     * its 48 px face, centred over the play area. */
    Font *banner_font;
    char  banner_victory[32];
    char  banner_defeat[32];
} ig;

/* Order-ack voice: legacy Unit_PlayOrderAck (legacy:221247-221281)
 * plays a sound class action on every player-issued order: attack,
 * guard, patrol, Move, select, anything else "default". It is a flat
 * centre-panned play, never positioned. The unit voiced is the first
 * of the selection that takes the order (legacy:238647). */
static void ig_play_order_ack(const GameWorld *world, const char *action) {
    int n = 0;
    (void)Units_GetSelection(&n);
    if (n <= 0 || !world) return;
    const UnitDef *def = Units_GetSelectedDef();
    if (!def || !def->soundcategory[0]) return;
    GameSound_UnitVoice(def->soundcategory, action);
}

/* A box select voices the last unit it took (legacy:237800-237806). */
static void ig_play_box_select_ack(void) {
    int n = 0;
    const int *sel = Units_GetSelection(&n);
    if (n <= 0) return;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    int h = sel[n - 1];
    if (h < 0 || h >= count) return;
    const UnitDef *def = Units_GetDef(units[h].def_idx);
    if (!def || !def->soundcategory[0]) return;
    GameSound_UnitVoice(def->soundcategory, "select");
}

/* The FBI commander flag (legacy:163074), the same test the original's
 * death handler makes (legacy:227174). */
static int unit_is_commander_def(const UnitDef *def) {
    return def && def->commander;
}

static int player_team_id(const GameWorld *world, int player_id) {
    (void)world;
    /* One resolver for every lane: a team-less slot is its own side,
     * numbered past the team range (units.c). */
    return Units_PlayerTeamId(player_id);
}

static int player_slot_active(const GameWorld *world, int player_id) {
    if (!world || player_id < 1 || player_id > TAK_MAX_PLAYERS) return 0;
    return world->cfg.players[player_id - 1].kind != TAK_SLOT_CLOSED;
}

/* Units a player still has on the map. A dying unit counts until its
 * death sequence ends: the original's live count only drops when the
 * record is freed (legacy:227378). An eliminated player counts as none
 * (legacy:227541). */
static int player_units_present(const GameWorld *world, int player_id,
                                const Unit *units, int unit_count) {
    if (world->stats[player_id].eliminated) return 0;
    int n = 0;
    for (int i = 0; i < unit_count; i++) {
        const Unit *u = &units[i];
        if (u->player_id != player_id) continue;
        if (u->alive == UNIT_ALIVE_DEAD) continue;
        n++;
    }
    return n;
}

/* How the local seat reads the verdict (legacy:206655-206662): defeat
 * once it built something and has nothing left (legacy:240018-240028),
 * victory when the battle ends with it standing (legacy:239992-240013).
 * Presentation only, and read once. */
static void InGame_ReadVerdict(GameWorld *world, const int *present) {
    int local = Units_LocalPlayer();
    if (!player_slot_active(world, local)) return;
    if (world->skirmish_local_result != 0) return;
    int result = 0;
    if (present[local] == 0 && world->stats[local].units_built > 0) {
        result = -1;
    } else if (world->skirmish_game_over && present[local] > 0) {
        result = 1;
    }
    if (result == 0) return;
    world->skirmish_local_result = result;
    strncpy(world->skirmish_end_reason, result > 0 ? "Victory" : "Defeat",
            sizeof(world->skirmish_end_reason) - 1);
    fprintf(stderr, "Skirmish: %s for seat %d at tick %d\n",
            world->skirmish_end_reason, local,
            world->skirmish_elapsed_ticks);
}

/* The verdict belongs to the simulation and is the same on every
 * machine. The battle is over when no two seats still standing are
 * enemies, or when no human seat still stands. A seat that resigned
 * counts as gone. The original decided from the local player's record
 * alone, which lockstep cannot allow: one player beaten while the
 * others fight on now sees the defeat and stops nobody's battle. There
 * is no grace period. The 30 s one belongs to the Boneyards branch
 * (legacy:240032-240063). */
static void InGame_EvaluateSkirmishRules(GameWorld *world) {
    if (!world || world->skirmish_game_over) return;
    if (world->mission.objective_count > 0 ||
        world->mission.placement_count > 0) {
        return;
    }
    int built = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        built += world->stats[p].units_built;
    }
    if (built <= 0) return;

    int unit_count = 0;
    const Unit *units = Units_GetActive(&unit_count);
    int present[TAK_MAX_PLAYERS + 1] = { 0 };
    int standing[TAK_MAX_PLAYERS];
    int n_standing = 0, human_standing = 0;
    for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
        present[p] = world->resigned[p]
                   ? 0 : player_units_present(world, p, units, unit_count);
        if (present[p] <= 0) continue;
        /* The stamp the end screen prints as Time (legacy:206617). */
        world->stats[p].last_alive_tick = world->skirmish_elapsed_ticks;
        standing[n_standing++] = p;
        if (world->cfg.players[p - 1].kind == TAK_SLOT_HUMAN) {
            human_standing = 1;
        }
    }
    int split = 0;
    for (int i = 0; i < n_standing && !split; i++) {
        for (int j = i + 1; j < n_standing; j++) {
            if (Units_PlayersAreEnemies(standing[i], standing[j])) {
                split = 1;
                break;
            }
        }
    }
    if (split && human_standing) {
        InGame_ReadVerdict(world, present);
        return;
    }

    world->skirmish_game_over = 1;
    world->skirmish_end_tick = world->skirmish_elapsed_ticks;
    /* One cue for any outcome (legacy:240280). */
    GameSound_PlayUI("Victory Condition");
    world->skirmish_winner_team = (!split && n_standing > 0)
                                ? player_team_id(world, standing[0]) : 0;
    InGame_ReadVerdict(world, present);
    fprintf(stderr, "Skirmish ended: winner_team=%d tick=%d\n",
            world->skirmish_winner_team, world->skirmish_end_tick);
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

/* Banner time before the statistics screen: 90 ticks at the original's
 * 30 Hz (legacy:206566), 180 here. */
#define IG_BANNER_TICKS 180

static void InGame_SimulationStep(GameWorld *world) {
    if (!world || !world->loaded) return;
    /* A dialog is up: the clock stops and the battle holds where it
     * stands (legacy:242962, legacy:242986). */
    if (ig.paused) return;
    /* The battle keeps running under the banner; it stops when the
     * statistics screen opens (legacy:244081). */
    if (world->skirmish_stats_open) return;

    /* Scenario work for --perf-probe, outside the tick measure. */
    PerfProbe_BeforeTick(world);

    /* Orders first: every player action waits in the queue for its
     * tick, and a tick applies them before anything moves. */
    TAK_CmdQueue_Run();

    /* Current prototype sim systems still live in render/ui modules.
     * Keep the fixed-step boundary here until those systems move under
     * src/game/SimulationState. Nothing gameplay-owned should tick from
     * raw frame_dt. */
    double t0 = prof_now_ms();
    TAK_AI_TickSkirmish(world);
    double t1 = prof_now_ms();
    Units_TickEngines();
    Ambient_Tick(world);
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
        uint32_t owners = Units_PlayersWithUnits();
        for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
            /* A closed slot can still own units, as mission armies do,
             * and they pick targets through their side's sight. */
            if (world->cfg.players[p - 1].kind == TAK_SLOT_CLOSED &&
                !(owners & (1u << p))) continue;
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
        if (world->skirmish_game_over && !world->skirmish_stats_open &&
            world->skirmish_elapsed_ticks - world->skirmish_end_tick >= IG_BANNER_TICKS) {
            world->skirmish_stats_open = 1;
        }
    }
    PerfProbe_AfterTick(world, prof_now_ms() - t0);
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
    Ambient_Reset();
    /* Visual Options: Show Damage (legacy:157728), off until set. */
    Units_SetHealthBarsOn(Settings_GetInt("DisplayDamageBars", 0));
    /* Visual Options: Shadows (legacy:197182), on unless turned off. */
    Units_SetShadowsOn(Settings_GetInt("DrawShadows", 1));

    GameWorld *world = World_Get();
    if (!world || !world->loaded) {
        fprintf(stderr, "InGame_Init: no loaded world; bailing to menu\n");
        return -1;
    }
    fprintf(stderr, "InGame_Init: world=%dx%d tiles (%dx%d px), cam=(%d, %d)\n",
            world->tnt.width_tiles, world->tnt.height_tiles,
            world->map_pixels_w, world->map_pixels_h,
            world->cam_x, world->cam_y);

    /* The sidebar belongs to the screen's entry, not to the first frame
     * drawn. The original builds <nameprefix>ingame.gui from the local
     * player's side and loads it right here (legacy:243412-243468,
     * legacy:153034), ahead of the desktop, the bottom bar and the
     * minimap (legacy:153044, legacy:153058, legacy:153061). Choosing it
     * a frame later leaves the previous game's sidebar standing, so a
     * Creon player reads Aramon's. */
    HUD_Init(platform, world);

    /* Non-fatal: if minimap fails to init (no overview image on this
     * map, or GPU upload fails), the screen still runs. */
    (void)Minimap_Init(platform);

    /* Debug overlay (TAK_DEBUG only — release builds get inline no-ops). */
    DebugPanel_Init(platform);

    /* The chat console is created once at load, in every mode, single
     * player included (legacy:243251). A missing font is not fatal: the
     * battle runs, Enter just paints nothing. */
    (void)Chat_Init();
    Chat_Reset();
    Chat_SetLocalPlayer(1, "Player");

    /* The banner dialogs are one label each in font48
     * (legacy:152731, legacy:152762). */
    ig.banner_font = Font_Load("data/fonts/font48", UI_RGBAFormat());
    strncpy(ig.banner_victory, "Victory", sizeof(ig.banner_victory) - 1);
    strncpy(ig.banner_defeat, "Defeat", sizeof(ig.banner_defeat) - 1);
    {
        static const char *const files[2] = {
            "data/guis/victorytext.gui", "data/guis/defeattext.gui"
        };
        for (int k = 0; k < 2; k++) {
            GUIDialog d;
            if (GUIDialog_Load(&d, files[k]) != 0) continue;
            const GUIWidget *label = NULL;
            if (d.root.type == GUI_WT_LABEL && d.root.display_text[0]) {
                label = &d.root;
            }
            for (int i = 0; !label && i < d.num_children; i++) {
                if (d.children[i].display_text[0]) label = &d.children[i];
            }
            if (label) {
                char *dst = k == 0 ? ig.banner_victory : ig.banner_defeat;
                strncpy(dst, label->display_text, sizeof(ig.banner_victory) - 1);
                dst[sizeof(ig.banner_victory) - 1] = '\0';
            }
            GUIDialog_Free(&d);
        }
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

/* The banner: the original parents a 512x72 label dialog to the in-game
 * desktop and centres it over the play area, the screen less the 128 px
 * sidebar and the 48 px bottom strip (legacy:145740-145744). It stays
 * over the running battle until the statistics screen opens. */
static void InGame_DrawSkirmishBanner(const GameWorld *world) {
    if (!world || world->skirmish_stats_open) return;
    if (!world->skirmish_game_over && world->skirmish_local_result >= 0) return;
    if (!ig.banner_font) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    SDL_Rect play = { 0, 0, 512, 432 };
    (void)HUD_GetViewportCanvasRect(&play);
    const char *text = world->skirmish_local_result > 0 ? ig.banner_victory
                     : world->skirmish_local_result < 0 ? ig.banner_defeat
                     : world->skirmish_end_reason;   /* a load-time setup error */
    int tw = Font_MeasureString(ig.banner_font, text);
    int top = 0, bottom = 0;
    if (Font_InkExtent(ig.banner_font, text, &top, &bottom) != 0) return;
    Font_DrawString(ig.banner_font, off,
                    play.x + (play.w - tw) / 2,
                    play.y + (play.h - (bottom - top)) / 2 - top, text);
}

/* A released drag box: with Load armed and one transport selected, a
 * pickup of every unit in it (legacy:238654-238692), else a box select. */
void InGame_WorldDrag(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int shift_held) {
    GameWorld *world = World_Get();
    if (!world || !world->loaded) return;
    if (HUD_GetCommandMode() == HUD_CMD_LOAD &&
        TAK_Cmd_EmitLoadInRect(x0, y0, x1, y1, shift_held) >= 0) {
        if (shift_held) ig.load_shift_hold = 1;
        else HUD_ClearCommandMode();
        return;
    }
    int n = Units_SelectInRect(x0, y0, x1, y1, shift_held);
    /* Shift adds to the selection without a voice (legacy:237803). */
    if (n > 0 && !shift_held) ig_play_box_select_ack();
    fprintf(stderr, "Marquee select: %d units\n", n);
}

/* The cursor the world shows under a point with no command armed
 * (legacy manual §IV.2): resume-build over your own frame with a
 * builder selected, attack over an enemy with your units selected, the
 * select hand over any other unit, revive over a body the selection can
 * raise, and the pointer otherwise. */
int InGame_HoverCursorAt(int32_t world_x, int32_t world_y) {
    int hover = Units_PickAt(world_x, world_y, 48);
    if (hover >= 0) {
        if (g_units_get_player(hover) == Units_LocalPlayer() &&
            Units_IsUnderConstruction(hover) &&
            Units_SelectionHasBuilder())
            return HUD_CMD_HEAL;   /* resume-build cursor */
        if (g_units_get_player(hover) != Units_LocalPlayer() &&
            Units_SelectionOwnedCount() > 0)
            return HUD_CMD_ATTACK;
        return HUD_CUR_SELECT;
    }
    if (Units_SelectionRaiseModeAt(world_x, world_y) >= 0)
        return HUD_CUR_REVIVE;
    return HUD_CUR_NORMAL;
}

/* The cursor an armed command shows: the sweep cursor turns to revive
 * over a body the selection would raise instead of sweep. */
int InGame_CommandCursorAt(int mode, int32_t world_x, int32_t world_y) {
    if (mode == HUD_CMD_CLEAR &&
        Units_SelectionRaiseModeAt(world_x, world_y) >= 0)
        return HUD_CUR_REVIVE;
    return mode;
}

/* One left click on the game world, in world coordinates. The tick
 * calls this on release and tests call it directly, so the pending
 * command, the pick and the order ack all resolve in one place
 * (manual section IV.2: a pending order executes, else a friendly is
 * selected, an enemy attacked, and bare ground is a Move). */
/* Ctrl+digit files the selection as a squad, a bare digit recalls it,
 * each with its own cue (legacy:122211, legacy:122226). */
static void ig_control_group(int d, int assign) {
    if (assign) {
        Units_AssignControlGroup(d);
        GameSound_PlayUI("CreateSquad");
        fprintf(stderr, "Control group %d assigned\n", d);
    } else {
        int n = Units_RecallControlGroup(d);
        GameSound_PlayUI("SelectSquad");
        fprintf(stderr, "Control group %d recalled (%d units)\n", d, n);
    }
}

void InGame_DebugControlGroup(int digit, int assign) {
    if (digit < 0 || digit > 9) return;
    ig_control_group(digit, assign);
}

/* Cancel: an armed command goes first and the selection survives
 * (legacy:242517-242525), and with nothing armed the selection is
 * cleared instead (legacy:237360-237385). Escape and the right click
 * are the same act (legacy:242914-242921). */
static void ig_cancel(void) {
    if (HUD_GetCommandMode() != HUD_CMD_NONE) {
        HUD_ClearCommandMode();
    } else {
        Units_SelectSingle(-1);
    }
}

static void ig_battle_keys(int has_focus, const GameWorld *world,
                          const Uint8 *keys);

/* The direction the held keys ask the camera to move, one step an axis.
 * The tick scales it by the frame's scroll distance. */
static void ig_scroll_dir(const Uint8 *keys, int *out_dx, int *out_dy) {
    int dx = 0, dy = 0;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) dx -= 1;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) dx += 1;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) dy -= 1;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) dy += 1;
    *out_dx = dx;
    *out_dy = dy;
}

/* Move the camera and keep it on the map. */
static void ig_move_camera(GameWorld *world, int32_t dx, int32_t dy) {
    if (!world || (!dx && !dy)) return;
    int32_t new_x = world->cam_x + dx;
    int32_t new_y = world->cam_y + dy;
    int32_t max_x = world->map_pixels_w - world->viewport_w;
    int32_t max_y = world->map_pixels_h - world->viewport_h;
    if (new_x < 0) new_x = 0; else if (new_x > max_x) new_x = max_x;
    if (new_y < 0) new_y = 0; else if (new_y > max_y) new_y = max_y;
    world->cam_x = new_x;
    world->cam_y = new_y;
}

/* The chat console's claim on one frame of keyboard. Enter opens the
 * line, Escape throws it away and Enter sends it (legacy:242892-242913,
 * legacy:154417-154425). Returns 1 while the console is open, which is
 * every key belonging to it and none reaching the battle.
 *
 * The console never pauses and never takes the frame. The battle keeps
 * running and keeps drawing behind it, because stopping the clock on a
 * chat line would desync a networked match. */
static int ig_console_keys(int has_focus, const Uint8 *keys,
                           const char *text_in) {
#define IG_HIT(sc) (keys[sc] && !ig.prev_keys[sc])
    if (!has_focus) return Chat_IsOpen();
    int alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    int enter = IG_HIT(SDL_SCANCODE_RETURN) || IG_HIT(SDL_SCANCODE_KP_ENTER);

    if (!Chat_IsOpen()) {
        /* Alt+Enter is the fullscreen toggle, not a chat line. */
        if (enter && !alt) {
            Chat_Open();
            return 1;
        }
        return 0;
    }

    if (text_in && text_in[0]) Chat_TypeText(text_in);
    if (IG_HIT(SDL_SCANCODE_BACKSPACE)) Chat_Backspace();
    if (IG_HIT(SDL_SCANCODE_DELETE))    Chat_Delete();
    if (IG_HIT(SDL_SCANCODE_LEFT))      Chat_CaretLeft();
    if (IG_HIT(SDL_SCANCODE_RIGHT))     Chat_CaretRight();
    if (IG_HIT(SDL_SCANCODE_HOME))      Chat_CaretHome();
    if (IG_HIT(SDL_SCANCODE_END))       Chat_CaretEnd();
    if (IG_HIT(SDL_SCANCODE_ESCAPE))    Chat_Cancel();
    else if (enter)                     (void)Chat_Submit(SDL_GetTicks());
    return 1;
#undef IG_HIT
}

void InGame_SetPaused(int paused) { ig.paused = (uint8_t)(paused != 0); }
int  InGame_IsPaused(void) { return ig.paused; }

void InGame_DebugToggleMenu(void) {
    if (InGameMenu_IsOpen()) InGameMenu_Close();
    else (void)InGameMenu_Open();
}

void InGame_DebugEscape(int down) {
    /* An open chat console takes Escape first and the battle never sees
     * it, the same order the tick runs them in. */
    if (down && !ig.prev_keys[SDL_SCANCODE_ESCAPE]) {
        if (Chat_IsOpen()) Chat_Cancel();
        else ig_cancel();
    }
    ig.prev_keys[SDL_SCANCODE_ESCAPE] = (uint8_t)(down != 0);
}

/* Test seam: one frame of keyboard, given the one key that is down and
 * the characters the platform collected this frame. It runs the same
 * console gate and the same battle hotkeys the tick runs, so a test can
 * prove a letter typed into the console never reaches the hotkey that
 * shares it. SDL_GetKeyboardState cannot be driven from a test, which
 * is why this exists. */
void InGame_DebugKeyFrame(int scancode, const char *text_in) {
    static uint8_t frame_keys[SDL_NUM_SCANCODES];
    memset(frame_keys, 0, sizeof(frame_keys));
    if (scancode > 0 && scancode < SDL_NUM_SCANCODES) {
        frame_keys[scancode] = 1;
    }
    int console_keys = ig_console_keys(1, frame_keys, text_in);
    GameWorld *world = World_Get();
    if (!console_keys && world) {
        ig_battle_keys(1, world, frame_keys);
        /* The camera step is the seam's own, a fixed sixteen pixels,
         * because a test has no frame time to scale by. */
        int sx = 0, sy = 0;
        ig_scroll_dir(frame_keys, &sx, &sy);
        ig_move_camera(world, sx * 16, sy * 16);
    }
    memcpy(ig.prev_keys, frame_keys, sizeof(ig.prev_keys));
}

void InGame_WorldClick(int32_t world_x, int32_t world_y, int shift_held) {
    GameWorld *world = World_Get();
    if (!world || !world->loaded) return;
    int cmd = HUD_GetCommandMode();
    int hit = Units_PickAt(world_x, world_y, 48);
    int n_sel = 0;
    Units_GetSelection(&n_sel);
    if (HUD_IsTargetingMode(cmd)) {
        /* Pending targeting command: world-click executes it
         * and clears the mode. Mirrors legacy
         * Selection_IssueAttackOrder dispatch which routes by
         * the registered hotkey + click target. */
        switch (cmd) {
            case HUD_CMD_MOVE:
                TAK_Cmd_EmitSelection(TAK_CMD_MOVE, world_x, world_y,
                                      -1, 0, 0);
                break;
            case HUD_CMD_PATROL:
                TAK_Cmd_EmitSelection(TAK_CMD_PATROL, world_x, world_y,
                                      -1, 0, 0);
                break;
            case HUD_CMD_ATTACK:
                if (hit >= 0)
                    TAK_Cmd_EmitSelection(TAK_CMD_ATTACK, world_x, world_y,
                                          hit, 0, 0);
                else
                    TAK_Cmd_EmitSelection(TAK_CMD_ATTACK_GROUND,
                                          world_x, world_y, -1, 0, 0);
                break;
            case HUD_CMD_HEAL:
                /* Heal and load reach only your own units. */
                if (hit >= 0 && g_units_get_player(hit) == Units_LocalPlayer())
                    TAK_Cmd_EmitSelection(TAK_CMD_REPAIR, world_x, world_y,
                                          hit, 0, 0);
                break;
            case HUD_CMD_LOAD:
                if (hit >= 0 && g_units_get_player(hit) == Units_LocalPlayer())
                    TAK_Cmd_EmitSelection(TAK_CMD_LOAD, world_x, world_y,
                                          hit, 0, shift_held ? 1u : 0u);
                break;
            case HUD_CMD_CLEAR:
                /* Sweep cursor: legacy's CLEAR order resolves on
                 * the map cell, so a tree/rock/rubble under the
                 * click is the target and a live unit is not
                 * (legacy:187127-187207). The executor tries the
                 * cell first and falls back to the unit on the
                 * tick. */
                TAK_Cmd_EmitSelection(TAK_CMD_RECLAIM_FEATURE,
                                      world_x, world_y, hit, 0, 0);
                break;
            case HUD_CMD_GUARD:
                if (hit >= 0 && g_units_get_player(hit) == Units_LocalPlayer())
                    TAK_Cmd_EmitSelection(TAK_CMD_GUARD, world_x, world_y,
                                          hit, 0, 0);
                break;
            case HUD_CMD_UNLOAD:
                TAK_Cmd_EmitSelection(TAK_CMD_UNLOAD, world_x, world_y,
                                      -1, 0, 0);
                break;
            case HUD_CMD_W_SPECIAL:
                /* Special-weapon shot: slot 2, then an attack at
                 * the click target or a walk to the spot. */
                TAK_Cmd_EmitSelection(TAK_CMD_SPECIAL_WEAPON,
                                      world_x, world_y, hit, 0, 0);
                break;
            case HUD_CMD_PLACE_BUILD: {
                /* Building placement: spawn the building at
                 * 1 HP and issue the selected builder a BUILD
                 * order. The TickCombat MOVING handler walks
                 * the builder to the site and adds HP per
                 * tick (1.0/workertime of max, legacy:162905). */
                int bdef = HUD_GetBuildPlacementDefIdx();
                if (bdef >= 0) {
                    /* Same cell snap the ghost drew at, so the
                     * building lands where the preview was
                     * (legacy:184168). */
                    int32_t bx = world_x, by = world_y;
                    Units_SnapBuildSite(bdef, &bx, &by);
                    if (TAK_Cmd_EmitSelection(TAK_CMD_BUILD, bx, by, -1,
                                              (uint16_t)bdef, 0) == 0) {
                        /* The click is answered now
                         * (legacy:243684-243688). Whether the site
                         * takes the building is settled on the tick
                         * that runs the order. */
                        GameSound_PlayUI("oktobuild");
                        fprintf(stderr, "Build: ordered def=%d at (%d,%d)\n",
                                bdef, bx, by);
                    } else {
                        GameSound_PlayUI("notoktobuild");
                        fprintf(stderr, "Build: no unit of yours to build it\n");
                    }
                }
                break;
            }
        }
        if (cmd != HUD_CMD_PLACE_BUILD) {
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
        /* With Shift held Load stays armed for the next pickup
         * (legacy:243644-243646). */
        if (cmd == HUD_CMD_LOAD && shift_held) ig.load_shift_hold = 1;
        else HUD_ClearCommandMode();
    } else if (hit >= 0 && g_units_get_player(hit) == Units_LocalPlayer() &&
               Units_IsUnderConstruction(hit) &&
               Units_SelectionHasBuilder() && !shift_held) {
        /* Builder + nanoframe click = resume (legacy HelpBuild). */
        TAK_Cmd_EmitSelection(TAK_CMD_REPAIR, world_x, world_y, hit, 0, 0);
        ig_play_order_ack(world, "default");
    } else if (hit >= 0 && g_units_get_player(hit) == Units_LocalPlayer()) {
        /* Friendly unit click: replace selection; shift-click
         * toggles the unit in/out of the selection. */
        /* A plain click voices the unit, a shift toggle does
         * not (legacy:237940-237950). */
        if (shift_held) {
            Units_SelectToggle(hit);
        } else {
            Units_SelectSingle(hit);
            ig_play_order_ack(world, "select");
        }
        fprintf(stderr, "Selected unit %d\n", hit);
    } else if (hit >= 0 && Units_SelectionOwnedCount() == 0) {
        /* Nothing of yours is selected, so a click on any unit
         * inspects it: the sidebar shows its portrait, name and
         * health. Orders all check ownership, so a unit that is
         * not yours takes none. */
        Units_SelectForInspect(hit);
    } else if (Units_SelectionOwnedCount() > 0) {
        if (hit >= 0) {
            TAK_Cmd_EmitSelection(TAK_CMD_ATTACK, world_x, world_y, hit, 0, 0);
            ig_play_order_ack(world, "attack");
            fprintf(stderr, "Attack -> unit %d\n", hit);
        } else if (Units_SelectionRaiseModeAt(world_x, world_y) >= 0 &&
                   Units_CommandResurrectFeatureSelected(world_x,
                                                         world_y) > 0) {
            /* A click on a body the selection can raise raises it. */
            ig_play_order_ack(world, "default");
            fprintf(stderr, "Raise -> (%d,%d)\n", world_x, world_y);
        } else {
            TAK_Cmd_EmitSelection(TAK_CMD_MOVE, world_x, world_y, -1, 0, 0);
            ig_play_order_ack(world, "Move");
            fprintf(stderr, "Move -> (%d,%d)\n",
                    world_x, world_y);
        }
    }
}

/* The battle hotkeys, as one frame of keyboard sees them. The chat
 * console gets first refusal: while it is open it owns every key,
 * so a player typing "attack" issues no orders and moves no camera
 * (legacy:242892-242913). */
static void ig_battle_keys(int has_focus, const GameWorld *world,
                          const Uint8 *keys) {
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
    /* Escape cancels. It never opens a menu, never pauses and never
     * quits (legacy:242914-242921), and a dialog that is up takes it
     * first (legacy:243003-243004). */
    if (has_focus && IG_PRESSED(SDL_SCANCODE_ESCAPE)) ig_cancel();
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
        Settings_SetInt("DisplayDamageBars", Units_GetHealthBarsOn());
        Settings_Save();
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
                ig_control_group(d, ctrl);
            }
        }
    }
#undef IG_PRESSED
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

    int sim_ticks = 0;
    while (Timer_ConsumeTick(timer)) {
        InGame_SimulationStep(world);
        sim_ticks++;
    }
    PerfProbe_FrameTicks(sim_ticks, timer->max_ticks_per_frame);

    /* The statistics screen replaces the battle view and owns the
     * input until one of its buttons leaves (legacy:244079-244086).
     * main.c releases the world on the way out. */
    if (world->skirmish_stats_open) {
        if (!EndScreen_IsOpen() && EndScreen_Open(platform, world) != 0) {
            return GAMESTATE_MENU;
        }
        if (off) SDL_FillRect(off, NULL, SDL_MapRGBA(off->format, 0, 0, 0, 255));
        int next = EndScreen_Tick(platform, world);
        SDL_ShowCursor(SDL_ENABLE);
        UI_Present(platform);
        memcpy(ig.prev_keys, SDL_GetKeyboardState(NULL), sizeof(ig.prev_keys));
        return next;
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
    InGame_DrawSkirmishBanner(world);

    /* Chat. The block sits in the top left of the whole screen and the
     * edit box over the bottom strip (legacy:205958-206051,
     * legacy:154381-154391). Both go in before the F1 block, so the
     * menu paints over them the way the original's rule that Enter does
     * nothing under another dialog implies. Expiry runs off the frame
     * clock, never off a simulation tick. */
    {
        SDL_Surface *chat_off = UI_Offscreen();
        Chat_Expire(SDL_GetTicks());
        Chat_Draw(chat_off);
        Chat_DrawInput(chat_off);
    }

    /* F1 opens the in game menu (keys.tdf:169 binds F1 to F2Menu,
     * loader legacy:122746-122785). While it is up it owns the frame:
     * the battle stays drawn behind it and every key and click goes to
     * the dialog (legacy:154694-154752). */
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int console_keys = ig_console_keys(platform->has_focus, keys,
                                       platform->text_in);
    if (platform->has_focus && !console_keys && keys[SDL_SCANCODE_F1] &&
        !ig.prev_keys[SDL_SCANCODE_F1] && !InGameMenu_IsOpen()) {
        (void)InGameMenu_Open();
    }
    if (InGameMenu_IsOpen()) {
        int next = InGameMenu_Tick(platform);
        SDL_ShowCursor(SDL_ENABLE);
        DebugPanel_TickFPS((float)Timer_GetFrameDT(timer));
        DebugPanel_Draw();
        UI_Present(platform);
        memcpy(ig.prev_keys, keys, sizeof(ig.prev_keys));
        return next;
    }

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
            int mode = HUD_GetCommandMode();
            int cid = InGame_CommandCursorAt(mode, world->cam_x + mx,
                                             world->cam_y + my);
            if (cid != mode)
                drew_cursor = HUD_DrawCursorById(platform, cid, mx, my);
            if (!drew_cursor) {
                HUD_DrawCommandCursor(platform, mx, my);
                drew_cursor = 1;
            }
        } else if (over_world) {
            int cur_id = InGame_HoverCursorAt(world->cam_x + mx,
                                              world->cam_y + my);
            drew_cursor = HUD_DrawCursorById(platform, cur_id, mx, my);
        }
        SDL_ShowCursor(drew_cursor ? SDL_DISABLE : SDL_ENABLE);
    }

    /* Debug overlay paints onto UI canvas before UI_Present uploads. */
    DebugPanel_TickFPS((float)Timer_GetFrameDT(timer));
    DebugPanel_Draw();

    UI_Present(platform);

    /* The chat console owns the keyboard while it is open, so the
     * battle hotkeys only run when it is shut. */
    if (!console_keys) ig_battle_keys(platform->has_focus, world, keys);


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
    if (!console_keys) {
        int sx = 0, sy = 0;
        ig_scroll_dir(keys, &sx, &sy);
        dx += (int32_t)(sx * scroll_px);
        dy += (int32_t)(sy * scroll_px);
    }

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
    if (ig.load_shift_hold && !shift_held) {
        ig.load_shift_hold = 0;
        if (HUD_GetCommandMode() == HUD_CMD_LOAD) HUD_ClearCommandMode();
    }

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
            InGame_WorldDrag(ig.drag_world_x, ig.drag_world_y,
                             world_click_x, world_click_y, shift_held);
            ig.drag_tracking = 0;
            ig.drag_active   = 0;
        } else if (left_released && ig.drag_tracking) {
            ig.drag_tracking = 0;
            InGame_WorldClick(world_click_x, world_click_y, shift_held);
        }
        if (right_pressed) ig_cancel();
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
            /* An inspected foreign unit takes no Move: the minimap
             * jumps the camera as with nothing selected. */
            if (Units_SelectionOwnedCount() > 0) {
                if (left_pressed) {
                    TAK_Cmd_EmitSelection(TAK_CMD_MOVE,
                                          cam_x + world->viewport_w / 2,
                                          cam_y + world->viewport_h / 2,
                                          -1, 0, 0);
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
    ig_move_camera(world, dx, dy);

    /* Snapshot for next frame's edge-detect on debug hotkeys. */
    memcpy(ig.prev_keys, keys, sizeof(ig.prev_keys));

    return GAMESTATE_IN_GAME;
}

void InGame_Shutdown(void) {
    Ambient_Reset();
    /* Nothing transient yet. GameWorld teardown is main.c's responsibility
     * via World_End() — that outlives this screen and Phase D's pause
     * menu will want to re-enter InGame without rebuilding the world. */
    DebugPanel_Shutdown();
    EndScreen_Close();
    InGameMenu_Close();
    Chat_Shutdown();
    if (ig.banner_font) {
        Font_Free(ig.banner_font);
        ig.banner_font = NULL;
    }
    memset(&ig, 0, sizeof(ig));
}
