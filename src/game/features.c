/*
 * features.c -- Global feature definition registry.
 *
 * Walks data/features/<subdir>/*.tdf at startup, parses every
 * [section] into a FeatureDef, and indexes them by load order so the
 * TNT feature_layer's uint16 IDs resolve to authored feature data
 * (sprite filename, category, footprint, etc.).
 *
 * Mirrors the legacy engine's feature loader (the legacy reference
 * ~126890 — `FileSystem_FindFiles("features", "tdf", ...)`).
 * Subdirectory order is alphabetical, file order alphabetical
 * within each, and section order is the order they appear in each
 * file. This deterministic ordering matches what the TNT was
 * authored against.
 */

#include "tak_features.h"
#include "tak_world.h"
#include "tak_pathing.h"
#include "tak_tdf.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "tak_hpi.h"
#include "tak_sim_rand.h"
#include "tak_terrain.h"
#include "tak_unit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <strings.h>
#  include <sys/stat.h>
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static FeatureDef *g_feats = NULL;
static int         g_feat_count = 0;
static int         g_feat_cap = 0;

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src || cap == 0) { if (cap) dst[0] = 0; return; }
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int feat_grow(int new_count) {
    if (new_count <= g_feat_cap) return 0;
    int new_cap = g_feat_cap ? g_feat_cap : 256;
    while (new_cap < new_count) new_cap *= 2;
    FeatureDef *p = (FeatureDef *)tak_malloc((size_t)new_cap * sizeof(FeatureDef));
    if (!p) return -1;
    if (g_feats) {
        memcpy(p, g_feats, (size_t)g_feat_count * sizeof(FeatureDef));
        tak_free(g_feats);
    }
    g_feats = p;
    g_feat_cap = new_cap;
    return 0;
}

static int parse_feature_tdf(const char *vfs_path) {
    /* Each TDF holds N [section] feature definitions. We walk every
     * top-level section in declared order. */
    TDFFile *t = TDF_Open(vfs_path);
    if (!t) return 0;
    if (TDF_Load(t) != 0) { TDF_Close(t); return 0; }
    int added = 0;
    const char *name = TDF_GetFirstSection(t);
    while (name) {
        if (TDF_PushSection(t, name) == 0) {
            if (feat_grow(g_feat_count + 1) == 0) {
                FeatureDef *f = &g_feats[g_feat_count];
                memset(f, 0, sizeof(*f));
                copy_str(f->name,     sizeof(f->name),     name);
                copy_str(f->world,    sizeof(f->world),
                         TDF_ReadString(t, "world",    ""));
                copy_str(f->category, sizeof(f->category),
                         TDF_ReadString(t, "category", ""));
                copy_str(f->filename, sizeof(f->filename),
                         TDF_ReadString(t, "filename", ""));
                copy_str(f->seqname,  sizeof(f->seqname),
                         TDF_ReadString(t, "seqname",  ""));
                copy_str(f->seqname_shad, sizeof(f->seqname_shad),
                         TDF_ReadString(t, "seqnameshad", ""));
                f->shadtrans      = TDF_ReadInt(t, "shadtrans",      0);
                f->no_shadow      = TDF_ReadInt(t, "noshadow",       0);
                /* `object` names a 3DO and rules out the sprite path.
                 * Legacy reads it first and only reaches filename +
                 * seqname when it is absent (legacy:127098, 127136). */
                copy_str(f->object,   sizeof(f->object),
                         TDF_ReadString(t, "object",   ""));
                copy_str(f->feature_dead, sizeof(f->feature_dead),
                         TDF_ReadString(t, "featuredead", ""));
                f->footprint_x    = TDF_ReadInt(t, "footprintx",     1);
                f->footprint_z    = TDF_ReadInt(t, "footprintz",     1);
                f->height         = TDF_ReadInt(t, "height",         0);
                f->blocking       = TDF_ReadInt(t, "blocking",       0);
                f->reclaimable    = TDF_ReadInt(t, "reclaimable",    0);
                f->indestructible = TDF_ReadInt(t, "indestructible", 0);
                f->sacred_site    = TDF_ReadFloat(t, "sacredsite", 0.0f);
                f->damage         = TDF_ReadInt(t, "damage",         0);
                /* Reclaim payout and default-reclaim flag, parsed in the
                 * same feature block as the rest (legacy:127332,
                 * legacy:127349-127351). */
                f->energy           = TDF_ReadFloat(t, "energy", 0.0f);
                f->autoreclaimable  = TDF_ReadInt(t, "autoreclaimable", 1);
                /* Corpse lifetime and the two orders a corpse can
                 * take. decomposetime is seconds: the original reads
                 * it as a float, scales it by 30 into its frames and
                 * keeps the low 16 bits (legacy:127384-127386). The
                 * flag bits sit alongside reclaimable
                 * (legacy:127369-127378). */
                f->decompose_time   = (int)(TDF_ReadFloat(t, "decomposetime", 0.0f)
                                            * 30.0f) & 0xFFFF;
                f->resurrectable    = TDF_ReadInt(t, "resurrectable", 0);
                f->animatable       = TDF_ReadInt(t, "animatable",    0);
                f->is_building      = TDF_ReadInt(t, "isbuilding",    0);
                /* Death and fire (legacy:127184-127401). sparktime is
                 * seconds, scaled by 30 into the original's frames and
                 * kept to 16 bits. */
                copy_str(f->feature_burnt, sizeof(f->feature_burnt),
                         TDF_ReadString(t, "featureburnt", ""));
                copy_str(f->seqname_die, sizeof(f->seqname_die),
                         TDF_ReadString(t, "seqnamedie", ""));
                copy_str(f->seqname_die_shad, sizeof(f->seqname_die_shad),
                         TDF_ReadString(t, "seqnamedieshad", ""));
                copy_str(f->seqname_burn, sizeof(f->seqname_burn),
                         TDF_ReadString(t, "seqnameburn", ""));
                copy_str(f->seqname_burn_shad, sizeof(f->seqname_burn_shad),
                         TDF_ReadString(t, "seqnameburnshad", ""));
                copy_str(f->seqname_front_flame, sizeof(f->seqname_front_flame),
                         TDF_ReadString(t, "seqnamefrontflame", ""));
                copy_str(f->seqname_back_flame, sizeof(f->seqname_back_flame),
                         TDF_ReadString(t, "seqnamebackflame", ""));
                f->flamable      = TDF_ReadInt(t, "flamable", 0) & 1;
                f->spread_chance = TDF_ReadInt(t, "spreadchance", 0) & 0xff;
                f->spark_time    = (int)((double)TDF_ReadFloat(t, "sparktime", 0.0f)
                                         * 30.0) & 0xffff;
                copy_str(f->sound_class, sizeof(f->sound_class),
                         TDF_ReadString(t, "SoundClass", ""));
                if (f->sound_class[0]) {
                    float delay = TDF_ReadFloat(t, "SoundDelay", -1.0f);
                    float var   = TDF_ReadFloat(t, "SoundVariance", 0.0f);
                    if (delay < 1.0f) {
                        f->sound_class[0] = '\0';
                    } else {
                        f->sound_delay_ticks    = (int)(delay * 60.0f);
                        f->sound_variance_ticks = var > 0.0f ? (int)(var * 60.0f) : 0;
                    }
                }
                g_feat_count++;
                added++;
            }
            TDF_PopSection(t);
        }
        name = TDF_GetNextSection(t);
    }
    TDF_Close(t);
    return added;
}

static int str_cmp_qsort(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Order like the legacy walk: subdirectory, then file name, plain
 * strcmp on each. TNT feature ids index this order. */
static int feature_path_cmp(const void *a, const void *b) {
    const char *pa = *(const char *const *)a, *pb = *(const char *const *)b;
    const char *fa = strrchr(pa, '/'), *fb = strrchr(pb, '/');
    fa = fa ? fa + 1 : pa;
    fb = fb ? fb + 1 : pb;
    size_t da = (size_t)(fa - pa), db = (size_t)(fb - pb);
    int c = strncmp(pa, pb, da < db ? da : db);
    if (c) return c;
    if (da != db) return da < db ? -1 : 1;
    return strcmp(fa, fb);
}

/* Exactly one directory below root, like the legacy one-level walk. */
static int one_level_below(const char *path, const char *root) {
    size_t rl = strlen(root);
    if (tak_strnicmp(path, root, rl) != 0) return 0;
    const char *rest = path + rl;
    const char *slash = strchr(rest, '/');
    return slash && slash != rest && strchr(slash + 1, '/') == NULL;
}

int Features_LoadAll(void) {
    Features_FreeAll();

    /* Archives keep features/<dir>/*.tdf; the loose dev tree adds a
     * leading data/. Archive layout first so an archive-only browser
     * session works. */
    static const char *const roots[] = { "features/", "data/features/" };
    const char *root = roots[0];
    char **files = NULL;
    int n = 0;
    for (size_t r = 0; r < 2; r++) {
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "%s*/*.tdf", roots[r]);
        if (VFS_ListFiles(pattern, &files, &n) == 0 && n > 0) { root = roots[r]; break; }
        if (files) { for (int i = 0; i < n; i++) tak_free(files[i]); tak_free(files); }
        files = NULL;
        n = 0;
    }
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (one_level_below(files[i], root)) files[kept++] = files[i];
        else tak_free(files[i]);
    }
    if (kept > 1) qsort(files, kept, sizeof(char *), feature_path_cmp);
    for (int i = 0; i < kept; i++) {
        parse_feature_tdf(files[i]);
        tak_free(files[i]);
    }
    tak_free(files);

    fprintf(stderr, "Features_LoadAll: %d feature defs registered\n",
            g_feat_count);
    return g_feat_count;
}

const FeatureDef *Features_GetByIndex(int idx) {
    if (idx < 0 || idx >= g_feat_count) return NULL;
    return &g_feats[idx];
}

int Features_GetCount(void) { return g_feat_count; }

int Features_FindByName(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_feat_count; i++) {
#ifdef _WIN32
        if (_stricmp(g_feats[i].name, name) == 0) return i;
#else
        if (strcasecmp(g_feats[i].name, name) == 0) return i;
#endif
    }
    return -1;
}

static void hold_note_removed(int idx);
static void fx_reset(void);

/* The frame's sparks find a cell's feature in a grid by top-left cell,
 * under the remastered rules. A feature that comes or goes leaves the
 * grid stale until the next spark builds it again. */
static int *g_spark_cells;
static int  g_spark_cells_cap, g_spark_cells_w, g_spark_cells_h;
static int  g_spark_cells_ok;
static void feat_event(const struct GameWorld *w, int kind, int idx, int def, int new_def,
                       int damage, int left, int frames);
static int  g_remove_kind;

/* ── Placed instances ───────────────────────────────────────────────
 *
 * Legacy resolves a sweep-cursor click to the map cell record, then to
 * the feature the cell holds, and only issues the reclaim order when
 * that record is a live feature (legacy:187186-187198). Our TNT loader
 * keeps the same set as world->features, so the lookup is a footprint
 * test over that array. */

/* An instance's footprint, turned with it. */
static void inst_fp(const FeatureDef *fd, const struct MapFeature *mf,
                    int *fx, int *fz) {
    int x = (fd && fd->footprint_x > 0) ? fd->footprint_x : 1;
    int z = (fd && fd->footprint_z > 0) ? fd->footprint_z : 1;
    if (mf && (mf->facing & 1)) { int t = x; x = z; z = t; }
    *fx = x;
    *fz = z;
}

void Features_InstanceFootprint(const struct GameWorld *world, int idx,
                                int *out_fx, int *out_fz) {
    int fx = 1, fz = 1;
    if (world && world->features && idx >= 0 && idx < world->feature_count)
        inst_fp(Features_GetByIndex(world->features[idx].global_idx),
                &world->features[idx], &fx, &fz);
    if (out_fx) *out_fx = fx;
    if (out_fz) *out_fz = fz;
}

static int feat_footprint_hit(const FeatureDef *fd,
                              const struct MapFeature *mf,
                              int32_t wx, int32_t wy) {
    int fp_x, fp_z;
    inst_fp(fd, mf, &fp_x, &fp_z);
    int32_t x0 = (int32_t)mf->tile_x * 16;
    int32_t y0 = (int32_t)mf->tile_z * 16;
    return wx >= x0 && wx < x0 + fp_x * 16 &&
           wy >= y0 && wy < y0 + fp_z * 16;
}

/* An instance that has started to rot has lost its orders: the
 * original clears the reclaimable, resurrectable and animatable bits
 * on the instance the frame its countdown ends (legacy:128403-128405),
 * and every cursor check reads the instance bit as well as the def's
 * (legacy:129476-129480, 129501-129505). */
static int feat_instance_rotting(const struct MapFeature *mf) {
    return mf->sink_ticks > 0;
}

#define FEAT_WANT_RECLAIM   0
#define FEAT_WANT_RESURRECT 1
#define FEAT_WANT_ANIMATE   2

static int feat_def_offers(const FeatureDef *fd, int want) {
    switch (want) {
        case FEAT_WANT_RESURRECT: return fd->resurrectable != 0;
        case FEAT_WANT_ANIMATE:   return fd->animatable != 0;
        default:                  return fd->reclaimable != 0;
    }
}

static int find_offering_at(const struct GameWorld *world,
                            int32_t world_x, int32_t world_y, int want) {
    if (!world || !world->features) return -1;
    /* Nearest-centre wins when footprints overlap, so a click always
     * takes the feature it visually landed on. */
    int best = -1;
    int64_t best_d2 = 0;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || !feat_def_offers(fd, want)) continue;
        if (feat_instance_rotting(&world->features[i])) continue;
        if (!feat_footprint_hit(fd, &world->features[i], world_x, world_y))
            continue;
        int32_t cx, cy;
        if (Features_InstanceCentre(world, i, &cx, &cy) != 0) continue;
        int64_t dx = cx - world_x, dy = cy - world_y;
        int64_t d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < best_d2) { best = i; best_d2 = d2; }
    }
    return best;
}

int Features_FindReclaimableAt(const struct GameWorld *world,
                               int32_t world_x, int32_t world_y) {
    return find_offering_at(world, world_x, world_y, FEAT_WANT_RECLAIM);
}

int Features_FindResurrectableAt(const struct GameWorld *world,
                                 int32_t world_x, int32_t world_y) {
    return find_offering_at(world, world_x, world_y, FEAT_WANT_RESURRECT);
}

int Features_FindAnimatableAt(const struct GameWorld *world,
                              int32_t world_x, int32_t world_y) {
    return find_offering_at(world, world_x, world_y, FEAT_WANT_ANIMATE);
}

int Features_InstanceCentre(const struct GameWorld *world, int idx,
                            int32_t *out_x, int32_t *out_y) {
    if (!world || !world->features) return -1;
    if (idx < 0 || idx >= world->feature_count) return -1;
    int fp_x, fp_z;
    Features_InstanceFootprint(world, idx, &fp_x, &fp_z);
    if (out_x) *out_x = (int32_t)world->features[idx].tile_x * 16 + fp_x * 8;
    if (out_y) *out_y = (int32_t)world->features[idx].tile_z * 16 + fp_z * 8;
    return 0;
}

/* decompose_time holds the original's 30 Hz frames, counted down once
 * per frame (legacy:128400-128402). Our tick is twice as fine. */
static int32_t decompose_ticks_for(const FeatureDef *fd) {
    return (fd && fd->decompose_time > 0) ? fd->decompose_time * 2 : -1;
}

static void feat_rect(const struct GameWorld *world, int idx,
                      int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    const struct MapFeature *mf = &world->features[idx];
    int fp_x, fp_z;
    inst_fp(Features_GetByIndex(mf->global_idx), mf, &fp_x, &fp_z);
    *x0 = (int32_t)mf->tile_x * 16;
    *y0 = (int32_t)mf->tile_z * 16;
    *x1 = *x0 + fp_x * 16;
    *y1 = *y0 + fp_z * 16;
}

int Features_InstanceBlocks(const struct GameWorld *world, int idx) {
    if (!world || !world->features || idx < 0 || idx >= world->feature_count) return 0;
    const FeatureDef *fd = Features_GetByIndex(world->features[idx].global_idx);
    if (fd && fd->blocking) return 1;
    return world->features[idx].rubble && world->cfg.remastered;
}

int Features_AddInstance(struct GameWorld *world, int global_idx,
                         int cell_x, int cell_z,
                         int32_t world_x, int32_t world_y,
                         uint16_t heading, int color_idx) {
    return Features_AddInstanceFacing(world, global_idx, cell_x, cell_z,
                                      world_x, world_y, heading, color_idx, 0);
}

static int  feat_top_current(const struct GameWorld *w);
static void feat_top_stamp(struct GameWorld *w, int i, int x0, int z0,
                           int x1, int z1);

int Features_AddInstanceFacing(struct GameWorld *world, int global_idx,
                               int cell_x, int cell_z,
                               int32_t world_x, int32_t world_y,
                               uint16_t heading, int color_idx, int facing) {
    if (!world) return -1;
    const FeatureDef *fd = Features_GetByIndex(global_idx);
    if (!fd) return -1;
    int fp_x = (fd->footprint_x > 0) ? fd->footprint_x : 1;
    int fp_z = (fd->footprint_z > 0) ? fd->footprint_z : 1;
    facing &= 3;
    if (facing & 1) { int t = fp_x; fp_x = fp_z; fp_z = t; }
    /* The whole footprint has to fit on the map (legacy:128169-128173). */
    if (cell_x < 0 || cell_z < 0 || cell_x > 0xFFFF || cell_z > 0xFFFF)
        return -1;
    int cells_w = world->map_pixels_w / 16;
    int cells_h = world->map_pixels_h / 16;
    if (cells_w > 0 && cell_x + fp_x > cells_w) return -1;
    if (cells_h > 0 && cell_z + fp_z > cells_h) return -1;

    /* Whatever already stands in those cells is cleared first, and an
     * indestructible occupant refuses the whole placement
     * (legacy:128173-128185, 128843). */
    int32_t x0 = (int32_t)cell_x * 16, y0 = (int32_t)cell_z * 16;
    int32_t x1 = x0 + fp_x * 16, y1 = y0 + fp_z * 16;
    for (int i = 0; i < world->feature_count; i++) {
        int32_t ox0, oy0, ox1, oy1;
        feat_rect(world, i, &ox0, &oy0, &ox1, &oy1);
        if (ox1 <= x0 || ox0 >= x1 || oy1 <= y0 || oy0 >= y1) continue;
        const FeatureDef *od =
            Features_GetByIndex(world->features[i].global_idx);
        if (od && od->indestructible) return -1;
    }
    for (int i = world->feature_count - 1; i >= 0; i--) {
        int32_t ox0, oy0, ox1, oy1;
        feat_rect(world, i, &ox0, &oy0, &ox1, &oy1);
        if (ox1 <= x0 || ox0 >= x1 || oy1 <= y0 || oy0 >= y1) continue;
        Features_RemoveInstance(world, i);
    }
    int grid_current = feat_top_current(world);

    if (world->feature_count >= world->feature_cap) {
        int cap = world->feature_cap ? world->feature_cap * 2 : 64;
        struct MapFeature *p = (struct MapFeature *)
            tak_malloc((size_t)cap * sizeof(*p));
        if (!p) return -1;
        if (world->features) {
            memcpy(p, world->features,
                   (size_t)world->feature_count * sizeof(*p));
            tak_free(world->features);
        }
        world->features = p;
        world->feature_cap = cap;
    }
    struct MapFeature *mf = &world->features[world->feature_count];
    memset(mf, 0, sizeof(*mf));
    mf->feat_id    = 0xFFFFu;   /* no TNT id: this one was not authored */
    mf->tile_x     = (uint16_t)cell_x;
    mf->tile_z     = (uint16_t)cell_z;
    mf->global_idx = global_idx;
    mf->world_x    = world_x;
    mf->world_y    = world_y;
    mf->heading    = heading;
    mf->pitch      = 0;
    mf->roll       = 0;
    mf->color_idx  = (int16_t)((color_idx >= 0 && color_idx <= 11)
                               ? color_idx : -1);
    mf->decompose_ticks = decompose_ticks_for(fd);
    mf->sink_ticks = 0;
    mf->facing = (uint8_t)facing;
    if (fd->sacred_site > 0.0f) Features_NoteListReplaced();
    int idx = world->feature_count++;
    g_spark_cells_ok = 0;
    /* The shot grid takes the new feature in place when it was current. */
    if (grid_current) {
        feat_top_stamp(world, idx, 0, 0, world->feat_top_w, world->feat_top_h);
        world->feat_top_count = world->feature_count;
        world->feat_top_src = world->features;
    } else {
        Features_MarkChanged(world);
    }
    /* Route planning caches terrain blocking, so a body that blocks
     * has to tell it (the original's placement tells the pathfinder
     * the same way, legacy:128329). */
    if (fd->blocking)
        TAK_PathCacheFeatureChanged(world, cell_x, cell_z,
                                    cell_x + fp_x - 1, cell_z + fp_z - 1);
    feat_event(world, FEATURE_EVENT_PLACED, idx, global_idx, -1, 0, 0, 0);
    return idx;
}

void Features_RefreshDecompose(struct GameWorld *world, int idx) {
    if (!world || !world->features) return;
    if (idx < 0 || idx >= world->feature_count) return;
    const FeatureDef *fd =
        Features_GetByIndex(world->features[idx].global_idx);
    if (!fd || fd->decompose_time <= 0) return;
    if (world->features[idx].sink_ticks > 0) return;
    world->features[idx].decompose_ticks = decompose_ticks_for(fd);
}

void Features_TickDecompose(struct GameWorld *world) {
    if (!world || !world->features) return;
    /* Walk backwards: removal compacts the array, so counting down
     * keeps the untouched entries where they are. */
    for (int i = world->feature_count - 1; i >= 0; i--) {
        struct MapFeature *mf = &world->features[i];
        if (mf->decompose_ticks > 0 && --mf->decompose_ticks == 0) {
            /* Rotted: the body starts sinking and takes no more
             * orders (legacy:128403-128405). */
            mf->sink_ticks = 1;
        }
        if (mf->sink_ticks > 0) {
            /* Sinks for the original's 60 frames, then the record
             * goes (legacy:128407-128414). */
            if (++mf->sink_ticks > FEATURE_SINK_TICKS)
                Features_RemoveInstance(world, i);
        }
    }
}

int32_t Features_InstanceDecomposeTicks(const struct GameWorld *world,
                                        int idx) {
    if (!world || !world->features) return -1;
    if (idx < 0 || idx >= world->feature_count) return -1;
    return world->features[idx].decompose_ticks;
}

int Features_InstanceSinkTicks(const struct GameWorld *world, int idx) {
    if (!world || !world->features) return 0;
    if (idx < 0 || idx >= world->feature_count) return 0;
    return world->features[idx].sink_ticks;
}

static uint32_t g_sacred_gen;
uint32_t Features_SacredGeneration(void) { return g_sacred_gen; }
void     Features_NoteListReplaced(void) { g_sacred_gen++; }

int Features_RemoveInstance(struct GameWorld *world, int idx) {
    if (!world || !world->features) return -1;
    if (idx < 0 || idx >= world->feature_count) return -1;
    if (g_remove_kind >= 0)
        feat_event(world, g_remove_kind, idx, world->features[idx].global_idx, -1, 0, 0, 0);
    const FeatureDef *fd =
        Features_GetByIndex(world->features[idx].global_idx);
    int blocked = Features_InstanceBlocks(world, idx);
    int bx0 = 0, bz0 = 0, bx1 = -1, bz1 = -1;
    if (blocked) {
        int fx, fz;
        inst_fp(fd, &world->features[idx], &fx, &fz);
        bx0 = world->features[idx].tile_x;
        bz0 = world->features[idx].tile_z;
        bx1 = bx0 + fx - 1;
        bz1 = bz0 + fz - 1;
    }
    if (fd && fd->sacred_site > 0.0f) g_sacred_gen++;
    int grid_current = feat_top_current(world);
    int rx0 = 0, rz0 = 0, rx1 = 0, rz1 = 0;
    if (grid_current && fd) {
        int fx, fz;
        inst_fp(fd, &world->features[idx], &fx, &fz);
        rx0 = world->features[idx].tile_x;
        rz0 = world->features[idx].tile_z;
        rx1 = rx0 + fx;
        rz1 = rz0 + fz;
        if (rx1 > world->feat_top_w) rx1 = world->feat_top_w;
        if (rz1 > world->feat_top_h) rz1 = world->feat_top_h;
    }
    /* Compact rather than tombstone: every consumer (walkability,
     * rendering, sacred-site scan) walks the array live, so the cleared
     * cell stops blocking on the next query with no other edits. */
    for (int i = idx; i + 1 < world->feature_count; i++)
        world->features[i] = world->features[i + 1];
    world->feature_count--;
    hold_note_removed(idx);
    /* The shot grid gives up the feature's cells to whatever else stands
     * on them, in place when it was current. */
    if (grid_current) {
        for (int z = rz0; z < rz1; z++)
            for (int x = rx0; x < rx1; x++)
                world->feat_top[z * world->feat_top_w + x] = 0;
        int span = world->feat_top_span;
        for (int i = 0; i < world->feature_count; i++) {
            const struct MapFeature *mf = &world->features[i];
            if (mf->tile_x >= rx1 || mf->tile_z >= rz1 ||
                mf->tile_x + span <= rx0 || mf->tile_z + span <= rz0)
                continue;
            feat_top_stamp(world, i, rx0, rz0, rx1, rz1);
        }
        world->feat_top_count = world->feature_count;
    } else {
        Features_MarkChanged(world);
    }
    if (blocked) TAK_PathCacheFeatureChanged(world, bx0, bz0, bx1, bz1);
    return 0;
}

int Features_SweepInstance(struct GameWorld *world, int idx) {
    g_remove_kind = FEATURE_EVENT_SWEPT;
    int r = Features_RemoveInstance(world, idx);
    g_remove_kind = FEATURE_EVENT_REMOVED;
    return r;
}

void Features_MarkChanged(struct GameWorld *world) {
    if (world) world->feat_top_clean = 0;
}

/* The grid is built and matches the feature list. */
static int feat_top_current(const struct GameWorld *w) {
    return w->feat_top && w->feat_top_clean &&
           w->feat_top_count == w->feature_count &&
           w->feat_top_src == (const void *)w->features;
}

/* Feature i's height byte on the cells of its footprint that fall in
 * [x0, x1) by [z0, z1), where it stands tallest. The original keeps the
 * feature in the map cell record, so every cell of its footprint
 * answers for it (legacy:245444-245446). */
static void feat_top_stamp(struct GameWorld *w, int i, int x0, int z0,
                           int x1, int z1) {
    const struct MapFeature *mf = &w->features[i];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    if (!fd) return;
    int fx, fz;
    inst_fp(fd, mf, &fx, &fz);
    if (fx > w->feat_top_span) w->feat_top_span = fx;
    if (fz > w->feat_top_span) w->feat_top_span = fz;
    int ax = mf->tile_x > x0 ? mf->tile_x : x0;
    int az = mf->tile_z > z0 ? mf->tile_z : z0;
    int bx = mf->tile_x + fx < x1 ? mf->tile_x + fx : x1;
    int bz = mf->tile_z + fz < z1 ? mf->tile_z + fz : z1;
    uint16_t v = (uint16_t)(1 + (fd->height & 0xff));
    for (int z = az; z < bz; z++) {
        for (int x = ax; x < bx; x++) {
            uint16_t *c = &w->feat_top[z * w->feat_top_w + x];
            if (v > *c) *c = v;
        }
    }
}

static void feat_top_rebuild(struct GameWorld *w) {
    int cw = w->map_pixels_w / 16, ch = w->map_pixels_h / 16;
    if (cw <= 0 || ch <= 0) {
        tak_free(w->feat_top);
        w->feat_top = NULL;
        w->feat_top_w = w->feat_top_h = 0;
    } else if (!w->feat_top || w->feat_top_w != cw || w->feat_top_h != ch) {
        tak_free(w->feat_top);
        w->feat_top = (uint16_t *)tak_malloc((size_t)cw * (size_t)ch *
                                             sizeof(uint16_t));
        w->feat_top_w = w->feat_top ? cw : 0;
        w->feat_top_h = w->feat_top ? ch : 0;
    }
    w->feat_top_clean = 1;
    w->feat_top_count = w->feature_count;
    w->feat_top_src = w->features;
    if (!w->feat_top) return;
    memset(w->feat_top, 0, (size_t)cw * (size_t)ch * sizeof(uint16_t));
    w->feat_top_span = 0;
    for (int i = 0; i < w->feature_count; i++)
        feat_top_stamp(w, i, 0, 0, cw, ch);
}

int Features_TopAt(struct GameWorld *world, int cell_x, int cell_z) {
    if (!world) return 0;
    if (!world->feat_top_clean || world->feat_top_count != world->feature_count ||
        world->feat_top_src != (const void *)world->features)
        feat_top_rebuild(world);
    if (!world->feat_top || cell_x < 0 || cell_z < 0 ||
        cell_x >= world->feat_top_w || cell_z >= world->feat_top_h)
        return 0;
    return world->feat_top[cell_z * world->feat_top_w + cell_x];
}

/* ── Death and fire sequences ───────────────────────────────────────
 *
 * The simulation needs only how long each picture of a sequence shows,
 * so a sequence is read once for its frame count and durations and
 * kept by file and name. The pictures themselves are the view's. */

typedef struct FeatSeq {
    char     file[48];
    char     name[40];
    uint16_t frames;
    uint32_t dur_at;
} FeatSeq;

static FeatSeq  *g_seqs;
static int       g_seq_count, g_seq_cap;
static uint16_t *g_seq_dur;
static int       g_seq_dur_count, g_seq_dur_cap;
static char    (*g_seq_files)[48];
static int       g_seq_file_count, g_seq_file_cap;

/* Per def, what its death and burn resolve to, and its next stages. */
typedef struct FeatFxDef {
    uint8_t resolved;
    int16_t seq[4];      /* death, burn, front flame, back flame; -1 none */
    int32_t dead_idx;
    int32_t burnt_idx;
} FeatFxDef;

static FeatFxDef *g_fxdefs;
static int        g_fxdef_cap;

#define FSEQ_DIE   0
#define FSEQ_BURN  1
#define FSEQ_FRONT 2
#define FSEQ_BACK  3

static int fx_grow(int count) {
    if (count <= g_fxdef_cap) return 0;
    int cap = g_fxdef_cap ? g_fxdef_cap : 256;
    while (cap < count) cap *= 2;
    FeatFxDef *p = (FeatFxDef *)tak_calloc((size_t)cap, sizeof(FeatFxDef));
    if (!p) return -1;
    if (g_fxdefs) {
        memcpy(p, g_fxdefs, (size_t)g_fxdef_cap * sizeof(FeatFxDef));
        tak_free(g_fxdefs);
    }
    g_fxdefs = p;
    g_fxdef_cap = cap;
    return 0;
}

static void fx_reset(void) {
    tak_free(g_fxdefs);
    g_fxdefs = NULL;
    g_fxdef_cap = 0;
    tak_free(g_seqs);
    g_seqs = NULL;
    g_seq_count = g_seq_cap = 0;
    tak_free(g_seq_dur);
    g_seq_dur = NULL;
    g_seq_dur_count = g_seq_dur_cap = 0;
    tak_free(g_seq_files);
    g_seq_files = NULL;
    g_seq_file_count = g_seq_file_cap = 0;
}

static int seq_add(const char *file, const char *name, int frames,
                   const uint16_t *dur) {
    if (frames < 0) frames = 0;
    if (g_seq_count >= g_seq_cap) {
        int cap = g_seq_cap ? g_seq_cap * 2 : 64;
        FeatSeq *p = (FeatSeq *)tak_malloc((size_t)cap * sizeof(FeatSeq));
        if (!p) return -1;
        if (g_seqs) {
            memcpy(p, g_seqs, (size_t)g_seq_count * sizeof(FeatSeq));
            tak_free(g_seqs);
        }
        g_seqs = p;
        g_seq_cap = cap;
    }
    if (g_seq_dur_count + frames > g_seq_dur_cap) {
        int cap = g_seq_dur_cap ? g_seq_dur_cap : 1024;
        while (cap < g_seq_dur_count + frames) cap *= 2;
        uint16_t *p = (uint16_t *)tak_malloc((size_t)cap * sizeof(uint16_t));
        if (!p) return -1;
        if (g_seq_dur) {
            memcpy(p, g_seq_dur, (size_t)g_seq_dur_count * sizeof(uint16_t));
            tak_free(g_seq_dur);
        }
        g_seq_dur = p;
        g_seq_dur_cap = cap;
    }
    FeatSeq *s = &g_seqs[g_seq_count];
    copy_str(s->file, sizeof(s->file), file);
    copy_str(s->name, sizeof(s->name), name);
    s->frames = (uint16_t)frames;
    s->dur_at = (uint32_t)g_seq_dur_count;
    for (int i = 0; i < frames; i++) g_seq_dur[g_seq_dur_count++] = dur[i];
    return g_seq_count++;
}

static int seq_lookup(const char *file, const char *name) {
    for (int i = 0; i < g_seq_count; i++)
        if (tak_stricmp(g_seqs[i].file, file) == 0 &&
            tak_stricmp(g_seqs[i].name, name) == 0) return i;
    return -1;
}

static int seq_file_seen(const char *file) {
    for (int i = 0; i < g_seq_file_count; i++)
        if (tak_stricmp(g_seq_files[i], file) == 0) return 1;
    return 0;
}

static void seq_file_mark(const char *file) {
    if (g_seq_file_count >= g_seq_file_cap) {
        int cap = g_seq_file_cap ? g_seq_file_cap * 2 : 32;
        char (*p)[48] = (char (*)[48])tak_malloc((size_t)cap * 48);
        if (!p) return;
        if (g_seq_files) {
            memcpy(p, g_seq_files, (size_t)g_seq_file_count * 48);
            tak_free(g_seq_files);
        }
        g_seq_files = p;
        g_seq_file_cap = cap;
    }
    copy_str(g_seq_files[g_seq_file_count++], 48, file);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Every sequence of a GAF or TAF, kept under `key`: its frame count from
 * the entry and each frame's duration from the frame table, the second
 * word of each entry (legacy:255142-255152). Returns the entry count, or
 * -1 when the file cannot be read. */
static int seq_read_file(const char *path, const char *key) {
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &data, &size) != 0 || !data) return -1;
    const uint8_t *b = (const uint8_t *)data;
    int added = 0;
    if (size >= 12 && rd32(b) == 0x00010100u) {
        uint32_t n = rd32(b + 4);
        for (uint32_t e = 0; e < n && 12 + 4 * (e + 1) <= size; e++) {
            uint32_t off = rd32(b + 12 + 4 * e);
            if (off + 40 > size) continue;
            int frames = b[off] | (b[off + 1] << 8);
            char name[33];
            memcpy(name, b + off + 8, 32);
            name[32] = '\0';
            if (frames < 0 || off + 40 + (uint32_t)frames * 8 > size) continue;
            uint16_t dur[512];
            if (frames > 512) frames = 512;
            for (int f = 0; f < frames; f++)
                dur[f] = (uint16_t)rd32(b + off + 40 + 8 * (uint32_t)f + 4);
            if (seq_add(key, name, frames, dur) >= 0) added++;
        }
    }
    VFS_FreeBuffer(data);
    return added;
}

/* A sequence in a feature's GAF, data/anims/<file>.gaf, or -1. */
static int seq_in_gaf(const char *file, const char *name) {
    if (!file || !file[0] || !name || !name[0]) return -1;
    int s = seq_lookup(file, name);
    if (s >= 0 || seq_file_seen(file)) return s;
    seq_file_mark(file);
    char path[96];
    snprintf(path, sizeof(path), "data/anims/%s.gaf", file);
    seq_read_file(path, file);
    return seq_lookup(file, name);
}

/* A flame, its own TAF named after it (legacy:127296-127330), or -1.
 * A file whose sequence goes by another name gives its first one. */
static int seq_flame(const char *name) {
    if (!name || !name[0]) return -1;
    char key[48];
    snprintf(key, sizeof(key), "*%s", name);
    if (!seq_file_seen(key)) {
        seq_file_mark(key);
        static const char *const fmt[] = { "data/anims/%s_4444.taf",
                                           "data/anims/%s_1555.taf",
                                           "data/anims/%s.gaf" };
        for (size_t i = 0; i < sizeof(fmt) / sizeof(fmt[0]); i++) {
            char path[96];
            snprintf(path, sizeof(path), fmt[i], name);
            if (seq_read_file(path, key) >= 0) break;
        }
    }
    int s = seq_lookup(key, name);
    if (s >= 0) return s;
    for (int i = 0; i < g_seq_count; i++)
        if (tak_stricmp(g_seqs[i].file, key) == 0) return i;
    return -1;
}

static const FeatFxDef *fx_def(int global_idx) {
    if (global_idx < 0 || global_idx >= g_feat_count) return NULL;
    if (fx_grow(g_feat_count) != 0) return NULL;
    FeatFxDef *x = &g_fxdefs[global_idx];
    if (x->resolved) return x;
    x->resolved = 1;
    const FeatureDef *fd = &g_feats[global_idx];
    for (int k = 0; k < 4; k++) x->seq[k] = -1;
    /* A model feature has no sequences, the GAF ones alone die and burn
     * in pictures (legacy:127100-127139). */
    if (!fd->object[0]) {
        x->seq[FSEQ_DIE]   = (int16_t)seq_in_gaf(fd->filename, fd->seqname_die);
        x->seq[FSEQ_BURN]  = (int16_t)seq_in_gaf(fd->filename, fd->seqname_burn);
        x->seq[FSEQ_FRONT] = (int16_t)seq_flame(fd->seqname_front_flame);
        x->seq[FSEQ_BACK]  = (int16_t)seq_flame(fd->seqname_back_flame);
    }
    x->dead_idx  = fd->feature_dead[0]  ? Features_FindByName(fd->feature_dead)  : -1;
    x->burnt_idx = fd->feature_burnt[0] ? Features_FindByName(fd->feature_burnt) : -1;
    return x;
}

int Features_DebugSetSequence(int def_idx, int which, int frames, int frame_frames) {
    if (which < 0 || which > 3 || !fx_def(def_idx)) return -1;
    uint16_t dur[512];
    if (frames > 512) frames = 512;
    for (int i = 0; i < frames; i++) dur[i] = (uint16_t)frame_frames;
    char key[48];
    snprintf(key, sizeof(key), "#%d.%d", def_idx, which);
    int s = seq_add(key, key, frames, dur);
    if (s < 0) return -1;
    g_fxdefs[def_idx].seq[which] = (int16_t)s;
    return 0;
}

int Features_SequenceFrames(int def_idx, int which) {
    const FeatFxDef *x = fx_def(def_idx);
    if (!x || which < 0 || which > 3 || x->seq[which] < 0) return -1;
    const FeatSeq *s = &g_seqs[x->seq[which]];
    int total = 0;
    for (int i = 0; i < s->frames; i++) {
        int d = g_seq_dur[s->dur_at + (uint32_t)i];
        total += d > 1 ? d : 1;
    }
    return total;
}

/* The original's picture player: a frame shows for its duration in
 * frames, then the next, and a sequence that does not loop stops after
 * its last (legacy:255756-255795). */
static void anim_start(int seq, uint8_t *on, uint16_t *frame, uint16_t *wait) {
    *frame = 0;
    *on = (seq >= 0 && g_seqs[seq].frames > 0) ? 1 : 0;
    *wait = *on ? g_seq_dur[g_seqs[seq].dur_at] : 0;
}

static void anim_step(int seq, int loop, uint8_t *on, uint16_t *frame,
                      uint16_t *wait) {
    if (!*on || seq < 0) return;
    if (*wait >= 2) { (*wait)--; return; }
    (*frame)++;
    if (*frame >= g_seqs[seq].frames) {
        if (!loop) { *on = 0; return; }
        *frame = 0;
    }
    *wait = g_seqs[seq].frames > 0
            ? g_seq_dur[g_seqs[seq].dur_at + *frame] : 0xffff;
}

/* ── Indices a pass is holding ──────────────────────────────────────
 *
 * A blast or a frame lists the features it will work on before it
 * works on them. One that goes from the list meanwhile is struck off,
 * and those above it move down with it. */
static int *g_hold;
static int  g_hold_count;

/* ── What a host hears ─────────────────────────────────────────────── */

static FeatureEventHook g_event_hook = NULL;
void Features_SetEventHook(FeatureEventHook hook) { g_event_hook = hook; }

/* The blast whose pass is running, and what a removal is told as: -1
 * for a stage going, which its death or burn tells. */
static int     g_event_blast;
static int32_t g_event_bx, g_event_by;
static int     g_remove_kind = FEATURE_EVENT_REMOVED;

static void feat_event(const struct GameWorld *w, int kind, int idx, int def, int new_def,
                       int damage, int left, int frames) {
    if (!g_event_hook || !w || idx < 0 || idx >= w->feature_count) return;
    FeatureEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.idx = idx;
    e.def = def;
    e.new_def = new_def;
    e.x = w->features[idx].world_x;
    e.y = w->features[idx].world_y;
    e.damage = damage;
    e.left = left;
    e.frames = frames;
    e.blast = g_event_blast;
    e.bx = g_event_bx;
    e.by = g_event_by;
    g_event_hook(w, &e);
}

static void hold_note_removed(int idx) {
    g_spark_cells_ok = 0;
    for (int i = 0; i < g_hold_count; i++) {
        if (g_hold[i] == idx) g_hold[i] = -1;
        else if (g_hold[i] > idx) g_hold[i]--;
    }
}

/* ── The remastered rules (D-036) ──────────────────────────────────── */

/* Kinds the original never lets break that the remastered rules do:
 * rocks, ruins, spires and grass, never a lodestone, a sacred site, a
 * wave or a sound emitter. Each takes its file's damage, or the kind's
 * own figure where the file has none. */
static int remaster_breakable_hp(const FeatureDef *fd) {
    static const struct { const char *kind; int hp; } kinds[] = {
        { "rocks", 3000 }, { "ruins", 4000 }, { "spire", 2000 }, { "grasses", 200 },
    };
    if (!fd || fd->sacred_site > 0.0f) return 0;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (tak_stricmp(fd->category, kinds[i].kind) == 0)
            return (fd->damage & 0xffff) > 0 ? (fd->damage & 0xffff) : kinds[i].hp;
    return 0;
}

/* A wall, a model wall or a building leaves rubble that blocks. */
static int remaster_leaves_rubble(const FeatureDef *fd) {
    if (!fd) return 0;
    return tak_stricmp(fd->category, "walls") == 0 ||
           tak_stricmp(fd->category, "wall") == 0 ||
           tak_stricmp(fd->category, "buildings") == 0;
}

/* ── The next stage ────────────────────────────────────────────────── */

/* The cells of [x0, x1) by [z0, z1) take their shot heights again from
 * whatever stands on them. */
static void feat_top_refresh(struct GameWorld *w, int x0, int z0, int x1, int z1) {
    if (!feat_top_current(w)) { Features_MarkChanged(w); return; }
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > w->feat_top_w) x1 = w->feat_top_w;
    if (z1 > w->feat_top_h) z1 = w->feat_top_h;
    for (int z = z0; z < z1; z++)
        for (int x = x0; x < x1; x++)
            w->feat_top[z * w->feat_top_w + x] = 0;
    int span = w->feat_top_span;
    for (int i = 0; i < w->feature_count; i++) {
        const struct MapFeature *mf = &w->features[i];
        if (mf->tile_x >= x1 || mf->tile_z >= z1 ||
            mf->tile_x + span <= x0 || mf->tile_z + span <= z0)
            continue;
        feat_top_stamp(w, i, x0, z0, x1, z1);
    }
}

static void fx_clear(struct MapFeature *mf) {
    mf->damage_taken = 0;
    mf->fx = FEATURE_FX_NONE;
    mf->spark = 0;
    mf->sparks = 0;
    mf->anim_on = mf->front_on = mf->back_on = 0;
    mf->anim_frame = mf->anim_wait = 0;
    mf->front_frame = mf->front_wait = 0;
    mf->back_frame = mf->back_wait = 0;
    mf->fx_serial = 0;
}

/* Instance `idx` gives its cell to def `next`, or to nothing when next
 * is -1, the way the original clears the cell and places the stage on
 * it (legacy:127935-127955, 128130-128186). The stage keeps the cell,
 * so the list keeps its order. One that would land on an indestructible
 * feature is not placed. */
/* A stage with nothing after it, told as its death or burn and gone. */
static void feat_gone(struct GameWorld *w, int idx, int kind) {
    feat_event(w, kind, idx, w->features[idx].global_idx, -1, 0, 0, 0);
    g_remove_kind = -1;
    Features_RemoveInstance(w, idx);
    g_remove_kind = FEATURE_EVENT_REMOVED;
}

static void feat_replace(struct GameWorld *w, int idx, int next, int kind) {
    const FeatureDef *od = Features_GetByIndex(w->features[idx].global_idx);
    const FeatureDef *nd = Features_GetByIndex(next);
    int old = w->features[idx].global_idx;
    if (!nd) { feat_gone(w, idx, kind); return; }
    struct MapFeature *mf = &w->features[idx];
    int ofx, ofz;
    inst_fp(od, mf, &ofx, &ofz);
    int nfx = nd->footprint_x > 0 ? nd->footprint_x : 1;
    int nfz = nd->footprint_z > 0 ? nd->footprint_z : 1;
    if (mf->facing & 1) { int t = nfx; nfx = nfz; nfz = t; }
    int32_t x0 = (int32_t)mf->tile_x * 16, y0 = (int32_t)mf->tile_z * 16;
    int32_t x1 = x0 + nfx * 16, y1 = y0 + nfz * 16;
    for (int i = 0; i < w->feature_count; i++) {
        if (i == idx) continue;
        int32_t ax0, ay0, ax1, ay1;
        feat_rect(w, i, &ax0, &ay0, &ax1, &ay1);
        if (ax1 <= x0 || ax0 >= x1 || ay1 <= y0 || ay0 >= y1) continue;
        const FeatureDef *bd = Features_GetByIndex(w->features[i].global_idx);
        if (bd && bd->indestructible) { feat_gone(w, idx, kind); return; }
    }
    for (int i = w->feature_count - 1; i >= 0; i--) {
        if (i == idx) continue;
        int32_t ax0, ay0, ax1, ay1;
        feat_rect(w, i, &ax0, &ay0, &ax1, &ay1);
        if (ax1 <= x0 || ax0 >= x1 || ay1 <= y0 || ay0 >= y1) continue;
        Features_RemoveInstance(w, i);
        if (i < idx) idx--;
    }
    mf = &w->features[idx];
    int tx = mf->tile_x, tz = mf->tile_z;
    if ((od && od->sacred_site > 0.0f) || nd->sacred_site > 0.0f) g_sacred_gen++;
    int was_blocking = Features_InstanceBlocks(w, idx);
    /* A wall's or a building's next stage that would let units through
     * blocks until it is swept, under the remastered rules (D-036). */
    uint8_t rubble = (uint8_t)(w->cfg.remastered && !nd->blocking &&
                               (mf->rubble || remaster_leaves_rubble(od)));
    mf->global_idx = next;
    /* A stage drawn from a GAF stands on its cells' centre, a model
     * keeps the place and turn of the one before it (legacy:128190-128206). */
    if (!nd->object[0] || (od && !od->object[0])) {
        mf->world_x = (int32_t)tx * 16 + nfx * 8;
        mf->world_y = (int32_t)tz * 16 + nfz * 8;
    }
    if (!nd->object[0]) mf->heading = mf->pitch = mf->roll = 0;
    mf->decompose_ticks = decompose_ticks_for(nd);
    mf->sink_ticks = 0;
    fx_clear(mf);
    mf->rubble = rubble;
    int fx_w = ofx > nfx ? ofx : nfx, fx_h = ofz > nfz ? ofz : nfz;
    feat_top_refresh(w, tx, tz, tx + fx_w, tz + fx_h);
    if (was_blocking || Features_InstanceBlocks(w, idx))
        TAK_PathCacheFeatureChanged(w, tx, tz, tx + fx_w - 1, tz + fx_h - 1);
    feat_event(w, kind, idx, old, next, 0, 0, 0);
}

/* ── Death, fire and the blast ─────────────────────────────────────── */

static int fx_is_sprite(const FeatureDef *fd) { return fd && !fd->object[0]; }

/* A spark's countdown, the original's half to whole of sparktime, and
 * under the remastered rules FEATURE_SPARK_DELAY percent of that. */
static uint8_t spark_delay(const FeatureDef *fd, int remastered) {
    uint32_t half = fd ? (uint32_t)(fd->spark_time & 0xffff) >> 1 : 0;
    uint32_t d = World_Rand(half) + half;
    if (!remastered) return (uint8_t)d;
    d = d * FEATURE_SPARK_DELAY / 100u;
    return (uint8_t)(d > 255 ? 255 : d);
}

/* Set burning, the original's MapGrid cell update for a fire: only a
 * feature drawn from a GAF, standing idle, with a burn sequence
 * (legacy:127772-127835). Its spark comes half way through sparktime
 * at the earliest. */
static void feat_ignite(struct GameWorld *w, int idx) {
    struct MapFeature *mf = &w->features[idx];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    const FeatFxDef *x = fx_def(mf->global_idx);
    if (!fx_is_sprite(fd) || !x || mf->fx != FEATURE_FX_NONE) return;
    if (x->seq[FSEQ_BURN] < 0) return;
    mf->fx = FEATURE_FX_BURNING;
    mf->fx_serial = ++w->feat_fx_serial;
    mf->damage_taken = 0;
    mf->spark = spark_delay(fd, w->cfg.remastered);
    mf->sparks = (uint8_t)(w->cfg.remastered ? FEATURE_SPARKS - 1 : 0);
    anim_start(x->seq[FSEQ_BURN], &mf->anim_on, &mf->anim_frame, &mf->anim_wait);
    anim_start(x->seq[FSEQ_FRONT], &mf->front_on, &mf->front_frame, &mf->front_wait);
    anim_start(x->seq[FSEQ_BACK], &mf->back_on, &mf->back_frame, &mf->back_wait);
    int front = Features_SequenceFrames(mf->global_idx, FSEQ_FRONT);
    int back = Features_SequenceFrames(mf->global_idx, FSEQ_BACK);
    int burn = front > 0 || back > 0 ? (front > back ? front : back)
                                     : Features_SequenceFrames(mf->global_idx, FSEQ_BURN);
    feat_event(w, FEATURE_EVENT_BURNING, idx, mf->global_idx, -1, 0, 0, burn);
}

/* Destroyed: a GAF feature with a death sequence plays it first, unless
 * it is already dying or burning, and everything else gives its cell to
 * featuredead at once (legacy:127838-127936). */
static void feat_kill(struct GameWorld *w, int idx) {
    struct MapFeature *mf = &w->features[idx];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    const FeatFxDef *x = fx_def(mf->global_idx);
    if (!fd || !x) return;
    if (fx_is_sprite(fd) && x->seq[FSEQ_DIE] >= 0) {
        if (mf->fx != FEATURE_FX_NONE) return;
        mf->fx = FEATURE_FX_DYING;
        mf->fx_serial = ++w->feat_fx_serial;
        anim_start(x->seq[FSEQ_DIE], &mf->anim_on, &mf->anim_frame, &mf->anim_wait);
        mf->front_on = mf->back_on = 0;
        feat_event(w, FEATURE_EVENT_DYING, idx, mf->global_idx, -1, 0, 0,
                   Features_SequenceFrames(mf->global_idx, FSEQ_DIE));
        return;
    }
    feat_replace(w, idx, x->dead_idx, FEATURE_EVENT_DEAD);
}

/* One blast's damage on instance `idx` (legacy:128725-128822). A
 * flamable feature a fire starter reaches catches instead. One dying or
 * burning takes nothing more, a model always counts it. */
static void feat_hit(struct GameWorld *w, int idx, int damage, int fire) {
    struct MapFeature *mf = &w->features[idx];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    if (!fd) return;
    uint32_t hp = (uint32_t)fd->damage & 0xffffu;
    if (fd->indestructible) {
        int breakable = w->cfg.remastered ? remaster_breakable_hp(fd) : 0;
        if (!breakable) {
            feat_event(w, FEATURE_EVENT_HIT, idx, mf->global_idx, -1, 0, 0, 0);
            return;
        }
        hp = (uint32_t)breakable;
    }
    int sprite = fx_is_sprite(fd);
    int busy = !sprite || mf->fx != FEATURE_FX_NONE;
    uint32_t dmg = (uint32_t)damage & 0xffffu;
    if (!fd->flamable || !fire) {
        if (!busy) {
            uint32_t acc = dmg + mf->damage_taken;
            feat_event(w, FEATURE_EVENT_HIT, idx, mf->global_idx, -1, (int)dmg,
                       acc < hp ? (int)(hp - acc) : 0, 0);
            if (acc < hp) mf->damage_taken = (uint16_t)acc;
            else feat_kill(w, idx);
            return;
        }
    } else if (!busy) {
        feat_ignite(w, idx);
        return;
    }
    if (!sprite) {
        mf->damage_taken = (uint16_t)(mf->damage_taken + dmg);
        feat_event(w, FEATURE_EVENT_HIT, idx, mf->global_idx, -1, (int)dmg,
                   hp > mf->damage_taken ? (int)(hp - mf->damage_taken) : 0, 0);
        if (hp <= mf->damage_taken) feat_kill(w, idx);
    }
}

void Features_DebugHit(struct GameWorld *w, int idx, int damage, int fire_starter) {
    if (!w || idx < 0 || idx >= w->feature_count) return;
    feat_hit(w, idx, damage, fire_starter);
}

static int ivec_push(int **buf, int *n, int *cap, int v) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        int *p = (int *)tak_malloc((size_t)nc * sizeof(int));
        if (!p) return -1;
        if (*buf) {
            memcpy(p, *buf, (size_t)*n * sizeof(int));
            tak_free(*buf);
        }
        *buf = p;
        *cap = nc;
    }
    (*buf)[(*n)++] = v;
    return 0;
}

void Features_Blast(struct GameWorld *w, int32_t x, int32_t y, float height,
                    int radius, int damage, int fire_starter) {
    if (!w || !w->features || w->feature_count <= 0 || g_hold) return;
    if (radius < 0) radius = 0;
    int mw = w->map_pixels_w / 16, mh = w->map_pixels_h / 16;
    /* The cells within the radius and one more, cut to the map
     * (legacy:245089-245111). */
    int cx = x / 16, cz = y / 16;
    int ext = (radius >> 4) + 1;
    int x0 = cx - ext < 0 ? 0 : cx - ext, z0 = cz - ext < 0 ? 0 : cz - ext;
    int x1 = cx + ext > mw ? mw : cx + ext, z1 = cz + ext > mh ? mh : cz + ext;
    if (x0 >= x1 || z0 >= z1) return;
    int any = 0;
    for (int z = z0; z < z1 && !any; z++)
        for (int xx = x0; xx < x1 && !any; xx++)
            if (Features_TopAt(w, xx, z)) any = 1;
    if (!any) return;
    /* Which feature holds each cell, the last placed where two do. */
    int ww = x1 - x0, wh = z1 - z0;
    int stack_cells[48 * 48];
    int *cell = ww * wh <= 48 * 48 ? stack_cells
                                   : (int *)tak_malloc((size_t)ww * wh * sizeof(int));
    if (!cell) return;
    for (int i = 0; i < ww * wh; i++) cell[i] = -1;
    for (int i = 0; i < w->feature_count; i++) {
        const struct MapFeature *mf = &w->features[i];
        int fx, fz;
        inst_fp(Features_GetByIndex(mf->global_idx), mf, &fx, &fz);
        int ax = mf->tile_x > x0 ? mf->tile_x : x0;
        int az = mf->tile_z > z0 ? mf->tile_z : z0;
        int bx = mf->tile_x + fx < x1 ? mf->tile_x + fx : x1;
        int bz = mf->tile_z + fz < z1 ? mf->tile_z + fz : z1;
        for (int z = az; z < bz; z++)
            for (int xx = ax; xx < bx; xx++) cell[(z - z0) * ww + (xx - x0)] = i;
    }
    /* Each feature once, kept by its first cell, as far as the original's
     * 64 entries go. A feature reached on a cell within the radius, or on
     * the blast's own cell, takes the weapon's whole damage. A GAF
     * feature is measured from the cell plus half its footprint, a model
     * from where it lies, both on the ground (legacy:245236-245302). */
    int seen[64][2];
    int seen_n = 0;
    int *hits = NULL, hits_n = 0, hits_cap = 0;
    for (int z = z0; z < z1; z++) {
        for (int xx = x0; xx < x1; xx++) {
            int i = cell[(z - z0) * ww + (xx - x0)];
            if (i < 0) continue;
            const struct MapFeature *mf = &w->features[i];
            const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
            if (!fd) continue;
            int32_t px, py;
            if (fd->object[0] && xx == mf->tile_x && z == mf->tile_z) {
                px = mf->world_x;
                py = mf->world_y;
            } else {
                int fx = fd->footprint_x > 0 ? fd->footprint_x : 1;
                int fz = fd->footprint_z > 0 ? fd->footprint_z : 1;
                px = (int32_t)xx * 16 + fx * 8;
                py = (int32_t)z * 16 + fz * 8;
            }
            double dx = (double)(x - px), dy = (double)(y - py);
            double dh = (double)height - (double)Terrain_SampleHeight(w, px, py);
            int d = (int)sqrt(dx * dx + dy * dy + dh * dh);
            if (!(d < radius || (xx == cx && z == cz))) continue;
            int dup = 0;
            for (int k = 0; k < seen_n; k++)
                if (seen[k][0] == mf->tile_x && seen[k][1] == mf->tile_z) { dup = 1; break; }
            if (dup) continue;
            if (seen_n < 64) {
                seen[seen_n][0] = mf->tile_x;
                seen[seen_n][1] = mf->tile_z;
                seen_n++;
            }
            if (ivec_push(&hits, &hits_n, &hits_cap, i) != 0) break;
        }
    }
    if (cell != stack_cells) tak_free(cell);
    g_hold = hits;
    g_hold_count = hits_n;
    g_event_blast = 1;
    g_event_bx = x;
    g_event_by = y;
    for (int k = 0; k < g_hold_count; k++) {
        int i = g_hold[k];
        if (i >= 0) feat_hit(w, i, damage, fire_starter);
    }
    g_event_blast = 0;
    g_hold = NULL;
    g_hold_count = 0;
    tak_free(hits);
}

/* The instance whose top-left cell is (x, z), the last placed, or -1. */
static int feat_at_origin(const struct GameWorld *w, int x, int z) {
    int found = -1;
    for (int i = 0; i < w->feature_count; i++)
        if (w->features[i].tile_x == x && w->features[i].tile_z == z) found = i;
    return found;
}

/* A cell a spark lands on catches when it holds an idle flamable GAF
 * feature and the roll is under its spreadchance. The roll is made only
 * then (legacy:128039-128054, 128070-128086). */
static void spark_try(struct GameWorld *w, int x, int z) {
    if (x < 0 || z < 0 || x >= w->map_pixels_w / 16 || z >= w->map_pixels_h / 16) return;
    int j = feat_at_origin(w, x, z);
    if (j < 0) return;
    const struct MapFeature *mf = &w->features[j];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    if (!fx_is_sprite(fd) || mf->fx != FEATURE_FX_NONE || !fd->flamable) return;
    if ((int)World_Rand(100) < (fd->spread_chance & 0xff)) feat_ignite(w, j);
}

/* A burn's one spread: every cell within three of it, then five steps
 * along the wind, each step twice the wind's parts in 1/65536 of a cell
 * and a new cell tried only when the step reaches one
 * (legacy:128021-128088). */
static void feat_spread(struct GameWorld *w, int sx, int sz) {
    for (int z = sz - 3; z <= sz + 3; z++)
        for (int x = sx - 3; x <= sx + 3; x++)
            if (x != sx || z != sz) spark_try(w, x, z);
    int32_t fx = (int32_t)((uint32_t)sx << 16), fz = (int32_t)((uint32_t)sz << 16);
    int px = sx, pz = sz;
    for (int step = 0; step < 5; step++) {
        fx += w->wind_x * 2;
        fz += w->wind_z * 2;
        int nx = (int16_t)(fx >> 16), nz = (int16_t)(fz >> 16);
        if (nx == px && nz == pz) continue;
        px = nx;
        pz = nz;
        spark_try(w, nx, nz);
    }
}

/* Features the frame's sparks have lit, under the remastered rules. */
static int g_spark_catches;

/* An idle flamable GAF feature with a burn and a spreadchance, one a
 * spark can light. */
static int spark_can_catch(const struct GameWorld *w, int j) {
    if (j < 0) return 0;
    const struct MapFeature *mf = &w->features[j];
    const FeatureDef *fd = Features_GetByIndex(mf->global_idx);
    const FeatFxDef *x = fx_def(mf->global_idx);
    return fx_is_sprite(fd) && mf->fx == FEATURE_FX_NONE && fd->flamable && x &&
           x->seq[FSEQ_BURN] >= 0 && (fd->spread_chance & 0xff) != 0;
}

/* Five downwind steps of the wind's part `v` in whole cells, each
 * 2*v/2^FEATURE_SPARK_WIND_SHIFT of a cell, rounded alike either way and
 * held to FEATURE_SPARK_CARRY. */
static int spark_carry(int32_t v) {
    int64_t t = (int64_t)v * 10;
    int64_t half = (int64_t)1 << (FEATURE_SPARK_WIND_SHIFT - 1);
    int c = t >= 0 ? (int)((t + half) >> FEATURE_SPARK_WIND_SHIFT)
                   : -(int)((-t + half) >> FEATURE_SPARK_WIND_SHIFT);
    if (c > FEATURE_SPARK_CARRY) return FEATURE_SPARK_CARRY;
    return c < -FEATURE_SPARK_CARRY ? -FEATURE_SPARK_CARRY : c;
}

/* The frame's grid of features by top-left cell: the last placed on a
 * cell, as a spark reads it, or -1. */
static void spark_cells_build(const struct GameWorld *w) {
    int mw = w->map_pixels_w / 16, mh = w->map_pixels_h / 16;
    if (mw < 0) mw = 0;
    if (mh < 0) mh = 0;
    int n = mw * mh;
    if (n > g_spark_cells_cap) {
        tak_free(g_spark_cells);
        g_spark_cells = (int *)tak_malloc((size_t)n * sizeof(int));
        g_spark_cells_cap = g_spark_cells ? n : 0;
    }
    if (!g_spark_cells) mw = mh = 0;
    g_spark_cells_w = mw;
    g_spark_cells_h = mh;
    for (int c = 0; c < mw * mh; c++) g_spark_cells[c] = -1;
    for (int i = 0; i < w->feature_count; i++) {
        int x = w->features[i].tile_x, z = w->features[i].tile_z;
        if (x < mw && z < mh) g_spark_cells[z * mw + x] = i;
    }
    g_spark_cells_ok = 1;
}

static int spark_cell(int x, int z) {
    if (x < 0 || z < 0 || x >= g_spark_cells_w || z >= g_spark_cells_h) return -1;
    return g_spark_cells[z * g_spark_cells_w + x];
}

/* A spark under the remastered rules (D-036). It reaches every cell
 * within three, or out to the nearest ring holding a feature that can
 * catch, and the wind stretches that reach downwind by its carry. A
 * feature upwind catches at FEATURE_SPARK_UPWIND percent of the chance.
 * Cells go row by row from the corner `turn` names (bit 0 east first,
 * bit 1 south first), and the spark stops once the frame has lit its cap.
 * Gives 1 when the cap was full before the spark and a feature in its
 * reach could catch, and the spark then waits for the next frame. */
static int feat_spark_remastered(struct GameWorld *w, int sx, int sz, int turn) {
    enum { WIN = FEATURE_SPARK_REACH + FEATURE_SPARK_CARRY };
    if (!g_spark_cells_ok) spark_cells_build(w);
    int lx = spark_carry(w->wind_x), lz = spark_carry(w->wind_z);
    int bx0 = lx < 0 ? lx : 0, bx1 = lx > 0 ? lx : 0;
    int bz0 = lz < 0 ? lz : 0, bz1 = lz > 0 ? lz : 0;
    /* The nearest ring around the spark's cell, or with nothing there,
     * around the cells the wind carries it over. */
    int ring = -1;
    for (int d = 0; d <= FEATURE_SPARK_REACH && ring < 0; d++)
        for (int dz = -d; dz <= d && ring < 0; dz++) {
            int step = dz == -d || dz == d || d == 0 ? 1 : 2 * d;
            for (int dx = -d; dx <= d; dx += step)
                if (spark_can_catch(w, spark_cell(sx + dx, sz + dz))) {
                    ring = d;
                    break;
                }
        }
    if (ring < 0 && (lx || lz)) {
        for (int dz = -WIN; dz <= WIN; dz++)
            for (int dx = -WIN; dx <= WIN; dx++) {
                int ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
                if ((ax > az ? ax : az) <= FEATURE_SPARK_REACH) continue;
                int ex = dx < bx0 ? bx0 - dx : dx > bx1 ? dx - bx1 : 0;
                int ez = dz < bz0 ? bz0 - dz : dz > bz1 ? dz - bz1 : 0;
                int d = ex > ez ? ex : ez;
                if (d > FEATURE_SPARK_REACH || (ring >= 0 && d >= ring)) continue;
                if (spark_can_catch(w, spark_cell(sx + dx, sz + dz))) ring = d;
            }
    }
    if (ring < 0) return 0;
    if (g_spark_catches >= FEATURE_SPARK_CATCH_CAP) return 1;
    if (ring < 3) ring = 3;
    int x0 = bx0 - ring, x1 = bx1 + ring, z0 = bz0 - ring, z1 = bz1 + ring;
    for (int a = 0; a <= z1 - z0; a++)
        for (int b = 0; b <= x1 - x0; b++) {
            int dz = (turn & 2) ? z1 - a : z0 + a;
            int dx = (turn & 1) ? x1 - b : x0 + b;
            int j = spark_cell(sx + dx, sz + dz);
            if (!spark_can_catch(w, j)) continue;
            if (g_spark_catches >= FEATURE_SPARK_CATCH_CAP) return 0;
            const FeatureDef *fd = Features_GetByIndex(w->features[j].global_idx);
            int pct = (fd->spread_chance & 0xff) * FEATURE_SPARK_CHANCE / 100;
            if (dx * lx + dz * lz < 0) pct = pct * FEATURE_SPARK_UPWIND / 100;
            if ((int)World_Rand(100) < pct) {
                feat_ignite(w, j);
                g_spark_catches++;
            }
        }
    return 0;
}

/* ── The wind ─────────────────────────────────────────────────────── */

/* The original's sine, a quarter of a turn in 129 steps of 1/8192
 * (legacy:254683-254701). */
static const int16_t k_sin_quarter[129] = {
       0,  101,  201,  302,  402,  502,  603,  703,  803,  903, 1003, 1102,
    1202, 1301, 1401, 1499, 1598, 1697, 1795, 1893, 1990, 2088, 2185, 2282,
    2378, 2474, 2570, 2665, 2760, 2854, 2948, 3042, 3135, 3228, 3320, 3411,
    3503, 3593, 3683, 3773, 3862, 3950, 4038, 4125, 4212, 4297, 4383, 4467,
    4551, 4634, 4717, 4799, 4880, 4960, 5040, 5119, 5197, 5274, 5351, 5427,
    5501, 5575, 5649, 5721, 5793, 5863, 5933, 6002, 6070, 6137, 6203, 6268,
    6333, 6396, 6458, 6519, 6580, 6639, 6698, 6755, 6811, 6867, 6921, 6974,
    7027, 7078, 7128, 7177, 7225, 7272, 7317, 7362, 7405, 7448, 7489, 7529,
    7568, 7606, 7643, 7679, 7713, 7746, 7779, 7809, 7839, 7868, 7895, 7921,
    7946, 7970, 7993, 8014, 8035, 8054, 8071, 8088, 8103, 8117, 8130, 8142,
    8153, 8162, 8170, 8177, 8182, 8186, 8190, 8191, 8192 };

static int32_t sin_tab(int i) {
    i &= 511;
    if (i < 128) return k_sin_quarter[i];
    if (i < 256) return k_sin_quarter[256 - i];
    if (i < 384) return -k_sin_quarter[i - 256];
    return -k_sin_quarter[512 - i];
}

static int32_t fx_sin_mul(uint16_t a, int32_t v) {
    int64_t p = (int64_t)sin_tab(((uint32_t)a + 0x20u) >> 7) * v + 0x1000;
    return (int32_t)(p >> 13);
}

static int32_t fx_cos_mul(uint16_t a, int32_t v) {
    int64_t p = (int64_t)sin_tab(((uint32_t)a + 0x4020u) >> 7) * v + 0x1000;
    return (int32_t)(p >> 13);
}

/* The C library's generator the original times the wind with, its own
 * stream here, started where the original starts it (legacy:242447). */
static uint32_t wind_rand(struct GameWorld *w, uint32_t n) {
    w->wind_rand = w->wind_rand * 214013u + 2531011u;
    uint32_t r = (w->wind_rand >> 16) & 0x7fffu;
    return (uint32_t)(((uint64_t)r * n) / 0x8000u);
}

static void wind_set_next(struct GameWorld *w) {
    int32_t az = w->wind_z < 0 ? -w->wind_z : w->wind_z;
    if (w->wind_x < az) w->wind_next_frame = w->feat_frame + (wind_rand(w, 6) + 3) * 30;
    else                w->wind_next_frame = w->feat_frame + (wind_rand(w, 10) + 5) * 30;
    w->wind_changed = 1;
}

/* A new wind once its time comes: a speed in the map's range and, when
 * there is any, a turn of up to an eighth either way (legacy:241674-241718). */
static void wind_step(struct GameWorld *w) {
    if (w->feat_frame <= w->wind_next_frame) { w->wind_changed = 0; return; }
    w->wind_speed = w->wind_min + (int32_t)World_Rand((uint32_t)(w->wind_max - w->wind_min));
    if (w->wind_speed != 0)
        w->wind_heading = (uint16_t)(w->wind_heading + World_Rand(0x4000) - 0x2000);
    w->wind_x = fx_sin_mul(w->wind_heading, w->wind_speed);
    w->wind_z = fx_cos_mul(w->wind_heading, w->wind_speed);
    wind_set_next(w);
}

void Features_WindBegin(struct GameWorld *w, int min_speed, int max_speed) {
    if (!w) return;
    w->wind_min = min_speed;
    w->wind_max = max_speed;
    w->wind_speed = 0;
    w->wind_heading = 0;
    w->wind_x = w->wind_z = 0;
    w->wind_next_frame = 0;
    w->wind_changed = 0;
    w->wind_rand = 0x4d2u;
}

void Features_DebugSetWind(struct GameWorld *w, int speed, uint16_t heading) {
    if (!w) return;
    w->wind_speed = speed;
    w->wind_heading = heading;
    w->wind_x = fx_sin_mul(heading, speed);
    w->wind_z = fx_cos_mul(heading, speed);
    wind_set_next(w);
}

/* ── The frame ────────────────────────────────────────────────────── */

/* Newest first, the order of the original's list: by serial down, two
 * with one serial in list order. A merge, so a field of fires sorts in
 * n log n. */
static void work_sort(const struct GameWorld *w, int *a, int n) {
    if (n < 2) return;
    int *t = (int *)tak_malloc((size_t)n * sizeof(int));
    if (!t) {
        for (int i = 1; i < n; i++) {
            int v = a[i], b = i - 1;
            while (b >= 0 && w->features[a[b]].fx_serial < w->features[v].fx_serial) {
                a[b + 1] = a[b];
                b--;
            }
            a[b + 1] = v;
        }
        return;
    }
    int *src = a, *dst = t;
    for (int run = 1; run < n; run *= 2) {
        for (int lo = 0; lo < n; lo += 2 * run) {
            int mid = lo + run < n ? lo + run : n;
            int hi = lo + 2 * run < n ? lo + 2 * run : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                dst[k++] = w->features[src[j]].fx_serial > w->features[src[i]].fx_serial
                               ? src[j++] : src[i++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        int *x = src;
        src = dst;
        dst = x;
    }
    if (src != a) memcpy(a, src, (size_t)n * sizeof(int));
    tak_free(t);
}

void Features_TickFrame(struct GameWorld *w) {
    if (!w || g_hold) return;
    w->feat_frame++;
    g_spark_catches = 0;
    g_spark_cells_ok = 0;
    int *work = NULL, work_n = 0, cap = 0;
    for (int i = 0; i < w->feature_count; i++)
        if (w->features[i].fx != FEATURE_FX_NONE &&
            ivec_push(&work, &work_n, &cap, i) != 0) break;
    work_sort(w, work, work_n);
    g_hold = work;
    g_hold_count = work_n;
    for (int k = 0; k < g_hold_count; k++) {
        int i = g_hold[k];
        if (i < 0) continue;
        struct MapFeature *mf = &w->features[i];
        const FeatFxDef *x = fx_def(mf->global_idx);
        if (!x) continue;
        if (mf->fx == FEATURE_FX_DYING) {
            anim_step(x->seq[FSEQ_DIE], 0, &mf->anim_on, &mf->anim_frame, &mf->anim_wait);
            if (!mf->anim_on) feat_replace(w, i, x->dead_idx, FEATURE_EVENT_DEAD);
            continue;
        }
        if (mf->fx != FEATURE_FX_BURNING) continue;
        /* The burn loops while there are flames, and the flames end it
         * (legacy:128556-128598). */
        int flames = x->seq[FSEQ_FRONT] >= 0 || x->seq[FSEQ_BACK] >= 0;
        anim_step(x->seq[FSEQ_BURN], flames, &mf->anim_on, &mf->anim_frame, &mf->anim_wait);
        anim_step(x->seq[FSEQ_FRONT], 0, &mf->front_on, &mf->front_frame, &mf->front_wait);
        anim_step(x->seq[FSEQ_BACK], 0, &mf->back_on, &mf->back_frame, &mf->back_wait);
        /* Under the remastered rules a fire burns on until its spark, a
         * flame that ends starting over, so the spark spreads it (D-036). */
        if (w->cfg.remastered && mf->spark != 0) {
            if (x->seq[FSEQ_FRONT] >= 0 && !mf->front_on)
                anim_start(x->seq[FSEQ_FRONT], &mf->front_on, &mf->front_frame, &mf->front_wait);
            if (x->seq[FSEQ_BACK] >= 0 && !mf->back_on)
                anim_start(x->seq[FSEQ_BACK], &mf->back_on, &mf->back_frame, &mf->back_wait);
            if (!flames && !mf->anim_on)
                anim_start(x->seq[FSEQ_BURN], &mf->anim_on, &mf->anim_frame, &mf->anim_wait);
        }
        int done = flames ? (!mf->front_on && !mf->back_on) : !mf->anim_on;
        if (done) {
            feat_replace(w, i, x->burnt_idx, FEATURE_EVENT_BURNT);
            continue;
        }
        /* Under the remastered rules the fire hurts what stands in it,
         * the TreeBurn the files name and never define (D-036). */
        if (w->cfg.remastered && (w->feat_frame + mf->fx_serial) % FEATURE_BURN_EVERY == 0) {
            int32_t bx, by;
            if (Features_InstanceCentre(w, i, &bx, &by) == 0)
                Units_ScorchAt(bx, by, FEATURE_BURN_REACH, FEATURE_BURN_DAMAGE);
        }
        if (!w->cfg.remastered) {
            if (mf->spark != 0 && --mf->spark == 0) {
                int sx = mf->tile_x, sz = mf->tile_z;
                feat_spread(w, sx, sz);
            }
            continue;
        }
        if (mf->spark == 0) continue;
        if (mf->spark > 1) {
            mf->spark--;
            continue;
        }
        /* Each spark starts its cells from another corner, by the fire's
         * serial and the sparks it has left. One the frame's cap holds
         * back waits for the next frame. */
        if (feat_spark_remastered(w, mf->tile_x, mf->tile_z,
                                  (int)((mf->fx_serial + mf->sparks) & 3u)))
            continue;
        mf = &w->features[i];
        mf->spark = 0;
        if (mf->sparks != 0) {
            mf->sparks--;
            mf->spark = spark_delay(Features_GetByIndex(mf->global_idx), 1);
        }
    }
    g_hold = NULL;
    g_hold_count = 0;
    tak_free(work);
    wind_step(w);
}

int Features_DebugSetDefs(const FeatureDef *defs, int count) {
    Features_NoteListReplaced();
    Features_FreeAll();
    if (!defs || count <= 0) return 0;
    g_feats = (FeatureDef *)tak_calloc((size_t)count, sizeof(FeatureDef));
    if (!g_feats) return -1;
    memcpy(g_feats, defs, (size_t)count * sizeof(FeatureDef));
    g_feat_count = g_feat_cap = count;
    return count;
}

void Features_FreeAll(void) {
    Features_NoteListReplaced();
    fx_reset();
    tak_free(g_spark_cells);
    g_spark_cells = NULL;
    g_spark_cells_cap = g_spark_cells_w = g_spark_cells_h = 0;
    g_spark_cells_ok = 0;
    if (g_feats) {
        tak_free(g_feats);
        g_feats = NULL;
    }
    g_feat_count = 0;
    g_feat_cap = 0;
}
