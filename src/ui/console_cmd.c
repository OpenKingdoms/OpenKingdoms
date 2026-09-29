/*
 * console_cmd.c: the console's + commands.
 *
 * The names and what each does come from the original's three command
 * tables (legacy:38938-38946). A command that changes the battle leaves
 * as a TAK_CMD_POWER_CODE, which command_exec.c applies on every
 * machine. A command about this machine alone runs here.
 */

#include "tak_console_cmd.h"

#include "tak_camera.h"
#include "tak_command_emit.h"
#include "tak_commands.h"
#include "tak_music.h"
#include "tak_settings.h"
#include "tak_unit.h"
#include "tak_view_shake.h"
#include "tak_world.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { CK_POWER, CK_OPEN } ConsoleKind;

typedef struct {
    const char *name;
    int         id;
    ConsoleKind kind;
} ConsoleEntry;

/* The original's spelling. Lookup ignores case, as its does. */
static const ConsoleEntry g_table[] = {
    { "ATM",            CONSOLE_CMD_ATM,              CK_POWER },
    { "ManaMe",         CONSOLE_CMD_MANA_ME,          CK_POWER },
    { "NoMana",         CONSOLE_CMD_NO_MANA,          CK_POWER },
    { "NowISee",        CONSOLE_CMD_NOW_I_SEE,        CK_POWER },
    { "LOS",            CONSOLE_CMD_LOS,              CK_POWER },
    { "View",           CONSOLE_CMD_VIEW,             CK_POWER },
    { "Radar",          CONSOLE_CMD_RADAR,            CK_POWER },
    { "Mapping",        CONSOLE_CMD_MAPPING,          CK_POWER },
    { "DoubleShot",     CONSOLE_CMD_DOUBLE_SHOT,      CK_POWER },
    { "HalfShot",       CONSOLE_CMD_HALF_SHOT,        CK_POWER },
    { "ShootAll",       CONSOLE_CMD_SHOOT_ALL,        CK_POWER },
    { "IWin",           CONSOLE_CMD_I_WIN,            CK_POWER },
    { "ILose",          CONSOLE_CMD_I_LOSE,           CK_POWER },
    { "Kill",           CONSOLE_CMD_KILL,             CK_POWER },
    { "Gods",           CONSOLE_CMD_GODS,             CK_POWER },
    { "BurnOne",        CONSOLE_CMD_BURN_ONE,         CK_POWER },
    { "BurnAll",        CONSOLE_CMD_BURN_ALL,         CK_POWER },
    { "LotsaBlood",     CONSOLE_CMD_LOTSA_BLOOD,      CK_POWER },
    { "Clock",          CONSOLE_CMD_CLOCK,            CK_OPEN  },
    { "Contour",        CONSOLE_CMD_CONTOUR,          CK_OPEN  },
    { "NoShake",        CONSOLE_CMD_NO_SHAKE,         CK_OPEN  },
    { "ShowRanges",     CONSOLE_CMD_SHOW_RANGES,      CK_OPEN  },
    { "ScrollSpeed",    CONSOLE_CMD_SCROLL_SPEED,     CK_OPEN  },
    { "Shadow",         CONSOLE_CMD_SHADOW,           CK_OPEN  },
    { "Music",          CONSOLE_CMD_MUSIC,            CK_OPEN  },
    { "MusicPlay",      CONSOLE_CMD_MUSIC_PLAY,       CK_OPEN  },
    { "MusicStop",      CONSOLE_CMD_MUSIC_STOP,       CK_OPEN  },
    { "GiveMana",       CONSOLE_CMD_GIVE_MANA,        CK_OPEN  },
    { "ShareManaPct",   CONSOLE_CMD_SHARE_MANA_PCT,   CK_OPEN  },
    { "ShareManaLimit", CONSOLE_CMD_SHARE_MANA_LIMIT, CK_OPEN  },
    { "Logo",           CONSOLE_CMD_LOGO,             CK_OPEN  },
    { "NetStats",       CONSOLE_CMD_NET_STATS,        CK_OPEN  },
};
#define TABLE_N ((int)(sizeof(g_table) / sizeof(g_table[0])))

static const ConsoleEntry *entry_for(int id) {
    for (int i = 0; i < TABLE_N; i++)
        if (g_table[i].id == id) return &g_table[i];
    return NULL;
}

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int ConsoleCmd_Parse(const char *text, ConsoleCmdLine *out) {
    if (!out) return CONSOLE_CMD_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (!text) return CONSOLE_CMD_UNKNOWN;
    const char *s = text;
    while (*s && out->count < CONSOLE_MAX_TOKENS) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        size_t n = 0;
        while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
        size_t keep = n < CONSOLE_TOKEN_MAX - 1 ? n : CONSOLE_TOKEN_MAX - 1;
        memcpy(out->tok[out->count], s, keep);
        out->tok[out->count][keep] = '\0';
        out->count++;
        s += n;
    }
    if (out->count == 0) return CONSOLE_CMD_UNKNOWN;
    for (int i = 0; i < TABLE_N; i++) {
        if (same_name(out->tok[0], g_table[i].name)) {
            out->id = g_table[i].id;
            break;
        }
    }
    return out->id;
}

int ConsoleCmd_Int(const ConsoleCmdLine *l, int i, int dflt) {
    if (!l || i < 0 || i >= l->count) return dflt;
    return (int)strtol(l->tok[i], NULL, 10);
}

float ConsoleCmd_Float(const ConsoleCmdLine *l, int i, float dflt) {
    if (!l || i < 0 || i >= l->count) return dflt;
    return (float)strtod(l->tok[i], NULL);
}

const char *ConsoleCmd_Name(int id) {
    const ConsoleEntry *e = entry_for(id);
    return e ? e->name : "";
}

int ConsoleCmd_IsPowerCode(int id) {
    const ConsoleEntry *e = entry_for(id);
    return e && e->kind == CK_POWER;
}

/* ── the commands for every machine ─────────────────────────────── */

static ConsoleRun send_code(unsigned code, unsigned param, int32_t value) {
    uint16_t arg = (uint16_t)((code & 0xffu) | ((param & 0xffu) << 8));
    return TAK_Cmd_EmitSeat(TAK_CMD_POWER_CODE, value, 0, arg) < 0
         ? CONSOLE_RAN_NOTHING : CONSOLE_RAN_SENT;
}

/* The original numbers players from 0 (legacy:36498-36505 checks the
 * index under 10), seats here run from 1. -1 for no seat in play. */
static int seat_from_index(int index) {
    const GameWorld *w = World_Get();
    if (!w || index < 0 || index >= TAK_MAX_PLAYERS) return -1;
    if (w->cfg.players[index].kind == TAK_SLOT_CLOSED) return -1;
    return index + 1;
}

/* A fraction from 0 to 1, as +ShareManaLimit and +ShareManaPct insist,
 * in 16.16 so every machine reads the same value. */
static ConsoleRun send_fraction(unsigned code, const ConsoleCmdLine *l) {
    if (l->count < 2) return CONSOLE_RAN_NOTHING;
    float v = ConsoleCmd_Float(l, 1, 0.0f);
    if (!(v >= 0.0f && v <= 1.0f)) return CONSOLE_RAN_NOTHING;
    return send_code(code, 0, (int32_t)(v * 65536.0f + 0.5f));
}

static ConsoleRun run_power(const ConsoleCmdLine *l) {
    switch (l->id) {
        case CONSOLE_CMD_ATM:         return send_code(TAK_CODE_ATM, 0, 0);
        case CONSOLE_CMD_RADAR:       return send_code(TAK_CODE_RADAR, 0, 0);
        case CONSOLE_CMD_MAPPING:     return send_code(TAK_CODE_MAPPING, 0, 0);
        case CONSOLE_CMD_DOUBLE_SHOT: return send_code(TAK_CODE_DOUBLE_SHOT, 0, 0);
        case CONSOLE_CMD_HALF_SHOT:   return send_code(TAK_CODE_HALF_SHOT, 0, 0);
        case CONSOLE_CMD_NOW_I_SEE:   return send_code(TAK_CODE_NOW_I_SEE, 0, 0);
        case CONSOLE_CMD_I_WIN:       return send_code(TAK_CODE_I_WIN, 0, 0);
        case CONSOLE_CMD_I_LOSE:      return send_code(TAK_CODE_I_LOSE, 0, 0);
        case CONSOLE_CMD_KILL:
            /* Only bare: the original checks for no arguments. */
            return l->count == 1 ? send_code(TAK_CODE_KILL, 0, 0) : CONSOLE_RAN_NOTHING;
        case CONSOLE_CMD_LOS:
            /* Alone it flips, and "On" or "Off" sets (legacy:36597-36630). */
            if (l->count == 1) return send_code(TAK_CODE_LOS, TAK_CODE_LOS_TOGGLE, 0);
            if (same_name(l->tok[1], "Off")) return send_code(TAK_CODE_LOS, TAK_CODE_LOS_OFF, 0);
            if (same_name(l->tok[1], "On")) return send_code(TAK_CODE_LOS, TAK_CODE_LOS_ON, 0);
            return CONSOLE_RAN_NOTHING;
        case CONSOLE_CMD_VIEW: {
            int seat = seat_from_index(ConsoleCmd_Int(l, 1, 0));
            return seat < 0 ? CONSOLE_RAN_NOTHING
                            : send_code(TAK_CODE_VIEW, (unsigned)seat, 0);
        }
        case CONSOLE_CMD_MANA_ME:
        case CONSOLE_CMD_NO_MANA: {
            /* The typist's selection, named in the command. */
            unsigned code = l->id == CONSOLE_CMD_MANA_ME ? TAK_CODE_MANA_ME : TAK_CODE_NO_MANA;
            return TAK_Cmd_EmitSelection(TAK_CMD_POWER_CODE, 0, 0, -1, 0,
                                         (uint16_t)code) < 0
                 ? CONSOLE_RAN_NOTHING : CONSOLE_RAN_SENT;
        }
        default:
            /* ShootAll lifts a targeting rule not in yet, Gods, BurnOne
             * and BurnAll act on the gods and burning trees, neither in
             * yet, and LotsaBlood draws a Direct3D debug layer. */
            return CONSOLE_RAN_NOT_IN_YET;
    }
}

/* ── the commands for this machine ──────────────────────────────── */

static int toggle_setting(const char *key, int dflt) {
    int on = !Settings_GetInt(key, dflt);
    Settings_SetInt(key, on);
    Settings_Save();
    return on;
}

static ConsoleRun run_open(const ConsoleCmdLine *l) {
    switch (l->id) {
        case CONSOLE_CMD_CLOCK:
            toggle_setting("DisplayOnScreenClock", 0);
            return CONSOLE_RAN_LOCAL;
        case CONSOLE_CMD_NO_SHAKE:
            ViewShake_SetNoShake(toggle_setting("NoShake", 0));
            if (ViewShake_NoShake()) ViewShake_Reset();
            return CONSOLE_RAN_LOCAL;
        case CONSOLE_CMD_SHADOW:
            Units_SetShadowsOn(toggle_setting("DrawShadows", 1));
            return CONSOLE_RAN_LOCAL;
        case CONSOLE_CMD_SCROLL_SPEED: {
            /* Pixels a frame at the original's 30, from 1 to 64
             * (legacy:120739-120746). */
            int n = ConsoleCmd_Int(l, 1, 0);
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            CameraConfig cc = *Camera_GetConfig();
            cc.scroll_px_per_sec = (float)n * 30.0f;
            Camera_SetConfig(&cc);
            return CONSOLE_RAN_LOCAL;
        }
        case CONSOLE_CMD_MUSIC:
            /* On to the next track, while one plays. */
            if (TAK_Music_IsPlaying()) TAK_Music_DebugSkip();
            return CONSOLE_RAN_LOCAL;
        case CONSOLE_CMD_MUSIC_PLAY:
            TAK_Music_PlayTrack(ConsoleCmd_Int(l, 1, 0));
            return CONSOLE_RAN_LOCAL;
        case CONSOLE_CMD_MUSIC_STOP:
            TAK_Music_Stop();
            return CONSOLE_RAN_LOCAL;
        case CONSOLE_CMD_GIVE_MANA: {
            /* +GiveMana <player> <amount>, exactly those two. */
            if (l->count != 3) return CONSOLE_RAN_NOTHING;
            int seat = seat_from_index(ConsoleCmd_Int(l, 1, 0));
            float amount = ConsoleCmd_Float(l, 2, 0.0f);
            if (seat < 0 || !(amount >= 1.0f)) return CONSOLE_RAN_NOTHING;
            if (amount > 32767.0f) amount = 32767.0f;
            return TAK_Cmd_EmitSeat(TAK_CMD_MANA_GIFT, (int32_t)(amount * 65536.0f),
                                    0, (uint16_t)seat) < 0
                 ? CONSOLE_RAN_NOTHING : CONSOLE_RAN_SENT;
        }
        case CONSOLE_CMD_SHARE_MANA_LIMIT:
            return send_fraction(TAK_CODE_SHARE_LIMIT, l);
        case CONSOLE_CMD_SHARE_MANA_PCT:
            return send_fraction(TAK_CODE_SHARE_PCT, l);
        default:
            /* Contour and ShowRanges draw overlays not in yet, Logo
             * repaints a player in another colour, and NetStats resets
             * counters whose print the retail build left empty. */
            return CONSOLE_RAN_NOT_IN_YET;
    }
}

ConsoleRun ConsoleCmd_Run(const char *text) {
    ConsoleCmdLine l;
    if (ConsoleCmd_Parse(text, &l) == CONSOLE_CMD_UNKNOWN) return CONSOLE_RAN_NOTHING;
    if (ConsoleCmd_IsPowerCode(l.id)) {
        const GameWorld *w = World_Get();
        if (!w || !w->cfg.power_codes) return CONSOLE_RAN_REFUSED;
        return run_power(&l);
    }
    return run_open(&l);
}

void ConsoleCmd_ApplySettings(void) {
    ViewShake_SetNoShake(Settings_GetInt("NoShake", 0));
}

int ConsoleCmd_ClockOn(void) {
    return Settings_GetInt("DisplayOnScreenClock", 0) != 0;
}

void ConsoleCmd_ClockText(char *buf, size_t cap, uint32_t sim_tick) {
    if (!buf || cap == 0) return;
    uint32_t s = sim_tick / 60u;
    snprintf(buf, cap, "Game Time : %02u:%02u:%02u",
             (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
}
