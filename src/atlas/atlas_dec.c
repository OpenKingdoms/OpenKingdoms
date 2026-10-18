/*
 * atlas_dec.c -- the engine's own readers for web/atlas.html's worker, with
 * a stand-in VFS, so the atlas decodes a file as the game does.
 */

#include "tak_gaf.h"
#include "tak_hpi.h"
#include "tak_jpg.h"
#include "tak_memory.h"
#include "tak_obj3d.h"
#include "tak_palette.h"
#include "tak_png.h"
#include "tak_util.h"

#include <emscripten/emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define KEEP EMSCRIPTEN_KEEPALIVE
#define MAX_ARCHIVES 1024
#define MAX_SIDE 4096
#define MAX_SUBFRAMES 64
#define DEPTH_MAX 16

static HPIArchive *g_arc[MAX_ARCHIVES];
static char g_stem[MAX_ARCHIVES][64];   /* "data" for data.hpi */
static uint8_t g_kmp[MAX_ARCHIVES];
static int g_order[MAX_ARCHIVES];   /* the game's mount order, first to last */
static int g_order_n;
static int g_prefer = -1;           /* the asset's own archive, asked first */
static int g_last_w, g_last_h, g_last_x, g_last_y;
static uint32_t g_last_size;

/* Bytes the readers see at a fixed path: a loose model, a picture's palette. */
static const char *g_over_path;
static const uint8_t *g_over_data;
static uint32_t g_over_size;

KEEP void *atlas_alloc(uint32_t n) { return tak_malloc(n ? n : 1); }
KEEP void atlas_free(void *p) { tak_free(p); }
KEEP int atlas_last_w(void) { return g_last_w; }
KEEP int atlas_last_h(void) { return g_last_h; }
KEEP int atlas_last_x(void) { return g_last_x; }
KEEP int atlas_last_y(void) { return g_last_y; }
KEEP uint32_t atlas_last_size(void) { return g_last_size; }

static int valid_id(int id) { return id >= 0 && id < MAX_ARCHIVES && g_arc[id]; }

/* ── archives ─────────────────────────────────────────────────────────── */

/* The id of the archive at fs_path, named as the archive is, or -1. */
KEEP int atlas_open(const char *fs_path) {
    HPIArchive *a = HPI_OpenArchive(fs_path);
    if (!a) return -1;
    if (HPI_GetVersion(a) != HPI_VERSION_V2 || HPI_GetEntryCount(a) == 0) {
        HPI_CloseArchive(a);
        return -1;
    }
    const char *base = strrchr(fs_path, '/');
    base = base ? base + 1 : fs_path;
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    for (int i = 0; i < MAX_ARCHIVES; i++) {
        if (g_arc[i]) continue;
        g_arc[i] = a;
        snprintf(g_stem[i], sizeof(g_stem[i]), "%.*s", (int)(stem < 63 ? stem : 63), base);
        g_kmp[i] = dot && tak_stricmp(dot, ".kmp") == 0;
        return i;
    }
    HPI_CloseArchive(a);
    return -1;
}

KEEP void atlas_close(int id) {
    if (!valid_id(id)) return;
    HPI_CloseArchive(g_arc[id]);
    g_arc[id] = NULL;
    int k = 0;
    for (int i = 0; i < g_order_n; i++) if (g_order[i] != id) g_order[k++] = g_order[i];
    g_order_n = k;
    if (g_prefer == id) g_prefer = -1;
}

/* Every path in an archive, a line each. A star stays inside one folder,
   so each depth is listed apart. */
KEEP char *atlas_list(int id) {
    if (!valid_id(id)) return NULL;
    size_t cap = 4096, len = 0;
    char *out = (char *)tak_malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    char pattern[3 * DEPTH_MAX + 1];
    for (int depth = 1; depth <= DEPTH_MAX; depth++) {
        size_t p = 0;
        for (int d = 0; d < depth; d++) {
            if (d) pattern[p++] = '/';
            pattern[p++] = '*';
        }
        pattern[p] = '\0';
        char **paths = NULL;
        int n = 0;
        if (HPI_ListFiles(g_arc[id], pattern, &paths, &n) != 0) continue;
        for (int i = 0; i < n; i++) {
            size_t l = strlen(paths[i]);
            if (len + l + 2 > cap) {
                while (len + l + 2 > cap) cap *= 2;
                char *grown = (char *)tak_realloc(out, cap);
                if (!grown) { for (int j = i; j < n; j++) tak_free(paths[j]); tak_free(paths); return out; }
                out = grown;
            }
            memcpy(out + len, paths[i], l);
            len += l;
            out[len++] = '\n';
            out[len] = '\0';
            tak_free(paths[i]);
        }
        tak_free(paths);
    }
    return out;
}

/* The order the stand-in VFS asks archives in, as the game mounts them. */
KEEP void atlas_set_order(const int *ids, int n) {
    g_order_n = 0;
    for (int i = 0; i < n && i < MAX_ARCHIVES; i++)
        if (valid_id(ids[i])) g_order[g_order_n++] = ids[i];
}

KEEP void atlas_prefer(int id) { g_prefer = valid_id(id) ? id : -1; }

/* A file's bytes from one archive, or by the VFS's rule when id is -1. */
KEEP uint8_t *atlas_read(int id, const char *path) {
    void *data = NULL;
    uint32_t size = 0;
    g_last_size = 0;
    if (id >= 0) {
        if (!valid_id(id) || HPI_ReadFile(g_arc[id], path, &data, &size) != 0) return NULL;
    } else if (VFS_ReadFile(path, &data, &size) != 0) {
        return NULL;
    }
    g_last_size = size;
    return (uint8_t *)data;
}

/* ── the VFS the engine's readers ask ─────────────────────────────────── */

static int over_copy(const char *path, void **out, uint32_t *size) {
    if (!g_over_path || !path || tak_stricmp(path, g_over_path) != 0) return -1;
    uint8_t *b = (uint8_t *)tak_malloc(g_over_size ? g_over_size : 1);
    if (!b) return -1;
    memcpy(b, g_over_data, g_over_size);
    *out = b;
    *size = g_over_size;
    return 0;
}

static int is_kmp(int id) { return g_kmp[id]; }

/* The newest copy in mount order, the earlier winning a tie (VFS_Init).
   A map pack serves only kmap/. */
static int pick(const char *path) {
    int best = -1;
    uint32_t best_date = 0;
    int kmap = tak_strnicmp(path, "kmap/", 5) == 0;
    for (int i = 0; i < g_order_n; i++) {
        uint32_t date = 0;
        if (is_kmp(g_order[i]) && !kmap) continue;
        if (!HPI_FindFile(g_arc[g_order[i]], path, &date)) continue;
        if (best < 0 || date > best_date) { best = g_order[i]; best_date = date; }
    }
    return best;
}

/* The asset's own archive first, then as given, then without a leading
   archive name, as hpi_vfs.c strip_archive_prefix. */
int VFS_ReadFile(const char *path, void **out_data, uint32_t *out_size) {
    if (!path || !out_data || !out_size) return -1;
    if (over_copy(path, out_data, out_size) == 0) return 0;
    const char *slash = strchr(path, '/');
    const char *alt = NULL;
    int owner = -1;
    if (slash && slash != path) {
        size_t n = (size_t)(slash - path);
        for (int i = 0; i < g_order_n && !alt; i++) {
            const char *stem = g_stem[g_order[i]];
            if (!is_kmp(g_order[i]) && strlen(stem) == n && tak_strnicmp(stem, path, n) == 0) {
                alt = slash + 1;
                owner = g_order[i];
            }
        }
    }
    if (g_prefer >= 0) {
        if (HPI_ReadFile(g_arc[g_prefer], path, out_data, out_size) == 0) return 0;
        if (alt && HPI_ReadFile(g_arc[g_prefer], alt, out_data, out_size) == 0) return 0;
    }
    int at = pick(path);
    if (at >= 0) return HPI_ReadFile(g_arc[at], path, out_data, out_size);
    if (!alt) return -1;
    at = pick(alt);
    if (at >= 0) return HPI_ReadFile(g_arc[at], alt, out_data, out_size);
    const char *bare = strrchr(alt, '/');
    return bare ? HPI_ReadFile(g_arc[owner], bare + 1, out_data, out_size) : -1;
}

int VFS_ReadGameFile(const char *relative, void **out_data, uint32_t *out_size) {
    return over_copy(relative, out_data, out_size);
}

void VFS_FreeBuffer(void *data) { tak_free(data); }

/* ── palettes ─────────────────────────────────────────────────────────── */

static SDL_PixelFormat *rgba_format(void) {
    static SDL_PixelFormat *fmt;
    if (!fmt) fmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    return fmt;
}

static int load_table(const char *pcx_path, uint8_t transparent, uint32_t *table) {
    Palette pal;
    if (Palette_LoadPCX(&pal, pcx_path) != 0) return -1;
    Palette_BuildRGBATable(&pal, rgba_format(), table, transparent);
    return 0;
}

/* One palette by its path in the archives, index transparent cleared. */
KEEP int atlas_palette_file(const char *pcx_path, int transparent, uint32_t *table) {
    return load_table(pcx_path, (uint8_t)transparent, table);
}

static void greyscale(uint32_t *table, uint8_t transparent) {
    Palette pal;
    for (int i = 0; i < 256; i++) {
        pal.entries[i].r = pal.entries[i].g = pal.entries[i].b = (uint8_t)i;
        pal.entries[i].pad = 0;
    }
    Palette_BuildRGBATable(&pal, rgba_format(), table, transparent);
}

/* Rule 0 picks as UI_LoadGAFWithPalette, rule 1 as tex_atlas.c, with
   gameart for the map's palette. 1 when a grey ramp stood in. */
KEEP int atlas_gaf_palette(const char *gaf_path, int rule, uint32_t *table, char *name) {
    char path[256];
    name[0] = '\0';
    if (rule == 1) {
        const char *alt = Palette_LookupForGAFAlt(gaf_path);
        const char *primary = Palette_LookupForGAF(gaf_path);
        const char *tries[3] = { alt, (primary && strcmp(primary, "gameart.pcx") != 0) ? primary : NULL, "gameart.pcx" };
        for (int i = 0; i < 3; i++) {
            if (!tries[i]) continue;
            snprintf(path, sizeof(path), "data/palettes/%s", tries[i]);
            if (load_table(path, 0, table) == 0) { snprintf(name, 128, "%s", path); return 0; }
        }
        greyscale(table, 0);
        return 1;
    }
    size_t n = strlen(gaf_path);
    if (n > 4 && n < sizeof(path)) {
        memcpy(path, gaf_path, n - 4);
        memcpy(path + n - 4, ".pcx", 5);
        if (load_table(path, 9, table) == 0) { snprintf(name, 128, "%s", path); return 0; }
    }
    const char *resolved = Palette_LookupForGAF(gaf_path);
    if (resolved) {
        snprintf(path, sizeof(path), "data/palettes/%s", resolved);
        if (load_table(path, 9, table) == 0) { snprintf(name, 128, "%s", path); return 0; }
    }
    greyscale(table, 9);
    return 1;
}

/* Through the palette's .shd at one light level, as tex_atlas.c shades
   unit textures. -1 when it has none. */
KEEP int atlas_shade(const char *pcx_path, int level, uint32_t *table) {
    char path[256];
    size_t n = strlen(pcx_path);
    if (n < 5 || n >= sizeof(path) || level < 0 || level > 31) return -1;
    memcpy(path, pcx_path, n - 4);
    memcpy(path + n - 4, ".shd", 5);
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &data, &size) != 0 || !data) return -1;
    if (size < 8192) { tak_free(data); return -1; }
    uint32_t base[256];
    memcpy(base, table, sizeof(base));
    const uint8_t *row = (const uint8_t *)data + level * 256;
    for (int i = 0; i < 256; i++) table[i] = base[row[i]];
    tak_free(data);
    return 0;
}

/* ── GAF and TAF ──────────────────────────────────────────────────────── */

static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

/* The entry's header offset, or 0 when the table or header runs off the file. */
static uint32_t entry_offset(const uint8_t *d, uint32_t size, int entry) {
    if (size < 12 || rd32(d) != 0x00010100) return 0;
    uint32_t n = rd32(d + 4);
    if (entry < 0 || (uint32_t)entry >= n || 12u + 4u * (uint32_t)entry + 4u > size) return 0;
    uint32_t off = rd32(d + 12 + 4 * entry);
    if (off < 12 || (uint64_t)off + 40 > size) return 0;
    uint32_t frames = rd16(d + off);
    if ((uint64_t)off + 40 + 8ull * frames > size) return 0;
    return off;
}

static const FrameHeader *frame_at(GAFFile *g, uint32_t eoff, int frame) {
    FrameHeader *f = NULL;
    if (GAF_GetFrameInfo(g, eoff, frame, &f) != 0 || !f) return NULL;
    uint32_t at = (uint32_t)((const uint8_t *)f - g->data);
    if ((uint64_t)at + sizeof(FrameHeader) > g->data_size) return NULL;
    if (f->width == 0 || f->height == 0 || f->width > MAX_SIDE || f->height > MAX_SIDE) return NULL;
    if (f->subframes < 0 || f->subframes > MAX_SUBFRAMES) return NULL;
    return f;
}

/* A line a sequence: index, name, frames, then w,h,x,y,format a frame
   (0 raw, 1 run length, 4 and 5 TAF, -1 unreadable). */
KEEP char *atlas_gaf_info(const uint8_t *data, uint32_t size) {
    if (!data || size < 12 || rd32(data) != 0x00010100) return NULL;
    GAFFile g = { (uint8_t *)data, size, rd32(data + 4) };
    if (g.num_entries > 100000) return NULL;
    size_t cap = 1024, len = 0;
    char *out = (char *)tak_malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    for (uint32_t e = 0; e < g.num_entries; e++) {
        uint32_t eoff = entry_offset(data, size, (int)e);
        if (!eoff) continue;
        char name[33];
        memcpy(name, data + eoff + 8, 32);
        name[32] = '\0';
        for (char *c = name; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
        int frames = rd16(data + eoff);
        size_t need = len + 64 + (size_t)frames * 40;
        if (need > cap) {
            while (need > cap) cap *= 2;
            char *grown = (char *)tak_realloc(out, cap);
            if (!grown) break;
            out = grown;
        }
        len += (size_t)snprintf(out + len, cap - len, "%u\t%s\t%d\t", e, name, frames);
        for (int f = 0; f < frames; f++) {
            const FrameHeader *h = frame_at(&g, eoff, f);
            if (h) len += (size_t)snprintf(out + len, cap - len, "%s%u,%u,%d,%d,%d", f ? ";" : "",
                                           h->width, h->height, h->offset_x, h->offset_y,
                                           h->subframes > 0 ? 1 : h->compressed);
            else len += (size_t)snprintf(out + len, cap - len, "%s0,0,0,0,-1", f ? ";" : "");
        }
        out[len++] = '\n';
        out[len] = '\0';
    }
    return out;
}

/* One frame as RGBA through table, or a TAF frame's own colour. */
KEEP uint32_t *atlas_gaf_frame(const uint8_t *data, uint32_t size, int entry, int frame, const uint32_t *table) {
    g_last_w = g_last_h = 0;
    uint32_t eoff = entry_offset(data, size, entry);
    if (!eoff) return NULL;
    GAFFile g = { (uint8_t *)data, size, rd32(data + 4) };
    const FrameHeader *h = frame_at(&g, eoff, frame);
    if (!h) return NULL;
    uint32_t *px;
    if (h->subframes == 0 && (h->compressed == TAF_FORMAT_4444 || h->compressed == TAF_FORMAT_1555))
        px = TAF_DecodeFrameRGBA(&g, h);
    else
        px = table ? GAF_DecodeFrameRGBA(&g, h, table) : NULL;
    if (px) { g_last_w = h->width; g_last_h = h->height; g_last_x = h->offset_x; g_last_y = h->offset_y; }
    return px;
}

static int g_last_key;
KEEP int atlas_last_key(void) { return g_last_key; }

/* One frame as palette indices, for painting a remaster model. */
KEEP uint8_t *atlas_gaf_indices(const uint8_t *data, uint32_t size, int entry, int frame) {
    g_last_w = g_last_h = 0;
    uint32_t eoff = entry_offset(data, size, entry);
    if (!eoff) return NULL;
    GAFFile g = { (uint8_t *)data, size, rd32(data + 4) };
    const FrameHeader *h = frame_at(&g, eoff, frame);
    if (!h || h->compressed == TAF_FORMAT_4444 || h->compressed == TAF_FORMAT_1555) return NULL;
    uint8_t *idx = GAF_DecodeFrame(&g, h);
    if (idx) { g_last_w = h->width; g_last_h = h->height; g_last_key = h->transparency_index; }
    return idx;
}

/* A palette's 256 colours as RGB triples into out (768 bytes). */
KEEP int atlas_palette_rgb(const char *pcx_path, uint8_t *out) {
    Palette pal;
    if (Palette_LoadPCX(&pal, pcx_path) != 0) return -1;
    for (int i = 0; i < 256; i++) {
        out[i * 3] = pal.entries[i].r;
        out[i * 3 + 1] = pal.entries[i].g;
        out[i * 3 + 2] = pal.entries[i].b;
    }
    return 0;
}

/* ── pictures ─────────────────────────────────────────────────────────── */

static uint32_t *copy_stb(unsigned char *rgba, int w, int h) {
    if (!rgba) return NULL;
    uint32_t *px = (uint32_t *)tak_malloc((size_t)w * h * 4);
    if (px) memcpy(px, rgba, (size_t)w * h * 4);
    stbi_image_free(rgba);
    if (px) { g_last_w = w; g_last_h = h; }
    return px;
}

/* An 8-bit PCX of one plane or three. A palette is read by the engine's
   Palette_LoadPCX, as a GAF's is. */
static uint32_t *decode_pcx(const uint8_t *d, uint32_t size) {
    if (size < 128 || d[0] != 0x0A || d[3] != 8) return NULL;
    int w = rd16(d + 8) - rd16(d + 4) + 1, h = rd16(d + 10) - rd16(d + 6) + 1;
    int planes = d[65], bpl = rd16(d + 66), rle = d[2] == 1;
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE || bpl < w || (planes != 1 && planes != 3)) return NULL;
    size_t line = (size_t)bpl * planes;
    uint8_t *raw = (uint8_t *)tak_malloc(line * h);
    if (!raw) return NULL;
    memset(raw, 0, line * h);
    uint32_t at = 128;
    for (size_t o = 0; o < line * h && at < size;) {
        uint8_t b = d[at++];
        size_t run = 1;
        if (rle && (b & 0xC0) == 0xC0) {
            run = b & 0x3F;
            if (at >= size) break;
            b = d[at++];
        }
        while (run-- && o < line * h) raw[o++] = b;
    }
    uint32_t *px = (uint32_t *)tak_malloc((size_t)w * h * 4);
    if (!px) { tak_free(raw); return NULL; }
    if (planes == 1) {
        Palette pal;
        g_over_path = "atlas/picture.pcx";
        g_over_data = d;
        g_over_size = size;
        int ok = Palette_LoadPCX(&pal, g_over_path) == 0;
        g_over_path = NULL;
        if (!ok) for (int i = 0; i < 256; i++) pal.entries[i].r = pal.entries[i].g = pal.entries[i].b = (uint8_t)i;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                PaletteEntry e = pal.entries[raw[(size_t)y * line + x]];
                px[(size_t)y * w + x] = 0xFF000000u | ((uint32_t)e.b << 16) | ((uint32_t)e.g << 8) | e.r;
            }
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *r = raw + (size_t)y * line;
                px[(size_t)y * w + x] = 0xFF000000u | ((uint32_t)r[2 * bpl + x] << 16) |
                                        ((uint32_t)r[bpl + x] << 8) | r[x];
            }
    }
    tak_free(raw);
    g_last_w = w;
    g_last_h = h;
    return px;
}

/* kind 1 JPEG and 2 PNG by the engine's decoders, 3 PCX, 4 BMP, 5 TGA. */
KEEP uint32_t *atlas_image(const uint8_t *data, uint32_t size, int kind) {
    g_last_w = g_last_h = 0;
    if (!data || !size || size > 0x7fffffff) return NULL;
    uint32_t *px = NULL;
    int w = 0, h = 0, comp = 0;
    switch (kind) {
    case 1: if (JPG_DecodeRGBA(data, size, &px, &w, &h) != 0) return NULL; break;
    case 2: if (PNG_DecodeRGBA(data, size, &px, &w, &h) != 0) return NULL; break;
    case 3: return decode_pcx(data, size);
    case 4: case 5: {
        if (!stbi_info_from_memory(data, (int)size, &w, &h, &comp) || w > MAX_SIDE || h > MAX_SIDE) return NULL;
        return copy_stb(stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4), w, h);
    }
    default: return NULL;
    }
    if (px && (w > MAX_SIDE * 2 || h > MAX_SIDE * 2)) { tak_free(px); return NULL; }
    g_last_w = w;
    g_last_h = h;
    return px;
}

/* ── 3DO ──────────────────────────────────────────────────────────────── */

/* The last model read, as triangles in model pixels with corner UVs, a
   texture number (-1 flat) and a palette colour each. */
typedef struct Bake {
    float   *pos, *uv;
    int32_t *tex;
    uint8_t *col;
    int16_t *piece;                 /* the piece each triangle belongs to */
    float    off[256 * 3];          /* each piece's offset from its parent */
    int16_t  parent[256];
    int      tris, cap;
    char     names[8192];
    int      names_len, name_count;
    int      pieces;
    char     piece_names[256 * 34];
    int      piece_len;
} Bake;

static Bake g_bake;

static int name_index(const char *n) {
    char lower[33];
    size_t l = 0;
    for (; n[l] && l < 32; l++) lower[l] = (char)((n[l] >= 'A' && n[l] <= 'Z') ? n[l] + 32 : n[l]);
    lower[l] = '\0';
    int idx = 0;
    for (const char *p = g_bake.names; p < g_bake.names + g_bake.names_len; idx++) {
        size_t pl = strlen(p);
        if (strcmp(p, lower) == 0) return idx;
        p += pl + 1;
    }
    if (g_bake.names_len + (int)l + 1 >= (int)sizeof(g_bake.names)) return -1;
    memcpy(g_bake.names + g_bake.names_len, lower, l + 1);
    g_bake.names_len += (int)l + 1;
    return g_bake.name_count++;
}

static int grow(int more) {
    if (g_bake.tris + more <= g_bake.cap) return 0;
    int cap = g_bake.cap ? g_bake.cap * 2 : 1024;
    while (cap < g_bake.tris + more) cap *= 2;
    float *pos = (float *)tak_realloc(g_bake.pos, (size_t)cap * 9 * sizeof(float));
    if (pos) g_bake.pos = pos;
    float *uv = (float *)tak_realloc(g_bake.uv, (size_t)cap * 6 * sizeof(float));
    if (uv) g_bake.uv = uv;
    int32_t *tex = (int32_t *)tak_realloc(g_bake.tex, (size_t)cap * sizeof(int32_t));
    if (tex) g_bake.tex = tex;
    uint8_t *col = (uint8_t *)tak_realloc(g_bake.col, (size_t)cap);
    if (col) g_bake.col = col;
    int16_t *piece = (int16_t *)tak_realloc(g_bake.piece, (size_t)cap * sizeof(int16_t));
    if (piece) g_bake.piece = piece;
    if (!pos || !uv || !tex || !col || !piece) return -1;
    g_bake.cap = cap;
    return 0;
}

/* The corner a primitive's k-th vertex takes, as units.c prim_local_uv. */
static void corner(int k, float *u, float *v) {
    static const float c[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int i = k < 4 ? k : 2;
    *u = c[i][0];
    *v = c[i][1];
}

/* As units.c bake_emit: skip a selection mesh, fan each primitive, and
   fold the parent chain into positions here. */
static void bake_node(const Obj3DNode *n, float ax, float ay, float az, int parent) {
    float nx = ax + (float)n->offset_x / 65536.0f;
    float ny = ay + (float)n->offset_y / 65536.0f;
    float nz = az + (float)n->offset_z / 65536.0f;
    int me = g_bake.pieces < 256 ? g_bake.pieces : 255;
    if (g_bake.pieces < 256) {
        g_bake.parent[me] = (int16_t)parent;
        g_bake.off[me * 3] = (float)n->offset_x / 65536.0f;
        g_bake.off[me * 3 + 1] = (float)n->offset_y / 65536.0f;
        g_bake.off[me * 3 + 2] = (float)n->offset_z / 65536.0f;
    }
    g_bake.pieces++;
    size_t pl = strlen(n->name);
    if (g_bake.piece_len + (int)pl + 2 < (int)sizeof(g_bake.piece_names)) {
        memcpy(g_bake.piece_names + g_bake.piece_len, n->name, pl);
        g_bake.piece_len += (int)pl;
        g_bake.piece_names[g_bake.piece_len++] = '\n';
        g_bake.piece_names[g_bake.piece_len] = '\0';
    }
    int start = n->selection_marker != 0xFFFFFFFFu ? 1 : 0;
    for (int p = start; p < n->num_primitives; p++) {
        const Obj3DPrimitive *pr = &n->primitives[p];
        int nv = pr->num_vert_indices;
        if (nv < 3 || grow(nv - 2) != 0) continue;
        int tex = pr->texture_name[0] ? name_index(pr->texture_name) : -1;
        for (int t = 1; t < nv - 1; t++) {
            int ks[3] = { 0, t, t + 1 };
            int tri = g_bake.tris++;
            for (int c = 0; c < 3; c++) {
                const Obj3DVertex *v = &n->vertices[pr->vert_indices[ks[c]]];
                g_bake.pos[tri * 9 + c * 3 + 0] = nx + v->x / 65536.0f;
                g_bake.pos[tri * 9 + c * 3 + 1] = ny + v->y / 65536.0f;
                g_bake.pos[tri * 9 + c * 3 + 2] = nz + v->z / 65536.0f;
                corner(ks[c], &g_bake.uv[tri * 6 + c * 2], &g_bake.uv[tri * 6 + c * 2 + 1]);
            }
            g_bake.tex[tri] = tex;
            g_bake.col[tri] = (uint8_t)(pr->color_idx & 0xFF);
            g_bake.piece[tri] = (int16_t)me;
        }
    }
    for (const Obj3DNode *c = n->first_child; c; c = c->next_sibling) bake_node(c, nx, ny, nz, me);
}

/* A 3DO through the engine's Obj3D_Load: its triangle count, or -1. */
KEEP int atlas_3do(const uint8_t *data, uint32_t size) {
    g_bake.tris = 0;
    g_bake.names_len = 0;
    g_bake.names[0] = '\0';
    g_bake.name_count = 0;
    g_bake.pieces = 0;
    g_bake.piece_len = 0;
    g_bake.piece_names[0] = '\0';
    g_over_path = "objects3d/atlas.3do";
    g_over_data = data;
    g_over_size = size;
    Obj3DFile *obj = NULL;
    int rc = Obj3D_Load(&obj, g_over_path);
    g_over_path = NULL;
    if (rc != 0 || !obj) return -1;
    bake_node(obj->root, 0, 0, 0, -1);
    Obj3D_Close(obj);
    return g_bake.tris;
}

KEEP float *atlas_3do_pos(void) { return g_bake.pos; }
KEEP float *atlas_3do_uv(void) { return g_bake.uv; }
KEEP int32_t *atlas_3do_tex(void) { return g_bake.tex; }
KEEP uint8_t *atlas_3do_col(void) { return g_bake.col; }
KEEP const char *atlas_3do_names(void) { return g_bake.names_len ? g_bake.names : ""; }
KEEP int atlas_3do_name_count(void) { return g_bake.name_count; }
KEEP const char *atlas_3do_pieces(void) { return g_bake.piece_names; }
KEEP int atlas_3do_piece_count(void) { return g_bake.pieces; }
KEEP int16_t *atlas_3do_piece_of(void) { return g_bake.piece; }
KEEP int16_t *atlas_3do_parents(void) { return g_bake.parent; }
KEEP float *atlas_3do_offsets(void) { return g_bake.off; }
