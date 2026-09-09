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
    int      footprint_x;
    int      footprint_z;
    int      height;
    int      blocking;
    int      reclaimable;
    int      indestructible;
    int      damage;
    float    sacred_site;   /* sacredsite tier (1.0/1.5/2.0); 0 = none */
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

#endif /* TAK_FEATURES_H */
