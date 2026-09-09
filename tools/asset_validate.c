#include "tak_hpi.h"
#include "tak_memory.h"
#include "tak_tdf.h"
#include "tak_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void lowercase_into(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    while (i + 1 < cap && src[i]) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

static void strip_3do_suffix(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    if (n > 4 && tak_stricmp(s + n - 4, ".3do") == 0) {
        s[n - 4] = '\0';
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

static int parse_size_text(const char *text, int *out_x, int *out_y) {
    int x = 0, y = 0;
    if (!text) return -1;
    if (sscanf(text, " %d x %d", &x, &y) != 2) return -1;
    if (x <= 0 || y <= 0) return -1;
    *out_x = x;
    *out_y = y;
    return 0;
}

static void read_token(const char **p, char *out, size_t out_cap) {
    size_t n = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p && **p != ' ' && **p != '\t' && **p != ',' && **p != ';') {
        if (n + 1 < out_cap) out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
}

static int validate_initial_mission_refs(const UnitNameSet *units,
                                         const UnitNameSet *idents,
                                         const char *mission,
                                         const char *path,
                                         const char *section,
                                         int *out_refs) {
    int failures = 0;
    const char *p = mission ? mission : "";
    if (out_refs) *out_refs = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == 'a' || *p == 'A') {
            const char *q = p + 1;
            char token[64];
            read_token(&q, token, sizeof(token));
            if (token[0] && (unit_name_exists(units, token) ||
                             unit_name_exists(idents, token))) {
                if (out_refs) (*out_refs)++;
            } else if (token[0] && token[0] >= 'A' && token[0] <= 'Z') {
                fprintf(stderr, "FAIL %s:%s InitialMission unknown unit ref '%s' in '%s'\n",
                        path, section, token, mission);
                failures++;
            }
            p = q;
        } else {
            while (*p && *p != ',') p++;
        }
    }
    return failures;
}

static int validate_units(void) {
    char **paths = NULL;
    int count = 0;
    int checked = 0;
    int failures = 0;
    int cob_present = 0;
    int cob_missing = 0;
    int radar_fields = 0;
    int canfly_fields = 0;
    int floater_fields = 0;
    int waterline_fields = 0;
    int movement_class_fields = 0;
    int weapon_flag_fields = 0;
    int healtime_fields = 0;
    int transport_fields = 0;
    int weapon_burst_fields = 0;
    int weapon_spray_fields = 0;
    int weapon_edge_fields = 0;
    int damage_category_fields = 0;
    int weapon_damage_scale_fields = 0;

    if (VFS_ListFiles("*.fbi", &paths, &count) != 0) {
        fprintf(stderr, "asset_validate: VFS_ListFiles('*.fbi') failed\n");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        if (!has_prefix_ci(paths[i], "units/")) {
            tak_free(paths[i]);
            continue;
        }

        TDFFile *tdf = TDF_Open(paths[i]);
        char unitname[64];
        char objectname[64];
        char obj_lc[64];
        char path[160];

        checked++;
        if (!tdf || TDF_Load(tdf) != 0 || TDF_PushSection(tdf, "UNITINFO") != 0) {
            fprintf(stderr, "FAIL %s: parse/UNITINFO failed\n", paths[i]);
            failures++;
            if (tdf) TDF_Close(tdf);
            tak_free(paths[i]);
            continue;
        }

        copy_str(unitname, sizeof(unitname), TDF_ReadString(tdf, "unitname", ""));
        copy_str(objectname, sizeof(objectname), TDF_ReadString(tdf, "objectname", ""));
        int radar = TDF_ReadInt(tdf, "radardistance", 0);
        if (radar < 0) {
            fprintf(stderr, "FAIL %s: negative radardistance %d\n", paths[i], radar);
            failures++;
        } else if (radar > 0) {
            radar_fields++;
        }
        int canfly = TDF_ReadInt(tdf, "canfly", 0);
        if (canfly < 0) {
            fprintf(stderr, "FAIL %s: negative canfly %d\n", paths[i], canfly);
            failures++;
        } else if (canfly > 0) {
            canfly_fields++;
        }
        int floater = TDF_ReadInt(tdf, "floater", 0);
        if (floater < 0) {
            fprintf(stderr, "FAIL %s: negative floater %d\n", paths[i], floater);
            failures++;
        } else if (floater > 0) {
            floater_fields++;
        }
        int waterline = TDF_ReadInt(tdf, "waterline", 0);
        if (waterline < 0) {
            fprintf(stderr, "FAIL %s: negative waterline %d\n",
                    paths[i], waterline);
            failures++;
        } else if (waterline > 0) {
            waterline_fields++;
        }
        if (TDF_ReadString(tdf, "movementclass", "")[0]) {
            movement_class_fields++;
        }
        if (TDF_ReadString(tdf, "damagecategory", "")[0]) {
            damage_category_fields++;
        }
        float healtime = TDF_ReadFloat(tdf, "healtime", 0.0f);
        if (healtime < 0.0f) {
            fprintf(stderr, "FAIL %s: negative healtime %.3f\n",
                    paths[i], healtime);
            failures++;
        } else if (healtime > 0.0f) {
            healtime_fields++;
        }
        const char *transport_names[] = {
            "transportsize", "transportcapacity", "transportsizecapacity",
            "cantbetransported", "transportedsize", "transportdistance"
        };
        for (int tf = 0; tf < 6; tf++) {
            int v = TDF_ReadInt(tdf, transport_names[tf], 0);
            if (v < 0) {
                fprintf(stderr, "FAIL %s: negative %s %d\n",
                        paths[i], transport_names[tf], v);
                failures++;
            } else if (v > 0) {
                transport_fields++;
            }
        }
        const char *move_fields[] = {
            "minwaterdepth", "maxwaterdepth", "badminwaterdepth",
            "badmaxwaterdepth", "maxslope", "badslope",
            "maxwaterslope", "badwaterslope"
        };
        for (int mf = 0; mf < 8; mf++) {
            int v = TDF_ReadInt(tdf, move_fields[mf], 0);
            if (v < 0) {
                fprintf(stderr, "FAIL %s: negative %s %d\n",
                        paths[i], move_fields[mf], v);
                failures++;
            }
        }
        TDF_Close(tdf);

        if (!unitname[0]) {
            fprintf(stderr, "FAIL %s: missing unitname\n", paths[i]);
            failures++;
        }
        if (!objectname[0]) {
            fprintf(stderr, "FAIL %s: missing objectname\n", paths[i]);
            failures++;
        } else {
            lowercase_into(obj_lc, sizeof(obj_lc), objectname);
            snprintf(path, sizeof(path), "objects3d/%s.3do", obj_lc);
            if (VFS_FileExists(path) != 0) {
                fprintf(stderr, "FAIL %s: missing model %s\n", paths[i], path);
                failures++;
            }
        }

        if (unitname[0]) {
            lowercase_into(obj_lc, sizeof(obj_lc), unitname);
            snprintf(path, sizeof(path), "scripts/%s.cob", obj_lc);
            if (VFS_FileExists(path) == 0) cob_present++;
            else cob_missing++;
        }

        tdf = TDF_Open(paths[i]);
        if (tdf && TDF_Load(tdf) == 0) {
            for (int w = 1; w <= 3; w++) {
                char section[16];
                char weapon_model[64];
                snprintf(section, sizeof(section), "WEAPON%d", w);
                if (TDF_PushSection(tdf, section) != 0) continue;
                copy_str(weapon_model, sizeof(weapon_model),
                         TDF_ReadString(tdf, "model", ""));
                const char *flag_names[] = {
                    "waterweapon", "toairweapon", "noairweapon", "noradar"
                };
                for (int fi = 0; fi < 4; fi++) {
                    int flag = TDF_ReadInt(tdf, flag_names[fi], 0);
                    if (flag < 0) {
                        fprintf(stderr, "FAIL %s:%s negative %s=%d\n",
                                paths[i], section, flag_names[fi], flag);
                        failures++;
                    } else if (flag > 0) {
                        weapon_flag_fields++;
                    }
                }
                int burst = TDF_ReadInt(tdf, "burst", 0);
                float burst_rate = TDF_ReadFloat(tdf, "burstrate", 0.0f);
                int spray_angle = TDF_ReadInt(tdf, "sprayangle", 0);
                float edge_eff = TDF_ReadFloat(tdf, "edgeeffectiveness", 0.0f);
                if (burst < 0) {
                    fprintf(stderr, "FAIL %s:%s negative burst=%d\n",
                            paths[i], section, burst);
                    failures++;
                } else if (burst > 0) {
                    weapon_burst_fields++;
                }
                if (burst_rate < 0.0f) {
                    fprintf(stderr, "FAIL %s:%s negative burstrate=%.3f\n",
                            paths[i], section, burst_rate);
                    failures++;
                }
                if (spray_angle < 0) {
                    fprintf(stderr, "FAIL %s:%s negative sprayangle=%d\n",
                            paths[i], section, spray_angle);
                    failures++;
                } else if (spray_angle > 0) {
                    weapon_spray_fields++;
                }
                if (edge_eff < 0.0f || edge_eff > 1.0f) {
                    fprintf(stderr, "FAIL %s:%s edgeeffectiveness out of range %.3f\n",
                            paths[i], section, edge_eff);
                    failures++;
                } else if (edge_eff > 0.0f) {
                    weapon_edge_fields++;
                }
                if (TDF_PushSection(tdf, "DAMAGE") == 0) {
                    const char *key = TDF_GetFirstKey(tdf);
                    while (key) {
                        if (tak_stricmp(key, "default") != 0) {
                            float scale = TDF_ReadFloat(tdf, key, 1.0f);
                            if (scale < 0.0f) {
                                fprintf(stderr,
                                        "FAIL %s:%s[DAMAGE] negative %s=%.3f\n",
                                        paths[i], section, key, scale);
                                failures++;
                            } else {
                                weapon_damage_scale_fields++;
                            }
                        }
                        key = TDF_GetNextKey(tdf);
                    }
                    TDF_PopSection(tdf);
                }
                TDF_PopSection(tdf);
                if (!weapon_model[0]) continue;

                strip_3do_suffix(weapon_model);
                lowercase_into(obj_lc, sizeof(obj_lc), weapon_model);
                snprintf(path, sizeof(path), "objects3d/%s.3do", obj_lc);
                if (VFS_FileExists(path) != 0) {
                    fprintf(stderr, "FAIL %s: missing weapon model %s\n",
                            paths[i], path);
                    failures++;
                }
            }
        }
        if (tdf) TDF_Close(tdf);

        tak_free(paths[i]);
    }
    tak_free(paths);

    printf("asset_validate units: checked=%d failures=%d cob_present=%d cob_missing=%d radar_fields=%d canfly=%d floater=%d waterline=%d movementclass=%d damagecategory=%d healtime=%d transport=%d weapon_flags=%d weapon_burst=%d weapon_spray=%d weapon_edge=%d weapon_damage_scales=%d\n",
           checked, failures, cob_present, cob_missing, radar_fields,
           canfly_fields, floater_fields, waterline_fields,
           movement_class_fields, damage_category_fields,
           healtime_fields, transport_fields,
           weapon_flag_fields, weapon_burst_fields, weapon_spray_fields,
           weapon_edge_fields, weapon_damage_scale_fields);
    if (checked == 0) return 2;
    return failures == 0 ? 0 : 1;
}

static int validate_mission_units(void) {
    UnitNameSet units;
    char **paths = NULL;
    int count = 0;
    int checked_files = 0;
    int checked_units = 0;
    int initial_refs = 0;
    int failures = 0;

    if (load_unit_names(&units) != 0) {
        fprintf(stderr, "asset_validate: could not load unit name set\n");
        return 1;
    }

    if (VFS_ListFiles("*.ota", &paths, &count) != 0) {
        free_unit_names(&units);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        if (!has_prefix_ci(paths[i], "missions/missions/")) {
            tak_free(paths[i]);
            continue;
        }

        TDFFile *tdf = TDF_Open(paths[i]);
        UnitNameSet idents;
        int size_x = 0, size_y = 0;
        int max_x = 0, max_z = 0;
        checked_files++;

        memset(&idents, 0, sizeof(idents));

        if (!tdf || TDF_Load(tdf) != 0 ||
            TDF_PushSection(tdf, "GlobalHeader") != 0) {
            fprintf(stderr, "FAIL %s: parse/GlobalHeader failed\n", paths[i]);
            failures++;
            if (tdf) TDF_Close(tdf);
            tak_free(paths[i]);
            continue;
        }

        parse_size_text(TDF_ReadString(tdf, "size", ""), &size_x, &size_y);
        max_x = size_x > 0 ? size_x * 32 : 0;
        max_z = size_y > 0 ? size_y * 32 : 0;

        if (TDF_PushSection(tdf, "Map Data") == 0 &&
            TDF_PushSection(tdf, "units") == 0) {
            const char *section = TDF_GetFirstSection(tdf);
            while (section) {
                if (TDF_PushSection(tdf, section) == 0) {
                    const char *ident = TDF_ReadString(tdf, "Ident", "");
                    if (ident && *ident) add_name(&idents, ident);
                    TDF_PopSection(tdf);
                }
                section = TDF_GetNextSection(tdf);
            }

            section = TDF_GetFirstSection(tdf);
            while (section) {
                if (TDF_PushSection(tdf, section) == 0) {
                    const char *unitname = TDF_ReadString(tdf, "Unitname", "");
                    const char *initial = TDF_ReadString(tdf, "InitialMission", "");
                    int x = TDF_ReadInt(tdf, "XPos", -1);
                    int z = TDF_ReadInt(tdf, "ZPos", -1);
                    int player = TDF_ReadInt(tdf, "Player", -1);
                    int refs = 0;

                    checked_units++;
                    if (!unit_name_exists(&units, unitname)) {
                        fprintf(stderr, "FAIL %s:%s unknown Unitname=%s\n",
                                paths[i], section, unitname ? unitname : "");
                        failures++;
                    }
                    if (player < 0 || player > 10) {
                        fprintf(stderr, "FAIL %s:%s invalid Player=%d\n",
                                paths[i], section, player);
                        failures++;
                    }
                    if (x < 0 || z < 0 ||
                        (max_x > 0 && x >= max_x) ||
                        (max_z > 0 && z >= max_z)) {
                        fprintf(stderr, "FAIL %s:%s position out of size bucket x=%d z=%d limit=%dx%d\n",
                                paths[i], section, x, z, max_x, max_z);
                        failures++;
                    }
                    failures += validate_initial_mission_refs(&units, &idents, initial,
                                                             paths[i], section,
                                                             &refs);
                    initial_refs += refs;
                    TDF_PopSection(tdf);
                }
                section = TDF_GetNextSection(tdf);
            }
        }

        TDF_Close(tdf);
        free_unit_names(&idents);
        tak_free(paths[i]);
    }
    tak_free(paths);
    free_unit_names(&units);

    printf("asset_validate mission-units: files=%d units=%d initial_refs=%d failures=%d\n",
           checked_files, checked_units, initial_refs, failures);
    if (checked_files != 48 || checked_units == 0) return 2;
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int rc;
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "asset_validate: VFS init failed\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--units") == 0) {
        rc = validate_units();
        VFS_Shutdown();
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "--mission-units") == 0) {
        rc = validate_mission_units();
        VFS_Shutdown();
        return rc;
    }

    fprintf(stderr, "usage: %s --units | --mission-units\n", argv[0]);
    VFS_Shutdown();
    return 1;
}
