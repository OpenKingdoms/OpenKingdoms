#ifndef TAK_WEB_STATS_H
#define TAK_WEB_STATS_H

#include "tak_battle_config.h"

/*
 * What the browser page counts about play, told to the page.
 *
 * The page's own script, web/counts.js, decides whether anything
 * leaves the browser, and it sends nothing under Do Not Track, Global
 * Privacy Control or the player's own opt out. A desktop build has no
 * page, so there all of this does nothing at all.
 *
 * A start path says what it is starting just before the battle loads,
 * and the main loop says when the battle is under way and when it is
 * over. Nothing here touches the simulation.
 */

/* What the next battle is. mode is "skirmish", "campaign",
 * "multiplayer" or "load". sub says how: "host", "join", "dropin",
 * "watch" or "rejoin" for a multiplayer battle, and "skirmish",
 * "campaign" or "battle" for a loaded one. A replay needs no word,
 * the main loop knows one when it sees it. */
void WebStats_Starting(const char *mode, const char *sub);

/* The campaign and the mission a campaign start is. */
void WebStats_Mission(const char *campaign, const char *mission);

/* The battle is under way. local_player is 1 based, mod is the mod
 * set's name or NULL for the game itself. */
void WebStats_BattleBegan(const BattleConfig *cfg, const char *map, int local_player,
                          int network, int replaying, const char *mod);

/* What a battle came to. */
typedef struct WebStatsResult {
    int over;          /* the battle reached a verdict */
    int local;         /* 1 won, -1 lost, 0 a draw or none */
    int victory;       /* a mission's own verdict */
    int defeat;
    int ticks;         /* 60 Hz ticks played */
} WebStatsResult;

/* The battle is over. how is NULL to go by the result, or what the
 * main loop knows better: "restart", "disconnect" or "desync". */
void WebStats_BattleEnded(const WebStatsResult *r, const char *how);

/* Something went wrong that the player will have seen. */
void WebStats_Error(const char *what);

/* For a test: where the events go in place of the page. kind is
 * "start", "end" or "error" and json one object. */
typedef void (*WebStats_Sink)(const char *kind, const char *json);
void WebStats_SetSink(WebStats_Sink sink);

#endif /* TAK_WEB_STATS_H */
