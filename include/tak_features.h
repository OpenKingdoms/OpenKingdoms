#ifndef TAK_FEATURES_H
#define TAK_FEATURES_H

#include "tak_types.h"

/* ── Feature registry ────────────────────────────────────────────────
 *
 * The TNT feature_layer holds 16-bit indices into a global feature
 * registry built at startup from data/features/<world>/*.tdf. Each
 * TDF holds N [sections], each one a feature definition (sprite +
 * footprint + category). The legacy engine indexes them by load
 * order — alphabetical TDF iteration, then in-file section order.
 *
 * Categories drive gameplay: `mana` = lodestone (mana income on
 * capture), `rocks` = blocking decoration, `trees` = blocking
 * decoration, etc.
 */

typedef struct FeatureDef {
    char     name[40];          /* section name e.g. "AraHenge01"     */
    char     world[16];         /* "Aramon" / "Taros" / "Veruna" / "Zhon" / "All Worlds" */
    char     category[24];      /* "mana" / "rocks" / "trees" / etc.  */
    char     filename[40];      /* GAF stem for sprite                */
    char     seqname[40];       /* GAF entry name                     */
    /* The shadow sprite drawn under the feature while the Shadows
     * setting is on, translucent when shadtrans is set and skipped
     * when noshadow is (legacy:127225, 127343, 211159-211176). */
    char     seqname_shad[40];
    int      shadtrans;
    int      no_shadow;
    /* `object` names a 3DO instead of a GAF sequence. Corpses are
     * models, not sprites, and the two keys are exclusive: the loader
     * reads `object` first and only falls back to filename/seqname
     * when it is absent (legacy:127098-127136). */
    char     object[40];
    int      footprint_x;
    int      footprint_z;
    int      height;
    int      blocking;
    int      reclaimable;
    int      indestructible;
    int      damage;
    float    sacred_site;   /* sacredsite tier (1.0/1.5/2.0); 0 = none */
    float    energy;        /* mana paid back when reclaimed (legacy:127332) */
    int      autoreclaimable; /* default 1 (legacy:127351) */
    /* How long a placed instance lasts before it rots, in the
     * original's 30 Hz frames: the feature tick counts it down once
     * per frame with no scaling (legacy:127384-127386, 128400-128402).
     * 0 = for ever, which is what authored scenery carries. */
    int      decompose_time;
    int      resurrectable;   /* a raiser may target it (legacy:127369) */
    int      animatable;      /* an animator may target it (legacy:127372) */
    int      is_building;     /* isbuilding (legacy:127378) */
    /* `featuredead`: what this feature leaves behind when it is
     * destroyed. Kept as a name and resolved on use, because the
     * feature it names may load after this one (legacy:127524). */
    char     feature_dead[40];
    /* Ambient noise emitters: SoundClass names a class in ambient.tdf,
     * SoundDelay and SoundVariance are seconds (legacy:127081-127097).
     * A delay under one second drops the class. */
    char     sound_class[24];
    int      sound_delay_ticks;
    int      sound_variance_ticks;
    /* Death and fire, for features drawn from a GAF (legacy:127184-127401).
     * The death sequence plays once before featuredead takes the cell.
     * A flamable feature a fire starter reaches burns instead, its burn
     * sequence with a flame TAF drawn behind it and one in front, and
     * featureburnt takes the cell when it is done. sparktime is kept in
     * the original's 30 Hz frames, spreadchance in percent. */
    char     feature_burnt[40];
    char     seqname_die[40];
    char     seqname_die_shad[40];
    char     seqname_burn[40];
    char     seqname_burn_shad[40];
    char     seqname_front_flame[40];
    char     seqname_back_flame[40];
    int      flamable;
    int      spread_chance;
    int      spark_time;
} FeatureDef;

/* Build the registry by scanning data/features/<sub>/*.tdf in legacy
 * order ("all worlds", "aramon", "corpses", "taros", "veruna", "zhon"
 * — alphabetical subdirectory walk, alphabetical file walk). Returns
 * the number of features registered. Safe to call multiple times —
 * second call frees and rebuilds. */
int               Features_LoadAll(void);

/* Lookup by registry index (= TNT feature_layer value). Returns NULL
 * if out of range. */
const FeatureDef *Features_GetByIndex(int idx);

/* Total registered count. */
int               Features_GetCount(void);

/* Find feature index by name (case-insensitive). Returns -1 if not
 * found. */
int               Features_FindByName(const char *name);

/* Tear down + free the registry. */
void              Features_FreeAll(void);

/* ── Placed feature instances (world->features) ─────────────────────
 *
 * The reclaim ("CLEAR"/sweep) order works on placed instances, not on
 * defs. Legacy resolves the clicked map cell to a feature record and
 * only offers the order when that record is a live feature
 * (legacy:187186-187198). A live unit under the cursor gets no reclaim
 * order at all (legacy:187201). */
struct GameWorld;

/* Index into world->features of the reclaimable instance whose
 * footprint covers (world_x, world_y), or -1. */
int  Features_FindReclaimableAt(const struct GameWorld *world,
                                int32_t world_x, int32_t world_y);

/* Centre of instance `idx` in world pixels. Returns 0 on success. */
int  Features_InstanceCentre(const struct GameWorld *world, int idx,
                             int32_t *out_x, int32_t *out_y);

/* Index into world->features of the resurrectable (or animatable)
 * instance whose footprint covers (world_x, world_y), or -1. The
 * original offers the raise order only when the cell's feature def
 * carries the flag and the instance still does, which it loses the
 * moment it starts to rot (legacy:129469-129511, 187142-187175). */
int  Features_FindResurrectableAt(const struct GameWorld *world,
                                  int32_t world_x, int32_t world_y);
int  Features_FindAnimatableAt(const struct GameWorld *world,
                               int32_t world_x, int32_t world_y);

/* Delete instance `idx`. The array is compacted, so indices above idx
 * shift down by one and the cell stops blocking movement and drawing
 * from the next query on. Returns 0 on success. */
int  Features_RemoveInstance(struct GameWorld *world, int idx);
/* Features_RemoveInstance for a builder that swept the instance away. */
int  Features_SweepInstance(struct GameWorld *world, int idx);

/* What happens to a placed feature, told to a host that draws more than
 * the original. idx is the instance when it happened. A dead or burnt
 * one's new_def took its cell, -1 when nothing did and it is gone.
 * frames is a death's or a burn's length in the original's frames. A
 * blast's own hits carry its point (bx, by). The hook runs inside the
 * tick and only reads. NULL, the default, tells no one. */
enum { FEATURE_EVENT_HIT, FEATURE_EVENT_DYING, FEATURE_EVENT_DEAD, FEATURE_EVENT_BURNING,
       FEATURE_EVENT_BURNT, FEATURE_EVENT_SWEPT, FEATURE_EVENT_PLACED, FEATURE_EVENT_REMOVED };
typedef struct FeatureEvent {
    int     kind;
    int     idx;
    int     def, new_def;
    int32_t x, y;
    int     damage, left;     /* a hit's damage, 0 for one ignored, and the hit points left */
    int     frames;
    int     blast;            /* 1 when a blast at (bx, by) did it */
    int32_t bx, by;
} FeatureEvent;
typedef void (*FeatureEventHook)(const struct GameWorld *world, const FeatureEvent *e);
void Features_SetEventHook(FeatureEventHook hook);
/* Moves on whenever a sacred site joins or leaves a feature list, or a
 * list is replaced whole, so an index of the sites knows to rebuild. */
uint32_t Features_SacredGeneration(void);
void     Features_NoteListReplaced(void);

/* Place a feature instance with its footprint's top-left corner on
 * cell (cell_x, cell_z), its model at (world_x, world_y) facing
 * `heading` (65536 per turn), drawn in team colour `color_idx` (0..11,
 * or -1 for none). Anything already standing in the footprint is
 * cleared first, and an indestructible occupant refuses the placement
 * (legacy:128173-128185, 128843). Starts the def's decompose
 * countdown. Returns the new instance index, or -1. */
int  Features_AddInstance(struct GameWorld *world, int global_idx,
                          int cell_x, int cell_z,
                          int32_t world_x, int32_t world_y,
                          uint16_t heading, int color_idx);

/* The same, with the footprint turned `facing` quarter turns clockwise
 * (a turned building's wreck). The cell is the turned footprint's
 * top-left. */
int  Features_AddInstanceFacing(struct GameWorld *world, int global_idx,
                                int cell_x, int cell_z,
                                int32_t world_x, int32_t world_y,
                                uint16_t heading, int color_idx, int facing);

/* Instance `idx`'s footprint in cells, turned with it. */
void Features_InstanceFootprint(const struct GameWorld *world, int idx,
                                int *out_fx, int *out_fz);

/* Test hook: replace the feature registry with synthetic defs. */
int  Features_DebugSetDefs(const FeatureDef *defs, int count);

/* Anything that writes world->features says so here, so the per cell
 * feature heights shots read are rebuilt before their next use. */
void Features_MarkChanged(struct GameWorld *world);

/* What stands on the 16 px cell (cell_x, cell_z) for a shot: 0 for no
 * feature, else 1 plus the tallest height of the features covering it,
 * cut to the byte the original keeps (legacy:127053, :245444-245461). */
int  Features_TopAt(struct GameWorld *world, int cell_x, int cell_z);

/* ── Destruction, fire and wind ──────────────────────────────────────
 *
 * A placed feature counts the damage blasts do to it and dies at its
 * def's damage, plays its death sequence, and gives its cell to its
 * featuredead (legacy:127838-127955, 128725-128822). A fire starter's
 * blast sets a flamable one burning instead. A burn throws sparks once
 * at the neighbours and downwind, and leaves featureburnt
 * (legacy:127772-127835, 128021-128116). */
#define FEATURE_FX_NONE     0
#define FEATURE_FX_DYING    1
#define FEATURE_FX_BURNING  2

/* Under the remastered rules a burning feature scorches every unit
 * within FEATURE_BURN_REACH px of its centre for FEATURE_BURN_DAMAGE,
 * once every FEATURE_BURN_EVERY of the original's frames (D-036). */
#define FEATURE_BURN_REACH   32
#define FEATURE_BURN_DAMAGE  25
#define FEATURE_BURN_EVERY   15

/* Under the remastered rules a burning feature throws FEATURE_SPARKS
 * sparks, each FEATURE_SPARK_DELAY percent of its spark time after the
 * last, and burns until the last. A spark reaches every cell within
 * three, or out to the nearest ring with a feature that can catch, at
 * most FEATURE_SPARK_REACH. Five downwind steps of 2*wind/2^
 * FEATURE_SPARK_WIND_SHIFT of a cell, at most FEATURE_SPARK_CARRY cells
 * along each axis, stretch that reach downwind. A feature catches at
 * FEATURE_SPARK_CHANCE percent of its spreadchance, FEATURE_SPARK_UPWIND
 * percent of that upwind, and at most FEATURE_SPARK_CATCH_CAP catch from
 * sparks in a frame (D-036). */
#define FEATURE_SPARK_REACH       6
#define FEATURE_SPARKS            4
#define FEATURE_SPARK_DELAY       125
#define FEATURE_SPARK_WIND_SHIFT  12
#define FEATURE_SPARK_CARRY       2
#define FEATURE_SPARK_CHANCE      200
#define FEATURE_SPARK_UPWIND      25
#define FEATURE_SPARK_CATCH_CAP   4

/* A blast at (x, y) px, `height` up: every feature on a cell within
 * `radius` of it, or on the blast's own cell, takes `damage` once, or
 * catches fire when `fire_starter` and it can burn (legacy:245240-245302). */
void Features_Blast(struct GameWorld *world, int32_t x, int32_t y, float height,
                    int radius, int damage, int fire_starter);

/* One of the original's 30 Hz frames: death and burn sequences play on,
 * finished ones give their cell to the next stage, sparks spread, then
 * the wind takes its turn (legacy:128380-128610, 241674-241718). The
 * engine calls it every second tick. */
void Features_TickFrame(struct GameWorld *world);

/* Damage, set fire to or destroy instance `idx`, as a blast would. */
void Features_DebugHit(struct GameWorld *world, int idx, int damage, int fire_starter);

/* The wind a map blows: its minwindspeed and maxwindspeed (legacy:168998-
 * 169001), the first wind on the battle's first frame. */
void Features_WindBegin(struct GameWorld *world, int min_speed, int max_speed);
/* Set the wind now, as the original's +wind does. */
void Features_DebugSetWind(struct GameWorld *world, int speed, uint16_t heading);

/* Give def `def_idx` a sequence of `frames` pictures, `frame_frames` of
 * the original's frames each, in place of what its files hold. `which`
 * is 0 death, 1 burn, 2 front flame, 3 back flame. For tests. */
int  Features_DebugSetSequence(int def_idx, int which, int frames, int frame_frames);

/* How long def `def_idx`'s death (`which` 0) or burn (1) runs in the
 * original's frames, or -1 when it has none. */
int  Features_SequenceFrames(int def_idx, int which);

/* Whether instance `idx` blocks movement: its def's `blocking` decides,
 * the original's per feature flag (legacy:219128), and under the
 * remastered rules so does rubble that blocks until swept (D-036). */
int  Features_InstanceBlocks(const struct GameWorld *world, int idx);

/* Restart instance `idx`'s decompose countdown. A corpse cannot rot
 * out from under a sweep or a raise: both refresh it every tick they
 * work on it (legacy:32394, legacy:13143). */
void Features_RefreshDecompose(struct GameWorld *world, int idx);

/* Age every placed instance by one simulation tick. When a countdown
 * runs out the body starts sinking and stops taking orders, and after
 * FEATURE_SINK_TICKS more it is removed (legacy:128400-128414). Call
 * once per sim tick. */
void Features_TickDecompose(struct GameWorld *world);

/* The original sinks a rotted corpse for 60 of its 30 Hz frames, by
 * 0x2000 (one eighth of a world unit) per frame (legacy:128408-128414).
 * Our tick is twice as fine. */
#define FEATURE_SINK_TICKS       120
#define FEATURE_SINK_PER_TICK    0.0625f

/* Remaining decompose ticks for instance `idx`, or -1 when the
 * instance has no countdown. For tests. */
int32_t Features_InstanceDecomposeTicks(const struct GameWorld *world,
                                        int idx);

/* Ticks instance `idx` has spent sinking, 0 while it is still whole. */
int  Features_InstanceSinkTicks(const struct GameWorld *world, int idx);

#endif /* TAK_FEATURES_H */
