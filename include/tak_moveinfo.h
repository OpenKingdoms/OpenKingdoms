#ifndef TAK_MOVEINFO_H
#define TAK_MOVEINFO_H

#include <stdint.h>

#define TAK_MOVEINFO_NAME_MAX 32
#define TAK_MOVEINFO_MAX_CLASSES 64

typedef struct MoveClassDef {
    char    name[TAK_MOVEINFO_NAME_MAX];
    int32_t footprint_x;
    int32_t footprint_z;
    int32_t min_water_depth;
    int32_t max_water_depth;
    int32_t bad_min_water_depth;
    int32_t bad_max_water_depth;
    int32_t max_slope;
    int32_t bad_slope;
    int32_t max_water_slope;
    int32_t bad_water_slope;
} MoveClassDef;

typedef struct MoveInfoTable {
    int count;
    MoveClassDef classes[TAK_MOVEINFO_MAX_CLASSES];
} MoveInfoTable;

int TAK_MoveInfo_Load(MoveInfoTable *out, const char *vfs_path);
const MoveClassDef *TAK_MoveInfo_Find(const MoveInfoTable *table,
                                      const char *name);

#endif /* TAK_MOVEINFO_H */
