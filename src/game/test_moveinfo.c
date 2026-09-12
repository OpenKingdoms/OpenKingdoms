#include "tak_moveinfo.h"
#include "tak_hpi.h"

#include <stdio.h>

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#define ASSERT(x) do { \
    if (!(x)) { \
        fprintf(stderr, "ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_INT(exp, got) do { \
    int _e = (exp); \
    int _g = (got); \
    if (_e != _g) { \
        fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: expected %d got %d\n", \
                __FILE__, __LINE__, _e, _g); \
        return 1; \
    } \
} while (0)

static int setup_vfs(void) {
    VFS_Shutdown();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) return -1;
    return 0;
}

static int test_base_moveinfo(void) {
    MoveInfoTable table;
    ASSERT_EQ_INT(0, TAK_MoveInfo_Load(&table, "data/gamedata/moveinfo.tdf"));
    ASSERT(table.count >= 10);
    const MoveClassDef *ground2 = TAK_MoveInfo_Find(&table, "GROUND2");
    ASSERT(ground2 != NULL);
    ASSERT_EQ_INT(2, ground2->footprint_x);
    ASSERT_EQ_INT(2, ground2->footprint_z);
    ASSERT_EQ_INT(20, ground2->max_water_depth);
    ASSERT_EQ_INT(30, ground2->max_slope);
    ASSERT_EQ_INT(15, ground2->bad_slope);

    const MoveClassDef *water3 = TAK_MoveInfo_Find(&table, "WATER3");
    ASSERT(water3 != NULL);
    ASSERT_EQ_INT(14, water3->min_water_depth);
    ASSERT_EQ_INT(44, water3->bad_min_water_depth);
    /* Unauthored MaxWaterDepth = unbounded (legacy:187374), or ships
     * and hovercraft cannot legally stand in any water. */
    ASSERT_EQ_INT(10000, water3->max_water_depth);
    const MoveClassDef *hover2 = TAK_MoveInfo_Find(&table, "HOVER2");
    ASSERT(hover2 != NULL);
    ASSERT_EQ_INT(10000, hover2->max_water_depth);
    return 0;
}

static int test_expansion_moveinfo(void) {
    MoveInfoTable table;
    ASSERT_EQ_INT(0, TAK_MoveInfo_Load(&table, "V3Rocket/GameData/MOVEINFO.TDF"));
    ASSERT(table.count >= 10);
    const MoveClassDef *ground5 = TAK_MoveInfo_Find(&table, "GROUND5");
    ASSERT(ground5 != NULL);
    ASSERT_EQ_INT(4, ground5->footprint_x);
    ASSERT_EQ_INT(4, ground5->footprint_z);
    ASSERT_EQ_INT(10, ground5->max_water_depth);
    return 0;
}

int main(void) {
    if (setup_vfs() != 0) {
        /* Nothing was checked, so this run does not report success. */
        printf("test_moveinfo: SKIP, the VFS would not open\n");
        return 1;
    }
    if (test_base_moveinfo() != 0) return 1;
    if (test_expansion_moveinfo() != 0) return 1;
    VFS_Shutdown();
    puts("test_moveinfo: ok");
    return 0;
}
