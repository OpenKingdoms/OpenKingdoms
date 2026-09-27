/*
 * ok_embed.c -- the engine as a library, see ok_embed.h.
 *
 * It boots the way the battle tests do: the file system, a hidden
 * platform on SDL's dummy driver with a software renderer, the UI
 * tables, then the loading screen run to the end. Models come from the
 * unit renderer's bake and the glTF loader the 3D view uses, and poses
 * from the same piece composition, so nothing is decoded twice.
 */
#include "ok_embed.h"

#include "tak_battle_config.h"
#include "tak_command_emit.h"
#include "tak_cob_vm.h"
#include "tak_economy.h"
#include "tak_features.h"
#include "tak_fog.h"
#include "tak_gameloop.h"
#include "tak_game_sound.h"
#include "tak_gltf.h"
#include "tak_gpu.h"
#include "tak_hpi.h"
#include "tak_hud.h"
#include "tak_ingame.h"
#include "tak_jpg.h"
#include "tak_loading.h"
#include "tak_maps.h"
#include "tak_memory.h"
#include "tak_music.h"
#include "tak_model_gltf.h"
#include "tak_palette.h"
#include "tak_platform.h"
#include "tak_sound.h"
#include "tak_soundclass.h"
#include "tak_tdf.h"
#include "tak_terrain.h"
#include "tak_tnt.h"
#include "tak_savegame.h"
#include "tak_ui.h"
#include "tak_unit.h"
#include "tak_util.h"
#include "tak_world.h"

#include <SDL.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define okx_mkdir(p) _mkdir(p)
#else
#define okx_mkdir(p) mkdir(p, 0755)
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OKX_MAX_MODELS   1024
#define OKX_MAX_TEXTURES 1024
/* The 3D view's own constants: pixels a block and a tile cover, texels
 * a block takes from its chunk, and how far a sprite stands up. */
#define OKX_BLOCK_PX     32
#define OKX_TILE_PX      16
#define OKX_SUB_PX       32
#define OKX_SPRITE_RISE  1.6f

typedef struct Model {
    char      name[TAK_UNITDEF_OBJ_MAX];
    int       color;
    UnitMesh *mesh;
    float    *normals;          /* when the mesh brought none */
    int       from_gltf;
    int16_t   piece_src[UNIT_MESH_MAX_NODES];
    int       piece_src_count;
    int       batch_tex[UNIT_MESH_MAX_BATCHES];
} Model;

typedef struct Texture {
    const GPU_Texture *gpu;     /* a shipped atlas, pixels read back */
    uint32_t          *rgba;    /* or pixels of our own */
    int                w, h;
} Texture;

static struct {
    int           ready;
    int           in_game;
    TAK_Platform  plat;
    char          error[256];
    char          override_dir[512];
    char          user_dir[512];
    int           audio;
    char          game_dir[512];
    TAK_MapEntry *maps;
    int           map_count;
    Model        *models[OKX_MAX_MODELS];
    int           model_count;
    Texture       textures[OKX_MAX_TEXTURES];
    int           texture_count;
} g;

/* The map the battle was started on, by the name the map list gives. */
static char s_map_key[96];

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.error, sizeof(g.error), fmt, ap);
    va_end(ap);
}

int32_t okx_api_version(void) { return OKX_API_VERSION; }
const char *okx_last_error(void) { return g.error; }

void okx_set_user_dir(const char *dir) {
    snprintf(g.user_dir, sizeof(g.user_dir), "%s", dir ? dir : "");
}

void okx_set_override_dir(const char *dir) {
    snprintf(g.override_dir, sizeof(g.override_dir), "%s", dir ? dir : "");
}

/* ── Lifetime ──────────────────────────────────────────────────────── */

static int platform_up(TAK_Platform *p) {
    memset(p, 0, sizeof(*p));
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fail("SDL init failed: %s", SDL_GetError());
        return -1;
    }
    p->window = SDL_CreateWindow("okengine", 0, 0, 640, 480, SDL_WINDOW_HIDDEN);
    if (!p->window) { fail("no window: %s", SDL_GetError()); return -1; }
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE);
    if (!p->renderer) { fail("no renderer: %s", SDL_GetError()); return -1; }
    p->renderer_gen = TAK_Platform_NewRendererGen();
    p->canvas_w = p->window_w = 640;
    p->canvas_h = p->window_h = 480;
    p->scale = 1.0f;
    p->has_focus = 1;
    p->canvas_tex = SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING, 640, 480);
    if (!p->canvas_tex) { fail("no canvas: %s", SDL_GetError()); return -1; }
    return 0;
}

static void platform_down(TAK_Platform *p) {
    if (p->canvas_tex) SDL_DestroyTexture(p->canvas_tex);
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window) SDL_DestroyWindow(p->window);
    memset(p, 0, sizeof(*p));
    SDL_Quit();
}

int32_t okx_init(const char *game_dir, const char *data_dir) {
    if (g.ready) return 0;
    g.error[0] = 0;
    if (!game_dir || !game_dir[0]) { fail("no game folder"); return -1; }
    if (VFS_IsInitialized()) VFS_Shutdown();
    /* The user folder mounts over everything, read loose, so a map
     * saved there is found like a shipped one. */
    if (g.user_dir[0]) {
        const char *mods[1] = { g.user_dir };
        (void)okx_mkdir(g.user_dir);
        VFS_SetModArchives(mods, 1);
    } else {
        VFS_SetModArchives(NULL, 0);
    }
    if (VFS_Init(game_dir, data_dir && data_dir[0] ? data_dir : NULL) != 0) {
        fail("no game files in %s", game_dir);
        return -1;
    }
    snprintf(g.game_dir, sizeof(g.game_dir), "%s", game_dir);
    GPU_SetKeepPixels(1);
    if (platform_up(&g.plat) != 0) { VFS_Shutdown(); return -1; }
    if (UI_Init() != 0) {
        fail("UI tables did not load");
        platform_down(&g.plat);
        VFS_Shutdown();
        return -1;
    }
    g.ready = 1;
    return 0;
}

static void clear_models(void) {
    for (int i = 0; i < g.model_count; i++) {
        Model *m = g.models[i];
        if (m->from_gltf) Gltf_FreeUnitMesh(m->mesh);
        else Units_FreeBakedMesh(m->mesh);
        if (m->normals) tak_free(m->normals);
        tak_free(m);
    }
    g.model_count = 0;
    for (int i = 0; i < g.texture_count; i++)
        if (g.textures[i].rgba) tak_free(g.textures[i].rgba);
    g.texture_count = 0;
}

static void studio_release(void);

void okx_end_game(void) {
    if (!g.in_game) return;
    studio_release();
    clear_models();
    InGame_Shutdown();
    Loading_Shutdown();
    World_End(&g.plat);
    g.in_game = 0;
}

int32_t okx_audio(int32_t enable, int32_t volume, int32_t music) {
    if (!g.ready) return -1;
    if (!enable) {
        if (g.audio) {
            GameSound_Shutdown();
            TAK_Music_Shutdown();
            TAK_Sound_Shutdown();
            g.audio = 0;
        }
        return 0;
    }
    if (!g.audio) {
        if (TAK_Sound_Init() != 0) { fail("no audio device"); return -1; }
        (void)TAK_Music_Init(g.game_dir);
        SoundClass_LoadAll();
        GameSound_Init();
        g.audio = 1;
    }
    int v = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    TAK_Sound_SetMasterVolume(v);
    TAK_Sound_SetEnabled(1);
    TAK_Music_SetVolume(v);
    TAK_Music_SetMode(music ? TAK_MUSIC_SEQUENTIAL : TAK_MUSIC_OFF);
    return 0;
}

void okx_set_view(int32_t cx, int32_t cy, int32_t w, int32_t h) {
    GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || w <= 0 || h <= 0) return;
    wd->cam_x = cx - w / 2;
    wd->cam_y = cy - h / 2;
    wd->viewport_w = w;
    wd->viewport_h = h;
}

void okx_shutdown(void) {
    if (!g.ready) return;
    okx_audio(0, 0, 0);
    okx_end_game();
    if (g.maps) { TAK_Maps_Free(g.maps); g.maps = NULL; }
    g.map_count = 0;
    UI_Shutdown();
    platform_down(&g.plat);
    VFS_Shutdown();
    GPU_SetKeepPixels(0);
    g.ready = 0;
}

/* ── Maps and unit types ───────────────────────────────────────────── */

static int scan_maps(void) {
    if (g.maps || !g.ready) return g.ready ? 0 : -1;
    if (TAK_Maps_Scan(&g.maps, &g.map_count) != 0) {
        g.maps = NULL;
        g.map_count = 0;
        return -1;
    }
    return 0;
}

int32_t okx_map_count(void) {
    if (scan_maps() != 0) return 0;
    return g.map_count;
}

int32_t okx_map_name(int32_t index, char *out, int32_t cap) {
    if (scan_maps() != 0 || index < 0 || index >= g.map_count || !out || cap <= 0) return -1;
    snprintf(out, (size_t)cap, "%s", g.maps[index].key);
    return (int32_t)strlen(out);
}

/* The .ota's size text, "8 x 8". */
static void parse_size(const char *text, int32_t *x, int32_t *y) {
    int a = 0, b = 0;
    *x = *y = 0;
    if (text && sscanf(text, " %d %*1[xX] %d", &a, &b) == 2 && a > 0 && b > 0) { *x = a; *y = b; }
}

int32_t okx_map_info(int32_t index, OkxMapInfo *out) {
    if (scan_maps() != 0 || index < 0 || index >= g.map_count || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", g.maps[index].key);
    char path[512];
    TAK_Maps_FindFile(g.maps[index].key, "ota", path, sizeof(path));
    TDFFile *tdf = TDF_Open(path);
    if (tdf && TDF_Load(tdf) == 0 && TDF_PushSection(tdf, "GlobalHeader") == 0) {
        parse_size(TDF_ReadString(tdf, "size", ""), &out->size_x, &out->size_y);
        snprintf(out->description, sizeof(out->description), "%s",
                 TDF_ReadString(tdf, "missiondescription", ""));
        const char *k = TDF_ReadString(tdf, "kingdom", "");
        size_t n = 0;
        for (; k && k[n] && n + 1 < sizeof(out->kingdom); n++)
            out->kingdom[n] = (k[n] >= 'A' && k[n] <= 'Z') ? (char)(k[n] - 'A' + 'a') : k[n];
        out->kingdom[n] = 0;
        /* numplayers lists every lineup, such as "2, 4, 6". */
        const char *p = TDF_ReadString(tdf, "numplayers", "");
        while (p && *p && out->player_count_n < 8) {
            while (*p && (*p < '0' || *p > '9')) p++;
            if (!*p) break;
            char *end = NULL;
            long v = strtol(p, &end, 10);
            p = end;
            if (v <= 0) continue;
            out->player_counts[out->player_count_n++] = (int32_t)v;
            if (v > out->max_players) out->max_players = (int32_t)v;
        }
    }
    if (tdf) TDF_Close(tdf);
    return 0;
}

int32_t okx_map_preview(int32_t index, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    OkxMapInfo info;
    if (okx_map_info(index, &info) != 0) return -1;
    uint32_t table[256];
    memset(table, 0, sizeof(table));
    Palette pal;
    char pcx[128];
    snprintf(pcx, sizeof(pcx), "data/palettes/%s.pcx", info.kingdom[0] ? info.kingdom : "aramon");
    if (Palette_LoadPCX(&pal, pcx) != 0 && Palette_Load(&pal, "data/palettes/gameart.pal") != 0) return -1;
    Palette_BuildRGBATable(&pal, UI_RGBAFormat(), table, 0);
    char path[512];
    TAK_Maps_FindFile(g.maps[index].key, "tnt", path, sizeof(path));
    TNTFile tnt;
    memset(&tnt, 0, sizeof(tnt));
    if (TNT_Load(&tnt, path, table) != 0 || !tnt.minimap_rgba) {
        TNT_Close(&tnt);
        return -1;
    }
    /* The picture is square and the map sits in its top left corner at
     * the map's own aspect, the rest padding, as the lobby reads it. */
    int mw = tnt.minimap_w, mh = tnt.minimap_h;
    int map_w = tnt.width_tiles > 0 ? tnt.width_tiles : mw;
    int map_h = tnt.height_tiles > 0 ? tnt.height_tiles : mh;
    int cw = mw, ch = mh;
    if (map_w >= map_h) ch = (mh * map_h + map_w / 2) / map_w;
    else cw = (mw * map_w + map_h / 2) / map_h;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (cw > mw) cw = mw;
    if (ch > mh) ch = mh;
    int32_t need = cw * ch * 4;
    if (w) *w = cw;
    if (h) *h = ch;
    if (out && cap >= need)
        for (int y = 0; y < ch; y++)
            memcpy(out + (size_t)y * cw * 4, tnt.minimap_rgba + (size_t)y * mw, (size_t)cw * 4);
    TNT_Close(&tnt);
    return need;
}

int32_t okx_def_count(void) { return g.in_game ? Units_GetDefCount() : 0; }

int32_t okx_def_info(int32_t def, OkxDefInfo *out) {
    const UnitDef *d = g.in_game ? Units_GetDef(def) : NULL;
    if (!d || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", d->unitname);
    snprintf(out->object, sizeof(out->object), "%s", d->objectname);
    snprintf(out->side, sizeof(out->side), "%s", d->side);
    snprintf(out->category, sizeof(out->category), "%s", d->category);
    snprintf(out->description, sizeof(out->description), "%s", d->description);
    snprintf(out->display_name, sizeof(out->display_name), "%s", d->display_name);
    out->build_cost = d->build_cost;
    out->max_health = d->max_health;
    out->is_building = d->bmcode == 0;
    out->footprint_x = d->footprint_x;
    out->footprint_z = d->footprint_z;
    return 0;
}

int32_t okx_unit_picture(int32_t def, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    const UnitDef *d = g.in_game ? Units_GetDef(def) : NULL;
    if (!d || !d->unitname[0]) return -1;
    /* Where the HUD finds a build button's picture. */
    char lower[40], path[128];
    size_t n = 0;
    for (; d->unitname[n] && n + 1 < sizeof(lower); n++)
        lower[n] = (d->unitname[n] >= 'A' && d->unitname[n] <= 'Z') ? (char)(d->unitname[n] + 32) : d->unitname[n];
    lower[n] = 0;
    snprintf(path, sizeof(path), "data/anims/buildpic/%s.jpg", lower);
    void *bytes = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &bytes, &size) != 0 || !bytes) return -1;
    uint32_t *pixels = NULL;
    int pw = 0, ph = 0;
    int rc = JPG_DecodeRGBA((const uint8_t *)bytes, (size_t)size, &pixels, &pw, &ph);
    tak_free(bytes);
    if (rc != 0 || !pixels) return -1;
    int32_t need = pw * ph * 4;
    if (w) *w = pw;
    if (h) *h = ph;
    if (out && cap >= need) memcpy(out, pixels, (size_t)need);
    tak_free(pixels);
    return need;
}

int32_t okx_def_buildables(int32_t def, int32_t *out, int32_t cap) {
    if (!g.in_game || !Units_GetDef(def)) return -1;
    static int tmp[256];
    int n = Units_GetBuildables(def, tmp, 256);
    for (int i = 0; out && i < n && i < cap; i++) out[i] = tmp[i];
    return n;
}

/* ── The battle ────────────────────────────────────────────────────── */

static int side_of(const char *kingdom) {
    if (!kingdom || !kingdom[0]) return TAK_SIDE_ARAMON;
    if (tak_stricmp(kingdom, "taros") == 0) return TAK_SIDE_TAROS;
    if (tak_stricmp(kingdom, "veruna") == 0) return TAK_SIDE_VERUNA;
    if (tak_stricmp(kingdom, "zhon") == 0) return TAK_SIDE_ZHON;
    if (tak_stricmp(kingdom, "creon") == 0) return TAK_SIDE_CREON;
    return TAK_SIDE_ARAMON;
}

/* The map's own kingdom from its .ota, which picks its palettes. */
static void map_kingdom(const char *map, char *out, size_t cap) {
    out[0] = 0;
    char path[512];
    TAK_Maps_FindFile(map, "ota", path, sizeof(path));
    TDFFile *tdf = TDF_Open(path);
    if (tdf && TDF_Load(tdf) == 0 && TDF_PushSection(tdf, "GlobalHeader") == 0) {
        const char *k = TDF_ReadString(tdf, "kingdom", "");
        size_t n = 0;
        for (; k && k[n] && n + 1 < cap; n++)
            out[n] = (k[n] >= 'A' && k[n] <= 'Z') ? (char)(k[n] - 'A' + 'a') : k[n];
        out[n] = 0;
    }
    if (tdf) TDF_Close(tdf);
    if (!out[0]) snprintf(out, cap, "aramon");
}

static int s_loading;

int32_t okx_load_begin(const OkxSkirmish *cfg) {
    if (!g.ready) { fail("okx_init first"); return -1; }
    if (!cfg || !cfg->map[0]) { fail("no map"); return -1; }
    okx_end_game();
    if (s_loading) {
        Loading_Shutdown();
        World_End(&g.plat);
        s_loading = 0;
    }

    static const int rival[4] = { TAK_SIDE_TAROS, TAK_SIDE_VERUNA, TAK_SIDE_ZHON, TAK_SIDE_ARAMON };
    BattleConfig bc;
    BattleConfig_SetDefaults(&bc);
    snprintf(bc.map_name, sizeof(bc.map_name), "%s", cfg->map);
    bc.players[0].side = side_of(cfg->kingdom);
    int ai = cfg->ai_players < 0 ? 0 : cfg->ai_players > 4 ? 4 : cfg->ai_players;
    for (int i = 1; i < TAK_MAX_PLAYERS; i++) {
        if (i <= ai) {
            bc.players[i].kind = TAK_SLOT_AI;
            bc.players[i].side = rival[(i - 1) % 4];
            bc.players[i].team = i + 1;
            bc.players[i].color = i;
            bc.players[i].ai_difficulty = 1;
            snprintf(bc.players[i].name, sizeof(bc.players[i].name), "Computer %d", i);
        } else {
            bc.players[i].kind = TAK_SLOT_CLOSED;
        }
    }
    bc.line_of_sight = cfg->line_of_sight;
    bc.map_revealed = cfg->map_revealed;
    bc.seed = cfg->seed ? cfg->seed : (uint32_t)SDL_GetPerformanceCounter();
    if (cfg->seat_count > 0) {
        /* The lobby's own lineup. A random kingdom is drawn from the
         * seed, so every machine given the seed agrees on it. */
        static const int kingdoms[4] = { TAK_SIDE_ARAMON, TAK_SIDE_TAROS, TAK_SIDE_VERUNA, TAK_SIDE_ZHON };
        uint32_t draw = bc.seed;
        for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
            PlayerSlot *p = &bc.players[i];
            const OkxSeat *s = i < cfg->seat_count && i < 8 ? &cfg->seats[i] : NULL;
            if (!s || s->kind <= 0 || s->kind > 2) {
                p->kind = TAK_SLOT_CLOSED;
                continue;
            }
            p->kind = i == 0 ? TAK_SLOT_HUMAN : (s->kind == 2 ? TAK_SLOT_AI : TAK_SLOT_CLOSED);
            if (p->kind == TAK_SLOT_CLOSED) continue;
            draw = draw * 1103515245u + 12345u;
            p->side = s->side >= 0 ? s->side : kingdoms[(draw >> 16) % 4];
            p->team = s->team > 0 ? s->team : i + 1;
            p->color = s->color >= 0 && s->color <= 11 ? s->color : i;
            p->ai_difficulty = s->difficulty < 0 ? 0 : s->difficulty > 3 ? 3 : s->difficulty;
            if (i == 0) snprintf(p->name, sizeof(p->name), "Player");
            else snprintf(p->name, sizeof(p->name), "Computer %d", i);
        }
    }
    if (cfg->units_per_side > 0) bc.units_per_side = cfg->units_per_side;
    bc.monarch_expendable = cfg->monarch_expendable ? 1 : 0;
    bc.random_start_locations = cfg->random_start_locations ? 1 : 0;

    char kingdom[32];
    map_kingdom(cfg->map, kingdom, sizeof(kingdom));
    snprintf(s_map_key, sizeof(s_map_key), "%s", cfg->map);
    if (World_BeginLoad(&g.plat, &bc, cfg->map, kingdom) != 0) {
        fail("the world would not begin loading %s", cfg->map);
        return -1;
    }
    if (Loading_Init(&g.plat) != 0) {
        fail("the loading screen would not start");
        World_End(&g.plat);
        return -1;
    }
    s_loading = 1;
    return 0;
}

int32_t okx_load_step(int32_t max_ms, float *progress, char *status, int32_t cap) {
    if (g.in_game) {
        if (progress) *progress = 1.0f;
        if (status && cap > 0) status[0] = 0;
        return 1;
    }
    if (!s_loading) return -1;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t until = SDL_GetPerformanceCounter() + (uint64_t)(max_ms > 0 ? max_ms : 0) * freq / 1000u;
    int next = GAMESTATE_GAME_LOADING;
    int guard = 6000;
    do {
        next = Loading_Tick(&g.plat, 1.0f / 60.0f);
    } while (next == GAMESTATE_GAME_LOADING && --guard > 0 && SDL_GetPerformanceCounter() < until);
    if (progress) *progress = Loading_Progress();
    if (status && cap > 0) snprintf(status, (size_t)cap, "%s", Loading_Status());
    if (next == GAMESTATE_GAME_LOADING) return 0;
    s_loading = 0;
    if (next != GAMESTATE_IN_GAME || !World_Get()) {
        fail("the map did not finish loading");
        Loading_Shutdown();
        World_End(&g.plat);
        return -1;
    }
    if (InGame_Init(&g.plat) != 0) {
        fail("the battle screen would not start");
        Loading_Shutdown();
        World_End(&g.plat);
        return -1;
    }
    g.in_game = 1;
    if (progress) *progress = 1.0f;
    return 1;
}

int32_t okx_save(const char *path) {
    if (!g.in_game || !path || !path[0]) { fail("no battle to save"); return -1; }
    char err[256] = "";
    if (Save_Write(path, err, sizeof(err)) != 0) {
        fail("%s", err[0] ? err : "the save was not written");
        return -1;
    }
    return 0;
}

int32_t okx_save_info(const char *path, OkxSaveInfo *out) {
    if (!g.ready || !path || !out) return -1;
    char err[256] = "";
    TAK_SaveGame *sg = Save_Read(path, err, sizeof(err));
    if (!sg) { fail("%s", err[0] ? err : "not a saved game"); return -1; }
    const TAK_SaveInfo *info = Save_Info(sg);
    memset(out, 0, sizeof(*out));
    snprintf(out->map, sizeof(out->map), "%s", info->map_name);
    out->tick = info->sim_tick;
    out->saved_at = info->saved_at_utc;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++)
        if (info->cfg.players[i].kind != TAK_SLOT_CLOSED) out->players++;
    Save_ReadClose(sg);
    return 0;
}

int32_t okx_load_save_begin(const char *path) {
    if (!g.ready) { fail("okx_init first"); return -1; }
    if (!path || !path[0]) { fail("no save"); return -1; }
    /* Read first, so a save that will not load leaves the battle be. */
    char err[256] = "";
    TAK_SaveGame *sg = Save_Read(path, err, sizeof(err));
    if (!sg) { fail("%s", err[0] ? err : "not a saved game"); return -1; }
    okx_end_game();
    if (s_loading) {
        Loading_Shutdown();
        World_End(&g.plat);
        s_loading = 0;
    }
    const TAK_SaveInfo *info = Save_Info(sg);
    if (World_BeginLoad(&g.plat, &info->cfg, info->map_name, info->map_kingdom) != 0) {
        Save_ReadClose(sg);
        fail("the saved battle would not begin loading");
        return -1;
    }
    /* After BeginLoad, as the lobby does: the file carries the army,
     * the pools and the fog the final loading phase would create. */
    World_SetRestoring(1);
    Loading_SetPendingSave(sg);
    if (Loading_Init(&g.plat) != 0) {
        fail("the loading screen would not start");
        World_End(&g.plat);
        return -1;
    }
    s_loading = 1;
    return 0;
}

int32_t okx_start_skirmish(const OkxSkirmish *cfg) {
    if (okx_load_begin(cfg) != 0) return -1;
    for (;;) {
        int32_t rc = okx_load_step(1000, NULL, NULL, 0);
        if (rc != 0) return rc > 0 ? 0 : -1;
    }
}

int32_t okx_tick_rate(void) { return 60; }

int32_t okx_tick(int32_t n) {
    if (!g.in_game || n <= 0) return 0;
    InGame_DebugRunSimTicks(n);
    if (g.audio) {
        TAK_Sound_Update();
        TAK_Music_Update();
    }
    return n;
}

uint32_t okx_tick_count(void) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return 0;
    return (uint32_t)(w->skirmish_elapsed_ticks + w->mission_elapsed_ticks);
}

int32_t okx_local_player(void) { return g.in_game ? Units_LocalPlayer() : 0; }

int32_t okx_outcome(void) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || !w->skirmish_game_over) return 0;
    if (w->skirmish_local_result > 0) return 1;
    if (w->skirmish_local_result < 0) return -1;
    return 2;
}

int32_t okx_players(OkxPlayer *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return 0;
    uint32_t owners = Units_PlayersWithUnits();
    int32_t n = 0;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        const PlayerSlot *s = &w->cfg.players[i];
        if (s->kind == TAK_SLOT_CLOSED) continue;
        if (out && n < cap) {
            OkxPlayer *o = &out[n];
            memset(o, 0, sizeof(*o));
            o->index = i + 1;
            o->kind = s->kind == TAK_SLOT_AI ? 2 : 1;
            o->side = s->side;
            o->team = s->team;
            o->color = s->color;
            o->alive = (owners & (1u << (i + 1))) != 0;
            snprintf(o->name, sizeof(o->name), "%s", s->name);
        }
        n++;
    }
    return n;
}

int32_t okx_economy(int32_t player, OkxEconomy *out) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || !out || player < 1 || player > TAK_MAX_PLAYERS) return -1;
    const PlayerEconomy *e = &w->economy.players[player - 1];
    out->mana = e->mana;
    out->max_mana = e->max_mana;
    out->income = e->regen_per_sec;
    out->earned_last_sec = e->earned_last_sec;
    out->spent_last_sec = e->spent_last_sec;
    return 0;
}

int32_t okx_command(int32_t type, int32_t handle, int32_t x, int32_t y,
                    int32_t target, int32_t build_def, int32_t arg) {
    if (!g.in_game || type <= TAK_CMD_NONE || type >= TAK_CMD_COUNT) return -1;
    return TAK_Cmd_EmitUnit((uint8_t)type, handle, x, y, target,
                            (uint16_t)(build_def < 0 ? 0 : build_def), (uint16_t)arg);
}

int32_t okx_build_site(int32_t def, int32_t x, int32_t y, int32_t *sx, int32_t *sy) {
    if (!g.in_game || !Units_GetDef(def)) return 0;
    int32_t wx = x, wy = y;
    Units_SnapBuildSite(def, &wx, &wy);
    if (sx) *sx = wx;
    if (sy) *sy = wy;
    return Units_IsBuildSiteClear(def, x, y) ? 1 : 0;
}

int32_t okx_factory_queue(int32_t handle, int32_t def) {
    if (!g.in_game) return 0;
    return def < 0 ? Units_FactoryQueueCount(handle) : Units_FactoryQueuedCountForDef(handle, def);
}

int32_t okx_unit_order(int32_t handle, OkxOrder *out) {
    if (!g.in_game || !out) return -1;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (handle < 0 || handle >= count) return -1;
    const Unit *u = &units[handle];
    memset(out, 0, sizeof(*out));
    out->kind = u->cmd_kind;
    out->target = u->target;
    out->x = u->cmd_x;
    out->y = u->cmd_y;
    out->building = u->cmd_kind == UNIT_CMD_BUILD ? u->build_target : -1;
    return 0;
}

int32_t okx_fog(uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    const GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd) return -1;
    int32_t cw = wd->map_pixels_w / 16, ch = wd->map_pixels_h / 16;
    if (w) *w = cw;
    if (h) *h = ch;
    int32_t need = cw * ch;
    if (out && cap >= need) {
        for (int32_t cy = 0; cy < ch; cy++)
            for (int32_t cx = 0; cx < cw; cx++)
                out[cy * cw + cx] = (uint8_t)Fog_StateAt(wd, cx * 16 + 8, cy * 16 + 8);
    }
    return need;
}

int32_t okx_select(const int32_t *handles, int32_t n, int32_t add) {
    if (!g.in_game) return 0;
    if (!add) Units_SelectSingle(-1);
    for (int32_t i = 0; handles && i < n; i++) Units_SelectAdd(handles[i]);
    int count = 0;
    Units_GetSelection(&count);
    return count;
}

int32_t okx_selection(int32_t *out, int32_t cap) {
    if (!g.in_game) return 0;
    int count = 0;
    const int *sel = Units_GetSelection(&count);
    for (int i = 0; out && i < count && i < cap; i++) out[i] = sel[i];
    return count;
}

/* The flat reading of a ground point, the inverse of the classic lift,
 * which is what the game's click takes, as the 3D view hands it over. */
static int32_t flat_y(const GameWorld *w, int32_t x, int32_t z) {
    float h = (float)Terrain_SampleHeight(w, x, z);
    return (int32_t)((float)z - h * Units_GetTanTilt());
}

void okx_click(float x, float z, int32_t unit, int32_t shift) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return;
    int32_t cx = (int32_t)x, cz = (int32_t)z;
    if (unit >= 0) {
        int count = 0;
        const Unit *units = Units_GetActive(&count);
        if (unit < count && units[unit].alive == UNIT_ALIVE_ACTIVE) {
            cx = units[unit].world_x;
            cz = units[unit].world_y;
        }
    }
    InGame_WorldClick(cx, flat_y(w, cx, cz), shift ? 1 : 0);
}

void okx_cancel(void) {
    if (!g.in_game) return;
    if (HUD_GetCommandMode() != HUD_CMD_NONE) HUD_ClearCommandMode();
    else Units_SelectSingle(-1);
}

void okx_arm(int32_t mode, int32_t def) {
    if (!g.in_game) return;
    if (mode == OKX_ARM_NONE) { HUD_ClearCommandMode(); return; }
    if (mode == OKX_ARM_BUILD) { HUD_BeginBuildPlacement(def); return; }
    HUD_SetCommandMode(mode);
}

int32_t okx_armed(int32_t *def) {
    if (!g.in_game) return OKX_ARM_NONE;
    int mode = HUD_GetCommandMode();
    if (def) *def = mode == HUD_CMD_PLACE_BUILD ? HUD_GetBuildPlacementDefIdx() : -1;
    return mode == HUD_CMD_PLACE_BUILD ? OKX_ARM_BUILD : mode;
}

int32_t okx_order_selection(int32_t type, int32_t arg) {
    if (!g.in_game || type <= TAK_CMD_NONE || type >= TAK_CMD_COUNT) return -1;
    return TAK_Cmd_EmitSelection((uint8_t)type, 0, 0, -1, 0, (uint16_t)arg);
}

void okx_group_assign(int32_t group) {
    if (g.in_game && group >= 0 && group <= 9) Units_AssignControlGroup(group);
}

int32_t okx_group_recall(int32_t group) {
    if (!g.in_game || group < 0 || group > 9) return 0;
    return Units_RecallControlGroup(group);
}

/* ── Terrain ───────────────────────────────────────────────────────── */

int32_t okx_terrain_info(OkxTerrainInfo *out) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || !out || !w->grid || !w->tnt.heightmap) return -1;
    memset(out, 0, sizeof(*out));
    out->map_w = w->map_pixels_w;
    out->map_h = w->map_pixels_h;
    out->heights_w = w->tnt.height_w;
    out->heights_h = w->tnt.height_h;
    out->tile_px = OKX_TILE_PX;
    out->blocks_w = w->grid->blocks_w;
    out->blocks_h = w->grid->blocks_h;
    out->block_px = OKX_BLOCK_PX;
    out->sub_px = OKX_SUB_PX;
    out->chunk_count = w->grid->chunk_count;
    out->water_height = w->water_height;
    return 0;
}

int32_t okx_terrain_heights(float *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || !w->tnt.heightmap) return -1;
    int32_t n = w->tnt.height_w * w->tnt.height_h;
    for (int32_t i = 0; out && i < n && i < cap; i++) out[i] = (float)w->tnt.heightmap[i];
    return n;
}

int32_t okx_terrain_blocks(int32_t *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || !w->grid || !w->grid->blocks) return -1;
    int32_t n = w->grid->blocks_w * w->grid->blocks_h;
    for (int32_t i = 0; out && i < n && (i + 1) * 3 <= cap; i++) {
        const TerrainBlock *b = &w->grid->blocks[i];
        out[3 * i] = b->chunk_idx;
        out[3 * i + 1] = b->tex_x;
        out[3 * i + 2] = b->tex_y;
    }
    return n;
}

int32_t okx_terrain_chunk(int32_t chunk, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    const GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || !wd->grid || chunk < 0 || chunk >= wd->grid->chunk_count) return -1;
    char path[64];
    snprintf(path, sizeof(path), "terrain/%08x.jpg", wd->grid->chunks[chunk].chunk_id);
    void *bytes = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &bytes, &size) != 0) return -1;
    uint32_t *pixels = NULL;
    int pw = 0, ph = 0;
    int rc = JPG_DecodeRGBA((const uint8_t *)bytes, (size_t)size, &pixels, &pw, &ph);
    tak_free(bytes);
    if (rc != 0 || !pixels) return -1;
    int32_t need = pw * ph * 4;
    if (w) *w = pw;
    if (h) *h = ph;
    if (out && cap >= need) memcpy(out, pixels, (size_t)need);
    tak_free(pixels);
    return need;
}

float okx_ground_height(float x, float z) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return 0.0f;
    return (float)Terrain_SampleHeight(w, (int32_t)x, (int32_t)z);
}

/* ── The map editor ────────────────────────────────────────────────── */

int32_t okx_map_cells(uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    const GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || !wd->tnt.tile_map) return -1;
    int32_t W = wd->tnt.width_tiles, H = wd->tnt.height_tiles;
    if (w) *w = W;
    if (h) *h = H;
    int32_t need = W * H;
    if (out && cap >= need) memcpy(out, wd->tnt.tile_map, (size_t)need);
    return need;
}

int32_t okx_edit_cells(int32_t x0, int32_t z0, int32_t w, int32_t h, const uint8_t *values) {
    GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || !wd->tnt.tile_map || !wd->tnt.heightmap || !values || w <= 0 || h <= 0) return -1;
    const int W = wd->tnt.width_tiles, H = wd->tnt.height_tiles;
    if (x0 < 0 || z0 < 0 || x0 + w > W || z0 + h > H) { fail("edit off the map"); return -1; }
    /* The bytes live in the file image the map was read from, which is
     * what a save writes back out. */
    uint8_t *cells = (uint8_t *)(uintptr_t)wd->tnt.tile_map;
    for (int32_t z = 0; z < h; z++)
        memcpy(cells + (size_t)(z0 + z) * W + x0, values + (size_t)z * w, (size_t)w);
    /* The corner grid the ground is drawn and walked on copies the cell
     * bytes, the last row and column repeated, as the loader builds it. */
    for (int32_t z = z0; z <= z0 + h && z <= H; z++) {
        for (int32_t x = x0; x <= x0 + w && x <= W; x++) {
            int sx = x >= W ? W - 1 : x, sz = z >= H ? H - 1 : z;
            wd->tnt.heightmap[z * (W + 1) + x] = cells[sz * W + sx];
        }
    }
    return 0;
}

int32_t okx_chunk_library(uint32_t *ids, int32_t cap) {
    if (!g.ready) return 0;
    char **paths = NULL;
    int n = 0;
    if (VFS_ListFiles("terrain/*.jpg", &paths, &n) != 0 || !paths) return 0;
    int32_t found = 0;
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        char *end = NULL;
        unsigned long id = strtoul(base, &end, 16);
        if (end == base || (end && *end != '.')) continue;
        if (ids && found < cap) ids[found] = (uint32_t)id;
        found++;
    }
    for (int i = 0; i < n; i++) tak_free(paths[i]);
    tak_free(paths);
    return found;
}

int32_t okx_chunk_picture(uint32_t id, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    if (!g.ready) return -1;
    char path[64];
    snprintf(path, sizeof(path), "terrain/%08x.jpg", id);
    void *bytes = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &bytes, &size) != 0) return -1;
    uint32_t *pixels = NULL;
    int pw = 0, ph = 0;
    int rc = JPG_DecodeRGBA((const uint8_t *)bytes, (size_t)size, &pixels, &pw, &ph);
    tak_free(bytes);
    if (rc != 0 || !pixels) return -1;
    int32_t need = pw * ph * 4;
    if (w) *w = pw;
    if (h) *h = ph;
    if (out && cap >= need) memcpy(out, pixels, (size_t)need);
    tak_free(pixels);
    return need;
}

uint32_t okx_terrain_chunk_id(int32_t chunk) {
    const GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || !wd->grid || chunk < 0 || chunk >= wd->grid->chunk_count) return 0;
    return wd->grid->chunks[chunk].chunk_id;
}

int32_t okx_edit_blocks(int32_t bx, int32_t by, int32_t w, int32_t h,
                        const uint32_t *chunk_ids, const uint8_t *tex_x, const uint8_t *tex_y) {
    GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || !wd->tnt.block_chunk_ids || !chunk_ids || !tex_x || !tex_y || w <= 0 || h <= 0) return -1;
    const int BW = wd->tnt.blocks_w, BH = wd->tnt.blocks_h;
    if (bx < 0 || by < 0 || bx + w > BW || by + h > BH) { fail("paint off the map"); return -1; }
    uint32_t *ids = (uint32_t *)(uintptr_t)wd->tnt.block_chunk_ids;
    uint8_t *tx = (uint8_t *)(uintptr_t)wd->tnt.block_tex_x;
    uint8_t *ty = (uint8_t *)(uintptr_t)wd->tnt.block_tex_y;
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            size_t b = (size_t)(by + y) * BW + (bx + x);
            ids[b] = chunk_ids[y * w + x];
            tx[b] = tex_x[y * w + x];
            ty[b] = tex_y[y * w + x];
        }
    /* The grid indexes the distinct pictures, so a new one rebuilds it. */
    TerrainGrid *grid = TerrainGrid_Init(&wd->tnt);
    if (!grid) { fail("the ground table would not rebuild"); return -1; }
    TerrainGrid_Free(wd->grid, &g.plat);
    wd->grid = grid;
    return 0;
}

int32_t okx_feature_place(int32_t def, int32_t cx, int32_t cz) {
    GameWorld *wd = g.in_game ? World_Get() : NULL;
    const FeatureDef *fd = Features_GetByIndex(def);
    if (!wd || !fd) return -1;
    if (cx < 0 || cz < 0 || cx >= wd->tnt.width_tiles || cz >= wd->tnt.height_tiles) return -1;
    int fx = fd->footprint_x > 0 ? fd->footprint_x : 1;
    int fz = fd->footprint_z > 0 ? fd->footprint_z : 1;
    int idx = Features_AddInstance(wd, def, cx, cz, cx * 16 + fx * 8, cz * 16 + fz * 8, 0, -1);
    if (idx >= 0) {
        /* Scenery an editor places is the map's own: it never rots. */
        wd->features[idx].decompose_ticks = -1;
    }
    return idx;
}

int32_t okx_feature_remove(int32_t index) {
    GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || index < 0 || index >= wd->feature_count) return -1;
    return Features_RemoveInstance(wd, index);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A picture section of the source file (width, height, then a byte a
 * pixel) at the header offset `at`, as it stands, or an empty one. */
static size_t picture_section(const TNTFile *t, uint32_t at, const uint8_t **out) {
    *out = NULL;
    if (!t->raw || t->raw_size < 0x34) return 8;
    uint32_t off = get_u32(t->raw + at);
    if ((size_t)off + 8 > t->raw_size) return 8;
    size_t n = (size_t)get_u32(t->raw + off) * get_u32(t->raw + off + 4);
    if ((size_t)off + 8 + n > t->raw_size) return 8;
    *out = t->raw + off;
    return 8 + n;
}

/* The terrain file written afresh from the map as it stands, in the
 * layout the loader reads: the header, then the height bytes, the
 * feature layer, its names, the block tables and the two pictures. */
static uint8_t *write_tnt(const GameWorld *wd, size_t *out_size) {
    const TNTFile *t = &wd->tnt;
    const int W = t->width_tiles, H = t->height_tiles;
    const size_t cells = (size_t)W * H;
    const size_t nb = (size_t)t->blocks_w * t->blocks_h;

    /* The feature layer and the names it indexes, from the features
     * standing on the map now. */
    uint16_t *layer = (uint16_t *)tak_malloc(cells * 2);
    char (*names)[128] = (char (*)[128])tak_malloc(256 * 128);
    if (!layer || !names) { if (layer) tak_free(layer); if (names) tak_free(names); return NULL; }
    for (size_t i = 0; i < cells; i++) layer[i] = 0xFFFF;
    int nn = 0;
    for (int i = 0; i < wd->feature_count; i++) {
        const struct MapFeature *mf = &wd->features[i];
        const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
        if (!fd || mf->decompose_ticks >= 0 || mf->color_idx >= 0) continue;
        if (mf->tile_x >= W || mf->tile_z >= H) continue;
        int id = -1;
        for (int k = 0; k < nn; k++) if (tak_stricmp(names[k], fd->name) == 0) { id = k; break; }
        if (id < 0) {
            if (nn >= 256) continue;
            snprintf(names[nn], 128, "%s", fd->name);
            id = nn++;
        }
        int fx = fd->footprint_x > 0 ? fd->footprint_x : 1;
        int fz = fd->footprint_z > 0 ? fd->footprint_z : 1;
        for (int z = mf->tile_z; z < mf->tile_z + fz && z < H; z++)
            for (int x = mf->tile_x; x < mf->tile_x + fx && x < W; x++)
                if (layer[z * W + x] == 0xFFFF) layer[z * W + x] = 0xFFFB;
        layer[mf->tile_z * W + mf->tile_x] = (uint16_t)id;
    }

    const uint8_t *minimap = NULL, *overview = NULL;
    size_t sz_minimap = picture_section(t, 0x2C, &minimap);
    size_t sz_overview = picture_section(t, 0x30, &overview);
    size_t off = 0x34;
    size_t o_cells = off;           off += cells;
    size_t o_layer = off;           off += cells * 2;
    size_t o_names = off;           off += (size_t)nn * 132;
    size_t o_ids = off;             off += nb * 4;
    size_t o_tx = off;              off += nb;
    size_t o_ty = off;              off += nb;
    size_t o_minimap = off;         off += sz_minimap;
    size_t o_overview = off;        off += sz_overview;
    uint8_t *buf = (uint8_t *)tak_malloc(off);
    if (!buf) { tak_free(layer); tak_free(names); return NULL; }
    memset(buf, 0, off);
    put_u32(buf + 0x00, 0x4000);
    put_u32(buf + 0x04, (uint32_t)W);
    put_u32(buf + 0x08, (uint32_t)H);
    put_u32(buf + 0x0C, (uint32_t)t->sea_level);
    put_u32(buf + 0x10, (uint32_t)o_cells);
    put_u32(buf + 0x14, (uint32_t)o_layer);
    put_u32(buf + 0x18, (uint32_t)o_names);
    put_u32(buf + 0x1C, (uint32_t)nn);
    put_u32(buf + 0x20, (uint32_t)o_ids);
    put_u32(buf + 0x24, (uint32_t)o_tx);
    put_u32(buf + 0x28, (uint32_t)o_ty);
    put_u32(buf + 0x2C, (uint32_t)o_minimap);
    put_u32(buf + 0x30, (uint32_t)o_overview);
    if (t->tile_map) memcpy(buf + o_cells, t->tile_map, cells);
    for (size_t i = 0; i < cells; i++) {
        buf[o_layer + 2 * i] = (uint8_t)layer[i];
        buf[o_layer + 2 * i + 1] = (uint8_t)(layer[i] >> 8);
    }
    for (int k = 0; k < nn; k++) {
        put_u32(buf + o_names + (size_t)k * 132, (uint32_t)k);
        memcpy(buf + o_names + (size_t)k * 132 + 4, names[k], strlen(names[k]));
    }
    for (size_t b = 0; b < nb; b++) {
        put_u32(buf + o_ids + 4 * b, t->block_chunk_ids ? t->block_chunk_ids[b] : 0);
        buf[o_tx + b] = t->block_tex_x ? t->block_tex_x[b] : 0;
        buf[o_ty + b] = t->block_tex_y ? t->block_tex_y[b] : 0;
    }
    if (minimap) memcpy(buf + o_minimap, minimap, sz_minimap);
    if (overview) memcpy(buf + o_overview, overview, sz_overview);
    tak_free(layer);
    tak_free(names);
    *out_size = off;
    return buf;
}

static int copy_map_file(const char *key, const char *ext, const char *dir, const char *name) {
    char src[512];
    if (TAK_Maps_FindFile(key, ext, src, sizeof(src)) != 0) return 0;
    void *bytes = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(src, &bytes, &size) != 0 || !bytes) return 0;
    char dst[1024];
    snprintf(dst, sizeof(dst), "%s/%s.%s", dir, name, ext);
    FILE *f = fopen(dst, "wb");
    int ok = f && fwrite(bytes, 1, size, f) == size;
    if (f) fclose(f);
    tak_free(bytes);
    return ok ? 1 : -1;
}

int32_t okx_map_save(const char *name) {
    const GameWorld *wd = g.in_game ? World_Get() : NULL;
    if (!wd || !wd->tnt.raw) { fail("no map to save"); return -1; }
    if (!g.user_dir[0]) { fail("no user folder to save in"); return -1; }
    if (!name || !name[0]) { fail("no name"); return -1; }
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') { fail("a map name is a bare name"); return -1; }
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/maps", g.user_dir);
    (void)okx_mkdir(g.user_dir);
    (void)okx_mkdir(dir);
    size_t size = 0;
    uint8_t *tnt = write_tnt(wd, &size);
    if (!tnt) { fail("out of memory writing the map"); return -1; }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.tnt", dir, name);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(tnt, 1, size, f) == size;
    if (f) fclose(f);
    tak_free(tnt);
    if (!ok) { fail("could not write %s", path); return -1; }
    /* The description and the scenario come with it unchanged. */
    const char *key = s_map_key[0] ? s_map_key : wd->map_name;
    if (copy_map_file(key, "ota", dir, name) <= 0) { fail("the map's .ota did not copy"); return -1; }
    (void)copy_map_file(key, "crt", dir, name);
    (void)copy_map_file(key, "tdf", dir, name);
    /* The map list is read once, so a new map shows on the next ask. */
    if (g.maps) { TAK_Maps_Free(g.maps); g.maps = NULL; g.map_count = 0; }
    return 0;
}

/* ── Models ────────────────────────────────────────────────────────── */

static int add_texture(const GPU_Texture *gpu, uint32_t *rgba, int w, int h) {
    if (gpu) {
        for (int i = 0; i < g.texture_count; i++)
            if (g.textures[i].gpu == gpu) return i;
    }
    if (g.texture_count >= OKX_MAX_TEXTURES) {
        if (rgba) tak_free(rgba);
        return -1;
    }
    Texture *t = &g.textures[g.texture_count];
    t->gpu = gpu;
    t->rgba = rgba;
    t->w = w;
    t->h = h;
    if (gpu && GPU_TextureSize(gpu, &t->w, &t->h) != 0) return -1;
    return g.texture_count++;
}

/* Flat normals for a mesh that brought none: each triangle's face
 * normal summed onto its corners, as the 3D view's store does. */
static float *face_normals(const UnitMesh *m) {
    float *n = (float *)tak_malloc(sizeof(float) * 3 * (size_t)m->vert_count);
    if (!n) return NULL;
    memset(n, 0, sizeof(float) * 3 * (size_t)m->vert_count);
    for (int t = 0; t < m->tri_count; t++) {
        int i0 = m->indices[3 * t], i1 = m->indices[3 * t + 1], i2 = m->indices[3 * t + 2];
        const float *p0 = &m->positions[3 * i0], *p1 = &m->positions[3 * i1], *p2 = &m->positions[3 * i2];
        float ex = p1[0] - p0[0], ey = p1[1] - p0[1], ez = p1[2] - p0[2];
        float fx = p2[0] - p0[0], fy = p2[1] - p0[1], fz = p2[2] - p0[2];
        float nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len <= 0.0f) continue;
        int idx[3] = { i0, i1, i2 };
        for (int k = 0; k < 3; k++) {
            n[3 * idx[k] + 0] += nx / len;
            n[3 * idx[k] + 1] += ny / len;
            n[3 * idx[k] + 2] += nz / len;
        }
    }
    for (int v = 0; v < m->vert_count; v++) {
        float *p = &n[3 * v];
        float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len > 0.0f) { p[0] /= len; p[1] /= len; p[2] /= len; }
        else { p[1] = 1.0f; }
    }
    return n;
}

/* An override .glb from the host's folder, or NULL. */
static UnitMesh *load_override(const char *lower, int color, Model *m) {
    if (!g.override_dir[0]) return NULL;
    char path[640];
    snprintf(path, sizeof(path), "%s/%s.glb", g.override_dir, lower);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bytes = size > 0 ? (uint8_t *)tak_malloc((size_t)size) : NULL;
    if (!bytes || fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        if (bytes) tak_free(bytes);
        return NULL;
    }
    fclose(f);
    GltfModel *gm = NULL;
    int rc = Gltf_LoadFromMemoryEx(&gm, bytes, (size_t)size, GLTF_WITH_IMAGES);
    tak_free(bytes);
    if (rc != 0 || !gm) return NULL;
    GltfBatch bi[UNIT_MESH_MAX_BATCHES];
    memset(bi, 0, sizeof(bi));
    UnitMesh *src = Gltf_ToUnitMesh(gm, lower, Units_GetTeamColorRGBA(color), bi);
    if (src) {
        for (int b = 0; b < src->batch_count; b++) {
            int img = bi[b].base_image;
            m->batch_tex[b] = -1;
            if (img < 0 || img >= gm->image_count || !gm->images[img].rgba) continue;
            const GltfImage *pic = &gm->images[img];
            size_t bytes_n = (size_t)pic->w * (size_t)pic->h * 4;
            uint32_t *copy = (uint32_t *)tak_malloc(bytes_n);
            if (!copy) continue;
            memcpy(copy, pic->rgba, bytes_n);
            m->batch_tex[b] = add_texture(NULL, copy, pic->w, pic->h);
        }
        /* The script keeps piece state by the shipped model's node
         * order, and a node named like a shipped piece follows it. */
        UnitMesh *shipped = Units_BakeObjectMesh(lower, 0);
        if (shipped) {
            Gltf_MapPieces(src, shipped, m->piece_src);
            m->piece_src_count = shipped->node_count;
            Units_FreeBakedMesh(shipped);
        } else {
            for (int i = 0; i < UNIT_MESH_MAX_NODES; i++) m->piece_src[i] = -1;
        }
    }
    Gltf_Free(gm);
    return src;
}

int32_t okx_model_load(const char *object_name, int32_t color) {
    if (!g.in_game || !object_name || !object_name[0]) return -1;
    if (color < 0 || color > 11) color = 0;
    char lower[TAK_UNITDEF_OBJ_MAX];
    size_t n = 0;
    for (const char *p = object_name; *p && n + 1 < sizeof(lower); p++, n++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') return -1;
        lower[n] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    }
    lower[n] = 0;
    for (int i = 0; i < g.model_count; i++)
        if (g.models[i]->color == color && strcmp(g.models[i]->name, lower) == 0) return i;
    if (g.model_count >= OKX_MAX_MODELS) return -1;

    Model *m = (Model *)tak_malloc(sizeof(Model));
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", lower);
    m->color = color;
    m->mesh = load_override(lower, color, m);
    if (m->mesh) {
        m->from_gltf = 1;
    } else {
        m->mesh = Units_BakeObjectMesh(lower, color);
        if (m->mesh) {
            for (int b = 0; b < m->mesh->batch_count; b++) {
                const GPU_Texture *atlas = m->mesh->batches[b].atlas_tex;
                m->batch_tex[b] = atlas ? add_texture(atlas, NULL, 0, 0) : -1;
            }
        }
    }
    if (!m->mesh || m->mesh->vert_count <= 0 || m->mesh->tri_count <= 0) {
        if (m->mesh) {
            if (m->from_gltf) Gltf_FreeUnitMesh(m->mesh);
            else Units_FreeBakedMesh(m->mesh);
        }
        tak_free(m);
        return -1;
    }
    if (!m->mesh->normals) m->normals = face_normals(m->mesh);
    g.models[g.model_count] = m;
    return g.model_count++;
}

static const Model *model_at(int32_t id) {
    return (id >= 0 && id < g.model_count) ? g.models[id] : NULL;
}

int32_t okx_model_info(int32_t model, OkxModelInfo *out) {
    const Model *m = model_at(model);
    if (!m || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->vert_count = m->mesh->vert_count;
    out->index_count = m->mesh->tri_count * 3;
    out->node_count = m->mesh->node_count;
    out->batch_count = m->mesh->batch_count;
    memcpy(out->aabb_min, m->mesh->aabb_min, sizeof(out->aabb_min));
    memcpy(out->aabb_max, m->mesh->aabb_max, sizeof(out->aabb_max));
    out->scale = Units_GetTAScale();
    out->from_override = m->from_gltf;
    return 0;
}

int32_t okx_model_geometry(int32_t model, float *positions, float *normals, float *uvs,
                           uint32_t *colors, int32_t *nodes, int32_t *indices) {
    const Model *m = model_at(model);
    if (!m) return -1;
    const UnitMesh *s = m->mesh;
    const int V = s->vert_count;
    if (positions) memcpy(positions, s->positions, sizeof(float) * 3 * (size_t)V);
    if (normals) {
        const float *src = s->normals ? s->normals : m->normals;
        if (src) memcpy(normals, src, sizeof(float) * 3 * (size_t)V);
    }
    if (uvs) memcpy(uvs, s->uvs, sizeof(float) * 2 * (size_t)V);
    if (colors) memcpy(colors, s->colors, sizeof(uint32_t) * (size_t)V);
    if (nodes) for (int v = 0; v < V; v++) nodes[v] = s->vert_node_idx[v];
    if (indices) for (int i = 0; i < s->tri_count * 3; i++) indices[i] = s->indices[i];
    return 0;
}

int32_t okx_model_nodes(int32_t model, OkxNode *out, int32_t cap) {
    const Model *m = model_at(model);
    if (!m) return -1;
    for (int i = 0; out && i < m->mesh->node_count && i < cap; i++) {
        const UnitMeshNode *nd = &m->mesh->nodes[i];
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].name, sizeof(out[i].name), "%s", nd->name);
        out[i].parent = nd->parent;
        memcpy(out[i].offset, nd->offset, sizeof(out[i].offset));
    }
    return m->mesh->node_count;
}

int32_t okx_model_batches(int32_t model, OkxBatch *out, int32_t cap) {
    const Model *m = model_at(model);
    if (!m) return -1;
    for (int b = 0; out && b < m->mesh->batch_count && b < cap; b++) {
        out[b].first_index = m->mesh->batches[b].first_index;
        out[b].index_count = m->mesh->batches[b].index_count;
        out[b].texture = m->batch_tex[b];
    }
    return m->mesh->batch_count;
}

int32_t okx_texture(int32_t texture, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    if (texture < 0 || texture >= g.texture_count) return -1;
    const Texture *t = &g.textures[texture];
    const uint32_t *px = t->rgba ? t->rgba : GPU_TexturePixels(t->gpu);
    if (!px) return -1;
    int32_t need = t->w * t->h * 4;
    if (w) *w = t->w;
    if (h) *h = t->h;
    if (out && cap >= need) memcpy(out, px, (size_t)need);
    return need;
}

/* ── The frame ─────────────────────────────────────────────────────── */

/* The 3D view's model matrix: heading about y, pitch, roll, the model
 * scale and the models' mirror in x. Column major. */
static void model_matrix(float out[16], float x, float y, float z,
                         float heading, float pitch, float roll, float scale) {
    const float ch = cosf(heading), sh = sinf(heading);
    const float cp = cosf(pitch),   sp = sinf(pitch);
    const float cr = cosf(roll),    sr = sinf(roll);
    for (int col = 0; col < 3; col++) {
        float mx = col == 0 ? 1.0f : 0.0f;
        float my = col == 1 ? 1.0f : 0.0f;
        float mz = col == 2 ? 1.0f : 0.0f;
        float ax = cr * mx - sr * my;
        float ay = sr * mx + cr * my;
        float by = cp * ay - sp * mz;
        float bz = sp * ay + cp * mz;
        float rx = -(ch * ax + sh * bz);
        float rz = -(sh * ax - ch * bz);
        out[col * 4 + 0] = rx * scale;
        out[col * 4 + 1] = by * scale;
        out[col * 4 + 2] = rz * scale;
        out[col * 4 + 3] = 0.0f;
    }
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1.0f;
}

static CobPiece s_remap[UNIT_MESH_MAX_NODES];
static UnitNodeXform s_xf[UNIT_MESH_MAX_NODES];

/* Node poses composed from piece state, times the model matrix, as
 * row major 3x4 matrices. */
static int32_t write_pose(const Model *m, const CobPiece *pieces, int pieces_count,
                          int all_pieces, const float mm[16],
                          float *out, uint8_t *hidden, int32_t cap) {
    int n = m->mesh->node_count;
    if (n > UNIT_MESH_MAX_NODES) n = UNIT_MESH_MAX_NODES;
    if (m->from_gltf && pieces) {
        if (m->piece_src_count <= 0 || pieces_count < m->piece_src_count) {
            pieces = NULL;
        } else {
            memset(s_remap, 0, sizeof(CobPiece) * (size_t)n);
            for (int j = 0; j < n; j++) {
                int src = m->piece_src[j];
                if (src >= 0 && src < pieces_count) s_remap[j] = pieces[src];
            }
            pieces = s_remap;
        }
    }
    Units_ComposeNodeXforms(m->mesh, pieces, s_xf, !all_pieces);
    for (int i = 0; i < n && i < cap; i++) {
        const UnitNodeXform *x = &s_xf[i];
        float *o = out ? out + (size_t)i * 12 : NULL;
        if (o) {
            for (int r = 0; r < 3; r++) {
                /* world = M * (R v + t): M's row r against R's columns. */
                for (int c = 0; c < 3; c++) {
                    float s = 0.0f;
                    for (int k = 0; k < 3; k++) s += mm[k * 4 + r] * x->rot[k * 3 + c];
                    o[r * 4 + c] = s;
                }
                float t = mm[12 + r];
                for (int k = 0; k < 3; k++) t += mm[k * 4 + r] * x->trans[k];
                o[r * 4 + 3] = t;
            }
        }
        if (hidden) hidden[i] = x->hidden;
    }
    return n;
}

static int unit_drawn(const Unit *u) {
    if (u->alive != UNIT_ALIVE_ACTIVE && u->alive != UNIT_ALIVE_DYING) return 0;
    if (u->under_construction && u->max_health > 0 && u->health * 2 < u->max_health) return 0;
    return Units_IsVisibleToLocalPlayer(u);
}

int32_t okx_units(OkxUnit *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return 0;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    int32_t n = 0;
    for (int i = 0; i < count; i++) {
        const Unit *u = &units[i];
        if (!unit_drawn(u)) continue;
        const UnitDef *def = Units_GetDef(u->def_idx);
        if (!def) continue;
        if (out && n < cap) {
            OkxUnit *o = &out[n];
            o->handle = i;
            o->stable_id = u->stable_id;
            o->def = u->def_idx;
            o->player = u->player_id;
            o->color = u->team_color_idx;
            o->state = u->alive == UNIT_ALIVE_ACTIVE ? OKX_UNIT_ACTIVE : OKX_UNIT_DYING;
            o->x = (float)u->world_x;
            o->z = (float)u->world_y;
            o->y = (float)Terrain_SampleHeight(w, u->world_x, u->world_y) + u->flight_alt;
            o->heading = u->heading;
            o->pitch = u->pitch;
            o->roll = u->roll;
            o->health = u->health;
            o->max_health = u->max_health;
            o->building = u->under_construction ? 1 : 0;
            o->model = okx_model_load(def->objectname, u->team_color_idx);
        }
        n++;
    }
    return n;
}

int32_t okx_unit_pose(int32_t handle, float *matrices, uint8_t *hidden, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return -1;
    int count = 0;
    const Unit *units = Units_GetActive(&count);
    if (handle < 0 || handle >= count) return -1;
    const Unit *u = &units[handle];
    const UnitDef *def = Units_GetDef(u->def_idx);
    if (!def) return -1;
    const Model *m = model_at(okx_model_load(def->objectname, u->team_color_idx));
    if (!m) return -1;
    float y = (float)Terrain_SampleHeight(w, u->world_x, u->world_y) + u->flight_alt;
    float mm[16];
    model_matrix(mm, (float)u->world_x, y, (float)u->world_y,
                 u->heading, u->pitch, u->roll, Units_GetTAScale());
    return write_pose(m, u->cob ? u->cob->pieces : NULL, u->cob ? u->cob->piece_count : 0,
                      0, mm, matrices, hidden, cap);
}

/* Where a feature stands and, for a model, which one. */
static int feature_place(const GameWorld *w, int i, OkxFeature *o) {
    const struct MapFeature *mf = &w->features[i];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    if (!fd) return 0;
    memset(o, 0, sizeof(*o));
    o->index = i;
    o->def = mf->global_idx;
    o->model = -1;
    o->sprite = -1;
    const float to_rad = 6.2831853f / 65536.0f;
    if (fd->object[0]) {
        int c = (mf->color_idx >= 0 && mf->color_idx <= 11) ? mf->color_idx : 0;
        o->model = okx_model_load(fd->object, c);
        if (o->model < 0) return 0;
        o->x = (float)mf->world_x;
        o->z = (float)mf->world_y;
        o->y = (float)Terrain_SampleHeight(w, mf->world_x, mf->world_y);
        o->heading = (float)mf->heading * to_rad;
        o->pitch = (float)mf->pitch * to_rad;
        o->roll = (float)mf->roll * to_rad;
        return 1;
    }
    int fp_x = fd->footprint_x > 0 ? fd->footprint_x : 1;
    int fp_z = fd->footprint_z > 0 ? fd->footprint_z : 1;
    int32_t wx = mf->tile_x * 16 + fp_x * 8;
    int32_t wy = mf->tile_z * 16 + fp_z * 8;
    const uint32_t *px = NULL;
    int sw = 0, sh = 0, ox = 0, oy = 0;
    if (Units_FeatureSpriteFrame(fd, w->features_rgba, 0, &px, &sw, &sh, &ox, &oy) <= 0 || !px)
        return 0;
    float hgt = (float)Terrain_SampleHeight(w, wx, wy);
    o->sprite = mf->global_idx;
    o->x = (float)wx;
    o->z = (float)wy;
    o->y = hgt;
    o->flat = tak_stricmp(fd->category, "mana") == 0;
    if (o->flat) {
        o->bottom = hgt + 0.5f;
        o->top = o->bottom + (float)sh / Units_GetTanTilt();
    } else {
        o->top = hgt + (float)oy * OKX_SPRITE_RISE;
        o->bottom = hgt - (float)(sh - oy) * 0.5f;
    }
    o->off_x = (float)ox;
    o->w = (float)sw;
    return 1;
}

int32_t okx_features(OkxFeature *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || !w->features) return 0;
    int32_t n = 0;
    OkxFeature tmp;
    for (int i = 0; i < w->feature_count; i++) {
        OkxFeature *o = (out && n < cap) ? &out[n] : &tmp;
        if (feature_place(w, i, o)) n++;
    }
    return n;
}

int32_t okx_projectiles(OkxProjectile *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return 0;
    int pn = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    int32_t n = 0;
    for (int i = 0; i < pn; i++) {
        const Projectile *p = &ps[i];
        if (!p->alive || p->hidden) continue;
        if (!Units_ProjectileVisible(w, p)) continue;
        if (out && n < cap) {
            OkxProjectile *o = &out[n];
            memset(o, 0, sizeof(*o));
            o->id = i;
            o->player = p->player_id;
            o->color = p->color_idx > 11 ? 0 : p->color_idx;
            o->model = -1;
            o->kind = p->is_beam ? OKX_PROJ_BEAM
                    : p->art_kind == UNIT_WEAPON_ART_MODEL ? OKX_PROJ_MODEL
                    : p->art_kind == UNIT_WEAPON_ART_SPRITE ? OKX_PROJ_SPRITE : OKX_PROJ_DOT;
            if (o->kind == OKX_PROJ_MODEL && p->art_idx >= 0) {
                const char *name = Units_ProjectileModelName(p->art_idx);
                if (name) o->model = okx_model_load(name, o->color);
                if (o->model < 0) o->kind = OKX_PROJ_DOT;
            }
            o->x = (float)p->world_x;
            o->y = p->height;
            o->z = (float)p->world_y;
            o->vx = p->dir_x * p->speed_ppt;
            o->vy = p->vel_up_ppt;
            o->vz = p->dir_y * p->speed_ppt;
            o->heading = p->heading;
            o->pitch = p->pitch;
            o->roll = p->roll;
            if (p->is_beam) {
                o->from_x = (float)p->src_x;
                o->from_y = (float)p->src_height;
                o->from_z = (float)p->src_y;
                o->x = (float)p->dest_x;
                o->z = (float)p->dest_y;
                o->y = (float)Terrain_SampleHeight(w, p->dest_x, p->dest_y);
            }
        }
        n++;
    }
    return n;
}

/* A frame of a strip at a point, anchored the way the classic blit
 * anchors it, as the 3D view's put_billboard does. */
static int effect_frame(OkxEffect *o, int sprite, int frame, float x, float y, float z) {
    ProjSpriteStrip st;
    if (Units_ProjectileSpriteStrip(sprite, &st) <= 0 || st.num_frames <= 0) return 0;
    if (frame < 0 || frame >= st.num_frames) return 0;
    if (st.fw[frame] <= 0 || st.fh[frame] <= 0) return 0;
    float sw = (float)(st.cell_w * st.num_frames);
    o->sprite = sprite;
    o->frame = frame;
    o->x = x;
    o->y = y;
    o->z = z;
    o->off_x = (float)st.ox[frame];
    o->w = (float)st.fw[frame];
    o->top = y + (float)st.oy[frame];
    o->bottom = o->top - (float)st.fh[frame];
    o->u0 = (float)(frame * st.cell_w) / sw;
    o->u1 = (float)(frame * st.cell_w + st.fw[frame]) / sw;
    o->v1 = (float)st.fh[frame] / (float)st.cell_h;
    return 1;
}

int32_t okx_effects(OkxEffect *out, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w) return 0;
    int32_t n = 0;
    OkxEffect tmp;
    int en = 0;
    const ProjectileEffect *es = Units_GetProjectileEffects(&en);
    for (int i = 0; i < en; i++) {
        const ProjectileEffect *e = &es[i];
        if (!e->alive || e->delay_ticks) continue;
        if (!Fog_ShowsAt(w, e->world_x, e->world_y)) continue;
        ProjSpriteStrip st;
        if (Units_ProjectileSpriteStrip(e->sprite_idx, &st) <= 0) continue;
        int nf = st.num_frames;
        int frame = e->age_ticks / (e->ticks_per_frame ? e->ticks_per_frame : 2);
        if (frame >= nf) {
            if (!e->loops || nf <= 0) continue;
            frame %= nf;
        }
        OkxEffect *o = (out && n < cap) ? &out[n] : &tmp;
        memset(o, 0, sizeof(*o));
        o->kind = OKX_EFFECT_IMPACT;
        o->id = i;
        if (effect_frame(o, e->sprite_idx, frame, (float)e->world_x, (float)e->height, (float)e->world_y)) n++;
    }
    int pn = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    for (int i = 0; i < pn; i++) {
        const Projectile *p = &ps[i];
        if (!p->alive || p->is_beam || p->hidden) continue;
        if (p->art_kind != UNIT_WEAPON_ART_SPRITE || p->art_idx < 0) continue;
        if (!Units_ProjectileVisible(w, p)) continue;
        ProjSpriteStrip st;
        if (Units_ProjectileSpriteStrip(p->art_idx, &st) <= 0 || st.num_frames <= 0) continue;
        int frame = st.num_frames > 1 ? (int)((p->age_ticks / 2) % (uint16_t)st.num_frames) : 0;
        OkxEffect *o = (out && n < cap) ? &out[n] : &tmp;
        memset(o, 0, sizeof(*o));
        o->kind = OKX_EFFECT_PROJECTILE;
        o->id = i;
        if (effect_frame(o, p->art_idx, frame, (float)p->world_x, p->height, (float)p->world_y)) n++;
    }
    return n;
}

int32_t okx_effect_strip(int32_t sprite, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    if (!g.in_game) return -1;
    ProjSpriteStrip st;
    if (Units_ProjectileSpriteStrip(sprite, &st) <= 0 || !st.pixels) return -1;
    int32_t sw = st.cell_w * st.num_frames, sh = st.cell_h;
    int32_t need = sw * sh * 4;
    if (w) *w = sw;
    if (h) *h = sh;
    if (out && cap >= need) memcpy(out, st.pixels, (size_t)need);
    return need;
}

int32_t okx_projectile_pose(int32_t id, float *matrices, int32_t cap) {
    if (!g.in_game) return -1;
    int pn = 0;
    const Projectile *ps = Units_GetProjectiles(&pn);
    if (id < 0 || id >= pn || !ps[id].alive || ps[id].art_kind != UNIT_WEAPON_ART_MODEL) return -1;
    const Projectile *p = &ps[id];
    const char *name = Units_ProjectileModelName(p->art_idx);
    const Model *m = name ? model_at(okx_model_load(name, p->color_idx > 11 ? 0 : p->color_idx)) : NULL;
    if (!m) return -1;
    float mm[16];
    model_matrix(mm, (float)p->world_x, p->height, (float)p->world_y,
                 p->heading, p->pitch, p->roll, Units_GetTAScale());
    return write_pose(m, NULL, 0, 1, mm, matrices, NULL, cap);
}

int32_t okx_feature_pose(int32_t index, float *matrices, int32_t cap) {
    const GameWorld *w = g.in_game ? World_Get() : NULL;
    if (!w || index < 0 || index >= w->feature_count) return -1;
    OkxFeature f;
    if (!feature_place(w, index, &f) || f.model < 0) return -1;
    float mm[16];
    model_matrix(mm, f.x, f.y, f.z, f.heading, f.pitch, f.roll, Units_GetTAScale());
    return write_pose(model_at(f.model), NULL, 0, 1, mm, matrices, NULL, cap);
}

int32_t okx_sprite(int32_t def, uint8_t *out, int32_t cap, int32_t *w, int32_t *h) {
    const GameWorld *wd = g.in_game ? World_Get() : NULL;
    const FeatureDef *fd = Features_GetByIndex(def);
    if (!wd || !fd) return -1;
    const uint32_t *px = NULL;
    int sw = 0, sh = 0, ox = 0, oy = 0;
    if (Units_FeatureSpriteFrame(fd, wd->features_rgba, 0, &px, &sw, &sh, &ox, &oy) <= 0 || !px)
        return -1;
    int32_t need = sw * sh * 4;
    if (w) *w = sw;
    if (h) *h = sh;
    if (out && cap >= need) memcpy(out, px, (size_t)need);
    return need;
}

/* ── The studio ────────────────────────────────────────────────────── */

static struct {
    CobEngine *e;
    int        def, color, ticks;
    int        slot;             /* the animation's thread, -1 for none */
    char       script[64];
} s_studio = { NULL, -1, -1, 0, -1, "" };

static void studio_release(void) {
    if (s_studio.e) {
        Cob_EngineFree(s_studio.e);
        tak_free(s_studio.e);
    }
    s_studio.e = NULL;
    s_studio.def = s_studio.color = -1;
    s_studio.ticks = 0;
    s_studio.slot = -1;
    s_studio.script[0] = 0;
}

/* A unit with nothing to do: every port reads 0 and every engine call
 * does nothing, except that a unit playing a walk is moving at full
 * speed, which its script's move watcher waits for. The VM asks ports
 * through the call hook, with the port as the function id. */
static int s_studio_moving;

static int32_t studio_port(int port) {
    if (!s_studio_moving) return 0;
    if (port == 29) return 100;   /* CURRENT_SPEED, over the watchers' threshold */
    if (port == 33) return 40;    /* the movement scalar at full speed */
    return 0;
}

static int32_t studio_query(void *user, int param) {
    (void)user;
    return studio_port(param);
}

static int32_t studio_call(void *user, int fn_id, int n_args, const int32_t *args) {
    (void)user; (void)n_args; (void)args;
    return studio_port(fn_id);
}

int32_t okx_def_scripts(int32_t def, char *out, int32_t cap) {
    const UnitDef *d = g.in_game ? Units_GetDef(def) : NULL;
    if (!d || !d->cob_script || !out || cap <= 0) return -1;
    int32_t len = 0;
    out[0] = 0;
    for (uint16_t i = 0; i < d->cob_script->num_scripts; i++) {
        const char *name = d->cob_script->script_names[i];
        int32_t n = (int32_t)strlen(name);
        if (len + n + 2 > cap) break;
        memcpy(out + len, name, (size_t)n);
        len += n;
        out[len++] = '\n';
        out[len] = 0;
    }
    return len;
}

int32_t okx_studio_pose(int32_t def, int32_t color, const char *script,
                        int32_t ticks, float *matrices, uint8_t *hidden, int32_t cap) {
    const UnitDef *d = g.in_game ? Units_GetDef(def) : NULL;
    if (!d || ticks < 0) return -1;
    if (color < 0 || color > 11) color = 0;
    const Model *m = model_at(okx_model_load(d->objectname, color));
    if (!m) return -1;
    const char *want = script ? script : "";
    if (s_studio.def != def || s_studio.color != color || !s_studio.e ||
        strcmp(s_studio.script, want) != 0 || ticks < s_studio.ticks) {
        studio_release();
        s_studio_moving = tak_strnicmp(want, "walk", 4) == 0;
        if (d->cob_script && !m->from_gltf) {
            const char *names[UNIT_MESH_MAX_NODES];
            int nc = m->mesh->node_count < UNIT_MESH_MAX_NODES ? m->mesh->node_count : UNIT_MESH_MAX_NODES;
            for (int i = 0; i < nc; i++) names[i] = m->mesh->nodes[i].name;
            s_studio.e = (CobEngine *)tak_malloc(sizeof(CobEngine));
            if (s_studio.e && Cob_EngineInit(s_studio.e, d->cob_script, nc, names) == 0) {
                Cob_EngineSetHost(s_studio.e, NULL, studio_query, studio_call);
                Cob_StartThreadByName(s_studio.e, "Create", NULL, 0);
                Cob_RunAllThreads(s_studio.e);
                if (want[0]) s_studio.slot = Cob_StartThreadByName(s_studio.e, want, NULL, 0);
            } else if (s_studio.e) {
                tak_free(s_studio.e);
                s_studio.e = NULL;
            }
        }
        s_studio.def = def;
        s_studio.color = color;
        snprintf(s_studio.script, sizeof(s_studio.script), "%s", want);
    }
    /* At most a minute of animation a call. A cycle such as walk ends
     * after one stride and the engine starts it again while the unit
     * moves, so the studio loops it the same way. */
    int budget = 3600;
    while (s_studio.e && s_studio.ticks < ticks && budget-- > 0) {
        if (want[0] && (s_studio.slot < 0 || !Cob_IsThreadAlive(s_studio.e, s_studio.slot)))
            s_studio.slot = Cob_StartThreadByName(s_studio.e, want, NULL, 0);
        Cob_AnimatePieces(s_studio.e);
        Cob_RunAllThreads(s_studio.e);
        s_studio.ticks++;
    }
    float mm[16];
    model_matrix(mm, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, Units_GetTAScale());
    return write_pose(m, s_studio.e ? s_studio.e->pieces : NULL,
                      s_studio.e ? s_studio.e->piece_count : 0, 0, mm, matrices, hidden, cap);
}

int32_t okx_feature_def_count(void) { return Features_GetCount(); }

int32_t okx_feature_def_info(int32_t def, OkxFeatureDefInfo *out) {
    const FeatureDef *fd = Features_GetByIndex(def);
    if (!fd || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", fd->name);
    snprintf(out->object, sizeof(out->object), "%s", fd->object);
    snprintf(out->seqname, sizeof(out->seqname), "%s", fd->seqname);
    snprintf(out->category, sizeof(out->category), "%s", fd->category);
    out->footprint_x = fd->footprint_x;
    out->footprint_z = fd->footprint_z;
    out->height = fd->height;
    return 0;
}
