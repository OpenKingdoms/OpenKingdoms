/*
 * tex_atlas.c — Per-GAF texture atlas manager.
 *
 * Loads `textures/*.gaf`, decodes frame 0 of each entry to RGBA, packs
 * each GAF's entries into one CPU-side atlas via a simple shelf packer,
 * uploads the atlas as a single GPU_Texture, and indexes every texture
 * name into a global lookup.
 *
 * Per-GAF (rather than one mega-atlas) keeps the packer trivial — each
 * GAF has ≤100 entries averaging ~64×64, and a 1024×1024 atlas fits
 * any of them with room to spare. See docs/PHASE_C_3DO.md §3.6.
 *
 * Lookup is a sorted array + binary search. ~3,700 entries total →
 * log2 ≈ 12 comparisons per query. Fine without a hashtable.
 */

#include "tak_tex_atlas.h"
#include "tak_hpi.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_ui.h"           /* UI_RGBAFormat for palette → RGBA */
#include "tak_world.h"
#include "tak_memory.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls — pal_lookup.c picks the right palette per GAF. */
extern const char *Palette_LookupForGAF(const char *gaf_path);
extern const char *Palette_LookupForGAFAlt(const char *gaf_path);

#define TEAM_COLOR_RAMP_LO  0x10   /* per faction *_textures.pcx */
#define TEAM_COLOR_RAMP_HI  0x1F   /* inclusive */
#define TEAM_COLOR_RAMP_LEN 16
/* TAK_PLAYER_COLOR_COUNT comes from tak_battle_config.h via tak_world.h. */

/* Team colour is authored art, not a palette remap: a team-coloured
 * GAF entry ships one frame per player colour (legacy indexes it with
 * the owner's colour byte — :197904-197911, :255123). Variants are
 * rebuilt by re-decoding at that frame; there is no synthesized ramp
 * and no reserved palette range. */

/* Per-palette RGBA cache. The 88 texture GAFs typically resolve to ≤8
 * distinct palettes (one per faction's _textures.pcx, plus shared
 * fallbacks). Loading each once keeps LS_LOAD_TEXTURES fast.
 *
 * `rgba_shaded`: the same 256-entry RGBA table BUT with each entry
 * pre-routed through the side's `.shd` shade table at the engine's
 * default mid-light level (0x0F = 15, per decomp the legacy reference
 * ~197808). This is the table actually fed into atlas decode — the
 * raw `rgba` is kept only for reference and team-color slot lookup. */
typedef struct PalCacheEntry {
    char     name[64];        /* "ara_textures.pcx" etc. */
    uint32_t rgba[256];
    uint32_t rgba_shaded[256];
    int      valid;
} PalCacheEntry;

#define TAK_LIGHT_LEVEL_DEFAULT 0x0F   /* decomp default at line ~197808 */

static int           g_light_level     = TAK_LIGHT_LEVEL_DEFAULT;
static PalCacheEntry g_pal_cache[16];
static int           g_pal_cache_count = 0;

int  TexAtlas_GetLightLevel(void)        { return g_light_level; }
void TexAtlas_SetLightLevel(int level)   {
    if (level < 0)  level = 0;
    if (level > 31) level = 31;
    g_light_level = level;
}

/* Load and apply the side's `.shd` shade table to produce rgba_shaded.
 * The .shd file lives next to the .pcx with extension swapped:
 *   ara_textures.pcx → ara_textures.shd
 * It's 8192 bytes = 32 light levels × 256 entries. Each entry is the
 * palette-index that the original index resolves to at that light level.
 * We use the mid-light row (level 15) as TAK's default, which gives
 * the engine's signature dark moody appearance for unit textures. */
static void apply_shade_table(const char *pcx_name, PalCacheEntry *e) {
    /* Default fallback: shaded == base (no shade applied). */
    memcpy(e->rgba_shaded, e->rgba, sizeof(e->rgba));

    /* Build the .shd path from the .pcx name. */
    char shd_name[64];
    snprintf(shd_name, sizeof(shd_name), "%s", pcx_name);
    char *dot = strrchr(shd_name, '.');
    if (!dot) return;
    /* Replace ".pcx" with ".shd". */
    if (strlen(dot) >= 4) {
        dot[1] = 's'; dot[2] = 'h'; dot[3] = 'd';
    } else return;

    char path[128];
    snprintf(path, sizeof(path), "data/palettes/%s", shd_name);

    void *data = NULL;
    uint32_t sz = 0;
    if (VFS_ReadFile(path, &data, &sz) != 0 || !data || sz < 8192) {
        if (data) tak_free(data);
        /* Not all palettes have a shade table (e.g. gameart, guipal).
         * That's fine — we just skip shading for those. */
        return;
    }
    const uint8_t *shd = (const uint8_t *)data;
    const uint8_t *row = shd + (g_light_level * 256);
    for (int i = 0; i < 256; i++) {
        uint8_t shaded_idx = row[i];
        e->rgba_shaded[i] = e->rgba[shaded_idx];
    }
    tak_free(data);
    fprintf(stderr, "TexAtlas: applied %s @ light=%d\n",
            shd_name, g_light_level);
}

/* Look up a palette by name, loading it from data/palettes/ if not
 * already cached. Also loads the matching .shd shade table and
 * pre-applies it to produce rgba_shaded.
 *
 * Returns the SHADED 256-entry RGBA table (atlas decode uses this).
 * To get the raw unshaded values, use get_or_load_palette_raw. */
static const PalCacheEntry *get_or_load_palette_entry(const char *pcx_name) {
    if (!pcx_name) return NULL;
    for (int i = 0; i < g_pal_cache_count; i++) {
        if (strcmp(g_pal_cache[i].name, pcx_name) == 0) {
            return g_pal_cache[i].valid ? &g_pal_cache[i] : NULL;
        }
    }
    if (g_pal_cache_count >= (int)(sizeof(g_pal_cache)/sizeof(g_pal_cache[0]))) {
        return NULL;
    }
    PalCacheEntry *e = &g_pal_cache[g_pal_cache_count++];
    snprintf(e->name, sizeof(e->name), "%s", pcx_name);

    char path[128];
    snprintf(path, sizeof(path), "data/palettes/%s", pcx_name);

    Palette pal;
    if (Palette_LoadPCX(&pal, path) != 0) {
        fprintf(stderr, "TexAtlas: palette load failed: %s\n", path);
        e->valid = 0;
        return NULL;
    }
    Palette_BuildRGBATable(&pal, UI_RGBAFormat(), e->rgba, 0);
    apply_shade_table(pcx_name, e);
    e->valid = 1;
    fprintf(stderr, "TexAtlas: loaded palette %s\n", pcx_name);
    return e;
}

static const uint32_t *get_or_load_palette(const char *pcx_name) {
    const PalCacheEntry *e = get_or_load_palette_entry(pcx_name);
    return e ? e->rgba_shaded : NULL;
}

#define ATLAS_W 1024
#define ATLAS_H 1024

/* ── Module state ────────────────────────────────────────────────── */

/* AtlasGroup = one GAF's worth of pixels.
 *
 * Holds:
 *   - base_rgba: 1024×1024 RGBA buffer using the side's *_textures.pcx
 *     palette. Cached so per-player variants can be re-rendered without
 *     re-decoding the GAF.
 *   - tc_slot_rgba[16]: the 16 RGBA values from the side palette at
 *     indices 0x10..0x1F. These are what get matched-and-replaced to
 *     produce per-player variants.
 *   - has_team_color: 1 if any pixel in base_rgba matches tc_slot_rgba.
 *     If 0, all 12 player variants are the same — share one GPU
 *     texture across all colors.
 *   - atlas_tex_per_color[12]: per-player-color GPU texture variants,
 *     lazy-built on first lookup. */
typedef struct AtlasGroup {
    uint32_t    *base_rgba;             /* 1024×1024, free after all variants built */
    uint32_t     tc_slot_rgba[TEAM_COLOR_RAMP_LEN];
    int          has_team_color;
    /* Team colour is PRE-BAKED art: a team-coloured GAF entry ships one
     * frame per player colour. Variants are rebuilt by re-decoding the
     * source GAF at that frame, so keep what that needs. */
    char         src_path[160];
    uint32_t     pal_rgba[256];
    struct Placement *places;           /* placement per decoded entry */
    int          n_places;
    GPU_Texture *atlas_tex_per_color[TAK_PLAYER_COLOR_COUNT];
    int          atlas_idx;             /* index in g_atlases (for shutdown) */
} AtlasGroup;

typedef struct AtlasEntry {
    char         name_lower[32];   /* sorted key */
    int          group_idx;        /* which AtlasGroup this entry belongs to */
    float        u0, v0, u1, v1;   /* UV rect within the atlas */
} AtlasEntry;

static AtlasGroup   *g_groups        = NULL;
static int           g_group_count   = 0;
static int           g_group_cap     = 0;
static TAK_Platform *g_plat          = NULL;     /* cached for lazy variant build */

TAK_Platform *TexAtlas__GetPlatform(void) { return g_plat; }

static GPU_Texture **g_atlases       = NULL;
static int           g_atlas_count   = 0;
static int           g_atlas_cap     = 0;

static AtlasEntry   *g_entries       = NULL;
static int           g_entry_count   = 0;
static int           g_entry_cap     = 0;
static int           g_entries_sorted = 0;  /* 0 until LoadAll finishes */

/* ── Small helpers ──────────────────────────────────────────────── */

static void lowercase_into(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; n + 1 < cap && src[n]; n++) {
        char c = src[n];
        if (c >= 'A' && c <= 'Z') c += 32;
        dst[n] = c;
    }
    dst[n] = '\0';
}

static int ensure_atlas_cap(void);     /* forward decl */

static int ensure_group_cap(void) {
    if (g_group_count < g_group_cap) return 0;
    int new_cap = g_group_cap == 0 ? 16 : g_group_cap * 2;
    AtlasGroup *bigger = (AtlasGroup *)tak_malloc(new_cap * sizeof(AtlasGroup));
    if (!bigger) return -1;
    if (g_groups) {
        memcpy(bigger, g_groups, g_group_count * sizeof(AtlasGroup));
        tak_free(g_groups);
    }
    g_groups = bigger;
    g_group_cap = new_cap;
    return 0;
}

/* Build a player-color variant atlas. Allocates a 1024×1024 RGBA buffer,
 * copies base_rgba, then walks every pixel: any pixel matching any of
 * tc_slot_rgba[0..15] gets replaced with the corresponding player ramp
 * value (preserving the brightness ladder). Uploads as a new GPU
 * texture and stores in atlas_tex_per_color[color_idx]. */

static int ensure_atlas_cap(void) {
    if (g_atlas_count < g_atlas_cap) return 0;
    int new_cap = g_atlas_cap == 0 ? 16 : g_atlas_cap * 2;
    GPU_Texture **bigger = (GPU_Texture **)tak_malloc(new_cap * sizeof(GPU_Texture *));
    if (!bigger) return -1;
    if (g_atlases) {
        memcpy(bigger, g_atlases, g_atlas_count * sizeof(GPU_Texture *));
        tak_free(g_atlases);
    }
    g_atlases = bigger;
    g_atlas_cap = new_cap;
    return 0;
}

static int ensure_entry_cap(int need) {
    if (g_entry_count + need <= g_entry_cap) return 0;
    int new_cap = g_entry_cap == 0 ? 256 : g_entry_cap;
    while (new_cap < g_entry_count + need) new_cap *= 2;
    AtlasEntry *bigger = (AtlasEntry *)tak_malloc(new_cap * sizeof(AtlasEntry));
    if (!bigger) return -1;
    if (g_entries) {
        memcpy(bigger, g_entries, g_entry_count * sizeof(AtlasEntry));
        tak_free(g_entries);
    }
    g_entries = bigger;
    g_entry_cap = new_cap;
    return 0;
}

/* ── Shelf packer ──────────────────────────────────────────────────
 *
 * Sort entries by height descending; lay them into shelves of that
 * height. Adequate for our case (each GAF is ≤100 small textures).
 * Returns 0 on success, -1 if anything didn't fit in ATLAS_W×ATLAS_H.
 *
 * `placements[i]` is filled with (x, y) for the input entry i. */

typedef struct ShelfRect {
    int w, h;        /* input */
    int orig_idx;    /* preserved through sort */
} ShelfRect;

typedef struct Placement {
    int x, y;
} Placement;

static int cmp_shelf_rect_by_height(const void *a, const void *b) {
    const ShelfRect *ra = (const ShelfRect *)a;
    const ShelfRect *rb = (const ShelfRect *)b;
    return rb->h - ra->h;       /* descending */
}

static int shelf_pack(ShelfRect *rects, Placement *placements, int n,
                       int atlas_w, int atlas_h) {
    /* Sort a copy so we can write placements back by orig_idx. */
    qsort(rects, n, sizeof(ShelfRect), cmp_shelf_rect_by_height);

    int cur_x = 0, cur_y = 0, shelf_h = 0;
    for (int i = 0; i < n; i++) {
        if (rects[i].w > atlas_w || rects[i].h > atlas_h) return -1;
        if (cur_x + rects[i].w > atlas_w) {
            /* New shelf below */
            cur_y += shelf_h;
            cur_x = 0;
            shelf_h = 0;
        }
        if (cur_y + rects[i].h > atlas_h) return -1;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;

        placements[rects[i].orig_idx].x = cur_x;
        placements[rects[i].orig_idx].y = cur_y;
        cur_x += rects[i].w;
    }
    return 0;
}

/* ── GAF loader: pull each entry's frame-0 RGBA + dims ──────────── */

typedef struct DecodedEntry {
    char        name[32];
    int         w, h;
    int         nframes;     /* GAF entry frame count */
    uint32_t   *pixels;      /* tak_malloc'd; we free after upload */
} DecodedEntry;

/* Read one GAF, decode every entry's frame 0, return count + pixels.
 * Out-array is tak_malloc'd; caller must free pixels[].pixels and
 * the array itself. */
static int decode_gaf_entries(const char *vfs_path,
                               const uint32_t *rgba_table,
                               DecodedEntry **out, int *out_count,
                               int frame_index) {
    *out = NULL;
    *out_count = 0;

    GAFFile *gaf = NULL;
    if (GAF_Open(&gaf, vfs_path) != 0) return -1;

    /* GAFFile.data holds the raw bytes; entry table is at offset 12. */
    uint32_t num_entries = gaf->num_entries;
    if (num_entries == 0) {
        GAF_Close(gaf);
        return 0;
    }

    DecodedEntry *list = (DecodedEntry *)tak_malloc(num_entries * sizeof(DecodedEntry));
    if (!list) { GAF_Close(gaf); return -1; }
    memset(list, 0, num_entries * sizeof(DecodedEntry));

    int produced = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t entry_off = *(const uint32_t *)(gaf->data + 12 + i * 4);
        if (entry_off + 40 > gaf->data_size) continue;

        const uint8_t *eh = gaf->data + entry_off;
        /* EntryHeader: num_frames(2) unknown1(2) unknown2(4) name[32] */
        const char *name = (const char *)(eh + 8);
        if (name[0] == '\0') continue;

        /* Team colour in TAK is PRE-BAKED: team-coloured entries ship
         * exactly 10 frames, one per player colour (legacy indexes them
         * with the owner's colour byte, :197904-197911 / :255123).
         * Everything else is a time animation — always frame 0. */
        int nframes = (int)(*(const uint16_t *)eh);
        int fi = (nframes >= TAK_PLAYER_COLOR_COUNT) ? frame_index : 0;
        if (fi < 0 || fi >= nframes) fi = 0;
        FrameHeader *frame = NULL;
        if (GAF_GetFrameInfo(gaf, entry_off, fi, &frame) != 0 || !frame) continue;
        if (frame->width <= 0 || frame->height <= 0) continue;
        if (frame->width > 512 || frame->height > 512) {
            /* Defensive — texture entries are typically ≤256 a side.
             * Anything larger than 512 is probably a corrupt frame
             * header or a non-texture entry we shouldn't atlas. */
            continue;
        }

        uint32_t *pix = GAF_DecodeFrameRGBA(gaf, frame, rgba_table);
        if (!pix) continue;

        DecodedEntry *e = &list[produced++];
        memcpy(e->name, name, 32);
        e->name[31] = '\0';
        e->w       = frame->width;
        e->h       = frame->height;
        e->nframes = nframes;
        e->pixels  = pix;
    }

    GAF_Close(gaf);
    *out = list;
    *out_count = produced;
    return 0;
}

/* ── Build one atlas from a GAF and register its entries ────────── */

static int build_atlas_from_gaf(TAK_Platform *plat, const char *vfs_path,
                                 const uint32_t *fallback_rgba) {
    /* Per-GAF palette selection (R6 finding): TAK uses faction-specific
     * texture palettes (ara_textures.pcx for arahuman*.gaf, etc.). The
     * pre-existing pal_lookup.c maps GAF basename → palette name. We
     * try the *_textures variant first (artist-labeled "for unit
     * polygons"), fall back to *_features (terrain decoration palette,
     * still better than the kingdom palette), then fall back to the
     * caller's terrain palette as last resort. */
    const char *alt = Palette_LookupForGAFAlt(vfs_path);
    const char *primary = Palette_LookupForGAF(vfs_path);
    const uint32_t *rgba_table = NULL;
    if (alt)        rgba_table = get_or_load_palette(alt);
    if (!rgba_table && primary && strcmp(primary, "gameart.pcx") != 0)
                    rgba_table = get_or_load_palette(primary);
    if (!rgba_table) rgba_table = fallback_rgba;

    DecodedEntry *entries = NULL;
    int n = 0;
    if (decode_gaf_entries(vfs_path, rgba_table, &entries, &n, 0) != 0 || n == 0) {
        if (entries) {
            for (int i = 0; i < n; i++) if (entries[i].pixels) tak_free(entries[i].pixels);
            tak_free(entries);
        }
        return 0;  /* non-fatal — empty/invalid GAF, skip */
    }

    /* Pack. */
    ShelfRect *rects = (ShelfRect *)tak_malloc(n * sizeof(ShelfRect));
    Placement *places = (Placement *)tak_malloc(n * sizeof(Placement));
    if (!rects || !places) goto fail;
    for (int i = 0; i < n; i++) {
        rects[i].w = entries[i].w;
        rects[i].h = entries[i].h;
        rects[i].orig_idx = i;
    }
    if (shelf_pack(rects, places, n, ATLAS_W, ATLAS_H) != 0) {
        fprintf(stderr, "TexAtlas: %s didn't fit in %dx%d atlas (%d entries) — skipped\n",
                vfs_path, ATLAS_W, ATLAS_H, n);
        goto fail;
    }

    /* Composite all entries into one big RGBA buffer. */
    size_t atlas_pixels = (size_t)ATLAS_W * ATLAS_H;
    uint32_t *composite = (uint32_t *)tak_malloc(atlas_pixels * sizeof(uint32_t));
    if (!composite) goto fail;
    /* Transparent black background — UV rects only sample inside
     * placed entries, but better safe than reading garbage. */
    memset(composite, 0, atlas_pixels * sizeof(uint32_t));

    for (int i = 0; i < n; i++) {
        const DecodedEntry *e = &entries[i];
        int dst_x = places[i].x;
        int dst_y = places[i].y;
        for (int row = 0; row < e->h; row++) {
            uint32_t *dst = composite + (dst_y + row) * ATLAS_W + dst_x;
            const uint32_t *src = e->pixels + row * e->w;
            memcpy(dst, src, (size_t)e->w * sizeof(uint32_t));
        }
    }

    /* Step B: do NOT upload the base atlas yet. Cache the composite
     * RGBA on CPU; per-player variants will be lazy-built from it on
     * first lookup with each color_idx. */
    if (ensure_group_cap() != 0) { tak_free(composite); goto fail; }
    AtlasGroup *grp = &g_groups[g_group_count];
    memset(grp, 0, sizeof(*grp));
    grp->base_rgba = composite;
    grp->atlas_idx = -1;
    /* Keep the source + palette + placements so per-colour variants can
     * be rebuilt from the art's own per-colour frames. */
    snprintf(grp->src_path, sizeof(grp->src_path), "%s", vfs_path);
    memcpy(grp->pal_rgba, rgba_table, sizeof(grp->pal_rgba));
    grp->places = (Placement *)tak_malloc((size_t)n * sizeof(Placement));
    if (grp->places) {
        memcpy(grp->places, places, (size_t)n * sizeof(Placement));
        grp->n_places = n;
    }
    /* Team-coloured art ships one frame per player colour; anything
     * with fewer frames is a time animation and is colour-invariant. */
    grp->has_team_color = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].nframes >= TAK_PLAYER_COLOR_COUNT) {
            grp->has_team_color = 1;
            break;
        }
    }
    if (getenv("TAK_TC_TRACE")) {
        fprintf(stderr, "atlas %s: team_color=%d entries=%d\n",
                vfs_path, grp->has_team_color, n);
    }
    int group_idx = g_group_count++;

    /* Register every entry against this group. Atlas texture variants
     * are built lazily by build_color_variant on first GetByName. */
    if (ensure_entry_cap(n) != 0) goto fail;
    const float inv_w = 1.0f / (float)ATLAS_W;
    const float inv_h = 1.0f / (float)ATLAS_H;
    for (int i = 0; i < n; i++) {
        AtlasEntry *r = &g_entries[g_entry_count];
        lowercase_into(r->name_lower, sizeof(r->name_lower), entries[i].name);
        r->group_idx = group_idx;
        r->u0 = (float)places[i].x         * inv_w;
        r->v0 = (float)places[i].y         * inv_h;
        r->u1 = (float)(places[i].x + entries[i].w) * inv_w;
        r->v1 = (float)(places[i].y + entries[i].h) * inv_h;
        g_entry_count++;
    }

    tak_free(rects);
    tak_free(places);
    for (int i = 0; i < n; i++) tak_free(entries[i].pixels);
    tak_free(entries);
    return 0;

fail:
    if (rects)  tak_free(rects);
    if (places) tak_free(places);
    if (entries) {
        for (int i = 0; i < n; i++) if (entries[i].pixels) tak_free(entries[i].pixels);
        tak_free(entries);
    }
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

static int cmp_entry(const void *a, const void *b) {
    const AtlasEntry *ea = (const AtlasEntry *)a;
    const AtlasEntry *eb = (const AtlasEntry *)b;
    return strcmp(ea->name_lower, eb->name_lower);
}

int TexAtlas_LoadAll(TAK_Platform *plat) {
    if (!plat) return -1;

    /* Already loaded? Be idempotent. */
    if (g_entries_sorted) return 0;

    g_plat = plat;

    const GameWorld *world = World_Get();
    const uint32_t *rgba_table = world ? world->terrain_rgba : NULL;
    if (!rgba_table) {
        fprintf(stderr, "TexAtlas_LoadAll: no GameWorld palette available\n");
        return -1;
    }

    char **gafs = NULL;
    int n_gaf = 0;
    if (VFS_ListFiles("textures/*.gaf", &gafs, &n_gaf) != 0 || n_gaf <= 0) {
        fprintf(stderr, "TexAtlas_LoadAll: no textures/*.gaf found\n");
        return -1;
    }

    int built = 0, skipped = 0;
    for (int i = 0; i < n_gaf; i++) {
        if (build_atlas_from_gaf(plat, gafs[i], rgba_table) == 0) built++;
        else skipped++;
    }

    /* Sort the entry array for binary-search lookup. */
    qsort(g_entries, g_entry_count, sizeof(AtlasEntry), cmp_entry);
    g_entries_sorted = 1;

    fprintf(stderr,
        "TexAtlas_LoadAll: %d/%d GAFs built, %d entries registered, %d groups\n",
        built, n_gaf, g_entry_count, g_group_count);
    return built > 0 ? 0 : -1;
}

static GPU_Texture *build_color_variant(TAK_Platform *plat, AtlasGroup *g,
                                         int color_idx)
{
    if (!g || !g->base_rgba) return NULL;
    if (color_idx < 0 || color_idx >= TAK_PLAYER_COLOR_COUNT) return NULL;

    if (!g->has_team_color) {
        /* Atlas has no team-color pixels; all variants identical.
         * Build one GPU texture lazily and share across colors. */
        if (g->atlas_tex_per_color[0]) {
            g->atlas_tex_per_color[color_idx] = g->atlas_tex_per_color[0];
            return g->atlas_tex_per_color[0];
        }
        GPU_Texture *t = GPU_UploadRGBA(plat, g->base_rgba, ATLAS_W, ATLAS_H);
        if (!t) return NULL;
        GPU_SetTextureFilter(t, 1);
        GPU_SetTextureBlend(t, 1);
        for (int p = 0; p < TAK_PLAYER_COLOR_COUNT; p++) {
            g->atlas_tex_per_color[p] = t;
        }
        if (ensure_atlas_cap() == 0) {
            g_atlases[g_atlas_count++] = t;
        }
        return t;
    }

    /* Team-coloured: re-decode the source GAF selecting FRAME =
     * colour index for entries that carry one frame per colour, and
     * re-composite at the same atlas placements (all frames of an
     * entry share dimensions, so the UV rects stay valid). Legacy does
     * exactly this indexing at :197904-197911 / :255123 — there is no
     * palette remap and no reserved colour range. */
    size_t pix_total = (size_t)ATLAS_W * ATLAS_H;
    uint32_t *variant = (uint32_t *)tak_malloc(pix_total * sizeof(uint32_t));
    if (!variant) return NULL;
    memcpy(variant, g->base_rgba, pix_total * sizeof(uint32_t));

    DecodedEntry *cvs = NULL;
    int ncv = 0;
    if (g->src_path[0] &&
        decode_gaf_entries(g->src_path, g->pal_rgba, &cvs, &ncv,
                           color_idx) == 0) {
        int lim = (ncv < g->n_places) ? ncv : g->n_places;
        for (int i = 0; i < lim; i++) {
            const DecodedEntry *e = &cvs[i];
            if (!e->pixels || e->nframes < TAK_PLAYER_COLOR_COUNT) continue;
            int dx = g->places[i].x, dy = g->places[i].y;
            for (int row = 0; row < e->h; row++) {
                memcpy(variant + (size_t)(dy + row) * ATLAS_W + dx,
                       e->pixels + (size_t)row * e->w,
                       (size_t)e->w * sizeof(uint32_t));
            }
        }
    }
    if (cvs) {
        for (int i = 0; i < ncv; i++) if (cvs[i].pixels) tak_free(cvs[i].pixels);
        tak_free(cvs);
    }

    GPU_Texture *t = GPU_UploadRGBA(plat, variant, ATLAS_W, ATLAS_H);
    tak_free(variant);
    if (!t) return NULL;
    GPU_SetTextureFilter(t, 1);
    GPU_SetTextureBlend(t, 1);
    g->atlas_tex_per_color[color_idx] = t;
    if (ensure_atlas_cap() == 0) {
        g_atlases[g_atlas_count++] = t;
    }
    return t;
}

GPU_Texture *TexAtlas_GetByName(const char *texture_name, int color_idx,
                                 SDL_FRect *out_uv)
{
    if (!texture_name || !g_entries || !g_entries_sorted) return NULL;
    if (color_idx < 0 || color_idx >= TAK_PLAYER_COLOR_COUNT) color_idx = 0;

    char key[32];
    lowercase_into(key, sizeof(key), texture_name);

    int lo = 0, hi = g_entry_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(g_entries[mid].name_lower, key);
        if      (cmp < 0) lo = mid + 1;
        else if (cmp > 0) hi = mid;
        else {
            while (mid > 0 &&
                   strcmp(g_entries[mid - 1].name_lower, key) == 0) {
                mid--;
            }
            const AtlasEntry *e = &g_entries[mid];
            if (out_uv) {
                out_uv->x = e->u0;
                out_uv->y = e->v0;
                out_uv->w = e->u1 - e->u0;
                out_uv->h = e->v1 - e->v0;
            }
            AtlasGroup *grp = &g_groups[e->group_idx];
            if (!grp->atlas_tex_per_color[color_idx]) {
                /* Lazy-build the variant on first lookup with this color. */
                /* We need a TAK_Platform for upload; cache it from a
                 * trampoline since this lookup path doesn't otherwise
                 * have access to it. */
                extern TAK_Platform *TexAtlas__GetPlatform(void);
                TAK_Platform *plat = TexAtlas__GetPlatform();
                if (plat) build_color_variant(plat, grp, color_idx);
            }
            return grp->atlas_tex_per_color[color_idx];
        }
    }
    return NULL;
}

int TexAtlas_Reload(TAK_Platform *plat) {
    /* Drop everything and reload from VFS — used to re-apply a new
     * light level after SetLightLevel. The cached PalCacheEntry
     * cluster is invalidated too; LoadAll's get_or_load_palette
     * then re-loads the .shd at the new light level. */
    TexAtlas_Shutdown(plat);
    g_pal_cache_count = 0;     /* invalidate shaded palettes */
    return TexAtlas_LoadAll(plat);
}

void TexAtlas_Shutdown(TAK_Platform *plat) {
    /* Free all GPU textures (each variant gets one). g_atlases is the
     * authoritative list — atlas_tex_per_color in groups may have
     * shared pointers (when has_team_color=0) so don't double-free. */
    if (g_atlases) {
        for (int i = 0; i < g_atlas_count; i++) {
            if (g_atlases[i]) GPU_FreeTexture(plat, g_atlases[i]);
        }
        tak_free(g_atlases);
        g_atlases = NULL;
    }
    g_atlas_count = 0;
    g_atlas_cap = 0;

    /* Free cached base RGBA buffers. */
    if (g_groups) {
        for (int i = 0; i < g_group_count; i++) {
            if (g_groups[i].base_rgba) tak_free(g_groups[i].base_rgba);
        }
        tak_free(g_groups);
        g_groups = NULL;
    }
    g_group_count = 0;
    g_group_cap = 0;

    if (g_entries) {
        tak_free(g_entries);
        g_entries = NULL;
    }
    g_entry_count = 0;
    g_entry_cap = 0;
    g_entries_sorted = 0;
    g_plat = NULL;
}

int TexAtlas_GetEntryCount(void) { return g_entry_count; }
int TexAtlas_GetAtlasCount(void) { return g_atlas_count; }
