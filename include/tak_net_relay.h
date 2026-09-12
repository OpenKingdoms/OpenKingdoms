#ifndef TAK_NET_RELAY_H
#define TAK_NET_RELAY_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_protocol.h"
#include "tak_net_room.h"
#include "tak_net_turn.h"
#include "tak_net_transport.h"

/*
 * The relay core: connections, the room directory, and each match's turn
 * clock, joined up behind one transport interface.
 *
 * It holds no simulation, no game state and no game data. It orders input,
 * keeps the turn log, and counts membership. A host program calls the
 * four entry points below with the time, and everything the relay says
 * goes out through the transport. No sockets and no allocation, so the
 * same core runs in the loopback test, in the dedicated server, and one
 * day inside a native client hosting on a local network.
 */

#define TAK_RELAY_CLIENTS_MAX  64
#define TAK_RELAY_ROOMS_MAX    8
#define TAK_RELAY_PING_MS      2000u    /* the original's heartbeat cadence */
#define TAK_RELAY_HELLO_MS     10000u   /* a connection that never says hello */

typedef struct TAK_RelayCfg {
    char     server_name[TAK_NET_SERVER_NAME_MAX];
    char     motd[TAK_NET_MOTD_MAX];
    char     access_key[TAK_NET_KEY_MAX];   /* empty: no key needed */
    uint32_t flags;
    uint32_t newest_build;
    uint32_t seed;                           /* match seeds and room codes */
} TAK_RelayCfg;

typedef struct TAK_RelayClient {
    uint8_t      in_use;
    uint8_t      welcomed;
    TAK_ConnId   conn;
    uint32_t     id;              /* the session id, adopted again on rejoin */
    int          room;            /* index into rooms, -1 for none */
    uint64_t     opened_ms;
    uint32_t     ping_seq;
    TAK_MsgHello hello;
} TAK_RelayClient;

struct TAK_Relay;

typedef struct TAK_RelayRoom {
    uint8_t           in_use;
    struct TAK_Relay *relay;
    TAK_Room          room;
    /* The host's data, which every joiner must match. */
    uint64_t          schema_hash;
    uint64_t          content_hash;
    uint64_t          group_hash[TAK_NET_GROUP_HASHES];
    /* The match, once started. */
    uint32_t          match_id;
    uint32_t          seed;
    TAK_TurnLog       log;
    TAK_TurnLogEntry *log_entries;
    uint32_t          log_entry_cap;
    uint8_t          *log_arena;
    size_t            log_arena_cap;
    TAK_TurnClock     clock;
    uint8_t           sim_token[TAK_TURN_SIMS_MAX][TAK_NET_TOKEN_BYTES];
    uint64_t          world_hash[TAK_TURN_SIMS_MAX];
    uint32_t          loaded;       /* sims that reported LOADED */
} TAK_RelayRoom;

typedef struct TAK_Relay {
    TAK_RelayCfg     cfg;
    TAK_NetTransport tx;
    uint64_t         now;
    uint64_t         next_ping_ms;
    uint32_t         next_client_id;
    uint32_t         next_room_id;
    uint32_t         next_match_id;
    uint32_t         rng;
    TAK_RelayClient  client[TAK_RELAY_CLIENTS_MAX];
    TAK_RelayRoom    room[TAK_RELAY_ROOMS_MAX];
    uint8_t          out[TAK_NET_FRAME_MAX];
    /* For a test or a health endpoint. */
    uint32_t         frames_in;
    uint32_t         frames_refused;
    uint32_t         clients_dropped;
} TAK_Relay;

/* The turn logs' storage is the caller's, split evenly between rooms. */
void TAK_Relay_Init(TAK_Relay *r, const TAK_RelayCfg *cfg, TAK_NetTransport tx,
                    uint8_t *log_arena, size_t arena_bytes,
                    TAK_TurnLogEntry *log_entries, uint32_t entry_count);

void TAK_Relay_OnConnect(TAK_Relay *r, TAK_ConnId conn, uint64_t now_ms);
void TAK_Relay_OnFrame(TAK_Relay *r, TAK_ConnId conn,
                       const uint8_t *frame, size_t len, uint64_t now_ms);
void TAK_Relay_OnClose(TAK_Relay *r, TAK_ConnId conn, uint64_t now_ms);
void TAK_Relay_Tick(TAK_Relay *r, uint64_t now_ms);

/* Lookups for tests and health checks. NULL when there is none. */
TAK_RelayRoom   *TAK_Relay_FindRoom(TAK_Relay *r, uint32_t room_id);
TAK_RelayClient *TAK_Relay_FindClient(TAK_Relay *r, uint32_t client_id);

#endif /* TAK_NET_RELAY_H */
