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

/* Bumped whenever a function or struct below changes shape.
 * 23: OKX_EFFECT_NIMBUS and an effect's follow, a beam's source at its
 * firing piece with from_piece on OkxProjectile, okx_def_effect_strips.
 * Added since, without a bump: okx_battle_stats, okx_battle_series,
 * okx_battle_built, okx_battle_events, okx_unit_record, okx_blasts,
 * okx_weapon_info, okx_set_remastered, okx_remastered, okx_piece_events,
 * okx_feature_events, okx_wind and okx_feature_def_fate.
 * 22: okx_gui_art, okx_map_starts, map sizes in cells, a seat's claimed
 * start on OkxSeat and OkxNetSeat (TAK_EDIT_START and
 * TAK_EDIT_MOVE_START in a room), okx_sprite_by_name and
 * okx_texture_by_name, a weapon's lightmap on OkxProjectile and
 * OkxEffect, an effect's pace and okx_effect_frames. */
#define OKX_API_VERSION 23

OKX_API int32_t okx_api_version(void);

/* ── Lifetime ──────────────────────────────────────────────────────── */

/* game_dir holds the .hpi archives, data_dir is an optional folder of
 * loose files that wins over them (NULL or "" for none). 0 on success. */
OKX_API int32_t okx_init(const char *game_dir, const char *data_dir);
OKX_API void    okx_shutdown(void);
/* What the last call that failed said, never NULL. */
OKX_API const char *okx_last_error(void);

/* The player's own folder, mounted over the game's files and read
 * loose, where maps made in the editor are saved (under maps/) and
 * found again like any other. Call before okx_init. NULL for none. */
OKX_API void    okx_set_user_dir(const char *dir);

/* A folder of .glb models that replace the shipped ones by name: a
 * unit's or feature's object name, or a sprite feature's sequence name,
 * lower case. Asked before the game files. NULL or "" for none. */
OKX_API void    okx_set_override_dir(const char *dir);

/* The engine's own sound and music, played straight to the audio
 * device. volume is 0 to 127, music 1 to play the interface's list
 * between battles and the player's side's list in one. Call at start and
 * before a skirmish so the music follows the player's side. enable 0
 * stops it all. 0 on success. */
OKX_API int32_t okx_audio(int32_t enable, int32_t volume, int32_t music);
/* The music track playing, by its number, 0 for none. */
OKX_API int32_t okx_music_track(void);

/* ── Maps and unit types ───────────────────────────────────────────── */

OKX_API int32_t okx_map_count(void);
/* The map's name into out. Returns its length, or -1. */
OKX_API int32_t okx_map_name(int32_t index, char *out, int32_t cap);

typedef struct OkxMapInfo {
    char    name[96];
    char    description[256];
    char    kingdom[16];      /* the map's own, which picks its palettes */
    int32_t size_x, size_y;   /* cells: the .ota's "8 x 8" is 256 x 256 */
    int32_t max_players;
    int32_t player_counts[8]; /* every lineup the map supports */
    int32_t player_count_n;
} OkxMapInfo;

/* What the skirmish lobby shows for a map. 0 on success. */
OKX_API int32_t okx_map_info(int32_t index, OkxMapInfo *out);
/* The map's start positions, StartPos1 first, as x, z pairs in cells
 * from its north west corner, the order a seat's start counts in.
 * Writes up to cap pairs and returns how many there are, or -1. */
OKX_API int32_t okx_map_starts(int32_t index, int32_t *xz, int32_t cap);
/* The map's overview picture, RGBA, as okx_texture, cropped to the
 * map's own shape the way the lobby crops it. */
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
    int32_t floater;          /* 1 when it floats on the sea */
    int32_t waterline;        /* how far its hull sits under the sea, pixels */
    int32_t can_fly;
} OkxDefInfo;

/* Unit types known once a skirmish has loaded. */
OKX_API int32_t okx_def_count(void);
OKX_API int32_t okx_def_info(int32_t def, OkxDefInfo *out);
/* The unit's build-menu picture, RGBA, as okx_texture. -1 when the
 * game has none for it. */
OKX_API int32_t okx_unit_picture(int32_t def, uint8_t *out, int32_t cap, int32_t *w, int32_t *h);
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

typedef struct OkxSeat {
    int32_t kind;             /* 0 closed, 1 human, 2 computer */
    int32_t side;             /* 0 aramon, 1 taros, 2 veruna, 3 zhon, 7 creon, -1 random */
    int32_t team;             /* seats on one team are allies, 0 for everyone alone */
    int32_t color;            /* 0 to 11 */
    int32_t difficulty;       /* 0 easy, 1 normal, 2 hard, 3 brutal */
    /* The start the seat claimed, an index into okx_map_starts, -1 for
     * any. A claim is kept, and the other open seats take the starts
     * left in seat order, or dealt from the seed with random start
     * locations; a second seat claiming a start counts as any. */
    int32_t start;
} OkxSeat;

typedef struct OkxSkirmish {
    char    map[96];
    char    kingdom[16];      /* the local player's: aramon, veruna, taros, zhon, creon */
    int32_t ai_players;       /* computer opponents, 0 to 4 */
    int32_t line_of_sight;    /* the lobby checkbox */
    int32_t map_revealed;
    uint32_t seed;            /* 0 lets the engine pick */
    /* The lobby's seats, seat 0 the local player. With seat_count 0 the
     * kingdom and ai_players above set up a one against the rest game. */
    int32_t seat_count;
    OkxSeat seats[8];
    int32_t units_per_side;   /* 0 for the default */
    int32_t monarch_expendable;
    int32_t random_start_locations;
} OkxSkirmish;

/* Load a map with its armies and stand ready at tick 0. 0 on success. */
OKX_API int32_t okx_start_skirmish(const OkxSkirmish *cfg);

/* The remastered battlefield rules (D-036). With them on, every
 * skirmish this host starts plays them, every room it hosts has them,
 * and it joins only rooms that have them: okx_net_rooms marks the rest
 * not joinable and okx_net_join_room refuses them, saying why through
 * okx_net_why. 0, the default, leaves battles to the original's rules. */
OKX_API void    okx_set_remastered(int32_t on);
OKX_API int32_t okx_remastered(void);

/* The same load in slices, for a loading screen. okx_load_begin starts
 * it, and each okx_load_step works for up to max_ms and reports how far
 * it got, 0 to 1, and what it is doing. It returns 1 when the battle is
 * ready, 0 while loading, -1 when the load failed. */
OKX_API int32_t okx_load_begin(const OkxSkirmish *cfg);
OKX_API int32_t okx_load_step(int32_t max_ms, float *progress, char *status, int32_t cap);

/* Saved games. okx_save writes the running battle to a file. A save
 * comes back through okx_load_save_begin and then okx_load_step, like a
 * new battle. 0 on success, -1 with the reason in okx_last_error. */
OKX_API int32_t okx_save(const char *path);
OKX_API int32_t okx_load_save_begin(const char *path);

typedef struct OkxSaveInfo {
    char     map[96];
    uint32_t tick;            /* the battle's tick when it was saved */
    uint64_t saved_at;        /* seconds since 1970, UTC */
    int32_t  players;         /* seats in the battle */
} OkxSaveInfo;

/* What a save holds, for a load screen. 0 on success. */
OKX_API int32_t okx_save_info(const char *path, OkxSaveInfo *out);
OKX_API void    okx_end_game(void);

/* Simulation ticks per second of game time. */
OKX_API int32_t okx_tick_rate(void);
/* Run n simulation ticks. Returns how many ran. Outside a battle it runs
 * none and moves the music on, so call it once a frame in the menus. */
OKX_API int32_t okx_tick(int32_t n);
OKX_API uint32_t okx_tick_count(void);
/* Live units of a player, or of everyone for player 0, fog or not. */
OKX_API int32_t okx_unit_count(int32_t player);
/* The simulation's state hash, the one a match compares across machines. */
OKX_API uint32_t okx_sim_hash(void);
/* The same hash in parts (TAK_SimHashParts), to find where two machines
 * part ways. Returns how many parts. */
OKX_API int32_t okx_sim_hash_parts(uint32_t *out, int32_t cap);
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
 * For TAK_CMD_BUILD, arg is the building's facing, quarter turns
 * clockwise. Applied on the tick its turn comes round. 0 when queued.
 * OKX_QUEUE in arg puts a unit order behind the orders the unit holds,
 * as Shift does: MOVE, ATTACK, ATTACK_GROUND, PATROL, GUARD, REPAIR,
 * RECLAIM, RECLAIM_FEATURE, RESURRECT_FEATURE, CAPTURE, UNLOAD,
 * SPECIAL_WEAPON, RALLY and BUILD take it, and it is dropped from the
 * rest. Up to sixteen orders wait, each taken when the one before it is
 * done, and one that can no longer be carried out is passed over.
 * OKX_KEEP instead replaces the order in hand and keeps the queued ones
 * behind it, the manual's Ctrl-click. */
#define OKX_QUEUE 0x8000
#define OKX_KEEP  0x4000
OKX_API int32_t okx_command(int32_t type, int32_t handle, int32_t x, int32_t y,
                            int32_t target, int32_t build_def, int32_t arg);

/* A formation move: each of n units to a point of its own, xy holding
 * an x, z pair a unit in world pixels, as one order on one tick. With
 * face 1 each unit turns to heading on arrival, in radians as okx_units
 * reports a heading, and holds it until its next order. With
 * group_pace 1 no unit walks faster than the slowest one named. With
 * queue 1 the move waits behind the orders the units have in hand, as
 * Shift does, up to sixteen orders deep. Units the local player does not
 * own, and units that cannot walk, are left out. Past 256 units the
 * move goes as several orders, each keeping its own group's pace.
 * 0 when queued, -1 when nothing could be sent. */
OKX_API int32_t okx_move_formation(const int32_t *handles, const int32_t *xy, int32_t n,
                                   int32_t face, float heading,
                                   int32_t group_pace, int32_t queue);

/* A unit of def for player, set down whole on the clear ground nearest
 * that player's start, for Studio Mode trying a model in a battle. It
 * is no order, so a match refuses it. Returns the handle, or -1 when
 * there is no such def or player, a match is on, or nothing within 64
 * cells of the start will take it. */
OKX_API int32_t okx_place_unit(int32_t def, int32_t player);

/* Where a building of def would stand for a site at x, y: the site
 * snapped to the cell grid as the game places it, into sx, sy. 1 when
 * it can be built there, 0 when blocked. */
OKX_API int32_t okx_build_site(int32_t def, int32_t x, int32_t y, int32_t *sx, int32_t *sy);
/* The same for the building turned `facing` quarter turns clockwise,
 * seen from above. An odd facing swaps the footprint's sides. */
OKX_API int32_t okx_build_site_facing(int32_t def, int32_t facing, int32_t x, int32_t y,
                                      int32_t *sx, int32_t *sy);
/* 1 when a building of def can be placed turned. A lodestone cannot,
 * and any facing asked of it places it at 0. */
OKX_API int32_t okx_def_can_turn(int32_t def);
/* A ship's hull in px, read from its model, which ships keep off each
 * other's (M-012): the bow ahead of its centre, the stern behind and half
 * the beam. 1 for a ship, 0 for anything else, with the outputs zeroed. */
OKX_API int32_t okx_def_hull(int32_t def, int32_t *fore, int32_t *aft,
                             int32_t *half_beam);
/* The armed building's facing, for the click that places it. Arming a
 * building starts it at 0. */
OKX_API void    okx_set_build_facing(int32_t facing);

/* A factory's queue: how many of def are queued or in progress, or all
 * of them for def -1. A def trained without end counts one. */
OKX_API int32_t okx_factory_queue(int32_t handle, int32_t def);

/* The build button, as the original's: count > 0 queues that many of
 * def (a click 1, Shift 5), merged into the last run when it is the same
 * def, and count < 0 takes that many off, the last queued first and the
 * one in training last (the right click). Nothing is queued behind a def
 * trained without end. 0 when the order was sent. */
OKX_API int32_t okx_factory_add(int32_t factory, int32_t def, int32_t count);
/* Ctrl-click: on 1 trains def without end, "+++" on the button. on 0
 * is the original's right click on that button, which takes the repeat
 * and every queued one of def off, the one in training included. One
 * def repeats at a time, and it is always last. 0 when sent. */
OKX_API int32_t okx_factory_set_repeat(int32_t factory, int32_t def, int32_t on);
/* The def a factory trains without end, or -1. */
OKX_API int32_t okx_factory_repeat_of(int32_t factory);

/* Rules the remaster may relax, for the host alone. The classic view
 * keeps the original's. */
enum {
    /* A factory of yours still being built takes units on its queue
     * through okx_factory_add and okx_factory_set_repeat, and starts on
     * them once finished. Every other order to it is still refused.
     * On by default. */
    OKX_ALLOW_QUEUE_UNFINISHED = 1
};
OKX_API void    okx_allow(int32_t flags);
OKX_API int32_t okx_allowed(void);

enum {
    OKX_LEG_QUEUED = 1,      /* behind the order in hand */
    OKX_LEG_FORMATION = 2,   /* a formation move's part */
    OKX_LEG_FACE = 4,        /* heading is the one to turn to on arrival */
    OKX_LEG_RETURN = 8       /* the point a patrol comes back through */
};

typedef struct OkxOrderLeg {
    int32_t kind;      /* a TAK_CMD_* value, RALLY for a factory's rally */
    int32_t x, y;      /* world pixels, flat, as okx_command takes them */
    int32_t target;    /* a unit handle, or -1 */
    int32_t def;       /* BUILD: the def, else -1 */
    int32_t facing;    /* BUILD: quarter turns clockwise, else 0 */
    int32_t flags;     /* OKX_LEG_* */
    float   heading;   /* radians, as okx_units reports one, with OKX_LEG_FACE */
} OkxOrderLeg;

/* A unit's orders for drawing its order lines: the one in hand first,
 * then each queued in turn. A patrol lists its points and then the one
 * its route comes back through. A factory lists its rally, RALLY or
 * PATROL, and the standing orders its products carry on with, nothing
 * when it has no rally. Writes up to cap, returns how many there are,
 * -1 for a unit that is not there. */
OKX_API int32_t okx_unit_orders(int32_t handle, OkxOrderLeg *out, int32_t cap);

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

/* The local player's fog as the classic view draws it, one byte a 16
 * pixel cell, row by row from the north: 0 black (never seen), 1 dimmed
 * (seen before, with line of sight on), 2 clear (in sight now, or seen
 * before with line of sight off, where the original keeps showing what
 * was seen). With out NULL it only reports the size. Returns the bytes
 * it needs. */
OKX_API int32_t okx_fog(uint8_t *out, int32_t cap, int32_t *w, int32_t *h);
/* The whole map and every unit in view, as a watcher sees them, or the
 * local player's own sight again with 0. Presentation only, and a new
 * battle starts without it. */
OKX_API void okx_see_all(int32_t on);
/* After a skirmish defeat the computers fight on while two seats still
 * standing are at war, 1 when they do, never in a match. okx_outcome keeps
 * the defeat, and okx_playing_on is 1 until the war's quiet end. */
OKX_API int32_t okx_play_on(void);
OKX_API int32_t okx_playing_on(void);

/* ── The battle's record, for an end screen ────────────────────────── */

/* A kingdom's numbers: the original's tallies as its end screen prints
 * them, then what the engine keeps beside them. */
typedef struct OkxBattleStats {
    int32_t units_built;      /* every unit and building begun, walls aside */
    int32_t kills, losses;
    int32_t score;            /* the experience value of every kill */
    int32_t last_alive_tick;  /* the original's Time */
    int32_t eliminated;
    int32_t units_trained;    /* mobile units finished */
    int32_t buildings_raised; /* structures finished */
    int32_t damage_dealt;     /* hit points taken off other kingdoms */
    int32_t damage_taken;
    int32_t spells_cast;      /* shots that cost mana */
    int32_t fell_tick;        /* when it had nothing left, 0 while it stands */
    float   mana_gathered, mana_spent;
    /* Its champion, the unit with the most kills: kind (-1 for none),
     * kills, experience, veteran level 0 to 10, 1 while it lives and its
     * handle then, else -1. */
    int32_t best_def, best_kills, best_xp, best_rank;
    int32_t best_standing, best_handle;
} OkxBattleStats;

OKX_API int32_t okx_battle_stats(int32_t player, OkxBattleStats *out);

/* What the engine samples for each kingdom every few seconds. */
#define OKX_SERIES_ARMY       0   /* finished mobile units on the field */
#define OKX_SERIES_WORTH      1   /* what they cost, in mana */
#define OKX_SERIES_MANA       2   /* the pool */
#define OKX_SERIES_GATHERED   3   /* mana gathered so far */
#define OKX_SERIES_SPENT      4   /* mana spent so far */
#define OKX_SERIES_BUILT      5   /* units built so far */
#define OKX_SERIES_KILLS      6
#define OKX_SERIES_LOSSES     7
#define OKX_SERIES_LODESTONES 8   /* finished lodestones held */
#define OKX_SERIES_COUNT      9

/* One series of a kingdom: a sample every *every ticks from the start,
 * the gap doubling in a long battle, and last the value now, at
 * okx_tick_count(). Returns how many values there are, and fills what
 * fits. */
OKX_API int32_t okx_battle_series(int32_t player, int32_t series, int32_t *out,
                                  int32_t cap, int32_t *every);

/* The kinds a kingdom finished, as defs with how many of each, most
 * first. Returns how many kinds, and fills what fits. */
OKX_API int32_t okx_battle_built(int32_t player, int32_t *defs, int32_t *counts,
                                 int32_t cap);

/* The key moments, in order. player is whose moment it is and other
 * the kingdom on the other side of it, 0 for none. def and other_def
 * are their units in it, -1 for none. */
#define OKX_EVENT_FIRST_BLOOD   1   /* player killed a unit of other's first */
#define OKX_EVENT_MONARCH_SLAIN 2   /* player's monarch, killed by other */
#define OKX_EVENT_FELL          3   /* player has nothing left */
#define OKX_EVENT_YIELDED       4   /* player gave up */

typedef struct OkxBattleEvent {
    int32_t tick, kind, player, other, def, other_def;
} OkxBattleEvent;

OKX_API int32_t okx_battle_events(OkxBattleEvent *out, int32_t cap);

/* A living unit's kills, experience and veteran level (0 to 10, the
 * experience over its kind's experiencepoints). 0 on success. */
OKX_API int32_t okx_unit_record(int32_t handle, int32_t *kills, int32_t *xp,
                                int32_t *rank);

/* ── What a battle does to the field ───────────────────────────────── */

/* For a host that draws more than the original: report only, kept in a
 * ring outside the simulation, its save and its hash. Ids start at 1 and
 * rise by one, never reused while the library is loaded, and each battle
 * starts its ring empty. A call returns how many there are after since
 * and fills up to cap of them, oldest first. A ring keeps OKX_RING. */
#define OKX_RING 1024

enum { OKX_BLAST_WEAPON = 0, OKX_BLAST_DEATH = 1, OKX_BLAST_FEATURE = 2 };
#define OKX_BLAST_FIRE_STARTER 1   /* sets flammable scenery burning */
#define OKX_BLAST_UNITS_ONLY   2   /* never touches scenery */
#define OKX_BLAST_WATER        4   /* burst on the sea */
#define OKX_BLAST_DIRECT_HIT   8   /* under 17 px of area on a unit, scenery spared */
#define OKX_BLAST_UNSEEN       16  /* where the local player could not see */
/* A weapon slot meaning the unit's own death blast. */
#define OKX_SLOT_DEATH (-2)

/* A shot, a spell or a death bursting. radius is half the weapon's
 * areaofeffect, the reach the original gives a blast over scenery. */
typedef struct OkxBlast {
    int32_t  id;
    uint32_t tick;            /* okx_tick_count when it burst */
    int32_t  cause;           /* OKX_BLAST_* */
    int32_t  def, slot;       /* the weapon's unit def and slot, -1 unknown */
    int32_t  player;
    int32_t  shooter;         /* the handle that fired, while it lives, or -1 */
    float    x, y, z;         /* world pixels */
    float    dx, dy, dz;      /* the way it travelled, unit length, or 0 */
    float    radius;          /* pixels */
    int32_t  damage, flags;   /* flags OKX_BLAST_* bits */
    int32_t  unit;            /* the handle it came down on, or -1 */
    int32_t  feature;         /* the feature in its cell, okx_features' index, or -1 */
} OkxBlast;

OKX_API int32_t okx_blasts(int32_t since, OkxBlast *out, int32_t cap);

#define OKX_WEAPON_FIRE_STARTER 1
#define OKX_WEAPON_UNITS_ONLY   2
#define OKX_WEAPON_SPELL        4   /* costs mana */

/* A weapon as its data describes it, the keys spelled as the data has
 * them and "" where it leaves one out. */
typedef struct OkxWeaponInfo {
    char    name[32];
    char    type[32];
    char    subtype[32];
    char    damage_type[32];
    char    explosion_class[32];
    char    water_explosion_class[32];
    int32_t area_of_effect;   /* pixels, as the data gives it */
    int32_t damage;
    int32_t flags;            /* OKX_WEAPON_* */
    int32_t lightmap;         /* OKX_LIGHTMAP_* */
    float   shake_magnitude;  /* pixels */
    float   shake_duration;   /* seconds */
} OkxWeaponInfo;

/* slot 0 to 2, or OKX_SLOT_DEATH. 0 on success, -1 for no such weapon. */
OKX_API int32_t okx_weapon_info(int32_t def, int32_t slot, OkxWeaponInfo *out);

/* A piece a unit's script threw with EXPLODE: the unit's handle, def,
 * player and model (as okx_units gives it), the piece's node, the
 * script's explode type (FALL 4, SHATTER 1, SMOKE 8 and the rest) and
 * the piece's pose as okx_unit_pose gives a node, when it went. */
typedef struct OkxPieceEvent {
    int32_t  id;
    uint32_t tick;
    int32_t  unit, def, player, model, piece, how;
    int32_t  unseen;          /* the local player could not see the unit */
    float    m[12];
} OkxPieceEvent;

OKX_API int32_t okx_piece_events(int32_t since, OkxPieceEvent *out, int32_t cap);

/* What happened to a feature: its okx_features index when it happened,
 * what it was and what took its cell (-1 when nothing did and it is
 * gone), where it stands, the blast behind it (an okx_blasts id, 0 for
 * none) and where that burst, a hit's damage and the hit points left,
 * and a death's or a burn's length in ticks. The map's own scenery as a
 * battle loads is no news. */
enum { OKX_FEATURE_HIT = 0, OKX_FEATURE_DYING = 1, OKX_FEATURE_DEAD = 2, OKX_FEATURE_BURNING = 3,
       OKX_FEATURE_BURNT = 4, OKX_FEATURE_SWEPT = 5, OKX_FEATURE_PLACED = 6, OKX_FEATURE_REMOVED = 7 };

typedef struct OkxFeatureEvent {
    int32_t  id;
    uint32_t tick;
    int32_t  kind, feature, def, new_def;
    float    x, y, z;
    int32_t  blast;
    float    from_x, from_y, from_z;
    int32_t  damage, health, ticks;
} OkxFeatureEvent;

OKX_API int32_t okx_feature_events(int32_t since, OkxFeatureEvent *out, int32_t cap);

/* The simulation's wind: its speed, the map's highest, and the way it
 * blows as a unit vector in world x and z. -1 outside a battle. */
OKX_API int32_t okx_wind(float *speed, float *max_speed, float *dx, float *dz);

/* What destroying a feature def does: its hit points (damage), whether it
 * ignores hits or can burn, and the defs it leaves when destroyed and when
 * burnt out, -1 for nothing. 0 on success, -1 for no such def. */
typedef struct OkxFeatureFate {
    int32_t damage, indestructible, flammable, dead_def, burnt_def;
} OkxFeatureFate;

OKX_API int32_t okx_feature_def_fate(int32_t def, OkxFeatureFate *out);

/* ── The game's own controls ───────────────────────────────────────── */

/* The engine keeps a selection, and its click rules and unit voices
 * work from it. A host that lets the engine decide what a click means
 * keeps the selection here: okx_select sets it (add 1 keeps what is
 * selected, and n 0 without add clears it), okx_selection reads it. */
OKX_API int32_t okx_select(const int32_t *handles, int32_t n, int32_t add);
OKX_API int32_t okx_selection(int32_t *out, int32_t cap);

/* The original's left click at a ground point in world pixels, or on
 * the unit the host's picking found (unit >= 0): select a friend, order
 * the selection to move or attack or repair or raise, or carry out an
 * armed command, exactly as the game decides it, voices included. With
 * unit -1 the point is open ground and no unit near it is picked. shift
 * holds the keys: bit 0 Shift puts the order behind the ones the units
 * hold and keeps an armed command armed for the next click, bit 1 Ctrl
 * changes the order in hand and keeps the queue behind it. A unit still
 * being built is never selected, as in the original. */
OKX_API void    okx_click(float x, float z, int32_t unit, int32_t shift);

/* A sound by its wav name, "menubutton.wav" or "menubutton", played
 * flat as the game plays an interface sound. volume is 0 to 127, the
 * original's scale, where .gui widgets play at 85. 0 when played, -1
 * when there is no such sound or no audio. */
OKX_API int32_t okx_play_ui_sound(const char *wav, int32_t volume);
/* The right click and Escape: an armed command is cancelled first, and
 * with none armed the selection is cleared. */
OKX_API void    okx_cancel(void);

enum {
    OKX_ARM_NONE = 0, OKX_ARM_MOVE = 1, OKX_ARM_ATTACK = 2, OKX_ARM_GUARD = 3,
    OKX_ARM_PATROL = 4, OKX_ARM_LOAD = 5, OKX_ARM_UNLOAD = 6, OKX_ARM_HEAL = 7,
    OKX_ARM_CLEAR = 8, OKX_ARM_BUILD = 200
};

/* Arm a command button so the next click carries it out. OKX_ARM_BUILD
 * places a building of def. OKX_ARM_NONE disarms. */
OKX_API void    okx_arm(int32_t mode, int32_t def);
/* What is armed, OKX_ARM_*, and for a building the def into def. */
OKX_API int32_t okx_armed(int32_t *def);

/* An order for everything selected that needs no point: TAK_CMD_STOP,
 * TAK_CMD_SET_AGGRO with arg, TAK_CMD_SET_WEAPON with arg, TAK_CMD_GATE. */
OKX_API int32_t okx_order_selection(int32_t type, int32_t arg);

/* Control groups 0 to 9, as Ctrl and a number assign and a number
 * recalls. Recall returns how many it selected. */
OKX_API void    okx_group_assign(int32_t group);
OKX_API int32_t okx_group_recall(int32_t group);
/* Ctrl, Shift and a number: the group's units added to the selection.
 * Returns the selection count. */
OKX_API int32_t okx_group_add(int32_t group);

/* The original's select keys over the local player's finished units:
 * every one of a type the selection holds (Ctrl+Z), every one (Ctrl+A),
 * or those whose FBI category names the word, in place of the selection
 * or with add added to it (Ctrl+B and the rest). Returns the selection
 * count. */
enum { OKX_SELECT_SAME_TYPE = 1, OKX_SELECT_ALL = 2, OKX_SELECT_CATEGORY = 3 };
OKX_API int32_t okx_select_kind(int32_t kind, const char *category, int32_t add);

/* The pointer, as the classic view shows it. */
enum {
    OKX_CURSOR_NORMAL = 0, OKX_CURSOR_SELECT, OKX_CURSOR_MOVE, OKX_CURSOR_ATTACK,
    OKX_CURSOR_GUARD, OKX_CURSOR_PATROL, OKX_CURSOR_LOAD, OKX_CURSOR_UNLOAD,
    OKX_CURSOR_REPAIR, OKX_CURSOR_RECLAIM, OKX_CURSOR_REVIVE, OKX_CURSOR_PLACE,
    OKX_CURSOR_RED, OKX_CURSOR_BUSY, OKX_CURSOR_COUNT
};

/* The cursor the classic view shows over a ground point in world pixels,
 * or over the unit the host's picking found (unit >= 0), from the
 * selection and any armed command as the game decides it, except that
 * an armed attack, guard or repair over a unit it cannot take shows
 * OKX_CURSOR_RED where the classic view keeps the command's own. An
 * enemy no selected weapon can take, such as a flyer in the air under
 * noairweapon cannons, is OKX_CURSOR_RED for the classic too far. For
 * OKX_CURSOR_PLACE, where a building's ghost stands in for the pointer,
 * clear says whether the building can stand there. */
OKX_API int32_t okx_cursor_at(float x, float z, int32_t unit, int32_t *clear);

/* A frame of the game's interface art, anims/<gaf> ("mainscreen" or
 * "mainscreen.gaf") in the palette the game's own screens load it with:
 * RGBA, top row first, the frame's transparent index clear, subframes
 * composited. ox and oy are the frame's origin, frames how many the
 * entry has. With out NULL it only reports. Returns the bytes it needs,
 * or -1 when there is no such entry or frame. */
OKX_API int32_t okx_gui_art(const char *gaf, const char *entry, int32_t frame,
                            uint8_t *out, int32_t cap, int32_t *w, int32_t *h,
                            int32_t *ox, int32_t *oy, int32_t *frames);

/* A cursor's art from the game's cursors.gaf: frame `frame` as RGBA, top
 * row first, into out when cap is enough, with its size, the hotspot
 * the pointer's position falls on, and how long it shows in ms. Returns
 * the frame count, or -1 when the game has no such cursor. */
OKX_API int32_t okx_cursor_frame(int32_t cursor, int32_t frame, uint8_t *out, int32_t cap,
                                 int32_t *w, int32_t *h, int32_t *hot_x, int32_t *hot_y,
                                 int32_t *ms);

/* ── The sidebar's orders ──────────────────────────────────────────── */

/* The order buttons the classic sidebar shows for the selection, from
 * the table the classic HUD draws with. id is the HUD's own number,
 * which okx_hud_do and okx_arm take. */
enum { OKX_CMDKIND_TARGET = 1, OKX_CMDKIND_INSTANT = 2, OKX_CMDKIND_CHOICE = 3 };
/* The ids of the buttons that are not OKX_ARM_* orders. */
enum {
    OKX_HUD_STOP = 100, OKX_HUD_OFFENSIVE = 101, OKX_HUD_DEFENSIVE = 102,
    OKX_HUD_PASSIVE = 103, OKX_HUD_WEAPON1 = 110, OKX_HUD_WEAPON2 = 111,
    OKX_HUD_WEAPON3 = 112, OKX_HUD_CLOAK_ON = 120, OKX_HUD_CLOAK_OFF = 121,
    OKX_HUD_OPEN = 122, OKX_HUD_CLOSE = 123
};
enum { OKX_CMDGROUP_NONE = 0, OKX_CMDGROUP_STANCE = 1, OKX_CMDGROUP_WEAPON = 2,
       OKX_CMDGROUP_CLOAK = 3, OKX_CMDGROUP_GATE = 4 };
enum { OKX_WHY_OK = 0, OKX_WHY_MANA = 1, OKX_WHY_UNSUPPORTED = 2 };
typedef struct OkxHudCommand {
    int32_t id;          /* the HUD's command number, OKX_ARM_* for the targeted ones */
    int32_t kind;        /* OKX_CMDKIND_*: needs a click, happens at once, or one of a group */
    int32_t group;       /* OKX_CMDGROUP_*: the choices of one group exclude each other */
    int32_t enabled;
    int32_t active;      /* armed, or the choice the selection holds */
    int32_t mana_cost;   /* a spell's mana a cast, 0 otherwise */
    int32_t why;         /* OKX_WHY_* when not enabled */
    int32_t hotkey;      /* the letter keys.tdf binds, upper case, 0 for none */
    int32_t weapon_slot; /* 0 to 2 for a weapon button, else -1 */
    char    name[32];    /* the sidebar widget, "ATTACK", "PrimaryWeapon" */
    char    label[128];  /* the widget's help text */
    char    weapon[32];  /* a weapon button's weapon */
} OkxHudCommand;

/* The buttons the selection has, shown ones only. Returns the count. */
OKX_API int32_t okx_hud_commands(OkxHudCommand *out, int32_t cap);
/* A button's picture as the original draws it, RGBA: state 0 disabled,
 * 1 pressed or chosen, 2 at rest. A weapon button is its weapon's own
 * picture. With out NULL it only reports the size. Returns the bytes it
 * needs, or -1. */
OKX_API int32_t okx_hud_command_art(int32_t id, int32_t state, uint8_t *out, int32_t cap,
                                    int32_t *w, int32_t *h);
/* Press a button as the sidebar does: a targeted order arms (and a
 * second press disarms), the rest happen at once through the same
 * orders the sidebar sends. Returns 1 when taken, 0 when refused. */
OKX_API int32_t okx_hud_do(int32_t id);
/* A left drag over the ground, in world pixels: the armed load order
 * takes every rider in the box, and otherwise it selects, as the
 * classic view's drag does. */
OKX_API void    okx_drag(float x0, float z0, float x1, float z1, int32_t shift);
/* A unit's own mana, the blue bar under its red one. Returns 0, or -1
 * for a bad handle. */
OKX_API int32_t okx_unit_mana(int32_t handle, float *mana, float *max);

/* ── Multiplayer ───────────────────────────────────────────────────── */

/* A session with an OpenKingdoms relay, as the game's own multiplayer
 * screens hold one. The host pumps it every frame. A battle in a match
 * runs on the relay's turns: okx_tick never runs past what the turns
 * allow, and orders go to the relay as they do in the game. */
enum {
    OKX_NET_OFF = 0, OKX_NET_CONNECTING, OKX_NET_LOBBY, OKX_NET_ROOM,
    OKX_NET_LOADING, OKX_NET_PLAYING, OKX_NET_REFUSED, OKX_NET_GONE
};

/* address is ws://host:port/play or wss://..., name the player's. */
OKX_API int32_t okx_net_connect(const char *address, const char *name);
OKX_API void    okx_net_disconnect(void);
/* Move bytes, answer what needs answering, and return OKX_NET_*. Call
 * it every frame, in the lobby and in the battle. */
OKX_API int32_t okx_net_pump(void);
/* Why the session failed or was refused, never NULL. */
OKX_API const char *okx_net_why(void);

typedef struct OkxNetRoom {
    uint32_t id;
    char     code[16];
    char     name[32];
    char     host[16];
    char     map[64];
    int32_t  players, max_players, status;
    int32_t  joinable;         /* 1 when this install may join */
} OkxNetRoom;

/* Ask for the room list, then read it as it arrives. */
OKX_API int32_t okx_net_list_rooms(void);
OKX_API int32_t okx_net_rooms(OkxNetRoom *out, int32_t cap);
/* Host a room on a map, with TAK_ROOMOPT_* options, or join one by id
 * or by its code (id 0). 0 when asked. */
OKX_API int32_t okx_net_create_room(const char *name, const char *map, int32_t options);
OKX_API int32_t okx_net_join_room(uint32_t id, const char *code);
OKX_API int32_t okx_net_leave_room(void);

typedef struct OkxNetSeat {
    int32_t kind;              /* 0 empty, 1 human, 2 computer, 3 blocked */
    int32_t side, colour, team, ready, connected, load_percent, has_map;
    char    name[16];
    int32_t start;             /* claimed, an index into okx_map_starts, -1 any */
} OkxNetSeat;

typedef struct OkxNetRoomInfo {
    uint32_t   id;
    char       code[16];
    char       name[32];
    char       map[64];
    int32_t    options, unit_cap;
    int32_t    you_host, your_seat;
    int32_t    seat_count;
    OkxNetSeat seats[8];
} OkxNetRoomInfo;

/* The room we sit in, as the relay last described it. 0 on success. */
OKX_API int32_t okx_net_room(OkxNetRoomInfo *out);
/* A change to the room: field is TAK_EDIT_*, seat the row it is about
 * (-1 for our own or the room's), value and text as the field wants. A
 * map change carries the map's fingerprint by itself. TAK_EDIT_START (8)
 * takes start value for our own seat, refused while another holds it,
 * or -1 gives ours back. TAK_EDIT_MOVE_START (42), the host's, moves
 * seat to start value, swapping with its holder, or -1 frees it. A new
 * map frees every start. */
OKX_API int32_t okx_net_edit(int32_t field, int32_t seat, int32_t value, const char *text);
OKX_API int32_t okx_net_chat(const char *text);
/* The last chat line, and a count that goes up with each new one. */
OKX_API int32_t okx_net_last_chat(char *from, int32_t from_cap, char *text, int32_t text_cap);
/* The host starts the match when everyone is ready. */
OKX_API int32_t okx_net_start(void);
/* When the session says OKX_NET_LOADING, build the battle the relay
 * described, then okx_load_step it like any load. It finishes when the
 * relay says go. */
OKX_API int32_t okx_net_load_begin(void);

/* A match as it runs. When the server halts it because two worlds went
 * out of step, desynced is 1, halted_after is the last tick this world
 * checked its hash on, and bundle names the file written to the user
 * folder: the build, the map and seed, this world's hash in parts at
 * its last checks and the orders it ran, for comparing with the other
 * player's. */
typedef struct OkxNetMatch {
    int32_t  live;
    int32_t  desynced;
    uint32_t tick;
    uint32_t halted_after;
    char     waiting[96];      /* "Waiting for Zach", or empty */
    char     bundle[512];
} OkxNetMatch;
OKX_API int32_t okx_net_match(OkxNetMatch *out);
/* Test hook: nudge this world's first unit a pixel, so it goes out of
 * step with the others. */
OKX_API void    okx_debug_desync(void);

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

/* ── The map editor ────────────────────────────────────────────────── */

/* The loaded map's own height bytes, one a 16 pixel cell, row by row
 * from the north. With out NULL it only reports the size. Returns the
 * bytes it needs. */
OKX_API int32_t okx_map_cells(uint8_t *out, int32_t cap, int32_t *w, int32_t *h);
/* Write height bytes into the loaded map, w by h cells from x0, z0.
 * The ground the battle stands on follows at once. 0 on success. */
OKX_API int32_t okx_edit_cells(int32_t x0, int32_t z0, int32_t w, int32_t h,
                               const uint8_t *values);
/* Every ground picture the game has, by chunk id, and one of them as
 * RGBA (as okx_texture). The library returns how many there are. */
OKX_API int32_t okx_chunk_library(uint32_t *ids, int32_t cap);
OKX_API int32_t okx_chunk_picture(uint32_t id, uint8_t *out, int32_t cap, int32_t *w, int32_t *h);
/* The chunk id of the loaded map's chunk picture number chunk. */
OKX_API uint32_t okx_terrain_chunk_id(int32_t chunk);
/* Paint w by h blocks from bx, by: each takes a chunk id and the sub
 * square in it, one entry a block, row by row. The block table and the
 * chunk list the host reads are rebuilt, so read them again. 0 on success. */
OKX_API int32_t okx_edit_blocks(int32_t bx, int32_t by, int32_t w, int32_t h,
                                const uint32_t *chunk_ids, const uint8_t *tex_x,
                                const uint8_t *tex_y);
/* Put a feature of def with its footprint's corner on cell cx, cz, or
 * take feature index away. Place returns the new index or -1. */
OKX_API int32_t okx_feature_place(int32_t def, int32_t cx, int32_t cz);
OKX_API int32_t okx_feature_remove(int32_t index);

/* Save the loaded map, with its edits, as a new map called name in the
 * user folder, where the map list finds it. The terrain file is written
 * afresh from the map as it stands: heights, ground pictures and the
 * features placed on it. 0 on success. */
OKX_API int32_t okx_map_save(const char *name);

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
    float    x, y, z;      /* world pixels, y the ground plus any flight,
                            * and a floater on water rides the sea */
    float    heading, pitch, roll;   /* radians, as the engine draws */
    int32_t  health, max_health;
    int32_t  building;     /* 1 while under construction */
    int32_t  model;        /* okx model id for its object and colour, -1 for none */
    int32_t  facing;       /* a building's quarter turns clockwise, 0 to 3 */
} OkxUnit;

/* Every unit on the map that the local player may see. Returns how many
 * there are, writing up to cap. */
OKX_API int32_t okx_units(OkxUnit *out, int32_t cap);
/* One unit by handle, whether or not the frame lists it: a building
 * frame before it is half raised, say. 0, or -1 for no such unit. */
OKX_API int32_t okx_unit(int32_t handle, OkxUnit *out);

enum { OKX_ANIM_IDLE = 0, OKX_ANIM_MOVING = 1, OKX_ANIM_ATTACKING = 2,
       OKX_ANIM_BUILDING = 3, OKX_ANIM_DYING = 4, OKX_ANIM_DEAD = 5 };

/* What a unit is doing as its script sees it (OKX_ANIM_*), and the names
 * of the script functions its threads are running now, one a line, into
 * out. Returns the state, or -1. */
OKX_API int32_t okx_unit_anim(int32_t handle, char *out, int32_t cap);

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
    int32_t lightmap;         /* OKX_LIGHTMAP_*, the weapon's ground glow */
    float   x, y, z;          /* world pixels */
    float   vx, vy, vz;       /* world pixels a tick */
    float   heading, pitch, roll;
    /* A beam's source: the piece the unit's QueryWeapon script named as
     * it fired, from_y its height in world pixels with any flight. When
     * the unit has no such piece, or the shot came from a save, it is
     * the ground under the shooter plus 12 and from_piece is 0. */
    float   from_x, from_y, from_z;
    int32_t from_piece;
} OkxProjectile;

/* Everything in flight the local player may see. Returns how many. */
OKX_API int32_t okx_projectiles(OkxProjectile *out, int32_t cap);
/* OKX_EFFECT_NIMBUS: the glow a cast of a nimbus weapon lights on its
 * caster, the side's sidedata nimbus art (nimbus_aramon and so on),
 * played once at the art's own frame time. It rides the caster: follow
 * is the unit's handle, and x, y, z are where the unit stands now. */
enum { OKX_EFFECT_IMPACT = 0, OKX_EFFECT_PROJECTILE = 1, OKX_EFFECT_NIMBUS = 2 };
/* The glow a weapon's blast casts on the ground, its lightmap key. */
enum { OKX_LIGHTMAP_NONE = 0, OKX_LIGHTMAP_SMALL = 1, OKX_LIGHTMAP_MEDIUM = 2,
       OKX_LIGHTMAP_LARGE = 3 };

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
    int32_t lightmap;      /* OKX_LIGHTMAP_*, from the weapon that made it */
    /* Its pace: ticks since it began, ticks each picture shows, the
     * strip's pictures, and 1 when they repeat rather than play once. */
    int32_t age, ticks_per_frame, frame_count, loops;
    int32_t follow;        /* the unit handle it moves with, -1 for none */
} OkxEffect;

/* Explosions, sparks and smoke where things landed, projectiles drawn
 * as pictures and nimbuses on their casters, each at its current frame.
 * Returns how many. */
OKX_API int32_t okx_effects(OkxEffect *out, int32_t cap);
/* A strip's frames side by side, RGBA, as okx_texture. */
OKX_API int32_t okx_effect_strip(int32_t sprite, uint8_t *out, int32_t cap,
                                 int32_t *w, int32_t *h);
/* Every frame's geometry in a strip, four ints a frame: the picture's
 * width and height in its cell, and its anchor x and y, as okx_effects
 * gives the frame on show. Writes up to cap frames, returns how many
 * the strip has, or -1. */
OKX_API int32_t okx_effect_frames(int32_t sprite, int32_t *geometry, int32_t cap);

/* The strips a def's weapons can show, as okx_effect_strip takes them:
 * weaponart, shadow and spell art, every variant of each weapon's
 * explosion class, and the side's nimbus for a weapon that casts one.
 * Each strip once. For warming art before the battle runs: call it
 * once the load has finished. Writes up to cap, returns how many there
 * are, or -1 for no such def. */
OKX_API int32_t okx_def_effect_strips(int32_t def, int32_t *out, int32_t cap);

/* A model projectile's pose by id, as okx_unit_pose. */
OKX_API int32_t okx_projectile_pose(int32_t id, float *matrices, int32_t cap);
/* A pose matrix a node for a feature's model, as okx_unit_pose. */
OKX_API int32_t okx_feature_pose(int32_t index, float *matrices, int32_t cap);
/* A sprite feature's first picture, RGBA, as okx_texture. */
OKX_API int32_t okx_sprite(int32_t def, uint8_t *out, int32_t cap,
                           int32_t *w, int32_t *h);

/* Pictures by name, for a model painted at load from the player's own
 * files (a material's okPaint), outside a battle as well as in one.
 * okx_sprite_by_name takes a sprite feature by its name or its sequence
 * name and gives its first frame in world's features palette
 * (<world>_features.pcx, the feature's own world for NULL or ""),
 * clear where the frame's transparent index is. okx_texture_by_name
 * takes a 3DO texture by name from textures/*.gaf and gives it in
 * world's textures palette (<ara|tar|ver|zon|npc>_textures.pcx, the
 * palette the game gives its sheet for NULL or ""), clear where the
 * frame's transparent index is and where the colour of its top left
 * texel is, as a painted card marks its clear pixels. world is a
 * kingdom, "aramon", or its prefix, "ara". Both return the bytes they
 * need, or -1, and with out NULL only report the size. */
OKX_API int32_t okx_sprite_by_name(const char *name, const char *world, uint8_t *out,
                                   int32_t cap, int32_t *w, int32_t *h);
OKX_API int32_t okx_texture_by_name(const char *name, const char *world, uint8_t *out,
                                    int32_t cap, int32_t *w, int32_t *h);

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
