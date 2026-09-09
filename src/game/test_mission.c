#include "test_framework.h"
#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_mission.h"
#include "tak_tdf.h"
#include "tak_util.h"

#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static int vfs_ready = 0;

static void ensure_vfs(void) {
    if (!vfs_ready) {
        tak_mem_init();
        ASSERT_EQ_INT(0, VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR));
        vfs_ready = 1;
    }
}

static int has_prefix_ci(const char *path, const char *prefix) {
    size_t n;
    if (!path || !prefix) return 0;
    n = strlen(prefix);
    return tak_strnicmp(path, prefix, n) == 0;
}

typedef struct UnitNameSet {
    char **names;
    int count;
} UnitNameSet;

static void free_unit_names(UnitNameSet *set) {
    if (!set || !set->names) return;
    for (int i = 0; i < set->count; i++) tak_free(set->names[i]);
    tak_free(set->names);
    set->names = NULL;
    set->count = 0;
}

static int unit_name_exists(const UnitNameSet *set, const char *name) {
    if (!set || !name || !*name) return 0;
    for (int i = 0; i < set->count; i++) {
        if (tak_stricmp(set->names[i], name) == 0) return 1;
    }
    return 0;
}

static int add_name(UnitNameSet *set, const char *name) {
    char **bigger;
    if (!set || !name || !*name) return 0;
    if (unit_name_exists(set, name)) return 0;
    bigger = (char **)tak_realloc(set->names,
                                  (size_t)(set->count + 1) * sizeof(char *));
    if (!bigger) return -1;
    set->names = bigger;
    set->names[set->count] = tak_strdup(name);
    if (!set->names[set->count]) return -1;
    set->count++;
    return 0;
}

static int load_unit_names(UnitNameSet *set) {
    char **paths = NULL;
    int count = 0;
    int out_count = 0;

    memset(set, 0, sizeof(*set));
    if (VFS_ListFiles("*.fbi", &paths, &count) != 0) return -1;

    set->names = (char **)tak_malloc((size_t)count * sizeof(char *));
    if (!set->names) {
        for (int i = 0; i < count; i++) tak_free(paths[i]);
        tak_free(paths);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (has_prefix_ci(paths[i], "units/")) {
            TDFFile *tdf = TDF_Open(paths[i]);
            if (tdf && TDF_Load(tdf) == 0 &&
                TDF_PushSection(tdf, "UNITINFO") == 0) {
                const char *unitname = TDF_ReadString(tdf, "unitname", "");
                if (unitname && *unitname) {
                    set->names[out_count] = tak_strdup(unitname);
                    if (set->names[out_count]) out_count++;
                }
            }
            if (tdf) TDF_Close(tdf);
        }
        tak_free(paths[i]);
    }
    tak_free(paths);
    set->count = out_count;
    return out_count > 0 ? 0 : -1;
}

static int objective_references_unit(const MissionObjective *obj) {
    switch (obj->type) {
    case MISSION_OBJ_ALL_UNITS_KILLED_OF_TYPE:
    case MISSION_OBJ_KILL_ALL_OF_TYPE:
    case MISSION_OBJ_KILL_UNIT_TYPE:
    case MISSION_OBJ_MOVE_UNIT_TO_RADIUS:
    case MISSION_OBJ_UNIT_TYPE_KILLED:
    case MISSION_OBJ_UNIT_TYPE_PASSES_X:
    case MISSION_OBJ_UNIT_TYPE_PASSES_Z:
        return 1;
    default:
        return 0;
    }
}

static int command_has_valid_refs(const MissionCommand *cmd,
                                  const UnitNameSet *units,
                                  const UnitNameSet *idents,
                                  int max_x,
                                  int max_z) {
    if (!cmd) return 0;
    switch (cmd->type) {
    case MISSION_CMD_WAIT:
        return cmd->a >= 0 && cmd->b >= 0;
    case MISSION_CMD_WAIT_ANIMATION:
        return !cmd->text[0] || unit_name_exists(idents, cmd->text);
    case MISSION_CMD_MOVE:
    case MISSION_CMD_PATROL:
    case MISSION_CMD_UNLOAD:
        return cmd->a >= 0 && cmd->b >= 0 &&
               cmd->a < max_x && cmd->b < max_z;
    case MISSION_CMD_ATTACK:
        if (cmd->text[0]) {
            return unit_name_exists(units, cmd->text) ||
                   unit_name_exists(idents, cmd->text);
        }
        return cmd->a >= 0 && cmd->b >= 0 &&
               cmd->a < max_x && cmd->b < max_z;
    case MISSION_CMD_BUILD:
        if (!cmd->text[0] || !unit_name_exists(units, cmd->text)) {
            /* Several shipped campaign OTAs contain truncated legacy build
               commands. The parser must preserve them, but the runtime should
               treat them as no-ops instead of rejecting the corpus. */
            return 1;
        }
        return cmd->b >= 0 && cmd->c >= 0 &&
               cmd->b < max_x && cmd->c < max_z;
    case MISSION_CMD_IDENT:
        return cmd->text[0] != '\0';
    case MISSION_CMD_OWNER:
        return cmd->a >= 0 && cmd->a <= 10;
    case MISSION_CMD_CLOAK:
    case MISSION_CMD_DEFEND:
    case MISSION_CMD_STOP:
    case MISSION_CMD_SPEED:
        return 1;
    default:
        return 0;
    }
}

TEST(loads_known_campaign_mission) {
    MissionData mission;
    ensure_vfs();
    ASSERT_EQ_INT(0, Mission_LoadOTA("missions/missions/takmission01_mt.ota",
                                     &mission));
    ASSERT_EQ_STR("Aramon", mission.kingdom);
    ASSERT_EQ_INT(6, mission.size_x);
    ASSERT_EQ_INT(6, mission.size_y);
    ASSERT_EQ_INT(1, mission.objective_count);
    ASSERT_EQ_INT(MISSION_OBJ_MOVE_UNIT_TO_RADIUS, mission.objectives[0].type);
    ASSERT_EQ_STR("NPCEMEN", mission.objectives[0].text);
    ASSERT_EQ_INT(130, mission.objectives[0].a);
    ASSERT_EQ_INT(76, mission.objectives[0].b);
    ASSERT_EQ_INT(15, mission.objectives[0].c);
    ASSERT(mission.placement_count > 0);
    ASSERT(mission.placements[0].unitname[0] != '\0');
    ASSERT(mission.placements[0].player >= 0);
    ASSERT(mission.placements[0].x >= 0);
    ASSERT(mission.placements[0].z >= 0);
    Mission_Free(&mission);
}

TEST(missing_mission_fails_cleanly) {
    MissionData mission;
    ensure_vfs();
    ASSERT(Mission_LoadOTA("missions/missions/not_a_real_mission.ota",
                           &mission) != 0);
    ASSERT_NULL(mission.placements);
    ASSERT_EQ_INT(0, mission.placement_count);
}

TEST(parses_initial_mission_command_language) {
    MissionCommand *commands = NULL;
    int count = 0;
    ASSERT_EQ_INT(0, Mission_ParseInitialMission(
        "w 1800 30, a NPCBEG, p 52 21, b ARALODE 0 124 222, wa, c,;",
        &commands, &count));
    ASSERT_EQ_INT(6, count);
    ASSERT_EQ_INT(MISSION_CMD_WAIT, commands[0].type);
    ASSERT_EQ_INT(1800, commands[0].a);
    ASSERT_EQ_INT(30, commands[0].b);
    ASSERT_EQ_INT(MISSION_CMD_ATTACK, commands[1].type);
    ASSERT_EQ_STR("NPCBEG", commands[1].text);
    ASSERT_EQ_INT(MISSION_CMD_PATROL, commands[2].type);
    ASSERT_EQ_INT(52, commands[2].a);
    ASSERT_EQ_INT(21, commands[2].b);
    ASSERT_EQ_INT(MISSION_CMD_BUILD, commands[3].type);
    ASSERT_EQ_STR("ARALODE", commands[3].text);
    ASSERT_EQ_INT(0, commands[3].a);
    ASSERT_EQ_INT(124, commands[3].b);
    ASSERT_EQ_INT(222, commands[3].c);
    ASSERT_EQ_INT(MISSION_CMD_WAIT_ANIMATION, commands[4].type);
    ASSERT_EQ_INT(MISSION_CMD_CLOAK, commands[5].type);
    Mission_FreeCommands(commands);
}

TEST(campaign_corpus_placements_parse) {
    char **paths = NULL;
    int count = 0;
    int files = 0;
    int placements = 0;
    int objectives = 0;
    int commands = 0;
    int bad = 0;
    UnitNameSet unit_names;

    ensure_vfs();
    ASSERT_EQ_INT(0, load_unit_names(&unit_names));
    ASSERT_EQ_INT(0, VFS_ListFiles("*.ota", &paths, &count));
    for (int i = 0; i < count; i++) {
        if (!has_prefix_ci(paths[i], "missions/missions/")) {
            tak_free(paths[i]);
            continue;
        }

        MissionData mission;
        files++;
        if (Mission_LoadOTA(paths[i], &mission) != 0) {
            bad++;
        } else {
            UnitNameSet idents;
            memset(&idents, 0, sizeof(idents));
            placements += mission.placement_count;
            objectives += mission.objective_count;
            if (mission.size_x <= 0 || mission.size_y <= 0) bad++;
            for (int j = 0; j < mission.placement_count; j++) {
                if (mission.placements[j].ident[0]) {
                    if (add_name(&idents, mission.placements[j].ident) != 0) bad++;
                }
            }
            for (int j = 0; j < mission.objective_count; j++) {
                const MissionObjective *obj = &mission.objectives[j];
                if (obj->type == MISSION_OBJ_UNKNOWN || !obj->key[0]) bad++;
                if (objective_references_unit(obj) &&
                    !unit_name_exists(&unit_names, obj->text)) bad++;
            }
            for (int j = 0; j < mission.placement_count; j++) {
                const MissionPlacement *p = &mission.placements[j];
                int max_x = mission.size_x * 32;
                int max_z = mission.size_y * 32;
                if (!p->unitname[0]) bad++;
                if (p->player < 0 || p->player > 10) bad++;
                if (p->x < 0 || p->z < 0 || p->x >= max_x || p->z >= max_z) bad++;
                commands += p->command_count;
                for (int k = 0; k < p->command_count; k++) {
                    if (p->commands[k].type == MISSION_CMD_UNKNOWN) bad++;
                    if (!command_has_valid_refs(&p->commands[k], &unit_names,
                                                &idents, max_x, max_z)) {
                        bad++;
                    }
                }
            }
            free_unit_names(&idents);
            Mission_Free(&mission);
        }
        tak_free(paths[i]);
    }
    tak_free(paths);
    free_unit_names(&unit_names);

    ASSERT_EQ_INT(48, files);
    ASSERT_EQ_INT(4239, placements);
    ASSERT_EQ_INT(78, objectives);
    ASSERT(commands > 2500);
    ASSERT_EQ_INT(0, bad);
}

TEST(evaluates_objective_vocabulary) {
    MissionUnitSnapshot units[4];
    MissionObjective obj;

    memset(units, 0, sizeof(units));
    strcpy(units[0].unitname, "ARAKING");
    units[0].player = 1;
    units[0].x = 42;
    units[0].z = 40;
    units[0].alive = 1;
    units[0].mobile = 1;
    units[0].commander = 1;
    strcpy(units[1].unitname, "TARKING");
    units[1].player = 2;
    units[1].x = 100;
    units[1].z = 70;
    units[1].alive = 1;
    units[1].mobile = 1;
    units[1].commander = 1;
    strcpy(units[2].unitname, "NPCEMEN");
    units[2].player = 1;
    units[2].x = 130;
    units[2].z = 80;
    units[2].alive = 1;
    units[2].mobile = 1;
    strcpy(units[3].unitname, "TARSOLD");
    units[3].player = 2;
    units[3].x = 140;
    units[3].z = 80;
    units[3].alive = 0;
    units[3].mobile = 1;

    memset(&obj, 0, sizeof(obj));
    obj.type = MISSION_OBJ_MOVE_UNIT_TO_RADIUS;
    strcpy(obj.text, "NPCEMEN");
    obj.a = 130;
    obj.b = 76;
    obj.c = 15;
    ASSERT(Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));

    obj.type = MISSION_OBJ_UNIT_TYPE_KILLED;
    strcpy(obj.text, "TARSOLD");
    ASSERT(Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));

    obj.type = MISSION_OBJ_KILL_ENEMY_COMMANDER;
    strcpy(obj.text, "TARKING");
    ASSERT(!Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));
    units[1].alive = 0;
    ASSERT(Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));

    obj.type = MISSION_OBJ_DESTROY_ALL_UNITS;
    ASSERT(Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));

    obj.type = MISSION_OBJ_UNIT_TYPE_PASSES_X;
    strcpy(obj.text, "NPCEMEN");
    obj.a = 120;
    ASSERT(Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));
    obj.type = MISSION_OBJ_UNIT_TYPE_PASSES_Z;
    obj.a = 90;
    ASSERT(!Mission_ObjectiveSatisfied(&obj, units, 4, 1, 0));

    obj.type = MISSION_OBJ_VICTORY_TIMER_RUNS_OUT;
    obj.a = 30;
    ASSERT(!Mission_ObjectiveSatisfied(&obj, units, 4, 1, 29));
    ASSERT(Mission_ObjectiveSatisfied(&obj, units, 4, 1, 30));
}

TEST(evaluates_all_objectives) {
    MissionData mission;
    MissionUnitSnapshot units[1];

    memset(&mission, 0, sizeof(mission));
    mission.objectives = (MissionObjective *)tak_malloc(2 * sizeof(MissionObjective));
    ASSERT_NOT_NULL(mission.objectives);
    mission.objective_count = 2;
    memset(mission.objectives, 0, 2 * sizeof(MissionObjective));
    mission.objectives[0].type = MISSION_OBJ_MOVE_UNIT_TO_RADIUS;
    strcpy(mission.objectives[0].text, "NPCEMEN");
    mission.objectives[0].a = 130;
    mission.objectives[0].b = 76;
    mission.objectives[0].c = 15;
    mission.objectives[1].type = MISSION_OBJ_VICTORY_TIMER_RUNS_OUT;
    mission.objectives[1].a = 10;

    memset(units, 0, sizeof(units));
    strcpy(units[0].unitname, "NPCEMEN");
    units[0].player = 1;
    units[0].x = 130;
    units[0].z = 76;
    units[0].alive = 1;
    units[0].mobile = 1;

    ASSERT(!Mission_AllObjectivesSatisfied(&mission, units, 1, 1, 9));
    ASSERT(Mission_AllObjectivesSatisfied(&mission, units, 1, 1, 10));
    Mission_Free(&mission);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    TEST_SUITE("Mission OTA parser");
    RUN(loads_known_campaign_mission);
    RUN(missing_mission_fails_cleanly);
    RUN(parses_initial_mission_command_language);
    RUN(campaign_corpus_placements_parse);
    RUN(evaluates_objective_vocabulary);
    RUN(evaluates_all_objectives);

    if (vfs_ready) VFS_Shutdown();
    tak_mem_shutdown();

    TEST_REPORT();
}
