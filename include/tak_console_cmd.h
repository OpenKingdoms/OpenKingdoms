#ifndef TAK_CONSOLE_CMD_H
#define TAK_CONSOLE_CMD_H

#include <stddef.h>
#include <stdint.h>

/* ── The console's + commands ─────────────────────────────────────────
 *
 * A chat line whose first non space character is '+' is a command
 * (legacy:154470-154500). The original looks the first word up, case
 * blind, in three tables it registers at legacy:38938-38946: one any
 * player may use, one that needs the room's power codes, and developer
 * tooling, which is not here.
 *
 * Whatever changes the battle goes out as a TAK_CMD_POWER_CODE, so
 * every machine applies it on the same tick. The rest changes this
 * machine alone: the clock, the view shake, scrolling, shadows and the
 * music. */

typedef enum ConsoleCmdId {
    CONSOLE_CMD_UNKNOWN = 0,
    /* The power codes. */
    CONSOLE_CMD_ATM,
    CONSOLE_CMD_MANA_ME,
    CONSOLE_CMD_NO_MANA,
    CONSOLE_CMD_NOW_I_SEE,
    CONSOLE_CMD_LOS,
    CONSOLE_CMD_VIEW,
    CONSOLE_CMD_RADAR,
    CONSOLE_CMD_MAPPING,
    CONSOLE_CMD_DOUBLE_SHOT,
    CONSOLE_CMD_HALF_SHOT,
    CONSOLE_CMD_I_WIN,
    CONSOLE_CMD_I_LOSE,
    CONSOLE_CMD_KILL,
    CONSOLE_CMD_GODS,
    CONSOLE_CMD_BURN_ONE,
    CONSOLE_CMD_BURN_ALL,
    CONSOLE_CMD_LOTSA_BLOOD,
    /* Open to every room. */
    CONSOLE_CMD_CLOCK,
    CONSOLE_CMD_CONTOUR,
    CONSOLE_CMD_NO_SHAKE,
    CONSOLE_CMD_SHOW_RANGES,
    CONSOLE_CMD_SCROLL_SPEED,
    CONSOLE_CMD_SHADOW,
    CONSOLE_CMD_MUSIC,
    CONSOLE_CMD_MUSIC_PLAY,
    CONSOLE_CMD_MUSIC_STOP,
    CONSOLE_CMD_SHOOT_ALL,
    CONSOLE_CMD_GIVE_MANA,
    CONSOLE_CMD_SHARE_MANA_PCT,
    CONSOLE_CMD_SHARE_MANA_LIMIT,
    CONSOLE_CMD_LOGO,
    CONSOLE_CMD_NET_STATS,
    CONSOLE_CMD_COUNT
} ConsoleCmdId;

#define CONSOLE_MAX_TOKENS 8
#define CONSOLE_TOKEN_MAX  32

/* One typed line, split on spaces. count includes the name, as the
 * original's own count does, so "+LOS" alone is 1. */
typedef struct ConsoleCmdLine {
    int  id;
    int  count;
    char tok[CONSOLE_MAX_TOKENS][CONSOLE_TOKEN_MAX];
} ConsoleCmdLine;

/* Split `text`, the line after its '+', and name the command. Returns
 * the id, CONSOLE_CMD_UNKNOWN for a name not in the table. */
int ConsoleCmd_Parse(const char *text, ConsoleCmdLine *out);

/* Token i read as a number, or `dflt` when the line is shorter. */
int   ConsoleCmd_Int(const ConsoleCmdLine *l, int i, int dflt);
float ConsoleCmd_Float(const ConsoleCmdLine *l, int i, float dflt);

/* The name as the table spells it, "" for none. */
const char *ConsoleCmd_Name(int id);
/* 1 for a power code, which needs the room's permission. */
int ConsoleCmd_IsPowerCode(int id);

typedef enum ConsoleRun {
    CONSOLE_RAN_NOTHING = 0, /* unknown, or arguments it will not take */
    CONSOLE_RAN_LOCAL,       /* changed something on this machine      */
    CONSOLE_RAN_SENT,        /* went out for every machine to apply    */
    CONSOLE_RAN_REFUSED,     /* a power code the room did not allow    */
    CONSOLE_RAN_NOT_IN_YET   /* known, with nothing here to act on yet */
} ConsoleRun;

/* Run the line after its '+'. The caller echoes the line itself. */
ConsoleRun ConsoleCmd_Run(const char *text);

/* The settings a command keeps between battles, handed to the modules
 * that read them. Called when a battle starts. */
void ConsoleCmd_ApplySettings(void);

/* 1 while +Clock has the battle clock up (DisplayOnScreenClock). */
int ConsoleCmd_ClockOn(void);
/* "Game Time : 00:01:05", the original's clock line (legacy:210353),
 * for an engine tick count at 60 a second. */
void ConsoleCmd_ClockText(char *buf, size_t cap, uint32_t sim_tick);

#endif /* TAK_CONSOLE_CMD_H */
