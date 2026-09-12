#include "tak_mission.h"
#include "tak_battle_config.h"
#include "tak_memory.h"
#include "tak_tdf.h"
#include "tak_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int parse_size_text(const char *text, int *out_x, int *out_y) {
    int x = 0, y = 0;
    if (!text || !out_x || !out_y) return -1;
    if (sscanf(text, " %d x %d", &x, &y) != 2) return -1;
    if (x <= 0 || y <= 0) return -1;
    *out_x = x;
    *out_y = y;
    return 0;
}

static int append_placement(MissionData *mission,
                            int *capacity,
                            const MissionPlacement *placement) {
    MissionPlacement *bigger;
    if (!mission || !capacity || !placement) return -1;
    if (mission->placement_count >= *capacity) {
        int next = (*capacity == 0) ? 64 : (*capacity * 2);
        bigger = (MissionPlacement *)tak_realloc(
            mission->placements, (size_t)next * sizeof(MissionPlacement));
        if (!bigger) return -1;
        mission->placements = bigger;
        *capacity = next;
    }
    mission->placements[mission->placement_count++] = *placement;
    return 0;
}

static int append_objective(MissionData *mission,
                            int *capacity,
                            const MissionObjective *objective) {
    MissionObjective *bigger;
    if (!mission || !capacity || !objective) return -1;
    if (mission->objective_count >= *capacity) {
        int next = (*capacity == 0) ? 8 : (*capacity * 2);
        bigger = (MissionObjective *)tak_realloc(
            mission->objectives, (size_t)next * sizeof(MissionObjective));
        if (!bigger) return -1;
        mission->objectives = bigger;
        *capacity = next;
    }
    mission->objectives[mission->objective_count++] = *objective;
    return 0;
}

static void skip_delims(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == ',' || **p == ';') (*p)++;
}

static int is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

static int is_number_token(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!is_digit_char(*s)) return 0;
    while (*s) {
        if (!is_digit_char(*s)) return 0;
        s++;
    }
    return 1;
}

static int mission_unit_matches(const MissionUnitSnapshot *u,
                                const char *unitname) {
    if (!u || !unitname || !*unitname) return 0;
    return tak_stricmp(u->unitname, unitname) == 0;
}

static int mission_unit_is_enemy(const MissionUnitSnapshot *u,
                                 int local_player) {
    return u && u->player > 0 && u->player != local_player;
}

static int any_matching_dead(const MissionUnitSnapshot *units,
                             int unit_count,
                             const char *unitname) {
    int seen = 0;
    for (int i = 0; i < unit_count; i++) {
        if (!mission_unit_matches(&units[i], unitname)) continue;
        seen = 1;
        if (!units[i].alive) return 1;
    }
    return !seen ? 0 : 0;
}

static int no_matching_alive(const MissionUnitSnapshot *units,
                             int unit_count,
                             const char *unitname,
                             int local_player,
                             int enemy_only,
                             int mobile_only,
                             int commander_only) {
    int seen = 0;
    for (int i = 0; i < unit_count; i++) {
        const MissionUnitSnapshot *u = &units[i];
        if (unitname && *unitname && !mission_unit_matches(u, unitname)) continue;
        if (enemy_only && !mission_unit_is_enemy(u, local_player)) continue;
        if (mobile_only && !u->mobile) continue;
        if (commander_only && !u->commander) continue;
        seen = 1;
        if (u->alive) return 0;
    }
    return seen;
}

static int any_matching_inside_radius(const MissionUnitSnapshot *units,
                                      int unit_count,
                                      const char *unitname,
                                      int x,
                                      int z,
                                      int radius) {
    int64_t rr = (int64_t)radius * (int64_t)radius;
    for (int i = 0; i < unit_count; i++) {
        const MissionUnitSnapshot *u = &units[i];
        int64_t dx, dz;
        if (!u->alive || !mission_unit_matches(u, unitname)) continue;
        dx = (int64_t)u->x - (int64_t)x;
        dz = (int64_t)u->z - (int64_t)z;
        if (dx * dx + dz * dz <= rr) return 1;
    }
    return 0;
}

static void read_token(const char **p, char *out, size_t out_cap) {
    size_t n = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p && **p != ' ' && **p != '\t' &&
           **p != ',' && **p != ';') {
        if (n + 1 < out_cap) out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
}

static int read_int_arg(const char **p, int *out) {
    char token[32];
    read_token(p, token, sizeof(token));
    if (!is_number_token(token)) return -1;
    *out = (int)strtol(token, NULL, 10);
    return 0;
}

static int next_nonspace_is_number(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return is_digit_char(*p) || ((*p == '-' || *p == '+') && is_digit_char(p[1]));
}

static int next_nonspace_is_value(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return *p != '\0' && *p != ',' && *p != ';';
}

static int append_command(MissionCommand **commands,
                          int *count,
                          int *capacity,
                          const MissionCommand *cmd) {
    MissionCommand *bigger;
    if (*count >= *capacity) {
        int next = (*capacity == 0) ? 4 : (*capacity * 2);
        bigger = (MissionCommand *)tak_realloc(
            *commands, (size_t)next * sizeof(MissionCommand));
        if (!bigger) return -1;
        *commands = bigger;
        *capacity = next;
    }
    (*commands)[(*count)++] = *cmd;
    return 0;
}

int Mission_ParseInitialMission(const char *text,
                                MissionCommand **out_commands,
                                int *out_count) {
    const char *p;
    MissionCommand *commands = NULL;
    int count = 0;
    int capacity = 0;

    if (!out_commands || !out_count) return -1;
    *out_commands = NULL;
    *out_count = 0;
    if (!text || !*text) return 0;

    p = text;
    while (*p) {
        MissionCommand cmd;
        char op;
        memset(&cmd, 0, sizeof(cmd));
        skip_delims(&p);
        if (!*p) break;

        op = *p++;
        cmd.token[0] = op;
        cmd.token[1] = '\0';
        if (op >= 'A' && op <= 'Z') op = (char)(op - 'A' + 'a');

        switch (op) {
        case 'w':
            if (*p == 'a' || *p == 'A') {
                p++;
                cmd.type = MISSION_CMD_WAIT_ANIMATION;
                if (next_nonspace_is_value(p)) {
                    read_token(&p, cmd.text, sizeof(cmd.text));
                }
            } else {
                cmd.type = MISSION_CMD_WAIT;
                read_int_arg(&p, &cmd.a);
                if (next_nonspace_is_number(p)) read_int_arg(&p, &cmd.b);
            }
            break;
        case 'm':
            cmd.type = MISSION_CMD_MOVE;
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            break;
        case 'p':
            cmd.type = MISSION_CMD_PATROL;
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            break;
        case 'a': {
            char first[64];
            cmd.type = MISSION_CMD_ATTACK;
            read_token(&p, first, sizeof(first));
            if (is_number_token(first)) {
                cmd.a = (int)strtol(first, NULL, 10);
                read_int_arg(&p, &cmd.b);
            } else {
                copy_str(cmd.text, sizeof(cmd.text), first);
            }
            break;
        }
        case 'b':
            cmd.type = MISSION_CMD_BUILD;
            read_token(&p, cmd.text, sizeof(cmd.text));
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            read_int_arg(&p, &cmd.c);
            break;
        case 'i':
            cmd.type = MISSION_CMD_IDENT;
            read_token(&p, cmd.text, sizeof(cmd.text));
            break;
        case 'o':
            cmd.type = MISSION_CMD_OWNER;
            read_int_arg(&p, &cmd.a);
            break;
        case 'c':
            cmd.type = MISSION_CMD_CLOAK;
            break;
        case 'd':
            cmd.type = MISSION_CMD_DEFEND;
            break;
        case 's':
            cmd.type = MISSION_CMD_STOP;
            break;
        case 'v': {
            char value[32];
            cmd.type = MISSION_CMD_SPEED;
            read_token(&p, value, sizeof(value));
            cmd.value = (float)strtod(value, NULL);
            break;
        }
        case 'u':
            cmd.type = MISSION_CMD_UNLOAD;
            read_int_arg(&p, &cmd.a);
            read_int_arg(&p, &cmd.b);
            break;
        default:
            cmd.type = MISSION_CMD_UNKNOWN;
            read_token(&p, cmd.text, sizeof(cmd.text));
            break;
        }

        if (append_command(&commands, &count, &capacity, &cmd) != 0) {
            Mission_FreeCommands(commands);
            return -1;
        }
    }

    *out_commands = commands;
    *out_count = count;
    return 0;
}

void Mission_FreeCommands(MissionCommand *commands) {
    if (commands) tak_free(commands);
}

int Mission_ObjectiveSatisfied(const MissionObjective *objective,
                               const MissionUnitSnapshot *units,
                               int unit_count,
                               int local_player,
                               int elapsed_seconds) {
    if (!objective || unit_count < 0 || (!units && unit_count > 0)) return 0;
    if (local_player <= 0) local_player = 1;

    switch (objective->type) {
    case MISSION_OBJ_ALL_UNITS_KILLED:
    case MISSION_OBJ_DESTROY_ALL_UNITS:
        return no_matching_alive(units, unit_count, NULL, local_player,
                                 1, 0, 0);
    case MISSION_OBJ_ALL_UNITS_KILLED_OF_TYPE:
    case MISSION_OBJ_KILL_ALL_OF_TYPE:
        return no_matching_alive(units, unit_count, objective->text,
                                 local_player, 0, 0, 0);
    case MISSION_OBJ_KILL_ALL_MOBILE_UNITS:
        return no_matching_alive(units, unit_count, NULL, local_player,
                                 1, 1, 0);
    case MISSION_OBJ_COMMANDER_KILLED:
    case MISSION_OBJ_KILL_ENEMY_COMMANDER:
        return no_matching_alive(units, unit_count, objective->text,
                                 local_player, 1, 0, 1);
    case MISSION_OBJ_KILL_UNIT_TYPE:
    case MISSION_OBJ_UNIT_TYPE_KILLED:
        return any_matching_dead(units, unit_count, objective->text);
    case MISSION_OBJ_MOVE_UNIT_TO_RADIUS:
        return any_matching_inside_radius(units, unit_count, objective->text,
                                          objective->a, objective->b,
                                          objective->c);
    case MISSION_OBJ_UNIT_TYPE_PASSES_X:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_matches(&units[i], objective->text) &&
                units[i].x >= objective->a) return 1;
        }
        return 0;
    case MISSION_OBJ_UNIT_TYPE_PASSES_Z:
        for (int i = 0; i < unit_count; i++) {
            if (units[i].alive &&
                mission_unit_matches(&units[i], objective->text) &&
                units[i].z >= objective->a) return 1;
        }
        return 0;
    case MISSION_OBJ_DEATH_TIMER_RUNS_OUT:
    case MISSION_OBJ_VICTORY_TIMER_RUNS_OUT:
        return elapsed_seconds >= objective->a;
    default:
        return 0;
    }
}

int Mission_AllObjectivesSatisfied(const MissionData *mission,
                                   const MissionUnitSnapshot *units,
                                   int unit_count,
                                   int local_player,
                                   int elapsed_seconds) {
    if (!mission || mission->objective_count <= 0) return 0;
    for (int i = 0; i < mission->objective_count; i++) {
        if (!Mission_ObjectiveSatisfied(&mission->objectives[i], units,
                                        unit_count, local_player,
                                        elapsed_seconds)) {
            return 0;
        }
    }
    return 1;
}

void Mission_Free(MissionData *mission) {
    int i;
    if (!mission) return;
    for (i = 0; i < mission->placement_count; i++) {
        Mission_FreeCommands(mission->placements[i].commands);
    }
    if (mission->placements) tak_free(mission->placements);
    if (mission->objectives) tak_free(mission->objectives);
    memset(mission, 0, sizeof(*mission));
}

void Mission_ApplyVisibility(const MissionData *mission, BattleConfig *cfg) {
    if (!mission || !cfg) return;
    /* A mission keeps line of sight on whatever its lineofsight key says
     * and starts black only when mapping is 1 (legacy:168880-168885).
     * map_revealed is the opposite sense of the file's key. */
    cfg->line_of_sight = 1;
    cfg->map_revealed = (mission->mapping != 1);
}

typedef struct ObjectiveKeySpec {
    const char *key;
    MissionObjectiveType type;
} ObjectiveKeySpec;

static const ObjectiveKeySpec g_objective_keys[] = {
    { "AllUnitsKilled",       MISSION_OBJ_ALL_UNITS_KILLED },
    { "AllUnitsKilledOfType", MISSION_OBJ_ALL_UNITS_KILLED_OF_TYPE },
    { "CommanderKilled",      MISSION_OBJ_COMMANDER_KILLED },
    { "DeathTimerRunsOut",    MISSION_OBJ_DEATH_TIMER_RUNS_OUT },
    { "DestroyAllUnits",      MISSION_OBJ_DESTROY_ALL_UNITS },
    { "KillAllMobileUnits",   MISSION_OBJ_KILL_ALL_MOBILE_UNITS },
    { "KillAllOfType",        MISSION_OBJ_KILL_ALL_OF_TYPE },
    { "KillEnemyCommander",   MISSION_OBJ_KILL_ENEMY_COMMANDER },
    { "KillUnitType",         MISSION_OBJ_KILL_UNIT_TYPE },
    { "MoveUnitToRadius",     MISSION_OBJ_MOVE_UNIT_TO_RADIUS },
    { "UnitTypeKilled",       MISSION_OBJ_UNIT_TYPE_KILLED },
    { "UnitTypePassesX",      MISSION_OBJ_UNIT_TYPE_PASSES_X },
    { "UnitTypePassesZ",      MISSION_OBJ_UNIT_TYPE_PASSES_Z },
    { "VictoryTimerRunsOut",  MISSION_OBJ_VICTORY_TIMER_RUNS_OUT },
};

static void objective_from_value(MissionObjective *obj, const char *value) {
    if (!value || !*value) return;

    /* Values are all compact comma-separated scalars. Parsing locally
     * avoids building a temporary TDF just to split one value. */
    const char *p = value;
    int slot = 0;
    while (*p && slot < 4) {
        char token[64];
        skip_delims(&p);
        read_token(&p, token, sizeof(token));
        if (!token[0]) break;
        if (slot == 0 && !is_number_token(token)) {
            copy_str(obj->text, sizeof(obj->text), token);
        } else {
            int v = is_number_token(token) ? (int)strtol(token, NULL, 10) : 0;
            if (slot == 0) obj->a = v;
            else if (slot == 1) obj->a = v;
            else if (slot == 2) obj->b = v;
            else if (slot == 3) obj->c = v;
        }
        slot++;
    }
}

static int parse_objectives(TDFFile *tdf, MissionData *out) {
    int capacity = 0;
    int n_specs = (int)(sizeof(g_objective_keys) / sizeof(g_objective_keys[0]));
    for (int i = 0; i < n_specs; i++) {
        const ObjectiveKeySpec *spec = &g_objective_keys[i];
        const char *value = TDF_ReadString(tdf, spec->key, NULL);
        if (value) {
            MissionObjective obj;
            memset(&obj, 0, sizeof(obj));
            obj.type = spec->type;
            copy_str(obj.key, sizeof(obj.key), spec->key);
            objective_from_value(&obj, value);
            if (append_objective(out, &capacity, &obj) != 0) return -1;
        }
    }
    return 0;
}

int Mission_LoadOTA(const char *vfs_path, MissionData *out) {
    TDFFile *tdf;
    int capacity = 0;
    int rc = -1;

    if (!vfs_path || !out) return -1;
    memset(out, 0, sizeof(*out));
    copy_str(out->path, sizeof(out->path), vfs_path);

    tdf = TDF_Open(vfs_path);
    if (!tdf || TDF_Load(tdf) != 0) goto done;
    if (TDF_PushSection(tdf, "GlobalHeader") != 0) goto done;

    copy_str(out->mission_name, sizeof(out->mission_name),
             TDF_ReadString(tdf, "MissionName", ""));
    copy_str(out->kingdom, sizeof(out->kingdom),
             TDF_ReadString(tdf, "Kingdom", ""));
    parse_size_text(TDF_ReadString(tdf, "Size", ""), &out->size_x, &out->size_y);
    /* A missing key reads as 0 (legacy:168883). */
    out->mapping = TDF_ReadInt(tdf, "mapping", 0);
    if (parse_objectives(tdf, out) != 0) goto done;

    if (TDF_PushSection(tdf, "Map Data") == 0 &&
        TDF_PushSection(tdf, "units") == 0) {
        const char *section = TDF_GetFirstSection(tdf);
        while (section) {
            if (TDF_PushSection(tdf, section) == 0) {
                MissionPlacement p;
                memset(&p, 0, sizeof(p));
                copy_str(p.section, sizeof(p.section), section);
                copy_str(p.unitname, sizeof(p.unitname),
                         TDF_ReadString(tdf, "Unitname", ""));
                copy_str(p.ident, sizeof(p.ident),
                         TDF_ReadString(tdf, "Ident", ""));
                p.player = TDF_ReadInt(tdf, "Player", -1);
                p.x = TDF_ReadInt(tdf, "XPos", -1);
                p.y = TDF_ReadInt(tdf, "YPos", 0);
                p.z = TDF_ReadInt(tdf, "ZPos", -1);
                p.angle = TDF_ReadInt(tdf, "Angle", 0);
                p.health_percent = TDF_ReadInt(tdf, "HealthPercentage", 100);
                p.mana_percent = TDF_ReadInt(tdf, "ManaPercentage", 100);
                copy_str(p.initial_mission, sizeof(p.initial_mission),
                         TDF_ReadString(tdf, "InitialMission", ""));
                if (Mission_ParseInitialMission(p.initial_mission,
                                                &p.commands,
                                                &p.command_count) != 0) {
                    TDF_PopSection(tdf);
                    goto done;
                }
                if (append_placement(out, &capacity, &p) != 0) {
                    Mission_FreeCommands(p.commands);
                    TDF_PopSection(tdf);
                    goto done;
                }
                TDF_PopSection(tdf);
            }
            section = TDF_GetNextSection(tdf);
        }
        TDF_PopSection(tdf);
        TDF_PopSection(tdf);
    }

    rc = 0;

done:
    if (tdf) TDF_Close(tdf);
    if (rc != 0) Mission_Free(out);
    return rc;
}
