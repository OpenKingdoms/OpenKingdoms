/*
 * soundclass.c — Data-driven sound class system (Phase 5)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  GAME AUDIO LESSON: Data-driven audio and probability weights
 * ═══════════════════════════════════════════════════════════════════
 *
 * Hardcoding "when knight is selected, play knight_select.wav" doesn't
 * scale. With 150 unit types × 6 actions × multiple variants, you'd
 * have thousands of if-statements. Instead, TA:K uses TDF config files
 * that map (unit_type, action) → weighted sound pool.
 *
 * WEIGHTED RANDOM SELECTION:
 *
 * Consider: [select] { TONEARA = 3.0; ARAKNIGHSEL3 = 1.0; }
 * Total weight = 4.0. TONEARA has 3/4 = 75% chance, ARAKNIGHSEL3
 * has 1/4 = 25% chance.
 *
 * Algorithm (CDF sampler):
 *   1. Pick random float r in [0, total_weight)
 *   2. Walk entries, subtracting each weight from r
 *   3. When r goes negative, that entry wins
 *
 * This is the same algorithm used by loot drop tables, dialogue
 * systems, and procedural generation. Simple, efficient, exact.
 *
 * WHY VARIETY MATTERS:
 *
 * If the same sound plays every time you select a unit, it gets
 * annoying within minutes. By mixing in a generic faction tone
 * (TONEARA) at 75% and a unique voice at 25%, the audio feels
 * varied and alive without the player consciously noticing the
 * system. This is a well-known game audio trick called "round
 * robin with weighting."
 *
 * TWO TDF FORMATS:
 *
 * 1. Unit classes (prioritized=1): keys are sound NAMES, values
 *    are probability WEIGHTS (floats).
 *
 * 2. Hit/ambient classes: keys are "sound0", "sound1", etc., values
 *    are WAV FILENAMES. All variants have equal probability.
 */

#include "tak_soundclass.h"
#include "tak_tdf.h"
#include "tak_hpi.h"
#include "tak_util.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Limits ──────────────────────────────────────────────────────── */

/* Base plus expansion tables together: 236 unit classes and 16 hit
 * classes load from a full install. */
#define SC_MAX_CLASSES      512
#define SC_MAX_ACTIONS       16
#define SC_MAX_POOL_ENTRIES  16
#define SC_MAX_HIT_CLASSES   32
#define SC_MAX_MATERIALS      8
#define SC_MAX_HIT_SOUNDS    16

/* ── Internal types ──────────────────────────────────────────────── */

typedef struct {
    char  name[64];   /* e.g. "TONEARA" or "SWRDFL01.wav" */
    float weight;     /* probability weight (1.0 for equal-chance) */
} PoolEntry;

typedef struct {
    char       action[32];     /* e.g. "select", "move", "attack" */
    PoolEntry  entries[SC_MAX_POOL_ENTRIES];
    int        count;
    float      total_weight;
} SoundAction;

typedef struct {
    char         name[64];     /* e.g. "ARAKNIGH" */
    SoundAction  actions[SC_MAX_ACTIONS];
    int          action_count;
    int          prioritized;  /* 1 = weighted, 0 = indexed */
} SoundClass;

/* Hit sound class: weapon type → material → sound variants */
typedef struct {
    char  name[32];            /* material name: "flesh", "armor", etc. */
    char  sounds[SC_MAX_HIT_SOUNDS][64];
    int   count;
} HitMaterial;

typedef struct {
    char         weapon[32];   /* "sword", "arrow", "cannon", etc. */
    HitMaterial  materials[SC_MAX_MATERIALS];
    int          material_count;
} HitSoundClass;

/* ── Global state ────────────────────────────────────────────────── */

static SoundClass    s_classes[SC_MAX_CLASSES];
static int           s_class_count = 0;

static HitSoundClass s_hit_classes[SC_MAX_HIT_CLASSES];
static int           s_hit_class_count = 0;

/* ── Parsing helpers ─────────────────────────────────────────────── */

/* The TDF section cursor is one per file, so walking a nested level
 * ends the outer walk. Snapshot the names at each level first. Only
 * the first hit class ever loaded before this: sword hits played,
 * arrows and cannon shells did not. */
#define SC_MAX_SECTIONS 64

static int collect_sections(TDFFile *tdf, char names[][64], int max) {
    int n = 0;
    const char *name = TDF_GetFirstSection(tdf);
    while (name && n < max) {
        strncpy(names[n], name, 63);
        names[n][63] = '\0';
        n++;
        name = TDF_GetNextSection(tdf);
    }
    return n;
}

/* Parse a unit soundclass TDF (weighted format).
 * Structure: [UNITNAME] { prioritized=1; [select] { SOUND=weight; } } */
static int parse_unit_soundclass(TDFFile *tdf) {
    char classes[SC_MAX_SECTIONS][64];
    int n_classes = collect_sections(tdf, classes, SC_MAX_SECTIONS);
    for (int c = 0; c < n_classes && s_class_count < SC_MAX_CLASSES; c++) {
        const char *class_name = classes[c];
        SoundClass *sc = &s_classes[s_class_count];
        memset(sc, 0, sizeof(*sc));
        strncpy(sc->name, class_name, sizeof(sc->name) - 1);

        if (TDF_PushSection(tdf, class_name) == 0) {
            sc->prioritized = TDF_ReadInt(tdf, "prioritized", 0);

            /* Enumerate action subsections */
            char actions[SC_MAX_ACTIONS][64];
            int n_actions = collect_sections(tdf, actions, SC_MAX_ACTIONS);
            for (int a = 0; a < n_actions; a++) {
                const char *action_name = actions[a];
                SoundAction *sa = &sc->actions[sc->action_count];
                memset(sa, 0, sizeof(*sa));
                strncpy(sa->action, action_name, sizeof(sa->action) - 1);

                if (TDF_PushSection(tdf, action_name) == 0) {
                    if (sc->prioritized) {
                        /* Weighted format: key=sound_name, value=weight */
                        const char *key = TDF_GetFirstKey(tdf);
                        while (key && sa->count < SC_MAX_POOL_ENTRIES) {
                            PoolEntry *pe = &sa->entries[sa->count];
                            strncpy(pe->name, key, sizeof(pe->name) - 1);
                            pe->weight = TDF_ReadFloat(tdf, key, 1.0f);
                            sa->total_weight += pe->weight;
                            sa->count++;
                            key = TDF_GetNextKey(tdf);
                        }
                    } else {
                        /* Indexed format: sound0=file.wav, sound1=file.wav */
                        for (int i = 0; i < SC_MAX_POOL_ENTRIES; i++) {
                            char key[16];
                            snprintf(key, sizeof(key), "sound%d", i);
                            const char *val = TDF_ReadString(tdf, key, NULL);
                            if (!val) break;
                            PoolEntry *pe = &sa->entries[sa->count];
                            strncpy(pe->name, val, sizeof(pe->name) - 1);
                            pe->weight = 1.0f;
                            sa->total_weight += 1.0f;
                            sa->count++;
                        }
                    }
                    TDF_PopSection(tdf);
                }

                sc->action_count++;
            }

            TDF_PopSection(tdf);
        }

        if (sc->action_count > 0) {
            s_class_count++;
        }
    }

    return 0;
}

/* Parse the hit soundclass TDF (soundclasses.tdf).
 * Structure: [weapon] { [material] { sound0=file.wav; } } */
static int parse_hit_soundclass(TDFFile *tdf) {
    char weapons[SC_MAX_SECTIONS][64];
    int n_weapons = collect_sections(tdf, weapons, SC_MAX_SECTIONS);
    for (int w = 0; w < n_weapons && s_hit_class_count < SC_MAX_HIT_CLASSES; w++) {
        const char *weapon = weapons[w];
        HitSoundClass *hc = &s_hit_classes[s_hit_class_count];
        memset(hc, 0, sizeof(*hc));
        strncpy(hc->weapon, weapon, sizeof(hc->weapon) - 1);

        if (TDF_PushSection(tdf, weapon) == 0) {
            char materials[SC_MAX_MATERIALS][64];
            int n_materials = collect_sections(tdf, materials, SC_MAX_MATERIALS);
            for (int m = 0; m < n_materials; m++) {
                const char *material = materials[m];
                HitMaterial *hm = &hc->materials[hc->material_count];
                memset(hm, 0, sizeof(*hm));
                strncpy(hm->name, material, sizeof(hm->name) - 1);

                if (TDF_PushSection(tdf, material) == 0) {
                    for (int i = 0; i < SC_MAX_HIT_SOUNDS; i++) {
                        char key[16];
                        snprintf(key, sizeof(key), "sound%d", i);
                        const char *val = TDF_ReadString(tdf, key, NULL);
                        if (!val) break;
                        strncpy(hm->sounds[hm->count], val,
                                sizeof(hm->sounds[0]) - 1);
                        hm->count++;
                    }
                    TDF_PopSection(tdf);
                }

                if (hm->count > 0) hc->material_count++;
            }
            TDF_PopSection(tdf);
        }

        if (hc->material_count > 0) s_hit_class_count++;
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

int SoundClass_LoadAll(void) {
    s_class_count = 0;
    s_hit_class_count = 0;

    /* Enumerate all .tdf files in gamedata/soundclasses/ via VFS */
    char **paths = NULL;
    int count = 0;
    if (VFS_ListFiles("gamedata/soundclasses/*", &paths, &count) != 0 || count == 0) {
        fprintf(stderr, "SoundClass: no TDF files found in gamedata/soundclasses/\n");
        return -1;
    }

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        /* Skip non-.tdf files */
        size_t len = strlen(paths[i]);
        if (len < 4 || tak_stricmp(paths[i] + len - 4, ".tdf") != 0) {
            tak_free(paths[i]);
            continue;
        }

        TDFFile *tdf = TDF_Open(paths[i]);
        if (!tdf) { tak_free(paths[i]); continue; }
        if (TDF_Load(tdf) != 0) { TDF_Close(tdf); tak_free(paths[i]); continue; }

        /* soundclasses.tdf is the hit-sound definitions; all others
         * are unit or ambient sound classes */
        const char *basename = paths[i];
        /* Find the filename part after the last / */
        const char *slash = strrchr(basename, '/');
        if (slash) basename = slash + 1;

        if (tak_stricmp(basename, "soundclasses.tdf") == 0) {
            parse_hit_soundclass(tdf);
        } else {
            parse_unit_soundclass(tdf);
        }

        TDF_Close(tdf);
        loaded++;
        tak_free(paths[i]);
    }
    tak_free(paths);

    fprintf(stderr, "SoundClass: loaded %d files → %d unit classes, %d hit classes\n",
            loaded, s_class_count, s_hit_class_count);

    return 0;
}

void SoundClass_FreeAll(void) {
    /* All data is in static arrays — just reset counts */
    s_class_count = 0;
    s_hit_class_count = 0;
}

int SoundClass_Find(const char *class_name) {
    if (!class_name) return -1;
    for (int i = 0; i < s_class_count; i++) {
        if (tak_stricmp(s_classes[i].name, class_name) == 0)
            return i;
    }
    return -1;
}

const char *SoundClass_SelectSound(int class_id, const char *action) {
    if (class_id < 0 || class_id >= s_class_count) return NULL;

    SoundClass *sc = &s_classes[class_id];

    /* Find the matching action */
    SoundAction *sa = NULL;
    for (int i = 0; action && i < sc->action_count; i++) {
        if (tak_stricmp(sc->actions[i].action, action) == 0) {
            sa = &sc->actions[i];
            break;
        }
    }

    /* An unknown action (or none) plays the first subsection in file
     * order, which is what the original falls back to (legacy:221615). */
    if (!sa && sc->action_count > 0) sa = &sc->actions[0];

    if (!sa || sa->count == 0) return NULL;

    /* Weighted random selection (CDF sampler):
     * Pick a random value in [0, total_weight), walk the entries
     * subtracting each weight. The entry that makes r go negative wins. */
    float r = ((float)rand() / (float)RAND_MAX) * sa->total_weight;
    for (int i = 0; i < sa->count; i++) {
        r -= sa->entries[i].weight;
        if (r <= 0.0f) return sa->entries[i].name;
    }

    /* Fallback (shouldn't happen with correct weights) */
    return sa->entries[sa->count - 1].name;
}

const char *SoundClass_SelectHitSound(const char *weapon_type,
                                       const char *material) {
    if (!weapon_type) return NULL;

    /* Find the weapon class */
    HitSoundClass *hc = NULL;
    for (int i = 0; i < s_hit_class_count; i++) {
        if (tak_stricmp(s_hit_classes[i].weapon, weapon_type) == 0) {
            hc = &s_hit_classes[i];
            break;
        }
    }
    if (!hc) return NULL;

    /* No material, or one the class lacks, plays the first subsection
     * in file order (the [default] block) like the original
     * (legacy:221657-221665). */
    HitMaterial *hm = NULL;
    for (int i = 0; material && i < hc->material_count; i++) {
        if (tak_stricmp(hc->materials[i].name, material) == 0) {
            hm = &hc->materials[i];
            break;
        }
    }
    if (!hm && hc->material_count > 0) hm = &hc->materials[0];
    if (!hm || hm->count == 0) return NULL;

    /* Uniform random selection (all hit variants are equal weight) */
    return hm->sounds[rand() % hm->count];
}

int SoundClass_GetCount(void) {
    return s_class_count;
}
