/*
 * ok_embed.h -- the engine as a library for a host such as Unity.
 *
 * The host loads okengine, points it at a game install, starts a
 * skirmish, ticks it and reads back what to draw: terrain, models with
 * their piece hierarchy and textures, and every unit and feature each
 * frame with its pose from the unit scripts. Orders go in through the
 * engine's own command queue. Plain C types and caller-owned buffers
 * only, so a C# P/Invoke mirror is blittable.
 *
 * Coordinates are the engine's: x east and z south in world pixels
 * (16 per cell), y up in the same pixels. A pose matrix maps a node's
 * local vertices to world pixels and carries the models' mirror in x,
 * so its determinant is negative.
 *
 * The engine runs headless inside the host: SDL on its dummy video
 * driver with a software renderer, no window, no sound.
 */
#ifndef OK_EMBED_H
#define OK_EMBED_H

#include <stdint.h>

#if defined(_WIN32) && defined(OKX_SHARED)
#  if defined(OKX_BUILD)
#    define OKX_API __declspec(dllexport)
#  else
#    define OKX_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(OKX_SHARED)
#  define OKX_API __attribute__((visibility("default")))
#else
#  define OKX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a function or struct below changes shape. */
#define OKX_API_VERSION 7

OKX_API int32_t okx_api_version(void);

/* ── Lifetime ──────────────────────────────────────────────────────── */

/* game_dir holds the .hpi archives, data_dir is an optional folder of
 * loose files that wins over them (NULL or "" for none). 0 on success. */
OKX_API int32_t okx_init(const char *game_dir, const char *data_dir);
OKX_API void    okx_shutdown(void);
/* What the last call that failed said, never NULL. */
OKX_API const char *okx_last_error(void);

/* A folder of .glb models that replace the shipped ones by name: a
 * unit's or feature's object name, or a sprite feature's sequence name,
 * lower case. Asked before the game files. NULL or "" for none. */
OKX_API void    okx_set_override_dir(const char *dir);

/* The engine's own sound and music, played straight to the audio
 * device. volume is 0 to 127, music 1 to play the battle's side list.
 * Call before starting a skirmish so the music follows the player's
 * side. enable 0 stops it all. 0 on success. */
OKX_API int32_t okx_audio(int32_t enable, int32_t volume, int32_t music);

/* ── Maps and unit types ───────────────────────────────────────────── */

OKX_API int32_t okx_map_count(void);
/* The map's name into out. Returns its length, or -1. */
OKX_API int32_t okx_map_name(int32_t index, char *out, int32_t cap);

typedef struct OkxMapInfo {
    char    name[96];
    char    description[256];
    char    kingdom[16];      /* the map's own, which picks its palettes */
    int32_t size_x, size_y;   /* the .ota's size, in its own units */
    int32_t max_players;
    int32_t player_counts[8]; /* every lineup the map supports */
    int32_t player_count_n;
} OkxMapInfo;

/* What the skirmish lobby shows for a map. 0 on success. */
OKX_API int32_t okx_map_info(int32_t index, OkxMapInfo *out);
/* The map's overview picture, RGBA, as okx_texture. */
OKX_API int32_t okx_map_preview(int32_t index, uint8_t *out, int32_t cap,
                                int32_t *w, int32_t *h);

typedef struct OkxDefInfo {
    char    name[32];         /* UnitName, the engine's key */
    char    object[32];       /* the 3DO it draws */
    char    side[16];
    char    category[64];
    char    description[64];
    char    display_name[64];
    int32_t max_health;
    int32_t is_building;
    int32_t footprint_x, footprint_z;   /* cells */
    int32_t build_cost;       /* mana */
} OkxDefInfo;

/* Unit types known once a skirmish has loaded. */
OKX_API int32_t okx_def_count(void);
OKX_API int32_t okx_def_info(int32_t def, OkxDefInfo *out);
/* The defs a builder can make, in menu order. Returns how many. */
OKX_API int32_t okx_def_buildables(int32_t def, int32_t *out, int32_t cap);
/* The def's unit script functions, one name a line, into out. Returns
 * the length written, or -1 for a def without a script. */
OKX_API int32_t okx_def_scripts(int32_t def, char *out, int32_t cap);

/* A studio pose outside the battle: the def's script runs Create, then
 * the named function (NULL or "" for none), for ticks simulation ticks,
 * and the pose is written as okx_unit_pose does, for the model standing
 * at the origin facing south. Asking again with more ticks carries on
 * from where the last call stopped. Returns the node count, or -1. */
OKX_API int32_t okx_studio_pose(int32_t def, int32_t color, const char *script,
                                int32_t ticks, float *matrices, uint8_t *hidden,
                                int32_t cap);

/* ── The battle ────────────────────────────────────────────────────── */

typedef struct OkxSkirmish {
    char    map[96];
    char    kingdom[16];      /* the local player's: aramon, veruna, taros, zhon, creon */
    int32_t ai_players;       /* computer opponents, 0 to 4 */
    int32_t line_of_sight;    /* the lobby checkbox */
    int32_t map_revealed;
    uint32_t seed;            /* 0 lets the engine pick */
} OkxSkirmish;

/* Load a map with its armies and stand ready at tick 0. 0 on success. */
OKX_API int32_t okx_start_skirmish(const OkxSkirmish *cfg);

/* The same load in slices, for a loading screen. okx_load_begin starts
 * it, and each okx_load_step works for up to max_ms and reports how far
 * it got, 0 to 1, and what it is doing. It returns 1 when the battle is
 * ready, 0 while loading, -1 when the load failed. */
OKX_API int32_t okx_load_begin(const OkxSkirmish *cfg);
OKX_API int32_t okx_load_step(int32_t max_ms, float *progress, char *status, int32_t cap);
OKX_API void    okx_end_game(void);

/* Simulation ticks per second of game time. */
OKX_API int32_t okx_tick_rate(void);
/* Run n simulation ticks. Returns how many ran. */
OKX_API int32_t okx_tick(int32_t n);
OKX_API uint32_t okx_tick_count(void);
OKX_API int32_t okx_local_player(void);
/* 0 while the battle runs, 1 won, -1 lost, 2 over with no winner. */
OKX_API int32_t okx_outcome(void);

/* Where the player is looking: the centre and size of the view in
 * world pixels. Sounds are placed and faded by it, as the classic view
 * places them by its own. */
OKX_API void    okx_set_view(int32_t cx, int32_t cy, int32_t w, int32_t h);

typedef struct OkxPlayer {
    int32_t index;            /* 1 based, as unit.player */
    int32_t kind;             /* 1 human, 2 computer */
    int32_t side;             /* 0 aramon, 1 taros, 2 veruna, 3 zhon, 7 creon */
    int32_t team, color;
    int32_t alive;            /* has units */
    char    name[32];
} OkxPlayer;

/* The seats in play. Returns how many. */
OKX_API int32_t okx_players(OkxPlayer *out, int32_t cap);

typedef struct OkxEconomy {
    float   mana;
    int32_t max_mana;
    float   income;           /* per second */
    int32_t earned_last_sec, spent_last_sec;
} OkxEconomy;

OKX_API int32_t okx_economy(int32_t player, OkxEconomy *out);

/* An order for one unit, through the engine's command queue: type is
 * a TAK_CMD_* value, target a unit handle or -1, x and y world pixels.
 * Applied on the tick its turn comes round. 0 when queued. */
OKX_API int32_t okx_command(int32_t type, int32_t handle, int32_t x, int32_t y,
                            int32_t target, int32_t build_def, int32_t arg);

/* Where a building of def would stand for a site at x, y: the site
 * snapped to the cell grid as the game places it, into sx, sy. 1 when
 * it can be built there, 0 when blocked. */
OKX_API int32_t okx_build_site(int32_t def, int32_t x, int32_t y, int32_t *sx, int32_t *sy);

/* A factory's queue: how many of def are queued or in progress, or all
 * of them for def -1. */
OKX_API int32_t okx_factory_queue(int32_t handle, int32_t def);

enum {
    OKX_ORDER_NONE = 0, OKX_ORDER_MOVE, OKX_ORDER_ATTACK, OKX_ORDER_BUILD,
    OKX_ORDER_PATROL, OKX_ORDER_GUARD, OKX_ORDER_REPAIR, OKX_ORDER_RECLAIM,
    OKX_ORDER_LOAD, OKX_ORDER_UNLOAD, OKX_ORDER_ATTACK_GROUND, OKX_ORDER_RESURRECT,
    OKX_ORDER_BOARD
};

typedef struct OkxOrder {
    int32_t kind;          /* OKX_ORDER_* */
    int32_t target;        /* a unit handle, or -1 */
    int32_t x, y;          /* where, world pixels */
    int32_t building;      /* the building a builder works on, or -1 */
} OkxOrder;

/* What a unit is doing now. 0 on success. */
OKX_API int32_t okx_unit_order(int32_t handle, OkxOrder *out);

/* The local player's fog, one byte a 16 pixel cell, row by row from
 * the north: 0 never seen, 1 seen before, 2 in sight now. With out NULL
 * it only reports the size. Returns the bytes it needs. */
OKX_API int32_t okx_fog(uint8_t *out, int32_t cap, int32_t *w, int32_t *h);

/* ── Terrain ───────────────────────────────────────────────────────── */

typedef struct OkxTerrainInfo {
    int32_t map_w, map_h;         /* world pixels */
    int32_t heights_w, heights_h; /* height samples, one per tile corner */
    int32_t tile_px;              /* pixels between height samples */
    int32_t blocks_w, blocks_h;   /* texture blocks */
    int32_t block_px;             /* world pixels a block covers */
    int32_t sub_px;               /* texels a block takes from its chunk */
    int32_t chunk_count;          /* chunk pictures */
    int32_t water_height;         /* sea level in pixels, 0 for none */
} OkxTerrainInfo;

OKX_API int32_t okx_terrain_info(OkxTerrainInfo *out);
/* heights_w * heights_h floats, row by row from the north. */
OKX_API int32_t okx_terrain_heights(float *out, int32_t cap);
/* Three ints per block, row by row: chunk, sub square x, sub square y. */
OKX_API int32_t okx_terrain_blocks(int32_t *out, int32_t cap);
/* One chunk picture, RGBA. With out NULL it only reports the size.
 * Returns the bytes it needs, or -1. */
OKX_API int32_t okx_terrain_chunk(int32_t chunk, uint8_t *out, int32_t cap,
                                  int32_t *w, int32_t *h);
/* The ground height the engine uses, at a point in world pixels. */
OKX_API float   okx_ground_height(float x, float z);

/* ── Models ────────────────────────────────────────────────────────── */

typedef struct OkxModelInfo {
    int32_t vert_count, index_count, node_count, batch_count;
    float   aabb_min[3], aabb_max[3];  /* model units, at rest */
    float   scale;                     /* model units to world pixels */
    int32_t from_override;             /* an override .glb, not the shipped model */
} OkxModelInfo;

typedef struct OkxNode {
    char    name[32];
    int32_t parent;       /* -1 for the root, parents come first */
    float   offset[3];    /* from the parent, model units */
} OkxNode;

typedef struct OkxBatch {
    int32_t first_index, index_count;
    int32_t texture;      /* okx texture id, -1 for vertex colour only */
} OkxBatch;

/* A model by object name in a team colour (0..11), baked once and kept.
 * Returns a model id, or -1 when there is no such model. */
OKX_API int32_t okx_model_load(const char *object_name, int32_t color);
OKX_API int32_t okx_model_info(int32_t model, OkxModelInfo *out);
/* Vertices are in their node's local space. positions and normals take
 * 3 floats a vertex, uvs 2, colors one RGBA8 word, nodes one int, and
 * indices 3 a triangle. Any pointer may be NULL. */
OKX_API int32_t okx_model_geometry(int32_t model, float *positions, float *normals,
                                   float *uvs, uint32_t *colors, int32_t *nodes,
                                   int32_t *indices);
OKX_API int32_t okx_model_nodes(int32_t model, OkxNode *out, int32_t cap);
OKX_API int32_t okx_model_batches(int32_t model, OkxBatch *out, int32_t cap);
/* A texture's pixels, RGBA. With out NULL it only reports the size.
 * Returns the bytes it needs, or -1. */
OKX_API int32_t okx_texture(int32_t texture, uint8_t *out, int32_t cap,
                            int32_t *w, int32_t *h);

/* ── The frame ─────────────────────────────────────────────────────── */

enum { OKX_UNIT_ACTIVE = 1, OKX_UNIT_DYING = 2 };

typedef struct OkxUnit {
    int32_t  handle;
    uint32_t stable_id;
    int32_t  def;
    int32_t  player;
    int32_t  color;
    int32_t  state;        /* OKX_UNIT_* */
    float    x, y, z;      /* world pixels, y the ground plus any flight */
    float    heading, pitch, roll;   /* radians, as the engine draws */
    int32_t  health, max_health;
    int32_t  building;     /* 1 while under construction */
    int32_t  model;        /* okx model id for its object and colour, -1 for none */
} OkxUnit;

/* Every unit on the map that the local player may see. Returns how many
 * there are, writing up to cap. */
OKX_API int32_t okx_units(OkxUnit *out, int32_t cap);

/* A unit's pose: 12 floats a node, a row major 3x4 matrix from node
 * space to world pixels, and a hidden flag a node. Returns the node
 * count, or -1. */
OKX_API int32_t okx_unit_pose(int32_t handle, float *matrices, uint8_t *hidden,
                              int32_t cap);

typedef struct OkxFeature {
    int32_t index;
    int32_t def;           /* feature registry index */
    float   x, y, z;       /* world pixels */
    float   heading, pitch, roll;
    int32_t model;         /* okx model id, or -1 for a sprite */
    int32_t sprite;        /* the def whose picture it shows, -1 for a model */
    /* A sprite's quad: it stands at x, z from bottom to top, spans w
     * pixels with its anchor off_x from the left, facing the camera
     * about the vertical, or lies flat on the ground when flat is 1. */
    float   top, bottom, off_x, w;
    int32_t flat;
} OkxFeature;

OKX_API int32_t okx_features(OkxFeature *out, int32_t cap);

enum { OKX_PROJ_DOT = 0, OKX_PROJ_MODEL = 1, OKX_PROJ_SPRITE = 2, OKX_PROJ_BEAM = 3 };

typedef struct OkxProjectile {
    int32_t id;               /* slot, stable while it flies */
    int32_t player, color;
    int32_t kind;             /* OKX_PROJ_* */
    int32_t model;            /* okx model id for OKX_PROJ_MODEL, else -1 */
    float   x, y, z;          /* world pixels */
    float   vx, vy, vz;       /* world pixels a tick */
    float   heading, pitch, roll;
    float   from_x, from_y, from_z;   /* a beam's source */
} OkxProjectile;

/* Everything in flight the local player may see. Returns how many. */
OKX_API int32_t okx_projectiles(OkxProjectile *out, int32_t cap);
enum { OKX_EFFECT_IMPACT = 0, OKX_EFFECT_PROJECTILE = 1 };

typedef struct OkxEffect {
    int32_t kind;          /* OKX_EFFECT_* */
    int32_t id;
    int32_t sprite;        /* the strip, for okx_effect_strip */
    int32_t frame;
    float   x, y, z;       /* world pixels */
    /* The frame's quad, standing up and facing the camera: from bottom
     * to top, w wide with its anchor off_x from the left, drawing the
     * strip from u0 to u1 across and 0 to v1 down. */
    float   top, bottom, off_x, w;
    float   u0, u1, v1;
} OkxEffect;

/* Explosions, sparks and smoke where things landed, and projectiles
 * drawn as pictures, each at its current frame. Returns how many. */
OKX_API int32_t okx_effects(OkxEffect *out, int32_t cap);
/* A strip's frames side by side, RGBA, as okx_texture. */
OKX_API int32_t okx_effect_strip(int32_t sprite, uint8_t *out, int32_t cap,
                                 int32_t *w, int32_t *h);

/* A model projectile's pose by id, as okx_unit_pose. */
OKX_API int32_t okx_projectile_pose(int32_t id, float *matrices, int32_t cap);
/* A pose matrix a node for a feature's model, as okx_unit_pose. */
OKX_API int32_t okx_feature_pose(int32_t index, float *matrices, int32_t cap);
/* A sprite feature's first picture, RGBA, as okx_texture. */
OKX_API int32_t okx_sprite(int32_t def, uint8_t *out, int32_t cap,
                           int32_t *w, int32_t *h);

typedef struct OkxFeatureDefInfo {
    char    name[40];
    char    object[40];
    char    seqname[40];
    char    category[40];
    int32_t footprint_x, footprint_z;
    int32_t height;
} OkxFeatureDefInfo;

OKX_API int32_t okx_feature_def_count(void);
OKX_API int32_t okx_feature_def_info(int32_t def, OkxFeatureDefInfo *out);

#ifdef __cplusplus
}
#endif

#endif /* OK_EMBED_H */
