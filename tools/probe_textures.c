/*
 * probe_textures.c — Phase C R4 recon. Two passes:
 *
 *   1. Walk every objects3d/*.3do, parse the tree, collect every
 *      distinct texture name into a deduped set. Total tells us how
 *      many texture entries the atlas pipeline will see across the
 *      whole game.
 *
 *   2. For each texture GAF (88 files), enumerate its entry names.
 *      Cross-reference: which GAF holds each texture name? Are names
 *      unique across GAFs (single flat namespace)? Are GAFs organized
 *      by faction prefix?
 *
 * Output is verbose; pipe to a file for analysis. Most useful summary
 * lines start with "###" so you can grep for them.
 */

#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_gaf.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* ── Tiny string set (linear, fine for ~thousands) ──────────────── */
typedef struct {
    char  **names;     /* tak_malloc'd lowercase copies */
    int     count;
    int     cap;
} StrSet;

static int strset_contains(const StrSet *s, const char *name) {
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) return 1;
    }
    return 0;
}
static void strset_add(StrSet *s, const char *name) {
    if (strset_contains(s, name)) return;
    if (s->count >= s->cap) {
        int newc = s->cap ? s->cap * 2 : 64;
        char **bigger = (char **)tak_malloc(newc * sizeof(char *));
        if (s->names) {
            memcpy(bigger, s->names, s->count * sizeof(char *));
            tak_free(s->names);
        }
        s->names = bigger;
        s->cap = newc;
    }
    size_t n = strlen(name);
    char *copy = (char *)tak_malloc(n + 1);
    for (size_t i = 0; i <= n; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        copy[i] = c;
    }
    s->names[s->count++] = copy;
}
static void strset_free(StrSet *s) {
    for (int i = 0; i < s->count; i++) tak_free(s->names[i]);
    if (s->names) tak_free(s->names);
    s->names = NULL; s->count = 0; s->cap = 0;
}

/* ── 3DO walker — copy of probe_3do.c shape, no I/O, just collects ── */

typedef struct {
    uint32_t version, num_vertices, num_primitives, offset_selection;
    int32_t  x, y, z;
    uint32_t offset_name, unused, offset_vertices, offset_primitives;
    uint32_t offset_sibling, offset_child;
} Obj3DHeader;

typedef struct {
    uint32_t color_idx, num_vert_indices, always_zero, offset_vert_list;
    uint32_t offset_texture, unused1, unused2, unused3;
} Obj3DPrim;

static const uint8_t *g_buf;
static uint32_t       g_size;

static const uint8_t *at(uint32_t off, uint32_t len) {
    if ((uint64_t)off + len > g_size) return NULL;
    return g_buf + off;
}
static const char *str_at(uint32_t off) {
    if (off == 0 || off >= g_size) return NULL;
    return (const char *)g_buf + off;
}

/* Track non-zero values in fields the spec says are "unused" or
 * "always zero". If they all stay 0 across every primitive in every
 * shipped 3DO, the spec is honest. Otherwise we need to investigate
 * what TAK is using them for. */
static struct {
    int header_unused_nonzero;     /* Object3D.unused at 0x20 */
    int prim_always_zero_nonzero;  /* Primitive.always_zero at 0x08 */
    int prim_unused1_nonzero;      /* Primitive.unused1 at 0x14 */
    int prim_unused2_nonzero;      /* Primitive.unused2 at 0x18 */
    int prim_unused3_nonzero;      /* Primitive.unused3 at 0x1C */
    /* Sample one observed value of each so we can see the distribution */
    uint32_t sample_always_zero, sample_unused1, sample_unused2, sample_unused3;
    uint32_t sample_header_unused;
} g_field_audit;

static void collect(uint32_t off, StrSet *out) {
    const Obj3DHeader *h = (const Obj3DHeader *)at(off, sizeof(Obj3DHeader));
    if (!h) return;
    if (h->unused != 0) {
        g_field_audit.header_unused_nonzero++;
        if (g_field_audit.sample_header_unused == 0)
            g_field_audit.sample_header_unused = h->unused;
    }
    if (h->num_primitives > 0) {
        const Obj3DPrim *prims = (const Obj3DPrim *)at(h->offset_primitives,
            h->num_primitives * sizeof(Obj3DPrim));
        if (prims) {
            for (uint32_t i = 0; i < h->num_primitives; i++) {
                const Obj3DPrim *p = &prims[i];
                if (p->always_zero != 0) {
                    g_field_audit.prim_always_zero_nonzero++;
                    if (g_field_audit.sample_always_zero == 0)
                        g_field_audit.sample_always_zero = p->always_zero;
                }
                if (p->unused1 != 0) {
                    g_field_audit.prim_unused1_nonzero++;
                    if (g_field_audit.sample_unused1 == 0)
                        g_field_audit.sample_unused1 = p->unused1;
                }
                if (p->unused2 != 0) {
                    g_field_audit.prim_unused2_nonzero++;
                    if (g_field_audit.sample_unused2 == 0)
                        g_field_audit.sample_unused2 = p->unused2;
                }
                if (p->unused3 != 0) {
                    g_field_audit.prim_unused3_nonzero++;
                    if (g_field_audit.sample_unused3 == 0)
                        g_field_audit.sample_unused3 = p->unused3;
                }
                const char *t = str_at(p->offset_texture);
                if (t && t[0] != '\0') strset_add(out, t);
            }
        }
    }
    if (h->offset_child   != 0) collect(h->offset_child,   out);
    if (h->offset_sibling != 0) collect(h->offset_sibling, out);
}

/* ── Walk a GAF and collect each entry's name ───────────────────── */
/* GAF entry layout from tak_gaf.h:
 *   GAFHeader at offset 0 (12 bytes: version, subversion, num_entries, reserved)
 *   uint32 entry_pointers[num_entries] starting at offset 12
 *   each entry pointer points to an EntryHeader (32 bytes incl. 32-byte name) */

static void gaf_collect_entries(const uint8_t *gaf_buf, uint32_t gaf_size,
                                 StrSet *out) {
    if (gaf_size < 16) return;
    uint32_t num_entries = *(const uint32_t *)(gaf_buf + 4);
    uint32_t entry_table_size = 12 + num_entries * 4;
    if (entry_table_size > gaf_size) return;
    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t entry_off = *(const uint32_t *)(gaf_buf + 12 + i * 4);
        if (entry_off + 32 > gaf_size) continue;
        /* EntryHeader: num_frames(2) + unknown1(2) + unknown2(4) + name[32] */
        const char *name = (const char *)(gaf_buf + entry_off + 8);
        char nbuf[33];
        memcpy(nbuf, name, 32);
        nbuf[32] = '\0';
        if (nbuf[0] == '\0') continue;
        strset_add(out, nbuf);
    }
}

/* ── Find which GAF(s) contain a given texture name ─────────────── */

typedef struct {
    char       gaf_path[128];
    StrSet     entries;
} GafIndex;

static GafIndex *g_gaf_indexes;
static int       g_gaf_count;

static const char *find_gaf_for_texture(const char *tex_name) {
    static char both_buf[256];
    int hits = 0;
    both_buf[0] = '\0';
    for (int i = 0; i < g_gaf_count; i++) {
        if (strset_contains(&g_gaf_indexes[i].entries, tex_name)) {
            if (hits > 0) strncat(both_buf, ", ", sizeof(both_buf) - strlen(both_buf) - 1);
            strncat(both_buf, g_gaf_indexes[i].gaf_path,
                    sizeof(both_buf) - strlen(both_buf) - 1);
            hits++;
        }
    }
    if (hits == 0) return NULL;
    return both_buf;
}

int main(void) {
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    /* ── Pass 1: collect texture names from every 3DO ──────────── */
    StrSet tex_names = {0};
    char **objects = NULL;
    int n_obj = 0;
    if (VFS_ListFiles("objects3d/*.3do", &objects, &n_obj) != 0 || n_obj <= 0) {
        fprintf(stderr, "no .3do files found\n");
        return 1;
    }
    int parsed = 0, failed = 0;
    for (int i = 0; i < n_obj; i++) {
        void *raw = NULL;
        uint32_t sz = 0;
        if (VFS_ReadFile(objects[i], &raw, &sz) != 0) { failed++; continue; }
        g_buf = (const uint8_t *)raw;
        g_size = sz;
        collect(0, &tex_names);
        tak_free(raw);
        parsed++;
    }
    printf("### Pass 1: %d/%d 3DO files parsed, %d distinct texture names found\n",
            parsed, n_obj, tex_names.count);

    printf("### \"unused\" field audit across every primitive in every 3DO:\n");
    printf("    Object3D.unused (0x20)        non-zero count = %d  sample=0x%08x\n",
            g_field_audit.header_unused_nonzero, g_field_audit.sample_header_unused);
    printf("    Primitive.always_zero (0x08)  non-zero count = %d  sample=0x%08x\n",
            g_field_audit.prim_always_zero_nonzero, g_field_audit.sample_always_zero);
    printf("    Primitive.unused1 (0x14)      non-zero count = %d  sample=0x%08x\n",
            g_field_audit.prim_unused1_nonzero, g_field_audit.sample_unused1);
    printf("    Primitive.unused2 (0x18)      non-zero count = %d  sample=0x%08x\n",
            g_field_audit.prim_unused2_nonzero, g_field_audit.sample_unused2);
    printf("    Primitive.unused3 (0x1C)      non-zero count = %d  sample=0x%08x\n",
            g_field_audit.prim_unused3_nonzero, g_field_audit.sample_unused3);

    /* ── Pass 2: index every texture GAF ────────────────────────── */
    char **gafs = NULL;
    int n_gaf = 0;
    if (VFS_ListFiles("textures/*.gaf", &gafs, &n_gaf) != 0 || n_gaf <= 0) {
        fprintf(stderr, "no texture GAFs found\n");
        return 1;
    }
    g_gaf_indexes = (GafIndex *)tak_malloc(n_gaf * sizeof(GafIndex));
    g_gaf_count = n_gaf;
    int total_entries = 0;
    for (int i = 0; i < n_gaf; i++) {
        memset(&g_gaf_indexes[i], 0, sizeof(GafIndex));
        snprintf(g_gaf_indexes[i].gaf_path,
                 sizeof(g_gaf_indexes[i].gaf_path),
                 "%s", gafs[i]);
        void *raw = NULL;
        uint32_t sz = 0;
        if (VFS_ReadFile(gafs[i], &raw, &sz) == 0) {
            gaf_collect_entries((const uint8_t *)raw, sz, &g_gaf_indexes[i].entries);
            total_entries += g_gaf_indexes[i].entries.count;
            tak_free(raw);
        }
    }
    printf("### Pass 2: %d texture GAFs, %d total entries (sum across GAFs)\n",
            n_gaf, total_entries);

    /* ── Cross-reference ─────────────────────────────────────── */
    int found = 0, missing = 0, multi = 0;
    int show_missing = 0, show_multi = 0;
    const int SHOW_LIMIT = 10;
    for (int i = 0; i < tex_names.count; i++) {
        const char *t = tex_names.names[i];
        int hits = 0;
        const char *found_in = NULL;
        for (int g = 0; g < n_gaf; g++) {
            if (strset_contains(&g_gaf_indexes[g].entries, t)) {
                hits++;
                found_in = g_gaf_indexes[g].gaf_path;
            }
        }
        if (hits == 0) {
            missing++;
            if (show_missing++ < SHOW_LIMIT) {
                printf("  MISSING '%s'\n", t);
            }
        } else if (hits > 1) {
            multi++;
            if (show_multi++ < SHOW_LIMIT) {
                printf("  MULTI   '%s' (%d GAFs)\n", t, hits);
            }
        } else {
            found++;
        }
        (void)found_in;
    }
    printf("### Cross-reference: %d found in exactly one GAF, %d in multiple, %d missing\n",
            found, multi, missing);

    /* ── Sample a few names per faction prefix ────────────────── */
    static const char *fac_prefixes[] = {"ara", "ver", "tar", "zon"};
    for (size_t f = 0; f < 4; f++) {
        printf("\n### faction '%s' — sample texture mappings\n", fac_prefixes[f]);
        int shown = 0;
        for (int i = 0; i < tex_names.count && shown < 5; i++) {
            const char *t = tex_names.names[i];
            /* Naive prefix match — texture names don't always start
             * with a faction tag (most don't), but it gives a sample. */
            (void)f;
            for (int g = 0; g < n_gaf; g++) {
                if (strset_contains(&g_gaf_indexes[g].entries, t)) {
                    printf("  %-24s -> %s\n", t, g_gaf_indexes[g].gaf_path);
                    shown++;
                    break;
                }
            }
            if (shown >= 5) break;
        }
        break; /* one sample for now */
    }

    /* ── Cleanup ─────────────────────────────────────────────── */
    for (int i = 0; i < n_gaf; i++) strset_free(&g_gaf_indexes[i].entries);
    tak_free(g_gaf_indexes);
    strset_free(&tex_names);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
