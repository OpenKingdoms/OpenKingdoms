#include "tak_moveinfo.h"
#include "tak_tdf.h"
#include "tak_util.h"

#include <string.h>

static void copy_name(char *dst, const char *src) {
    if (!src) src = "";
    strncpy(dst, src, TAK_MOVEINFO_NAME_MAX - 1);
    dst[TAK_MOVEINFO_NAME_MAX - 1] = '\0';
}

int TAK_MoveInfo_Load(MoveInfoTable *out, const char *vfs_path) {
    if (!out || !vfs_path) return -1;
    memset(out, 0, sizeof(*out));

    TDFFile *tdf = TDF_Open(vfs_path);
    if (!tdf || TDF_Load(tdf) != 0) {
        if (tdf) TDF_Close(tdf);
        return -1;
    }

    const char *section = TDF_GetFirstSection(tdf);
    while (section && out->count < TAK_MOVEINFO_MAX_CLASSES) {
        if (tak_strnicmp(section, "CLASS", 5) == 0 &&
            TDF_PushSection(tdf, section) == 0) {
            MoveClassDef *mc = &out->classes[out->count];
            copy_name(mc->name, TDF_ReadString(tdf, "Name", ""));
            mc->footprint_x = TDF_ReadInt(tdf, "FootprintX", 0);
            mc->footprint_z = TDF_ReadInt(tdf, "FootprintZ", 0);
            /* Unauthored depth bounds are effectively unbounded, not 0
             * (legacy:187374 seeds the class with 10000 before the TDF
             * read). WATER2-5 and HOVER2/3 author only MinWaterDepth,
             * so a 0 default landlocked every ship and hovercraft. */
            mc->max_water_depth = TDF_ReadInt(tdf, "MaxWaterDepth", 10000);
            mc->min_water_depth = TDF_ReadInt(tdf, "MinWaterDepth", 0);
            mc->bad_max_water_depth = TDF_ReadInt(tdf, "BadMaxWaterDepth",
                                                  mc->max_water_depth);
            mc->bad_min_water_depth = TDF_ReadInt(tdf, "BadMinWaterDepth",
                                                  mc->min_water_depth);
            mc->max_slope = TDF_ReadInt(tdf, "MaxSlope", 0);
            mc->bad_slope = TDF_ReadInt(tdf, "BadSlope", mc->max_slope >> 1);
            mc->max_water_slope = TDF_ReadInt(tdf, "MaxWaterSlope", 0);
            mc->bad_water_slope = TDF_ReadInt(tdf, "BadWaterSlope",
                                              mc->max_water_slope >> 1);
            if (mc->name[0]) out->count++;
            TDF_PopSection(tdf);
        }
        section = TDF_GetNextSection(tdf);
    }

    TDF_Close(tdf);
    return out->count > 0 ? 0 : -1;
}

const MoveClassDef *TAK_MoveInfo_Find(const MoveInfoTable *table,
                                      const char *name) {
    if (!table || !name || !name[0]) return NULL;
    for (int i = 0; i < table->count; i++) {
        if (tak_stricmp(table->classes[i].name, name) == 0)
            return &table->classes[i];
    }
    return NULL;
}
