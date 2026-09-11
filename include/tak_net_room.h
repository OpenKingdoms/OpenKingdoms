#ifndef TAK_NET_ROOM_H
#define TAK_NET_ROOM_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_protocol.h"

/*
 * The battle room, as the server holds it.
 *
 * A pure state machine. No sockets, no clock, no allocation. Every entry
 * point either applies a change and bumps the revision, or refuses with one
 * of the protocol's reject reasons and changes nothing.
 *
 * The rules are the original's, with the deviations recorded in
 * docs/MANUAL_DEVIATIONS.md as N-002 through N-008:
 *   - a player edits only their own row, and any edit to it clears their
 *     ready, which toggles rather than latching
 *   - the host owns the map, the options and the unit cap, and changing any
 *     of them clears everyone's ready
 *   - a colour is a request, and the server hands back the next free one
 *   - the host may block a seat, unblock it and remove a player
 *   - a computer player belongs to the room, not to whoever added it
 *   - the host role moves to the lowest occupied human seat when the host
 *     leaves, instead of ending the room
 *   - watchers sit outside the eight seats
 *   - the start waits until every human has reported the room's map by
 *     its fingerprint, which is the original's matching map gate done
 *     with a hash rather than a name
 */

#define TAK_ROOM_CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define TAK_ROOM_TIMEOUT_MIN   30    /* the original's own bounds */
#define TAK_ROOM_TIMEOUT_MAX   150
#define TAK_ROOM_SIDES         4     /* Aramon, Taros, Veruna, Zhon */
#define TAK_ROOM_COLOURS       10
#define TAK_ROOM_TEAMS         4     /* 0 means every player for themselves */

typedef struct TAK_RoomCfg {
    char     name[TAK_NET_ROOM_NAME_MAX];
    char     password[TAK_NET_PASSWORD_MAX];
    char     map_name[TAK_NET_MAP_NAME_MAX];
    uint8_t  map_fingerprint[TAK_NET_FINGERPRINT_BYTES];
    uint32_t flags;         /* TAK_ROOMF_* */
    uint32_t options;       /* the rule checkboxes */
    uint8_t  max_players;   /* 2 to TAK_NET_SEATS */
    uint16_t unit_cap;
    uint16_t timeout_secs;
} TAK_RoomCfg;

typedef struct TAK_RoomWatcher {
    uint32_t client_id;
    char     name[TAK_NET_NAME_MAX];
    uint16_t ping_ms;
} TAK_RoomWatcher;

typedef struct TAK_Room {
    uint32_t        id;
    uint32_t        revision;
    char            code[TAK_NET_CODE_MAX];
    TAK_RoomCfg     cfg;
    uint8_t         status;          /* TAK_NetRoomStatus */
    uint32_t        host_client_id;
    uint32_t        engine_build_id;
    uint8_t         determinism_class;
    TAK_NetSlot     slot[TAK_NET_SEATS];
    TAK_RoomWatcher watcher[TAK_NET_WATCHERS_MAX];
    uint8_t         watcher_count;
} TAK_Room;

/* What a change did, beyond the room state itself. */
typedef struct TAK_RoomEffect {
    uint32_t removed_client_id;  /* a player the host removed, else 0 */
    uint8_t  ready_cleared;      /* everyone's ready went back to off */
    uint8_t  seat;               /* the seat the change landed on, or none */
} TAK_RoomEffect;

typedef struct TAK_RoomLeave {
    uint8_t  seat;                 /* the seat freed, or TAK_NET_SEAT_NONE */
    uint8_t  was_host;
    uint8_t  room_finished;        /* no human left, the relay drops it */
    uint32_t new_host_client_id;   /* 0 when the room finished */
} TAK_RoomLeave;

/* A room's six character invite code, derived from a seed so a test can
 * reproduce one. The alphabet leaves out I, O, 0 and 1 because people read
 * these aloud. */
void TAK_Room_MakeCode(uint32_t seed, char out[TAK_NET_CODE_MAX]);

/* Start a room with its creator in seat 0. Clamps the configuration into
 * range rather than refusing, because the creator's client may be older
 * than this server. */
void TAK_Room_Init(TAK_Room *r, uint32_t id, uint32_t code_seed,
                   const TAK_RoomCfg *cfg,
                   uint32_t host_client_id, const char *host_name,
                   uint32_t engine_build_id, uint8_t determinism_class);

/* Whether this client could join at all, without joining. Returns 0 or a
 * reject reason, which is what greys a row in the game list and says why. */
int TAK_Room_Compatible(const TAK_Room *r, uint32_t engine_build_id,
                        uint8_t determinism_class);

/* Join, taking the lowest free seat, or the watcher list. On success fills
 * *out_seat with the seat, or TAK_NET_SEAT_NONE for a watcher. Returns 0 or
 * a reject reason. The password is compared in constant time over the whole
 * field, so a wrong one costs the same as a right one. */
int TAK_Room_Join(TAK_Room *r, uint32_t client_id, const char *name,
                  const char *password, int as_watcher,
                  uint32_t engine_build_id, uint8_t determinism_class,
                  uint8_t *out_seat);

/* Remove a client. Migrates the host when the leaver held it. A computer
 * player is never removed by this, whoever added it. */
void TAK_Room_Leave(TAK_Room *r, uint32_t client_id, TAK_RoomLeave *out);

/* Apply one field change. Returns 0 or a reject reason. `fx` may be NULL. */
int TAK_Room_Edit(TAK_Room *r, uint32_t client_id,
                  const TAK_MsgRoomEdit *e, TAK_RoomEffect *fx);

/* The original's start gates, each with its own reason: the host asks,
 * at least one other human is present, not everyone is on the same team,
 * a map is chosen and every human holds it by fingerprint, and every
 * human is ready. Returns 0 or a reject reason. */
int TAK_Room_CanStart(const TAK_Room *r, uint32_t client_id);

/* Move the room to loading. Only valid when TAK_Room_CanStart agrees. */
int TAK_Room_Start(TAK_Room *r, uint32_t client_id);

/* Every world loaded and agreed: the match is under way. */
void TAK_Room_Go(TAK_Room *r);

/* A start that could not finish, such as worlds that loaded differently,
 * goes back to the lobby with everyone's ready cleared. */
void TAK_Room_Abort(TAK_Room *r);

/* In game a seat stays a seat when its player's connection goes. This
 * marks it, and moves the host role to the lowest seat still connected
 * when the host was the one who went, which is N-002 in game. Returns the
 * host's client id afterwards. */
uint32_t TAK_Room_SetConnected(TAK_Room *r, uint32_t client_id, int connected);

/* Snapshots, which is all a client ever receives of room state. */
void TAK_Room_Snapshot(const TAK_Room *r, TAK_MsgRoomState *out);
void TAK_Room_Summary(const TAK_Room *r, uint32_t viewer_build,
                      uint8_t viewer_class, TAK_RoomSummary *out);

/* Lookups. Seat returns TAK_NET_SEAT_NONE when the client is a watcher or
 * is not here at all. */
uint8_t TAK_Room_SeatOf(const TAK_Room *r, uint32_t client_id);
int     TAK_Room_IsWatcher(const TAK_Room *r, uint32_t client_id);
int     TAK_Room_HumanCount(const TAK_Room *r);
int     TAK_Room_OccupiedCount(const TAK_Room *r);

/* Record a measured round trip for the battle room's Ping column. */
void TAK_Room_SetPing(TAK_Room *r, uint32_t client_id, uint16_t ping_ms);

#endif /* TAK_NET_ROOM_H */
