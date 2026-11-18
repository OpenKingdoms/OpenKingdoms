/*
 * tex_atlas.c — Per-GAF texture atlas manager.
 *
 * Loads every GAF under `textures/`, decodes frame 0 of each entry to
 * palette indices (every picture, side by side, for one played by
 * time), packs each GAF's entries into one CPU-side atlas via a simple
 * shelf packer, uploads it as a single GPU_Texture holding a copy of the
 * art per shade band it needs (one, and an index copy, when the GPU
 * shades it), and indexes every texture name into a global
 * lookup.
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
#include "tak_sides.h"
#include "tak_memory.h"
#include "tak_gl3d.h"
#include "tak_settings.h"
#include "tak_unit.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls — pal_lookup.c picks the right palette per GAF. */
extern const char *Palette_LookupForGAF(const char *gaf_path);
extern const char *Palette_LookupForGAFAlt(const char *gaf_path);

/* TAK_PLAYER_COLOR_COUNT comes from tak_battle_config.h via tak_world.h. */

/* A team colour entry ships a frame per player colour (legacy:197904-197911,
 * :255123). Any other of several frames plays by time, as the lodestones do. */

/* Per-palette cache, each loaded once with its .shd ramp, `level * 256 +
 * index`. Every index is opaque, see Palette_KeyTextures. */
typedef struct PalCacheEntry {
    char     name[64];        /* "ara_textures.pcx" etc. */
    uint32_t rgba[256];
    uint32_t ramp[TEX_SHADE_LEVELS * 256];
    uint8_t  shd[TEX_SHADE_LEVELS * 256];
    uint8_t  magenta[256];    /* the 16-bit colour key, (128, 0, 128) */
    int      has_shd;
    int      valid;
    GL3D_Texture *ramp_tex;   /* the ramp for GPU shading, on first use */
} PalCacheEntry;

static PalCacheEntry g_pal_cache[16];
static int           g_pal_cache_count = 0;
/* The map's own palette, for a GAF no side palette claims. */
static PalCacheEntry g_fallback_pal;

/* Read the .shd that goes with a palette file name, any extension. */
static int read_shd(const char *pal_name, uint8_t *out) {
    char path[128];
    snprintf(path, sizeof(path), "data/palettes/%s", pal_name);
    char *dot = strrchr(path, '.');
    if (!dot || strlen(dot) != 4) return -1;
    memcpy(dot, ".shd", 4);
    void *data = NULL;
    uint32_t sz = 0;
    int ok = VFS_ReadFile(path, &data, &sz) == 0 && data && sz >= TEX_SHADE_LEVELS * 256;
    if (ok) memcpy(out, data, TEX_SHADE_LEVELS * 256);
    if (data) tak_free(data);
    return ok ? 0 : -1;
}

/* The ramp from the .shd beside the .pcx, else the side's sidedata palette's
 * (Creon ships none), else every level plain. */
static void build_ramp(PalCacheEntry *e, const char *pcx_name) {
    e->has_shd = 0;
    if (pcx_name) {
        e->has_shd = read_shd(pcx_name, e->shd) == 0;
        char prefix[8];
        size_t n = strcspn(pcx_name, "_.");
        if (n >= sizeof(prefix)) n = sizeof(prefix) - 1;
        memcpy(prefix, pcx_name, n);
        prefix[n] = '\0';
        const int side = Sides_FindByPrefix(prefix);
        const TakSideInfo *own = side >= 0 ? Sides_Get(side) : NULL;
        if (!e->has_shd && own && own->palette[0])
            e->has_shd = read_shd(own->palette, e->shd) == 0;
    }
    for (int l = 0; l < TEX_SHADE_LEVELS; l++)
        for (int i = 0; i < 256; i++)
            e->ramp[l * 256 + i] = e->rgba[e->has_shd ? e->shd[l * 256 + i] : i];
}

/* Look up a palette by name, loading it and its .shd from
 * data/palettes/ if not already cached. */
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
    uint32_t table[256];
    for (int i = 0; i < 256; i++)
        table[i] = SDL_MapRGBA(UI_RGBAFormat(), pal.entries[i].r, pal.entries[i].g,
                               pal.entries[i].b, 255);
    Palette_KeyTextures(table, e->rgba, e->magenta);
    build_ramp(e, pcx_name);
    e->valid = 1;
    fprintf(stderr, "TexAtlas: loaded palette %s%s\n", pcx_name,
            e->has_shd ? " with its shade ramp" : "");
    return e;
}

const uint32_t *TexAtlas_ShadeRamp(const char *pcx_name) {
    const PalCacheEntry *e = get_or_load_palette_entry(pcx_name);
    return e ? e->ramp : NULL;
}

/* The .shd row band k holds: 15, 16, and on up to 31 at most. */
static int band_row(int k) {
    const int row = TEX_SHADE_IDENTITY + k;
    return row < TEX_SHADE_LEVELS ? row : TEX_SHADE_LEVELS - 1;
}

void TexAtlas_BuildShadeMap(const uint32_t *rgba, const uint8_t *shd,
                            int bands, float band_dv, TexShadeMap *out) {
    if (bands < 1) bands = 1;
    out->band_dv = band_dv;
    out->bands = (uint8_t)bands;
    /* A row's brightness against another: the summed luma of every
     * palette colour bright enough to count, through it. */
    double sum[TEX_SHADE_LEVELS];
    for (int l = 0; l < TEX_SHADE_LEVELS; l++) {
        sum[l] = 0.0;
        for (int i = 0; shd && i < 256; i++) {
            const uint32_t b = rgba[i], c = rgba[shd[l * 256 + i]];
            if (77u * (b & 0xFFu) + 150u * ((b >> 8) & 0xFFu) + 29u * ((b >> 16) & 0xFFu) < 8u * 256u)
                continue;
            sum[l] += 77.0 * (c & 0xFFu) + 150.0 * ((c >> 8) & 0xFFu) + 29.0 * ((c >> 16) & 0xFFu);
        }
    }
    for (int l = 0; l < TEX_SHADE_LEVELS; l++) {
        int k = (shd && l > TEX_SHADE_IDENTITY) ? l - TEX_SHADE_IDENTITY : 0;
        if (k > bands - 1) k = bands - 1;
        const double base = sum[band_row(k)];
        double g = (shd && base > 0.0) ? 255.0 * sum[l] / base : 255.0;
        if (g > 255.0) g = 255.0;
        out->band[l] = (uint8_t)k;
        out->grey[l] = (uint8_t)(g + 0.5);
    }
}

#define ATLAS_W 1024
#define ATLAS_H 1024
/* The tallest texture an atlas with its shade copies may be: every
 * renderer the game targets takes 4096. */
#define ATLAS_MAX_H 4096

/* ── Module state ────────────────────────────────────────────────── */

/* One GAF's art. `base_idx` keeps it as indices, expanded on upload into
 * `bands` copies at band_row()'s rows. A colour variant re-decodes in place. */
typedef struct AtlasGroup {
    uint16_t    *base_idx;
    int          band_h, bands, atlas_h;
    int          shade_top;             /* brightest level noted, 0 none */
    const PalCacheEntry *pal;
    TexShadeMap  shade;
    int          has_team_color;
    char         src_path[160];
    struct Placement *places;           /* placement per decoded entry */
    uint8_t     *team;                  /* per placement: team coloured */
    int          n_places;
    GPU_Texture *atlas_tex_per_color[TAK_PLAYER_COLOR_COUNT];
    GL3D_Texture *index_per_color[TAK_PLAYER_COLOR_COUNT];
} AtlasGroup;

typedef struct AtlasEntry {
    char         name_lower[32];   /* sorted key */
    int          group_idx;        /* which AtlasGroup this entry belongs to */
    float        u0, v0, u1, v1;   /* UV rect within the atlas */
    int          anim;             /* 1 + its AnimTex, 0 when it holds still */
} AtlasEntry;

static AtlasGroup   *g_groups        = NULL;
static int           g_group_count   = 0;
static int           g_group_cap     = 0;
static TAK_Platform *g_plat          = NULL;     /* cached for lazy variant build */
/* GPU shading: atlases upload one copy, and the GL classic path shades it. */
static int           g_gpu_shade     = 0;
static int           g_shade_planned = 0;
static double        g_uploaded_mb   = 0.0;

TAK_Platform *TexAtlas__GetPlatform(void) { return g_plat; }

static GPU_Texture **g_atlases       = NULL;
static int           g_atlas_count   = 0;
static int           g_atlas_cap     = 0;

static AtlasEntry   *g_entries       = NULL;
static int           g_entry_count   = 0;
static int           g_entry_cap     = 0;
static int           g_entries_sorted = 0;  /* 0 until LoadAll finishes */

/* An entry played by time. Its pictures lie side by side in its slot in
 * every shade copy, so a face picks one by shifting u. */
typedef struct AnimTex {
    int       pics;
    float     pic_du;         /* one picture's width in u */
    float     du;             /* the shift to the picture on show */
    uint16_t *hold;           /* original frames each picture shows */
} AnimTex;
static AnimTex *g_anims;
static int      g_anim_count, g_anim_cap;

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
    int w, h;        /* the entry's frame 0 laid there */
} Placement;

static int cmp_shelf_rect_by_height(const void *a, const void *b) {
    const ShelfRect *ra = (const ShelfRect *)a;
    const ShelfRect *rb = (const ShelfRect *)b;
    return rb->h - ra->h;       /* descending */
}

static int shelf_pack(ShelfRect *rects, Placement *placements, int n,
                       int atlas_w, int atlas_h, int *used_h) {
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
    *used_h = cur_y + shelf_h;
    return 0;
}

/* ── GAF loader: each entry's palette indices + dims ────────────── */

typedef struct DecodedEntry {
    char        name[32];
    int         w, h;
    int         nframes;     /* GAF entry frame count */
    uint32_t    entry_off;   /* its entry header in the GAF */
    uint16_t   *texels;      /* palette index or PALETTE_TEXEL_CLEAR; tak_malloc'd */
} DecodedEntry;

/* Decode every entry of a GAF at `frame_index`, else frame 0. The colour key
 * is decided here, from the index, before any shade row remaps it. */
static int decode_gaf_entries(const char *vfs_path, const PalCacheEntry *pal,
                              DecodedEntry **out, int *out_count,
                              int frame_index, int skip_short) {
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

        /* A colour variant asks for the colour's frame, a time-played
         * entry for each of its pictures. Frame 0 for an entry with fewer. */
        int nframes = (int)(*(const uint16_t *)eh);
        int fi = frame_index;
        if (fi < 0 || fi >= nframes) {
            if (skip_short) continue;
            fi = 0;
        }
        FrameHeader *frame = NULL;
        if (GAF_GetFrameInfo(gaf, entry_off, fi, &frame) != 0 || !frame) continue;
        if (frame->width <= 0 || frame->height <= 0) continue;
        if (frame->width > 512 || frame->height > 512) {
            /* Defensive — texture entries are typically ≤256 a side.
             * Anything larger than 512 is probably a corrupt frame
             * header or a non-texture entry we shouldn't atlas. */
            continue;
        }

        uint8_t *idx = GAF_DecodeFrame(gaf, frame);
        if (!idx) continue;
        const size_t n = (size_t)frame->width * (size_t)frame->height;
        uint16_t *tx = (uint16_t *)tak_malloc(n * sizeof(uint16_t));
        if (!tx) { tak_free(idx); continue; }
        for (size_t k = 0; k < n; k++)
            tx[k] = Palette_KeyTexel(idx[k], pal->magenta);
        tak_free(idx);

        DecodedEntry *e = &list[produced++];
        memcpy(e->name, name, 32);
        e->name[31] = '\0';
        e->w       = frame->width;
        e->h       = frame->height;
        e->nframes = nframes;
        e->entry_off = entry_off;
        e->texels  = tx;
    }

    GAF_Close(gaf);
    *out = list;
    *out_count = produced;
    return 0;
}

static void free_entries(DecodedEntry *entries, int n) {
    if (!entries) return;
    for (int i = 0; i < n; i++) if (entries[i].texels) tak_free(entries[i].texels);
    tak_free(entries);
}

/* Copy one decoded entry into an index buffer ATLAS_W wide. */
static void place_entry(uint16_t *dst, const DecodedEntry *e, const Placement *at) {
    for (int row = 0; row < e->h; row++)
        memcpy(dst + (size_t)(at->y + row) * ATLAS_W + at->x,
               e->texels + (size_t)row * e->w, (size_t)e->w * sizeof(uint16_t));
}

/* Expand an index buffer through each band's shade row and upload. */
static GPU_Texture *upload_bands(TAK_Platform *plat, const AtlasGroup *g,
                                 const uint16_t *idx) {
    const size_t band_px = (size_t)ATLAS_W * (size_t)g->band_h;
    uint32_t *rgba = (uint32_t *)tak_malloc(band_px * (size_t)g->bands * sizeof(uint32_t));
    if (!rgba) return NULL;
    for (int b = 0; b < g->bands; b++) {
        const uint32_t *row = g->pal->ramp + band_row(b) * 256;
        uint32_t *dst = rgba + band_px * (size_t)b;
        for (size_t k = 0; k < band_px; k++)
            dst[k] = idx[k] == PALETTE_TEXEL_CLEAR ? 0u : row[idx[k]];
    }
    GPU_Texture *t = GPU_UploadRGBA(plat, rgba, ATLAS_W, g->atlas_h);
    tak_free(rgba);
    if (!t) return NULL;
    g_uploaded_mb += 4.0 * ATLAS_W * g->atlas_h / 1048576.0;
    /* Point sampling, like the original (legacy:265317 writes one
     * texel per pixel). It also keeps the half-texel inset safe. */
    GPU_SetTextureFilter(t, 0);
    GPU_SetTextureBlend(t, GPU_BLEND_ALPHA);
    if (ensure_atlas_cap() == 0) {
        g_atlases[g_atlas_count++] = t;
    }
    return t;
}

/* ── Pictures played by time ────────────────────────────────────── */

int TexAtlas_IsTeamColour(const char *gaf_path, int pictures) {
    if (!gaf_path || pictures < TAK_PLAYER_COLOR_COUNT) return 0;
    const char *base = strrchr(gaf_path, '/');
    char name[64];
    lowercase_into(name, sizeof(name), base ? base + 1 : gaf_path);
    return strstr(name, "logo") != NULL;
}

static int ensure_anim_cap(void) {
    if (g_anim_count < g_anim_cap) return 0;
    int cap = g_anim_cap ? g_anim_cap * 2 : 32;
    AnimTex *bigger = (AnimTex *)tak_malloc((size_t)cap * sizeof(AnimTex));
    if (!bigger) return -1;
    if (g_anims) {
        memcpy(bigger, g_anims, (size_t)g_anim_count * sizeof(AnimTex));
        tak_free(g_anims);
    }
    g_anims = bigger;
    g_anim_cap = cap;
    return 0;
}

/* Widen each played entry into a strip of its pictures with an AnimTex, 1-based
 * in `anim`. An unreadable or odd-sized picture repeats picture 0. */
static void lay_strips(const char *vfs_path, const PalCacheEntry *pal, DecodedEntry *entries,
                       int n, const uint8_t *team, int *pic_w, int *anim) {
    GAFFile *gaf = NULL;
    int most = 0;
    for (int i = 0; i < n; i++) {
        pic_w[i] = entries[i].w;
        anim[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        DecodedEntry *e = &entries[i];
        if (team[i] || e->nframes < 2 || e->w * e->nframes > ATLAS_W) continue;
        if (!gaf && GAF_Open(&gaf, vfs_path) != 0) return;
        if (ensure_anim_cap() != 0) continue;
        const int pics = e->nframes, w = e->w, h = e->h, sw = w * pics;
        AnimTex *a = &g_anims[g_anim_count];
        a->hold = (uint16_t *)tak_malloc((size_t)pics * sizeof(uint16_t));
        uint16_t *strip = (uint16_t *)tak_malloc((size_t)sw * (size_t)h * sizeof(uint16_t));
        if (!a->hold || !strip) {
            if (a->hold) tak_free(a->hold);
            if (strip) tak_free(strip);
            continue;
        }
        for (int k = 0; k < pics; k++) {
            a->hold[k] = (uint16_t)GAF_FrameHold(gaf, e->entry_off, k);
            for (int y = 0; y < h; y++)
                memcpy(strip + (size_t)y * sw + (size_t)k * w, e->texels + (size_t)y * w,
                       (size_t)w * sizeof(uint16_t));
        }
        a->pics = pics;
        a->pic_du = (float)w / (float)ATLAS_W;
        a->du = 0.0f;
        tak_free(e->texels);
        e->texels = strip;
        e->w = sw;
        anim[i] = ++g_anim_count;
        if (pics > most) most = pics;
    }
    if (gaf) GAF_Close(gaf);
    for (int k = 1; k < most; k++) {
        DecodedEntry *at = NULL;
        int na = 0;
        if (decode_gaf_entries(vfs_path, pal, &at, &na, k, 1) == 0) {
            for (int i = 0; i < n; i++) {
                DecodedEntry *e = &entries[i];
                if (!anim[i] || k >= e->nframes) continue;
                for (int j = 0; j < na; j++) {
                    if (strcmp(at[j].name, e->name) != 0) continue;
                    if (at[j].w == pic_w[i] && at[j].h == e->h && at[j].texels)
                        for (int y = 0; y < e->h; y++)
                            memcpy(e->texels + (size_t)y * e->w + (size_t)k * pic_w[i],
                                   at[j].texels + (size_t)y * pic_w[i],
                                   (size_t)pic_w[i] * sizeof(uint16_t));
                    break;
                }
            }
        }
        free_entries(at, na);
    }
}

void TexAtlas_Animate(uint32_t frame) {
    for (int i = 0; i < g_anim_count; i++) {
        AnimTex *a = &g_anims[i];
        a->du = (float)GAF_LoopPicture(a->hold, a->pics, frame) * a->pic_du;
    }
}

float TexAtlas_AnimU(int anim) {
    return anim > 0 && anim <= g_anim_count ? g_anims[anim - 1].du : 0.0f;
}

/* ── Build one atlas from a GAF and register its entries ────────── */

static int build_atlas_from_gaf(const char *vfs_path) {
    /* Per-GAF palette selection (R6 finding): TAK uses faction-specific
     * texture palettes (ara_textures.pcx for arahuman*.gaf, etc.). The
     * pre-existing pal_lookup.c maps GAF basename → palette name. We
     * try the *_textures variant first (artist-labeled "for unit
     * polygons"), fall back to *_features (terrain decoration palette,
     * still better than the kingdom palette), then fall back to the
     * map's palette as last resort. */
    const char *alt = Palette_LookupForGAFAlt(vfs_path);
    const char *primary = Palette_LookupForGAF(vfs_path);
    const PalCacheEntry *pal = NULL;
    if (alt)  pal = get_or_load_palette_entry(alt);
    if (!pal && primary && strcmp(primary, "gameart.pcx") != 0)
              pal = get_or_load_palette_entry(primary);
    if (!pal) pal = &g_fallback_pal;

    DecodedEntry *entries = NULL;
    int n = 0;
    if (decode_gaf_entries(vfs_path, pal, &entries, &n, 0, 0) != 0 || n == 0) {
        free_entries(entries, n);
        return 0;  /* non-fatal — empty/invalid GAF, skip */
    }

    /* Pack. */
    uint16_t *composite = NULL;
    int used_h = 0;
    const int anim_first = g_anim_count;   /* this GAF's records, dropped on failure */
    ShelfRect *rects = (ShelfRect *)tak_malloc(n * sizeof(ShelfRect));
    Placement *places = (Placement *)tak_malloc(n * sizeof(Placement));
    uint8_t *team = (uint8_t *)tak_malloc((size_t)n);
    int *pic_w = (int *)tak_malloc((size_t)n * sizeof(int));
    int *anim = (int *)tak_malloc((size_t)n * sizeof(int));
    if (!rects || !places || !team || !pic_w || !anim) goto fail;
    for (int i = 0; i < n; i++)
        team[i] = (uint8_t)TexAtlas_IsTeamColour(vfs_path, entries[i].nframes);
    lay_strips(vfs_path, pal, entries, n, team, pic_w, anim);
    for (int i = 0; i < n; i++) {
        rects[i].w = entries[i].w;
        rects[i].h = entries[i].h;
        rects[i].orig_idx = i;
    }
    if (shelf_pack(rects, places, n, ATLAS_W, ATLAS_H, &used_h) != 0 || used_h <= 0) {
        fprintf(stderr, "TexAtlas: %s didn't fit in %dx%d atlas (%d entries) — skipped\n",
                vfs_path, ATLAS_W, ATLAS_H, n);
        goto fail;
    }

    /* Composite all entries into one index buffer, clear between. */
    composite = (uint16_t *)tak_malloc((size_t)ATLAS_W * (size_t)used_h * sizeof(uint16_t));
    if (!composite) goto fail;
    for (size_t k = 0; k < (size_t)ATLAS_W * (size_t)used_h; k++) composite[k] = PALETTE_TEXEL_CLEAR;
    for (int i = 0; i < n; i++) {
        places[i].w = entries[i].w;
        places[i].h = entries[i].h;
        place_entry(composite, &entries[i], &places[i]);
    }

    /* GPU textures are built on first lookup per colour. */
    if (ensure_group_cap() != 0) goto fail;
    AtlasGroup *grp = &g_groups[g_group_count];
    memset(grp, 0, sizeof(*grp));
    grp->base_idx = composite;
    composite = NULL;
    grp->pal = pal;
    grp->band_h = used_h;
    /* The look takes up to TEX_SHADE_BANDS rows; GPU shading uploads one. */
    int look = pal->has_shd ? ATLAS_MAX_H / used_h : 1;
    if (look > TEX_SHADE_BANDS) look = TEX_SHADE_BANDS;
    grp->bands = g_gpu_shade ? 1 : look;
    grp->atlas_h = grp->bands * used_h;
    TexAtlas_BuildShadeMap(pal->rgba, pal->has_shd ? pal->shd : NULL, look,
                           (float)used_h / (float)grp->atlas_h, &grp->shade);
    grp->shade.bands = (uint8_t)grp->bands;
    snprintf(grp->src_path, sizeof(grp->src_path), "%s", vfs_path);
    grp->places = (Placement *)tak_malloc((size_t)n * sizeof(Placement));
    if (grp->places) {
        memcpy(grp->places, places, (size_t)n * sizeof(Placement));
        grp->n_places = n;
    }
    if (!grp->places) {
        tak_free(grp->base_idx);
        goto fail;
    }
    grp->team = team;
    team = NULL;
    grp->has_team_color = 0;
    for (int i = 0; i < n; i++) grp->has_team_color |= grp->team[i];
    if (getenv("TAK_TC_TRACE")) {
        fprintf(stderr, "atlas %s: team_color=%d entries=%d rows=%d bands=%d\n",
                vfs_path, grp->has_team_color, n, used_h, grp->bands);
    }
    int group_idx = g_group_count++;

    /* Register every entry against band 0 of this group. */
    if (ensure_entry_cap(n) != 0) goto fail;
    const float inv_w = 1.0f / (float)ATLAS_W;
    const float inv_h = 1.0f / (float)grp->atlas_h;
    for (int i = 0; i < n; i++) {
        AtlasEntry *r = &g_entries[g_entry_count];
        lowercase_into(r->name_lower, sizeof(r->name_lower), entries[i].name);
        r->group_idx = group_idx;
        r->u0 = (float)places[i].x         * inv_w;
        r->v0 = (float)places[i].y         * inv_h;
        r->u1 = (float)(places[i].x + pic_w[i]) * inv_w;
        r->v1 = (float)(places[i].y + entries[i].h) * inv_h;
        r->anim = anim[i];
        g_entry_count++;
    }

    tak_free(rects);
    tak_free(places);
    tak_free(pic_w);
    tak_free(anim);
    free_entries(entries, n);
    return 0;

fail:
    for (int i = anim_first; i < g_anim_count; i++) tak_free(g_anims[i].hold);
    g_anim_count = anim_first;
    if (composite) tak_free(composite);
    if (rects)  tak_free(rects);
    if (places) tak_free(places);
    if (team)   tak_free(team);
    if (pic_w)  tak_free(pic_w);
    if (anim)   tak_free(anim);
    free_entries(entries, n);
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

static const AtlasEntry *find_entry(const char *texture_name);

/* Every atlas once, its copies and, shaded on the GPU, its index copy. */
static double colour_mb(void) {
    double mb = 0.0;
    for (int i = 0; i < g_group_count; i++)
        mb += (4.0 * g_groups[i].atlas_h + (g_gpu_shade ? 2.0 * g_groups[i].band_h : 0.0)) *
              ATLAS_W / 1048576.0;
    return mb;
}

/* GPU shading needs the GL classic path, on the desktop and in the browser.
 * GpuShading=0 in options.cfg, or TAK_GPU_SHADING=0, keeps a lit copy per row. */
static int choose_gpu_shade(TAK_Platform *plat) {
    const char *e = getenv("TAK_GPU_SHADING");
    const int want = e ? atoi(e) : Settings_GetInt(TAK_SETTING_GPU_SHADING, 1);
    if (!want || !plat || !plat->renderer || !Units_ModelDepthReady(plat)) return 0;
    return GL3D_ClassicShadeInit(plat->renderer) == 0;
}

int TexAtlas_ShadePlanWanted(void) { return g_entries_sorted && !g_shade_planned; }

void TexAtlas_NoteShadeTop(const char *texture_name, int level) {
    const AtlasEntry *e = find_entry(texture_name);
    if (e && g_groups[e->group_idx].shade_top < level) g_groups[e->group_idx].shade_top = level;
}

void TexAtlas_PlanShades(void) {
    if (!TexAtlas_ShadePlanWanted()) return;
    g_shade_planned = 1;
    for (int i = 0; i < g_group_count; i++)
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++)
            if (g_groups[i].atlas_tex_per_color[c]) return;   /* too late: keep every copy */
    for (int i = 0; i < g_group_count; i++) {
        AtlasGroup *g = &g_groups[i];
        int bands = g->shade_top - TEX_SHADE_IDENTITY + 1;
        if (bands < 1) bands = 1;
        if (bands >= g->bands) continue;
        const float k = (float)g->bands / (float)bands;
        for (int j = 0; j < g_entry_count; j++)
            if (g_entries[j].group_idx == i) {
                g_entries[j].v0 *= k;
                g_entries[j].v1 *= k;
            }
        g->bands = bands;
        g->atlas_h = bands * g->band_h;
        g->shade.bands = (uint8_t)bands;
        g->shade.band_dv = (float)g->band_h / (float)g->atlas_h;
    }
    int by[TEX_SHADE_BANDS + 1] = { 0 };
    for (int i = 0; i < g_group_count; i++)
        if (g_groups[i].bands <= TEX_SHADE_BANDS) by[g_groups[i].bands]++;
    fprintf(stderr, "TexAtlas: copies kept to the levels used, %.1f MB a colour; atlases by copies:",
            colour_mb());
    for (int b = 1; b <= TEX_SHADE_BANDS; b++)
        if (by[b]) fprintf(stderr, " %d at %d", by[b], b);
    fputc('\n', stderr);
}

/* The index copy for GPU shading: luminance the palette index, alpha 255
 * where the texel is not clear. */
static GL3D_Texture *upload_index(const AtlasGroup *g, const uint16_t *idx) {
    const size_t px = (size_t)ATLAS_W * (size_t)g->band_h;
    uint8_t *la = (uint8_t *)tak_malloc(px * 2);
    if (!la) return NULL;
    for (size_t k = 0; k < px; k++) {
        la[2 * k] = (uint8_t)(idx[k] & 0xFFu);
        la[2 * k + 1] = idx[k] == PALETTE_TEXEL_CLEAR ? 0 : 255;
    }
    GL3D_Texture *t = GL3D_UploadClassicIndex(la, ATLAS_W, g->band_h);
    tak_free(la);
    if (t) g_uploaded_mb += 2.0 * (double)px / 1048576.0;
    return t;
}

/* The last atlas TexAtlas_GpuShade looked up, cleared with the atlases. */
static const GPU_Texture  *g_shade_last;
static const GL3D_Texture *g_shade_last_index, *g_shade_last_ramp;

int TexAtlas_GpuShade(const GPU_Texture *atlas, const GL3D_Texture **index,
                      const GL3D_Texture **ramp) {
    const GPU_Texture *last = g_shade_last;
    const GL3D_Texture *last_index = g_shade_last_index, *last_ramp = g_shade_last_ramp;
    if (!g_gpu_shade || !atlas) return 0;
    if (atlas != last) {
        last = atlas;
        last_index = last_ramp = NULL;
        for (int i = 0; i < g_group_count && !last_index; i++)
            for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++)
                if (g_groups[i].atlas_tex_per_color[c] == atlas) {
                    PalCacheEntry *pe = (PalCacheEntry *)g_groups[i].pal;
                    if (!pe->ramp_tex) pe->ramp_tex = GL3D_UploadClassicRamp(pe->ramp);
                    last_index = g_groups[i].index_per_color[c];
                    last_ramp = pe->ramp_tex;
                    break;
                }
        g_shade_last = last;
        g_shade_last_index = last_index;
        g_shade_last_ramp = last_ramp;
    }
    *index = last_index;
    *ramp = last_ramp;
    return last_index && last_ramp;
}

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
    g_gpu_shade = choose_gpu_shade(plat);
    g_shade_planned = g_gpu_shade;
    g_uploaded_mb = 0.0;

    const GameWorld *world = World_Get();
    const uint32_t *rgba_table = world ? world->terrain_rgba : NULL;
    if (!rgba_table) {
        fprintf(stderr, "TexAtlas_LoadAll: no GameWorld palette available\n");
        return -1;
    }
    memset(&g_fallback_pal, 0, sizeof(g_fallback_pal));
    /* terrain_rgba arrives with index 0 cleared to 0, so it draws black here. */
    Palette_KeyTextures(rgba_table, g_fallback_pal.rgba, g_fallback_pal.magenta);
    build_ramp(&g_fallback_pal, NULL);
    g_fallback_pal.valid = 1;

    char **gafs = NULL;
    int n_gaf = 0;
    if (VFS_ListFiles("textures/*.gaf", &gafs, &n_gaf) != 0 || n_gaf <= 0) {
        fprintf(stderr, "TexAtlas_LoadAll: no textures/*.gaf found\n");
        return -1;
    }

    int built = 0, skipped = 0;
    for (int i = 0; i < n_gaf; i++) {
        if (build_atlas_from_gaf(gafs[i]) == 0) built++;
        else skipped++;
    }

    /* Sort the entry array for binary-search lookup. */
    qsort(g_entries, g_entry_count, sizeof(AtlasEntry), cmp_entry);
    g_entries_sorted = 1;

    fprintf(stderr,
        "TexAtlas_LoadAll: %d/%d GAFs built, %d entries registered, %d groups, "
        "%.1f MB a colour, %d played by time, %s shading\n",
        built, n_gaf, g_entry_count, g_group_count, colour_mb(), g_anim_count,
        g_gpu_shade ? "GPU" : "band");
    return built > 0 ? 0 : -1;
}

static uint16_t *color_variant_idx(const AtlasGroup *g, int color_idx) {
    const size_t pix_total = (size_t)ATLAS_W * (size_t)g->band_h;
    uint16_t *variant = (uint16_t *)tak_malloc(pix_total * sizeof(uint16_t));
    if (!variant) return NULL;
    memcpy(variant, g->base_idx, pix_total * sizeof(uint16_t));

    DecodedEntry *cvs = NULL;
    int ncv = 0;
    if (g->src_path[0] &&
        decode_gaf_entries(g->src_path, g->pal, &cvs, &ncv, color_idx, 0) == 0) {
        int lim = (ncv < g->n_places) ? ncv : g->n_places;
        for (int i = 0; i < lim; i++) {
            if (!cvs[i].texels || !g->team[i]) continue;
            /* A colour frame of another size would spill out of its slot. */
            if (cvs[i].w != g->places[i].w || cvs[i].h != g->places[i].h) continue;
            place_entry(variant, &cvs[i], &g->places[i]);
        }
    }
    free_entries(cvs, ncv);
    return variant;
}

static GPU_Texture *build_color_variant(TAK_Platform *plat, AtlasGroup *g,
                                         int color_idx)
{
    if (!g || !g->base_idx) return NULL;
    if (color_idx < 0 || color_idx >= TAK_PLAYER_COLOR_COUNT) return NULL;

    if (!g->has_team_color) {
        /* Atlas has no team-color art; all variants identical.
         * Build one GPU texture lazily and share across colors. */
        if (g->atlas_tex_per_color[0]) {
            g->atlas_tex_per_color[color_idx] = g->atlas_tex_per_color[0];
            return g->atlas_tex_per_color[0];
        }
        GPU_Texture *t = upload_bands(plat, g, g->base_idx);
        if (!t) return NULL;
        GL3D_Texture *ix = g_gpu_shade ? upload_index(g, g->base_idx) : NULL;
        for (int p = 0; p < TAK_PLAYER_COLOR_COUNT; p++) {
            g->atlas_tex_per_color[p] = t;
            g->index_per_color[p] = ix;
        }
        fprintf(stderr, "TexAtlas: %.1f MB of atlases on the GPU\n", g_uploaded_mb);
        return t;
    }

    /* Team-coloured: re-decode the source GAF selecting FRAME =
     * colour index for entries that carry one frame per colour, and
     * re-composite at the same atlas placements (all frames of an
     * entry share dimensions, so the UV rects stay valid). Legacy does
     * exactly this indexing at :197904-197911 / :255123 — there is no
     * palette remap and no reserved colour range. */
    uint16_t *variant = color_variant_idx(g, color_idx);
    if (!variant) return NULL;
    GPU_Texture *t = upload_bands(plat, g, variant);
    if (t && g_gpu_shade) g->index_per_color[color_idx] = upload_index(g, variant);
    tak_free(variant);
    if (!t) return NULL;
    g->atlas_tex_per_color[color_idx] = t;
    fprintf(stderr, "TexAtlas: %.1f MB of atlases on the GPU\n", g_uploaded_mb);
    return t;
}

int TexAtlas_ShadeMap(const GPU_Texture *atlas, TexShadeMap *out) {
    for (int i = 0; atlas && i < g_group_count; i++)
        for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++)
            if (g_groups[i].atlas_tex_per_color[c] == atlas) {
                *out = g_groups[i].shade;
                return 0;
            }
    TexAtlas_BuildShadeMap(NULL, NULL, 1, 0.0f, out);
    return -1;
}

/* The first entry of that name, any case. */
static const AtlasEntry *find_entry(const char *texture_name) {
    if (!texture_name || !g_entries || !g_entries_sorted) return NULL;
    char key[32];
    lowercase_into(key, sizeof(key), texture_name);
    int lo = 0, hi = g_entry_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(g_entries[mid].name_lower, key);
        if      (cmp < 0) lo = mid + 1;
        else if (cmp > 0) hi = mid;
        else {
            while (mid > 0 && strcmp(g_entries[mid - 1].name_lower, key) == 0) mid--;
            return &g_entries[mid];
        }
    }
    return NULL;
}

int TexAtlas_AnimOf(const char *texture_name) {
    const AtlasEntry *e = find_entry(texture_name);
    return e ? e->anim : 0;
}

GPU_Texture *TexAtlas_GetByName(const char *texture_name, int color_idx,
                                 SDL_FRect *out_uv)
{
    if (color_idx < 0 || color_idx >= TAK_PLAYER_COLOR_COUNT) color_idx = 0;
    const AtlasEntry *e = find_entry(texture_name);
    if (!e) return NULL;
    if (out_uv) {
        out_uv->x = e->u0;
        out_uv->y = e->v0;
        out_uv->w = e->u1 - e->u0;
        out_uv->h = e->v1 - e->v0;
    }
    AtlasGroup *grp = &g_groups[e->group_idx];
    if (!grp->atlas_tex_per_color[color_idx]) {
        /* Build the variant on first lookup. This path has no platform, so
         * the upload takes it from a trampoline. */
        TAK_Platform *plat = TexAtlas__GetPlatform();
        if (plat) build_color_variant(plat, grp, color_idx);
    }
    return grp->atlas_tex_per_color[color_idx];
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

    /* Free cached index buffers and placements. */
    if (g_groups) {
        for (int i = 0; i < g_group_count; i++) {
            for (int c = 0; c < TAK_PLAYER_COLOR_COUNT; c++) {
                GL3D_Texture *ix = g_groups[i].index_per_color[c];
                if (!ix) continue;
                for (int d = c; d < TAK_PLAYER_COLOR_COUNT; d++)
                    if (g_groups[i].index_per_color[d] == ix) g_groups[i].index_per_color[d] = NULL;
                GL3D_FreeTexture(ix);
            }
            if (g_groups[i].base_idx) tak_free(g_groups[i].base_idx);
            if (g_groups[i].places) tak_free(g_groups[i].places);
            if (g_groups[i].team) tak_free(g_groups[i].team);
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
    for (int i = 0; i < g_pal_cache_count; i++) {
        GL3D_FreeTexture(g_pal_cache[i].ramp_tex);
        g_pal_cache[i].ramp_tex = NULL;
    }
    GL3D_FreeTexture(g_fallback_pal.ramp_tex);
    g_fallback_pal.ramp_tex = NULL;
    g_gpu_shade = g_shade_planned = 0;
    g_shade_last = NULL;
    g_shade_last_index = g_shade_last_ramp = NULL;

    for (int i = 0; i < g_anim_count; i++) tak_free(g_anims[i].hold);
    if (g_anims) tak_free(g_anims);
    g_anims = NULL;
    g_anim_count = g_anim_cap = 0;
}

int TexAtlas_GetEntryCount(void) { return g_entry_count; }
int TexAtlas_GetAtlasCount(void) { return g_atlas_count; }
