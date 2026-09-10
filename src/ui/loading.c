/*
 * loading.c -- Loading screen (GAMESTATE_GAME_LOADING).
 *
 * Renders loadscreen.gui as the backdrop + a simple progress bar drawn
 * as a filled rect overlay. The engine lane calls Loading_SetProgress
 * as assets load. When progress reaches 1.0, Tick returns GAMESTATE_IN_GAME.
 */

#include "tak_loading.h"
#include "tak_gameloop.h"
#include "tak_gui.h"
#include "tak_terrain.h"
#include "tak_unit.h"
#include "tak_features.h"
#include "tak_tex_atlas.h"
#include "tak_gui_render.h"
#include "tak_blit.h"
#include "tak_bink.h"
#include "tak_ui.h"
#include "tak_world.h"
#include "tak_tdf.h"
#include "tak_palette.h"
#include "tak_tnt.h"
#include "tak_memory.h"
#include "tak_mission.h"
#include "tak_fog.h"
#include "tak_moveinfo.h"
#include "tak_util.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

/* Asset-load state machine. Loading_Tick advances one step per frame
 * so the progress bar + Bink remain responsive. Each step is a stub
 * until step 6 wires in real work (TDF parse, TNT load, atlas build,
 * start position extraction, camera centering). */
typedef enum {
    LS_PARSE_OTA = 0,
    LS_LOAD_PALETTE,      /* must precede LS_LOAD_TNT — TNT_Load needs
                           * the faction's RGBA table to decode the minimap */
    LS_LOAD_TNT,
    LS_LOAD_CHUNKS,
    LS_LOAD_TEXTURES,     /* unit-mesh texture atlases (tex_atlas.c)        */
    LS_LOAD_UNITS,        /* parse every units/*.fbi into the registry      */
    LS_INIT_WORLD,
    LS_POSITION_CAMERA,
    LS_FINALIZE,
    LS_DONE
} LoadingStep;

static struct {
    int          initialized;
    int          has_dialog;       /* loadscreen.gui may or may not exist */
    GUIDialog    dialog;
    GUIRuntime  *rt;
    float        progress;         /* 0.0..1.0 */
    char         status[96];
    int          held_one_frame;   /* wait one frame after 100% before transition */
    LoadingStep  step;             /* current phase of the asset loader */

    /* Background Bink video, played over the AnimatedControl widget.
     * NULL when the file is missing or FFmpeg is not in the build, as
     * in the browser, and then the stained glass the dialog authors
     * behind the clip is what the arch shows. */
    BinkPlayer  *bg_bink;
    double       bink_timer;       /* accumulator for native-rate playback */
    SDL_Rect     bink_rect;        /* AnimatedControl widget rect          */
    int          next_chunk;       // Next terrain chunk idx to load
} ld;

static int load_side_water_height(const char *kingdom) {
    if (!kingdom || !kingdom[0]) return 0;
    const char *paths[] = {
        "data/gamedata/sidedata.tdf",
        "gamedata/sidedata.tdf"
    };
    for (int p = 0; p < 2; p++) {
        TDFFile *tdf = TDF_Open(paths[p]);
        if (!tdf || TDF_Load(tdf) != 0) {
            if (tdf) TDF_Close(tdf);
            continue;
        }
        for (int i = 0; i < 8; i++) {
            char section[16];
            snprintf(section, sizeof(section), "SIDE%d", i);
            if (TDF_PushSection(tdf, section) != 0) continue;
            const char *name = TDF_ReadString(tdf, "name", "");
            if (name && tak_stricmp(name, kingdom) == 0) {
                int water_height = TDF_ReadInt(tdf, "waterheight", 0);
                TDF_Close(tdf);
                return water_height;
            }
            TDF_PopSection(tdf);
        }
        TDF_Close(tdf);
    }
    return 0;
}

void Loading_SetProgress(float f) {
    if (f < 0.f) f = 0.f;
    if (f > 1.f) f = 1.f;
    ld.progress = f;
}

void Loading_SetStatus(const char *s) {
    if (!s) { ld.status[0] = '\0'; return; }
    strncpy(ld.status, s, sizeof(ld.status) - 1);
    ld.status[sizeof(ld.status) - 1] = '\0';
}

int Loading_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&ld, 0, sizeof(ld));

    /* loadscreen.gui is optional — if missing we still render a
     * flat-color backdrop so the user sees *something*. */
    if (GUIDialog_Load(&ld.dialog, "data/guis/loadscreen.gui") == 0) {
        ld.rt = GUIRuntime_Create(&ld.dialog);
        ld.has_dialog = (ld.rt != NULL);
    } else {
        fprintf(stderr, "Loading: loadscreen.gui not available; using plain backdrop\n");
    }

    /* The original starts from the dialog as authored and then clears
     * the placeholder text and hides the per-player rows before the
     * screen is ever drawn (legacy:158031). Without this the arch is
     * ringed with "Static0" and "100%" that the original never shows. */
    if (ld.rt) {
        for (int i = 0; i < 8; i++) {
            char row[24];
            snprintf(row, sizeof(row), "PlayerName%d", i);
            GUIRuntime_SetWidgetVisible(ld.rt, row, 0);
            snprintf(row, sizeof(row), "PlayerPercent%d", i);
            GUIRuntime_SetWidgetVisible(ld.rt, row, 0);
            snprintf(row, sizeof(row), "PlayerProgress%d", i);
            GUIRuntime_SetWidgetVisible(ld.rt, row, 0);
        }
        const GameWorld *world = World_Get();
        GUIRuntime_SetWidgetText(ld.rt, "LoadMap",
                                 world ? world->map_name : "");
        GUIRuntime_SetWidgetText(ld.rt, "LoadText", "");
        GUIRuntime_SetWidgetText(ld.rt, "Percent", "0%");
    }

    /* The clip plays over the AnimatedControl widget, at its top-left
     * and at its own size (legacy:158297). The stained glass authored
     * behind it stays on screen when the clip cannot open. */
    const GUIWidget *anim = ld.rt
        ? GUIRuntime_WidgetByName(ld.rt, "AnimatedControl")
        : NULL;
    ld.bink_rect = anim ? anim->rect : (SDL_Rect){ 168, 46, 423, 351 };

    /* Open Movies/Gui/Loadscreen.bik. Direct fopen path (not VFS) because
     * the Bink player streams via FFmpeg which wants a real file handle.
     * Same pattern main_menu.c uses for the hover clips. */
    char bik_path[512];
    snprintf(bik_path, sizeof(bik_path),
             "%s/Movies/Gui/Loadscreen.bik", TAK_GAME_DIR);
    ld.bg_bink = BinkPlayer_Open(bik_path);
    if (!ld.bg_bink) {
        /* Uppercase-extension fallback (some installs ship .BIK). */
        snprintf(bik_path, sizeof(bik_path),
                 "%s/Movies/Gui/Loadscreen.BIK", TAK_GAME_DIR);
        ld.bg_bink = BinkPlayer_Open(bik_path);
    }
    if (ld.bg_bink) {
        fprintf(stderr, "Loading: Loadscreen.bik opened (%dx%d, %.3fs/frame)\n",
                BinkPlayer_GetWidth(ld.bg_bink),
                BinkPlayer_GetHeight(ld.bg_bink),
                BinkPlayer_GetFrameDuration(ld.bg_bink));
        /* Decode the first frame right away so the first rendered
         * Loading_Tick has something to blit instead of a black arch. */
        BinkPlayer_NextFrame(ld.bg_bink);
    } else {
        fprintf(stderr, "Loading: no Loadscreen.bik (%s) — backdrop will be plain\n",
                bik_path);
    }

    strncpy(ld.status, "Loading...", sizeof(ld.status) - 1);
    ld.initialized = 1;
    return 0;
}

struct GUIRuntime *Loading_Runtime(void) {
    return ld.initialized ? ld.rt : NULL;
}

void Loading_Shutdown(void) {
    if (!ld.initialized) return;
    if (ld.bg_bink)     BinkPlayer_Close(ld.bg_bink);
    if (ld.rt)          GUIRuntime_Destroy(ld.rt);
    if (ld.has_dialog)  GUIDialog_Free(&ld.dialog);
    memset(&ld, 0, sizeof(ld));
}

static void fill_rect(SDL_Surface *s, SDL_Rect r, uint32_t rgba) {
    SDL_FillRect(s, &r, rgba);
}

/* Integer clamp helper for bounds checks (used by task 7b). */
static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void apply_initial_mission_commands(int handle,
                                           const MissionPlacement *placement) {
    if (handle < 0 || !placement) return;
    for (int i = 0; i < placement->command_count; i++) {
        const MissionCommand *cmd = &placement->commands[i];
        switch (cmd->type) {
        case MISSION_CMD_MOVE:
        case MISSION_CMD_UNLOAD:
            Units_CommandMoveUnit(handle, cmd->a * 16, cmd->b * 16);
            break;
        case MISSION_CMD_PATROL:
            /* Standing order, not a one-shot move. Multi-waypoint TDF
             * patrols degrade to a last-leg bounce until orders queue. */
            Units_CommandPatrolUnit(handle, cmd->a * 16, cmd->b * 16);
            break;
        case MISSION_CMD_OWNER:
            Units_SetOwner(handle, cmd->a, cmd->a > 0 ? (cmd->a - 1) % 12 : 0);
            break;
        case MISSION_CMD_SPEED:
            Units_SetVelocity(handle, (int32_t)(cmd->value * 65536.0f));
            break;
        case MISSION_CMD_BUILD: {
            int build_def = Units_FindDefByName(cmd->text);
            if (build_def >= 0) {
                Units_BeginBuildingForUnit(handle, build_def,
                                           cmd->b * 16, cmd->c * 16);
            }
            break;
        }
        default:
            break;
        }
    }
}

static int find_mission_target(const GameWorld *world,
                               const int *handles,
                               const MissionPlacement *self,
                               const MissionCommand *cmd) {
    int best = -1;
    int64_t best_d2 = 0;
    if (!world || !handles || !self || !cmd) return -1;

    for (int i = 0; i < world->mission.placement_count; i++) {
        const MissionPlacement *p = &world->mission.placements[i];
        int h = handles[i];
        if (h < 0 || p == self) continue;

        int matches = 0;
        if (cmd->text[0]) {
            if (p->ident[0] && tak_stricmp(p->ident, cmd->text) == 0) matches = 1;
            if (p->unitname[0] && tak_stricmp(p->unitname, cmd->text) == 0) matches = 1;
        } else if (cmd->a >= 0 && cmd->b >= 0) {
            int32_t tx = cmd->a * 16;
            int32_t ty = cmd->b * 16;
            int64_t dx = (int64_t)(p->x * 16 - tx);
            int64_t dy = (int64_t)(p->z * 16 - ty);
            matches = (dx * dx + dy * dy) <= (int64_t)(32 * 32);
        }
        if (!matches) continue;

        int64_t dx = (int64_t)(p->x - self->x);
        int64_t dz = (int64_t)(p->z - self->z);
        int64_t d2 = dx * dx + dz * dz;
        if (best < 0 || d2 < best_d2) {
            best = h;
            best_d2 = d2;
        }
    }
    return best;
}

static void apply_initial_attack_commands(const GameWorld *world,
                                          const int *handles) {
    if (!world || !handles) return;
    for (int i = 0; i < world->mission.placement_count; i++) {
        const MissionPlacement *p = &world->mission.placements[i];
        int h = handles[i];
        if (h < 0) continue;
        for (int k = 0; k < p->command_count; k++) {
            const MissionCommand *cmd = &p->commands[k];
            if (cmd->type != MISSION_CMD_ATTACK) continue;
            int target = find_mission_target(world, handles, p, cmd);
            if (target >= 0) {
                Units_CommandAttackUnitScript(h, target);
            } else if (!cmd->text[0]) {
                Units_CommandMoveUnit(h, cmd->a * 16, cmd->b * 16);
            }
        }
    }
}

/* Advance the loader one phase. Called once per Loading_Tick so each
 * phase is rendered between before the next one starts — the progress
 * bar and Bink both stay smooth. Dependency order: palette must build
 * before TNT_Load (TNT needs the RGBA table for minimap decode), TNT
 * before chunk upload, everything before the world/camera init.
 * platform is threaded through for LS_LOAD_CHUNKS — it needs the GPU
 * renderer to upload textures. */
static void loading_advance_step(TAK_Platform *platform) {
    switch (ld.step) {

    case LS_PARSE_OTA: {
        Loading_SetStatus("Loading map info...");
        Loading_SetProgress(0.10f);

        GameWorld *world = World_Get();
        if (!world) { ld.step = LS_DONE; break; }

        /* Try skirmish and campaign paths — the VFS is case-insensitive
         * so .ota / .OTA both resolve. */
        char ota_path[256];
        int is_campaign_ota = 0;
        snprintf(ota_path, sizeof(ota_path),
                 "maps/Maps/%s.ota", world->map_name);
        TDFFile *tdf = TDF_Open(ota_path);
        if (!tdf) {
            snprintf(ota_path, sizeof(ota_path),
                     "missions/missions/%s.ota", world->map_name);
            tdf = TDF_Open(ota_path);
            is_campaign_ota = (tdf != NULL);
        }
        if (!tdf || TDF_Load(tdf) != 0) {
            fprintf(stderr, "LS_PARSE_OTA: could not open/parse %s.ota\n",
                    world->map_name);
            if (tdf) TDF_Close(tdf);
            ld.step = LS_LOAD_PALETTE;
            break;
        }

        /* Navigate GlobalHeader -> Map Data -> specials -> specialN.
         * API quirks:
         *  - TDF_PushSection returns 0 on success, -1 on failure. Must
         *    be compared `== 0`; a naked `if (Push...)` inverts logic.
         *  - TDF_OpenSection(idx) mutates current_section to the child,
         *    so sibling enumeration with it is broken. Use the iterator
         *    API (GetFirstSection / GetNextSection) instead — it walks
         *    a separate iter_section cursor that survives Push/Pop. */
        world->num_start_positions = 0;
        if (TDF_PushSection(tdf, "GlobalHeader") == 0 &&
            TDF_PushSection(tdf, "Map Data")     == 0 &&
            TDF_PushSection(tdf, "specials")     == 0) {
            const char *name = TDF_GetFirstSection(tdf);
            while (name && world->num_start_positions < TAK_MAX_PLAYERS) {
                if (TDF_PushSection(tdf, name) == 0) {
                    const char *what = TDF_ReadString(tdf, "specialwhat", "");
                    if (what && strncmp(what, "StartPos", 8) == 0 &&
                        what[8] >= '1' && what[8] <= '9') {
                        StartPos *sp = &world->start_positions[world->num_start_positions++];
                        sp->player = what[8] - '0';
                        sp->x      = TDF_ReadInt(tdf, "XPos", 0);
                        sp->z      = TDF_ReadInt(tdf, "ZPos", 0);
                    }
                    TDF_PopSection(tdf);
                }
                name = TDF_GetNextSection(tdf);
            }
        }
        /* PushSection stack unwinds with TDF_Close; no need to pop each
         * level individually as long as we Close next. */
        TDF_Close(tdf);

        Mission_Free(&world->mission);
        if (is_campaign_ota && Mission_LoadOTA(ota_path, &world->mission) == 0) {
            fprintf(stderr,
                    "LS_PARSE_OTA: campaign mission placements parsed: %d\n",
                    world->mission.placement_count);
        }

        fprintf(stderr, "LS_PARSE_OTA: %d start positions parsed\n",
                world->num_start_positions);
        ld.step = LS_LOAD_PALETTE;
        break;
    }

    case LS_LOAD_PALETTE: {
        Loading_SetStatus("Building palette...");
        Loading_SetProgress(0.25f);

        GameWorld *world = World_Get();
        if (!world) { ld.step = LS_DONE; break; }

        /* Build the global feature registry up front so the TNT load
         * (next phase) can resolve feature_layer IDs to FeatureDefs.
         * Walks data/features/<world>/*.tdf alphabetically — matches
         * legacy load order so TNT IDs and our indices align. */
        Features_LoadAll();

        Palette pal;
        int ok = 0;
        if (world->map_kingdom[0]) {
            char pal_path[128];
            snprintf(pal_path, sizeof(pal_path),
                     "data/palettes/%s.pcx", world->map_kingdom);
            ok = (Palette_LoadPCX(&pal, pal_path) == 0);
        }
        if (!ok) {
            /* Fallback: generic gameart palette — minimap was authored
             * per-faction but this at least gives us readable tones. */
            ok = (Palette_LoadPCX(&pal, "data/palettes/gameart.pcx") == 0);
            if (ok) fprintf(stderr, "LS_LOAD_PALETTE: fell back to gameart.pcx\n");
        }
        if (ok) {
            Palette_BuildRGBATable(&pal, UI_RGBAFormat(),
                                    world->terrain_rgba, 0);
            fprintf(stderr, "LS_LOAD_PALETTE: kingdom=%s\n",
                    world->map_kingdom[0] ? world->map_kingdom : "(none)");
        } else {
            fprintf(stderr, "LS_LOAD_PALETTE: all palette loads failed\n");
            memset(world->terrain_rgba, 0, sizeof(world->terrain_rgba));
        }

        /* Features (lodestones, rocks, trees, lamps) are authored against
         * a dedicated <kingdom>_features.pcx palette. Different colours
         * than terrain — using terrain palette renders stones blue-green
         * and trees cyan. Fall back to terrain palette if the features
         * file is missing so rendering degrades instead of going black. */
        Palette feat_pal;
        int feat_ok = 0;
        if (world->map_kingdom[0]) {
            char fp_path[128];
            snprintf(fp_path, sizeof(fp_path),
                     "data/palettes/%s_features.pcx", world->map_kingdom);
            feat_ok = (Palette_LoadPCX(&feat_pal, fp_path) == 0);
        }
        if (feat_ok) {
            Palette_BuildRGBATable(&feat_pal, UI_RGBAFormat(),
                                    world->features_rgba, 0);
            fprintf(stderr, "LS_LOAD_PALETTE: features kingdom=%s\n",
                    world->map_kingdom);
        } else {
            fprintf(stderr, "LS_LOAD_PALETTE: features palette missing — "
                            "falling back to terrain palette\n");
            memcpy(world->features_rgba, world->terrain_rgba,
                   sizeof(world->features_rgba));
        }
        world->water_height = load_side_water_height(world->map_kingdom);
        fprintf(stderr, "LS_LOAD_PALETTE: waterheight=%d\n",
                world->water_height);
        ld.step = LS_LOAD_TNT;
        break;
    }

    case LS_LOAD_TNT: {
        Loading_SetStatus("Loading terrain...");
        Loading_SetProgress(0.55f);

        GameWorld *world = World_Get();
        if (!world) { ld.step = LS_DONE; break; }

        char tnt_path[256];
        snprintf(tnt_path, sizeof(tnt_path),
                 "maps/Maps/%s.tnt", world->map_name);
        int rc = TNT_Load(&world->tnt, tnt_path, world->terrain_rgba);
        if (rc != 0) {
            snprintf(tnt_path, sizeof(tnt_path),
                     "missions/missions/%s.tnt", world->map_name);
            rc = TNT_Load(&world->tnt, tnt_path, world->terrain_rgba);
        }
        if (rc == 0) {
            fprintf(stderr, "LS_LOAD_TNT: loaded %s.tnt (%dx%d tiles, %dx%d blocks)\n",
                    world->map_name, world->tnt.width_tiles,
                    world->tnt.height_tiles,
                    world->tnt.blocks_w, world->tnt.blocks_h);
            /* Full per-map name table dump so we can see every feature
             * the map can spawn — even the ones that aren't currently
             * referenced by feature_layer. Helps diagnose unresolved
             * IDs (red dots at render time). */
            if (world->tnt.feature_names && world->tnt.num_feature_names > 0) {
                fprintf(stderr,
                    "LS_LOAD_TNT: per-map name table has %d entries:\n",
                    world->tnt.num_feature_names);
                for (int q = 0; q < world->tnt.num_feature_names; q++) {
                    int g = Features_FindByName(world->tnt.feature_names[q]);
                    fprintf(stderr,
                        "  [%d] '%s' -> global=%d %s\n",
                        q, world->tnt.feature_names[q], g,
                        g >= 0 ? "OK" : "UNRESOLVED");
                }
            }

            /* Walk the TNT feature_layer and capture every non-sentinel
             * cell. Each cell is a uint16: 0xFFFF / 0xFFFB are
             * sentinels (empty / continuation), anything else is a
             * feature index pointing into the global feature registry
             * built from data/features/<world>/*.tdf. We only store the
             * leading cell (the one whose tile is the feature's
             * top-left footprint) — TAK marks continuation cells with
             * sentinel values, so plain-iteration of indexed cells
             * yields one entry per feature instance. */
            int W = world->tnt.width_tiles;
            int H = world->tnt.height_tiles;
            const uint16_t *fl = world->tnt.feature_layer;
            if (fl && W > 0 && H > 0) {
                /* First pass: count. */
                int n = 0;
                for (int i = 0; i < W * H; i++) {
                    if (fl[i] < 0xFFF0u) n++;
                }
                if (n > 0) {
                    world->features =
                        (struct MapFeature *)tak_malloc((size_t)n * sizeof(*world->features));
                    if (world->features) {
                        int k = 0;
                        for (int z = 0; z < H && k < n; z++) {
                            for (int x = 0; x < W && k < n; x++) {
                                uint16_t v = fl[z * W + x];
                                if (v < 0xFFF0u) {
                                    /* Resolve LOCAL TNT id -> per-map name ->
                                     * global registry idx. The legacy engine
                                     * does the same two-step lookup: each TNT
                                     * carries its own feature_names table
                                     * (parsed at TNT offset 0x18) and the
                                     * cell value is the index into THAT table,
                                     * not the global registry. */
                                    int gidx = -1;
                                    if (world->tnt.feature_names &&
                                        v < (uint16_t)world->tnt.num_feature_names) {
                                        const char *nm = world->tnt.feature_names[v];
                                        if (nm && nm[0]) {
                                            gidx = Features_FindByName(nm);
                                        }
                                    }
                                    world->features[k].feat_id    = v;
                                    world->features[k].tile_x     = (uint16_t)x;
                                    world->features[k].tile_z     = (uint16_t)z;
                                    world->features[k].global_idx = gidx;
                                    /* Authored scenery never decomposes
                                     * and belongs to no player. */
                                    world->features[k].decompose_ticks = -1;
                                    world->features[k].color_idx       = -1;
                                    k++;
                                }
                            }
                        }
                        world->feature_count = k;
                        world->feature_cap   = n;
                        fprintf(stderr,
                            "LS_LOAD_TNT: %d feature cells captured (lodestones, rocks, trees etc.)\n",
                            k);
                        /* Diagnostic: print every unique LOCAL id, with
                         * the per-map name and resolved global category. */
                        int seen[256] = {0};
                        int seen_cat[256] = {0};
                        int n_seen = 0;
                        for (int q = 0; q < k; q++) {
                            int id = world->features[q].feat_id;
                            int dup = 0;
                            for (int s = 0; s < n_seen; s++) {
                                if (seen[s] == id) {
                                    seen_cat[s]++;
                                    dup = 1;
                                    break;
                                }
                            }
                            if (dup || n_seen >= 256) continue;
                            seen[n_seen] = id;
                            seen_cat[n_seen] = 1;
                            n_seen++;
                        }
                        fprintf(stderr, "  %d distinct local feature IDs:\n", n_seen);
                        for (int s = 0; s < n_seen; s++) {
                            const char *map_name =
                                (seen[s] < world->tnt.num_feature_names &&
                                 world->tnt.feature_names)
                                    ? world->tnt.feature_names[seen[s]]
                                    : "<no-name>";
                            int gidx = (map_name && map_name[0])
                                       ? Features_FindByName(map_name) : -1;
                            const FeatureDef *fd = Features_GetByIndex(gidx);
                            fprintf(stderr,
                                "    local=%d (x%d) name='%s' -> global=%d %s [%s]\n",
                                seen[s], seen_cat[s], map_name,
                                gidx,
                                fd ? fd->name : "<unresolved>",
                                fd ? (fd->category[0] ? fd->category : "(no cat)") : "?");
                        }
                    }
                }
            }
        } else {
            fprintf(stderr, "LS_LOAD_TNT: failed to load %s.tnt\n",
                    world->map_name);
            /* Later phases check tnt.width_tiles > 0 before using it. */
        }
        ld.step = LS_LOAD_CHUNKS;
        break;
    }
    case LS_LOAD_CHUNKS: {
        Loading_SetStatus("Loading terrain textures...");
        GameWorld *world = World_Get();
        if (!world || !world->tnt.block_chunk_ids) {
            ld.step = LS_INIT_WORLD;
            break;
        }

        /* Lazy-init on first entry. TerrainGrid_Init dedupes chunk_ids,
         * which is what turns CASTLE's 74K blocks into ~1K unique JPG
         * uploads. */
        if (!world->grid) {
            world->grid = TerrainGrid_Init(&world->tnt);
            if (!world->grid) {
                fprintf(stderr, "LS_LOAD_CHUNKS: TerrainGrid_Init failed\n");
                ld.step = LS_INIT_WORLD;
                break;
            }
            ld.next_chunk = 0;
        }

        /* Upload BUDGET unique chunks per tick. Each upload = VFS read
         * + JPG decode + static-texture upload (~5 ms). 5 per frame
         * keeps a 60-FPS loading screen smooth. */
        const int BUDGET = 5;
        int total = world->grid->chunk_count;
        for (int k = 0; k < BUDGET && ld.next_chunk < total; k++) {
            TerrainGrid_LoadChunk(world->grid, ld.next_chunk++, platform);
        }
        Loading_SetProgress(0.55f + 0.20f * ((float)ld.next_chunk / (float)total));

        if (ld.next_chunk >= total) {
            fprintf(stderr, "LS_LOAD_CHUNKS: %d unique chunks uploaded "
                            "(%d x %d blocks)\n",
                    total, world->grid->blocks_w, world->grid->blocks_h);
            ld.step = LS_LOAD_TEXTURES;
        }
        break;
    }
    case LS_LOAD_TEXTURES: {
        /* Build per-GAF atlases for unit/feature textures. One-shot
         * (~88 GAFs × small payloads each) — fast enough to skip the
         * per-frame budget. */
        Loading_SetStatus("Loading unit textures...");
        Loading_SetProgress(0.76f);
        TexAtlas_LoadAll(platform);
        ld.step = LS_LOAD_UNITS;
        break;
    }
    case LS_LOAD_UNITS: {
        Loading_SetStatus("Loading units...");
        Loading_SetProgress(0.78f);
        /* All-at-once: 200-ish FBI files at ~2KB each, parsed through
         * the already-hot TDF code path. Fast enough that budgeting
         * across ticks isn't worth the complexity. */
        GameWorld *world = World_Get();
        if (world && TAK_MoveInfo_Load(&world->moveinfo,
                                       "data/gamedata/moveinfo.tdf") == 0) {
            fprintf(stderr, "LS_LOAD_UNITS: loaded %d movement classes\n",
                    world->moveinfo.count);
        } else {
            fprintf(stderr, "LS_LOAD_UNITS: moveinfo.tdf unavailable; "
                            "using FBI slope fallbacks\n");
        }
        Units_LoadDefs();
        Units_ClearInstances();
        /* M6: pre-bake the canonical monarch meshes so the LS_FINALIZE
         * spawn doesn't pause for 3DO load + bake on first frame.
         * Cheap (~100ms total for 4 meshes). Other unit defs lazy-bake
         * on first sight in M4 fashion. */
        Units_BakeMonarchMeshes();
        ld.step = LS_INIT_WORLD;
        break;
    }
    case LS_INIT_WORLD: {
        Loading_SetStatus("Initializing world...");
        Loading_SetProgress(0.80f);
        GameWorld *world = World_Get();
        if (!world) {
            fprintf(stderr, "No GameWorld struct? That's not good!");
            return;
        }
        /* 16 px per tile — confirmed in the legacy map init at
         * line 224930 which stores W_tiles*16 as the map pixel width.
         * Block size follows: 2 tiles per block × 16 = 32 map pixels. */
        world->map_pixels_w = world->tnt.width_tiles  * 16;
        world->map_pixels_h = world->tnt.height_tiles * 16;
        world->viewport_w = platform->window_w;
        world->viewport_h = platform->window_h;
        if (Fog_Init(world) != 0) {
            fprintf(stderr, "LS_INIT_WORLD: Fog_Init failed\n");
        }
        ld.step = LS_POSITION_CAMERA;
        break;
    }

    case LS_POSITION_CAMERA: {
        /* TASK 7b (user): center camera on player-1 StartPos from the OTA. */
        Loading_SetStatus("Positioning view...");
        Loading_SetProgress(0.90f);
        GameWorld *world = World_Get();
        if (!world) {
            fprintf(stderr, "No GameWorld struct? That's not good!");
            return;
        }
        int32_t cam_x = world->map_pixels_w / 2 - world->viewport_w / 2;
        int32_t cam_y = world->map_pixels_h / 2 - world->viewport_h / 2;
        for (int i = 0; i < world->num_start_positions; i++) {
            StartPos curr_start_pos = world->start_positions[i];
            if (curr_start_pos.player == 1) {
                cam_x = curr_start_pos.x * 16 - world->viewport_w / 2; // 16 is a best guess scale factor.. need to figure this out
                cam_y = curr_start_pos.z * 16 - world->viewport_h / 2;
            }
        }

        world->cam_x = clamp_i32(cam_x, 0, world->map_pixels_w - world->viewport_w);
        world->cam_y = clamp_i32(cam_y, 0, world->map_pixels_h - world->viewport_h);
        fprintf(stderr, "LS_POSITION_CAMERA: cam=(%d, %d)\n", cam_x, cam_y);

        ld.step = LS_FINALIZE;
        break;
    }

    case LS_FINALIZE: {
        Loading_SetStatus("Starting battle...");
        Loading_SetProgress(1.0f);

        /* M7: spawn each active player's monarch at their own start
         * position with their chosen faction + team color. OTA "map
         * squares" are 16 world pixels — same scale LS_POSITION_CAMERA
         * used to center the view. */
        GameWorld *world = World_Get();
        if (world && world->mission.placement_count > 0) {
            int spawned = 0;
            int selected = 0;
            int *handles = (int *)tak_malloc(
                (size_t)world->mission.placement_count * sizeof(int));
            if (handles) {
                for (int i = 0; i < world->mission.placement_count; i++) handles[i] = -1;
            }
            for (int i = 0; i < world->mission.placement_count; i++) {
                const MissionPlacement *p = &world->mission.placements[i];
                int def = Units_FindDefByName(p->unitname);
                int color = p->player > 0 ? (p->player - 1) % 12 : 0;
                if (def < 0) {
                    fprintf(stderr,
                        "LS_FINALIZE: missing mission unit def '%s' in %s; skipping\n",
                        p->unitname, p->section);
                    continue;
                }
                int handle = Units_Spawn(def, p->player, color, p->x * 16, p->z * 16);
                if (handle >= 0) {
                    if (handles) handles[i] = handle;
                    /* Authored angles are in the legacy frame, where 0
                     * faces south (the projector sends the model's -z
                     * front down-screen, legacy:197689). Ours puts 0 at
                     * north, so shift by half a turn. */
                    float heading = (float)p->angle * 6.2831853f / 65536.0f
                                  + 3.14159265f;
                    const UnitDef *d = Units_GetDef(def);
                    Units_SetHeading(handle, heading);
                    Units_SetHealthPercent(handle, p->health_percent);
                    apply_initial_mission_commands(handle, p);
                    if (d) {
                        int32_t cap_contrib = d->max_mana + d->mogrium_storage;
                        float regen_contrib = d->mana_recharge_per_sec
                                             + d->mogrium_income_per_sec;
                        if (cap_contrib || regen_contrib) {
                            Economy_OnMonarchSpawn(&world->economy, p->player,
                                                    cap_contrib, regen_contrib);
                        }
                    }
                    if (!selected && p->player == 1) {
                        Units_SelectSingle(handle);
                        selected = 1;
                    }
                    spawned++;
                }
            }
            if (handles) {
                apply_initial_attack_commands(world, handles);
                tak_free(handles);
            }
            fprintf(stderr, "LS_FINALIZE: spawned %d campaign unit(s)\n", spawned);
        } else if (world && world->num_start_positions > 0) {
            /* TakSide enum -> FBI category prefix. Order matches the
             * enum in tak_battle_config.h, so cfg.side indexes directly. */
            static const char *side_prefixes[] = {
                "ARA",  /* TAK_SIDE_ARAMON */
                "TAR",  /* TAK_SIDE_TAROS  */
                "VER",  /* TAK_SIDE_VERUNA */
                "ZON",  /* TAK_SIDE_ZHON   */
            };
            const int n_sides = (int)(sizeof(side_prefixes) / sizeof(side_prefixes[0]));

            int has_start[TAK_MAX_PLAYERS + 1] = { 0 };
            int spawnable_slots = 0;
            for (int i = 0; i < world->num_start_positions; i++) {
                int player = world->start_positions[i].player;
                if (player >= 1 && player <= TAK_MAX_PLAYERS) {
                    has_start[player] = 1;
                }
            }
            for (int player = 1; player <= TAK_MAX_PLAYERS; player++) {
                const PlayerSlot *slot = &world->cfg.players[player - 1];
                if (slot->kind == TAK_SLOT_CLOSED) continue;
                if (!has_start[player]) {
                    fprintf(stderr,
                        "LS_FINALIZE: active player %d has no start position on map '%s'\n",
                        player, world->map_name);
                    continue;
                }
                if (slot->side < 0 || slot->side >= n_sides) {
                    fprintf(stderr,
                        "LS_FINALIZE: active player %d has invalid side %d\n",
                        player, slot->side);
                    continue;
                }
                if (Units_FindMonarchDef(side_prefixes[slot->side]) < 0) {
                    fprintf(stderr,
                        "LS_FINALIZE: active player %d has no monarch for side '%s'\n",
                        player, side_prefixes[slot->side]);
                    continue;
                }
                spawnable_slots++;
            }

            int spawned = 0;
            for (int i = 0; i < world->num_start_positions; i++) {
                StartPos sp = world->start_positions[i];
                if (sp.player < 1 || sp.player > TAK_MAX_PLAYERS) continue;
                const PlayerSlot *slot = &world->cfg.players[sp.player - 1];
                if (slot->kind == TAK_SLOT_CLOSED) continue;
                if (slot->side < 0 || slot->side >= n_sides) continue;

                const char *prefix = side_prefixes[slot->side];
                int def = Units_FindMonarchDef(prefix);
                if (def < 0) {
                    fprintf(stderr,
                        "LS_FINALIZE: no monarch def for side='%s' (player %d); skipping\n",
                        prefix, sp.player);
                    continue;
                }
                int32_t wx = sp.x * 16;
                int32_t wy = sp.z * 16;
                int handle = Units_Spawn(def, sp.player, slot->color, wx, wy);
                if (handle >= 0) {
                    /* Units_Spawn already faces them south, toward the
                     * viewer, which is legacy heading 0 (legacy:197689).
                     * The old debug spread put the local monarch's back
                     * to the camera with his cape over his head.
                     */

                    /* Seed mana pool from the monarch's maxmana /
                     * manarechargerate. The legacy engine does
                     * the same thing — the pool follows the monarch.
                     * Lodestones add (mogriumstorage, mogriumincome)
                     * via the same hook on their spawn. */
                    const UnitDef *d = Units_GetDef(def);
                    if (d) {
                        int32_t cap_contrib   = d->max_mana
                                              + d->mogrium_storage;
                        float   regen_contrib = d->mana_recharge_per_sec
                                              + d->mogrium_income_per_sec;
                        Economy_OnMonarchSpawn(&world->economy, sp.player,
                                                cap_contrib, regen_contrib);
                    }

                    /* Auto-select the local human player's monarch so
                     * the HUD bottom strip immediately shows "Elsin /
                     * Standby" instead of an empty status box. Matches
                     * legacy behaviour where the game opens centred on
                     * the player's monarch with it pre-selected. */
                    if (sp.player == 1 && slot->kind == TAK_SLOT_HUMAN) {
                        Units_SelectSingle(handle);
                    }

                    spawned++;
                    fprintf(stderr,
                        "LS_FINALIZE: spawned %s for P%d (%s, tc=%d) at (%d, %d) mana=%d/%d +%.1f/s\n",
                        Units_GetDef(def)->unitname, sp.player, prefix,
                        slot->color, wx, wy,
                        Economy_GetMana(&world->economy, sp.player),
                        Economy_GetMaxMana(&world->economy, sp.player),
                        d ? (d->mana_recharge_per_sec + d->mogrium_income_per_sec) : 0.0f);
                }
            }
            fprintf(stderr, "LS_FINALIZE: %d monarch(s) spawned\n", spawned);
            if (spawnable_slots < 2 || spawned < 2) {
                world->skirmish_game_over = 1;
                world->skirmish_winner_team = 0;
                world->skirmish_local_result = 0;
                strncpy(world->skirmish_end_reason, "Setup Error",
                        sizeof(world->skirmish_end_reason) - 1);
                fprintf(stderr,
                    "LS_FINALIZE: skirmish setup error, spawnable=%d spawned=%d\n",
                    spawnable_slots, spawned);
            }
        }

        if (world) {
            for (int p = 1; p <= TAK_MAX_PLAYERS; p++) {
                if (world->cfg.players[p - 1].kind != TAK_SLOT_CLOSED)
                    Fog_Update(world, p);
            }
        }
        World_MarkLoaded();
        ld.step = LS_DONE;
        break;
    }

    case LS_DONE:
        /* Idle at 100%. main.c's held_one_frame path fires the transition. */
        break;
    }
}

int Loading_Tick(TAK_Platform *platform, float frame_dt) {
    if (!ld.initialized) return GAMESTATE_GAME_LOADING;

    SDL_Surface *off = UI_Offscreen();

    /* Advance the asset-load state machine by exactly one phase this
     * frame. Running the loader and the renderer in lockstep is what
     * keeps the progress bar + Bink smooth — the user sees each phase
     * tick past instead of a frozen UI during a big synchronous load. */
    loading_advance_step(platform);

    /* Advance the Bink clip at its native frame rate. Loop on reaching
     * the end so the montage keeps playing until loading completes.
     * Mirrors main_menu.c's accumulator pattern. */
    if (ld.bg_bink) {
        double fd = BinkPlayer_GetFrameDuration(ld.bg_bink);
        if (fd <= 0) fd = 1.0 / 30.0;
        ld.bink_timer += (double)frame_dt;
        while (ld.bink_timer >= fd) {
            ld.bink_timer -= fd;
            if (!BinkPlayer_NextFrame(ld.bg_bink)) {
                BinkPlayer_Rewind(ld.bg_bink);
                BinkPlayer_NextFrame(ld.bg_bink);
            }
        }
    }

    /* The phase and the percentage go into the dialog's own labels each
     * frame, the way the original writes them (legacy:158380). */
    if (ld.rt) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", (int)(ld.progress * 100.0f + 0.5f));
        GUIRuntime_SetWidgetText(ld.rt, "LoadText", ld.status);
        GUIRuntime_SetWidgetText(ld.rt, "Percent", pct);
    }

    /* The dialog paints the whole backdrop: the stone arch wall from the
     * root widget, then the unlit stained glass, then the lit glass in
     * the arch hollow. The clip, when there is one, goes over the top. */
    if (ld.has_dialog && ld.rt) {
        GUIRuntime_Render(ld.rt);
    } else {
        SDL_Rect whole = { 0, 0, 640, 480 };
        fill_rect(off, whole, SDL_MapRGBA(off->format, 20, 20, 30, 255));
    }

    /* Clip at the widget's top-left and its own size, as the original
     * hands the player only that corner (legacy:158297). It is 422x351
     * against a 423x351 widget, so the authored glass shows through the
     * last column rather than being stretched over. */
    if (ld.bg_bink) {
        const uint32_t *vpx = BinkPlayer_GetPixels(ld.bg_bink);
        int vw = BinkPlayer_GetWidth(ld.bg_bink);
        int vh = BinkPlayer_GetHeight(ld.bg_bink);
        if (vpx && vw > 0 && vh > 0) {
            Blit_RGBA(off, ld.bink_rect.x, ld.bink_rect.y, vpx, vw, vh);
        }
    }

    /* Progress bar: simple rect near the bottom-center. */
    SDL_Rect bar_bg = { 120, 420, 400, 18 };
    SDL_Rect bar_fg = bar_bg;
    bar_fg.w = (int)((float)bar_bg.w * ld.progress);
    fill_rect(off, bar_bg, SDL_MapRGBA(off->format, 40, 40, 40, 255));
    fill_rect(off, bar_fg, SDL_MapRGBA(off->format, 210, 170, 60, 255));

    UI_Present(platform);

    /* Hold on 100% for one frame so the finished bar is visible before
     * we transition. */
    if (ld.progress >= 1.0f) {
        if (ld.held_one_frame) {
            SDL_Rect whole = { 0, 0, 640, 480 };
            fill_rect(off, whole, SDL_MapRGBA(off->format, 0, 0, 0, 0));
            return GAMESTATE_IN_GAME;
        }
        ld.held_one_frame = 1;
    }
    return GAMESTATE_GAME_LOADING;
}
