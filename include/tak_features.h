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
