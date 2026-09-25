#ifndef TAK_NET_HTTP_H
#define TAK_NET_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_ledger.h"
#include "tak_net_protocol.h"

/*
 * The relay's read only JSON API, for the leaderboard page.
 *
 * It answers on the same port the WebSocket does. A connection whose
 * first bytes are a plain HTTP request rather than an upgrade gets one
 * response and a close. No sockets here: the server hands over the
 * request bytes and writes back what this returns.
 *
 *   GET /api/leaderboard?offset=0&limit=100   the table, wins first
 *   GET /api/players/<id>?offset=0&limit=25   one player and their games
 *   GET /api/games?offset=0&limit=25          recent games
 *   GET /api/games/<n>                        one game in full
 *   GET /api/health                           counts and a version stamp
 *   GET /api/rooms                            players online, open and running games
 *
 * Every answer carries Access-Control-Allow-Origin: *, because the page
 * that reads it is served from a different host and the data is
 * public. There is nothing to write, so there is nothing to protect.
 */

/* Room for the largest page any route can produce. */
#define TAK_HTTP_RESPONSE_MAX  (120u << 10)

/* What the relay is doing right now, for /api/rooms. The server fills
 * it from the relay before each answer (TAK_Relay_Live). */
#define TAK_HTTP_LIVE_ROOMS  8

typedef struct TAK_HttpLiveRoom {
    TAK_RoomSummary room;
    uint16_t        host_ping_ms;
    uint32_t        playing_secs;   /* 0 until the match starts */
} TAK_HttpLiveRoom;

typedef struct TAK_HttpLive {
    uint32_t         online;        /* connections past their hello */
    uint32_t         in_lobby;      /* of those, the ones in no room */
    uint32_t         count;
    TAK_HttpLiveRoom room[TAK_HTTP_LIVE_ROOMS];
} TAK_HttpLive;

/* Answer one request, as TAK_WsConn_PlainRequest handed it over. Writes
 * a whole HTTP/1.1 response, headers and body, and returns its length,
 * or 0 when even an error would not fit. */
size_t TAK_Http_Answer(const TAK_Ledger *l, const uint8_t *req, size_t len,
                       char *out, size_t cap);

/* The same with the relay's live view. NULL live makes /api/rooms a 404. */
size_t TAK_Http_AnswerLive(const TAK_Ledger *l, const TAK_HttpLive *live,
                           const uint8_t *req, size_t len, char *out, size_t cap);

#endif /* TAK_NET_HTTP_H */
