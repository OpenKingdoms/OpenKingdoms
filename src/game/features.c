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
#include "tak_tdf.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "tak_hpi.h"
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

/* ── Placed instances ───────────────────────────────────────────────
 *
 * Legacy resolves a sweep-cursor click to the map cell record, then to
 * the feature the cell holds, and only issues the reclaim order when
 * that record is a live feature (legacy:187186-187198). Our TNT loader
 * keeps the same set as world->features, so the lookup is a footprint
 * test over that array. */

static int feat_footprint_hit(const FeatureDef *fd,
                              const struct MapFeature *mf,
                              int32_t wx, int32_t wy) {
    int fp_x = (fd->footprint_x > 0) ? fd->footprint_x : 1;
    int fp_z = (fd->footprint_z > 0) ? fd->footprint_z : 1;
    int32_t x0 = (int32_t)mf->tile_x * 16;
    int32_t y0 = (int32_t)mf->tile_z * 16;
    return wx >= x0 && wx < x0 + fp_x * 16 &&
           wy >= y0 && wy < y0 + fp_z * 16;
}

int Features_FindReclaimableAt(const struct GameWorld *world,
                               int32_t world_x, int32_t world_y) {
    if (!world || !world->features) return -1;
    /* Nearest-centre wins when footprints overlap, so a click always
     * takes the feature it visually landed on. */
    int best = -1;
    int64_t best_d2 = 0;
    for (int i = 0; i < world->feature_count; i++) {
        const FeatureDef *fd =
            Features_GetByIndex(world->features[i].global_idx);
        if (!fd || !fd->reclaimable) continue;
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

int Features_InstanceCentre(const struct GameWorld *world, int idx,
                            int32_t *out_x, int32_t *out_y) {
    if (!world || !world->features) return -1;
    if (idx < 0 || idx >= world->feature_count) return -1;
    const FeatureDef *fd = Features_GetByIndex(world->features[idx].global_idx);
    int fp_x = (fd && fd->footprint_x > 0) ? fd->footprint_x : 1;
    int fp_z = (fd && fd->footprint_z > 0) ? fd->footprint_z : 1;
    if (out_x) *out_x = (int32_t)world->features[idx].tile_x * 16 + fp_x * 8;
    if (out_y) *out_y = (int32_t)world->features[idx].tile_z * 16 + fp_z * 8;
    return 0;
}

int Features_RemoveInstance(struct GameWorld *world, int idx) {
    if (!world || !world->features) return -1;
    if (idx < 0 || idx >= world->feature_count) return -1;
    /* Compact rather than tombstone: every consumer (walkability,
     * rendering, sacred-site scan) walks the array live, so the cleared
     * cell stops blocking on the next query with no other edits. */
    for (int i = idx; i + 1 < world->feature_count; i++)
        world->features[i] = world->features[i + 1];
    world->feature_count--;
    return 0;
}

void Features_FreeAll(void) {
    if (g_feats) {
        tak_free(g_feats);
        g_feats = NULL;
    }
    g_feat_count = 0;
    g_feat_cap = 0;
}
