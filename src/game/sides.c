/*
 * sides.c -- The side table, read from gamedata/sidedata.tdf.
 *
 * Everything that asks "which sides are there" asks this table, so the
 * base game offers its four kingdoms and Iron Plague's side data adds
 * Creon without any list being written down in code.
 */

#include "tak_sides.h"
#include "tak_dataset.h"
#include "tak_hpi.h"
#include "tak_tdf.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

static TakSideInfo sd_table[TAK_SIDES_MAX];
static int         sd_count = 0;
static int         sd_loaded = 0;
static unsigned    sd_generation = 0;

static void sd_copy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    while (n + 1 < cap && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

/* The merged archives decide which copy is read, newest entry first, as
 * in the original. The loose dev tree only fills in when no archive has
 * the file, so its base copy never hides Iron Plague's SIDE7. */
static TDFFile *sd_open(void) {
    static const char *const paths[] = {
        "gamedata/sidedata.tdf", "data/gamedata/sidedata.tdf"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (VFS_FileExists(paths[i]) != 0) continue;
        TDFFile *tdf = TDF_Open(paths[i]);
        if (!tdf) continue;
        if (TDF_Load(tdf) == 0) return tdf;
        TDF_Close(tdf);
    }
    return NULL;
}

static void sd_read_side(TDFFile *tdf, TakSideInfo *s) {
    sd_copy(s->name, sizeof(s->name), TDF_ReadString(tdf, "name", ""));
    /* Four bytes with the terminator, as the original reads it
     * (legacy:164606). */
    sd_copy(s->prefix, 4, TDF_ReadString(tdf, "nameprefix", ""));
    sd_copy(s->commander, sizeof(s->commander),
            TDF_ReadString(tdf, "commander", ""));
    sd_copy(s->logogaf, sizeof(s->logogaf), TDF_ReadString(tdf, "logogaf", ""));
    sd_copy(s->logoart, sizeof(s->logoart), TDF_ReadString(tdf, "logoart", ""));
    sd_copy(s->buildsparkle, sizeof(s->buildsparkle),
            TDF_ReadString(tdf, "buildsparklygaf", ""));
    sd_copy(s->buildsparkle_anim, sizeof(s->buildsparkle_anim),
            TDF_ReadString(tdf, "buildsparklyanim", ""));
    sd_copy(s->resurrectsparkle, sizeof(s->resurrectsparkle),
            TDF_ReadString(tdf, "resurrectsparklygaf", ""));
    sd_copy(s->resurrectsparkle_anim, sizeof(s->resurrectsparkle_anim),
            TDF_ReadString(tdf, "resurrectsparklyanim", ""));
}

static void sd_load(void) {
    if (!VFS_IsInitialized()) { sd_loaded = 0; sd_count = 0; return; }
    unsigned gen = VFS_Generation();
    if (sd_loaded && sd_generation == gen) return;
    sd_loaded = 1;
    sd_generation = gen;
    sd_count = 0;
    memset(sd_table, 0, sizeof(sd_table));

    TDFFile *tdf = sd_open();
    if (!tdf) return;
    /* SIDE0, SIDE1, ... until one is missing (legacy:164568-164578). */
    for (int i = 0; i < TAK_SIDES_MAX; i++) {
        char section[16];
        snprintf(section, sizeof(section), "SIDE%d", i);
        if (TDF_PushSection(tdf, section) != 0) break;
        sd_read_side(tdf, &sd_table[i]);
        TDF_PopSection(tdf);
        sd_count = i + 1;
    }
    TDF_Close(tdf);
}

int Sides_Count(void) {
    sd_load();
    return sd_count;
}

const TakSideInfo *Sides_Get(int side) {
    sd_load();
    if (side < 0 || side >= sd_count) return NULL;
    return &sd_table[side];
}

int Sides_IsPlayable(int side) {
    const TakSideInfo *s = Sides_Get(side);
    return s && s->commander[0] != '\0';
}

int Sides_Next(int side) {
    sd_load();
    int i = side;
    for (int guard = 0; guard < sd_count; guard++) {
        i++;
        if (i >= sd_count || i < 0) i = 0;
        if (sd_table[i].commander[0]) return i;   /* legacy:134964-134972 */
    }
    return side;
}

int Sides_Set(int side, TakSidesMode mode, int creon_allowed) {
    if (side < 0) return 0;
    if (mode == TAK_SIDES_CAMPAIGN || side <= 3) return side;
    /* Any side past the fourth is the expansion's (legacy:134910-134936). */
    if (!TAK_DataSet_HasIronPlague()) return 0;
    if (mode == TAK_SIDES_MULTIPLAYER && !creon_allowed) return 0;
    return side;
}

int Sides_Cycle(int side, TakSidesMode mode, int creon_allowed) {
    return Sides_Set(Sides_Next(side), mode, creon_allowed);
}

void Sides_DisplayName(int side, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    const TakSideInfo *s = Sides_Get(side);
    if (!s) return;
    sd_copy(out, cap, s->name);
    for (size_t i = 1; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
}

int Sides_FindByPrefix(const char *prefix) {
    if (!prefix || !prefix[0]) return -1;
    sd_load();
    for (int i = 0; i < sd_count; i++) {
        if (sd_table[i].prefix[0] && tak_stricmp(sd_table[i].prefix, prefix) == 0)
            return i;
    }
    return -1;
}

int Sides_FindByName(const char *name) {
    if (!name || !name[0]) return -1;
    sd_load();
    for (int i = 0; i < sd_count; i++) {
        if (sd_table[i].name[0] && tak_stricmp(sd_table[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void sd_lower(char *dst, size_t cap, const char *src) {
    sd_copy(dst, cap, src);
    for (char *c = dst; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
}

int Sides_FindInText(const char *text) {
    if (!text || !text[0]) return -1;
    sd_load();
    char line[256];
    sd_lower(line, sizeof(line), text);
    for (int i = 0; i < sd_count; i++) {
        char name[32];
        sd_lower(name, sizeof(name), sd_table[i].name);
        if (name[0] && strstr(line, name)) return i;
    }
    return -1;
}
