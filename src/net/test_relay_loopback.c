/*
 * test_relay_loopback.c -- eight clients, one relay, one process.
 *
 * Phase one plays whole matches over the fake network: protocol clients
 * join a room by its code, report the map, ready up, load and play, each
 * running a small stand in world, while the network adds latency, jitter,
 * loss, duplicates and reordering.
 *
 * Phase two is the harness the plan asks for. Each client's received turn
 * stream is replayed on its own, one after another, against a freshly
 * reset world, and the hash traces are compared. The engine cannot hold
 * two simulations in one process today, so phase two is where it plugs in
 * once the simulation runs headless. The stand in world keeps the harness
 * honest until then: it is deterministic, it hashes every 60 ticks, and it
 * reacts to commands and to the relay's own system commands.
 *
 * Data free, virtual time, no sockets.
 */

#include "test_framework.h"
#include "tak_net_relay.h"
#include "tak_bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_MAX      12
#define STEP_MS    10
#define TRACE_MAX  4096
#define BUILD      0x0b111d01u
#define SCHEMA     0x5c4e3a0000000001ull
#define CONTENT    0xc0de000000000002ull

/* ── A stand in world ─────────────────────────────────────────────────── */

typedef struct ToyWorld {
    uint32_t tick;
    uint32_t rng;
    int32_t  pos[TAK_NET_SEATS][2];
    int32_t  health[TAK_NET_SEATS];
    uint8_t  owner[TAK_NET_SEATS];   /* 0 player, 1 computer, 2 removed */
} ToyWorld;

static void toy_reset(ToyWorld *w, uint32_t seed) {
    memset(w, 0, sizeof(*w));
    w->rng = seed ? seed : 1u;
    for (int s = 0; s < TAK_NET_SEATS; s++) {
        w->pos[s][0] = s * 100;
        w->pos[s][1] = s * 50;
        w->health[s] = 1000;
    }
}

static void toy_command(ToyWorld *w, uint8_t seat, const uint8_t *b, uint16_t n) {
    if (seat >= TAK_NET_SEATS || w->owner[seat] == 2 || n < 4) return;
    w->pos[seat][0] += (int8_t)b[0];
    w->pos[seat][1] += (int8_t)b[1];
    w->health[b[2] & 7] -= b[3];
}

static void toy_step(ToyWorld *w) {
    w->tick++;
    w->rng = w->rng * 1103515245u + 12345u;
    for (int s = 0; s < TAK_NET_SEATS; s++) {
        if (w->owner[s] == 1) w->pos[s][0] += (int32_t)((w->rng >> 16) & 3u);
        w->pos[s][1] += (int32_t)((w->rng >> (s * 3)) & 1u);
    }
}

static uint64_t toy_hash(const ToyWorld *w) {
    uint64_t h = 1469598103934665603ull;
    uint32_t words[3 + TAK_NET_SEATS * 4];
    int k = 0;
    words[k++] = w->tick;
    words[k++] = w->rng;
    words[k++] = 0;
    for (int s = 0; s < TAK_NET_SEATS; s++) {
        words[k++] = (uint32_t)w->pos[s][0];
        words[k++] = (uint32_t)w->pos[s][1];
        words[k++] = (uint32_t)w->health[s];
        words[k++] = w->owner[s];
    }
    for (int i = 0; i < k; i++)
        for (int b = 0; b < 4; b++) {
            h ^= (uint8_t)(words[i] >> (8 * b));
            h *= 1099511628211ull;
        }
    return h;
}

/* ── Clients ──────────────────────────────────────────────────────────── */

typedef struct Bytes { uint8_t *p; size_t len, cap; } Bytes;

static void bytes_push(Bytes *b, const uint8_t *src, size_t n) {
    if (b->len + n + 4 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 4) cap *= 2;
        uint8_t *q = (uint8_t *)realloc(b->p, cap);
        if (!q) return;
        b->p = q;
        b->cap = cap;
    }
    tak_put_u32(b->p + b->len, (uint32_t)n);
    memcpy(b->p + b->len + 4, src, n);
    b->len += n + 4;
}

typedef struct Client {
    int        used, alive, watcher, attacker;
    TAK_ConnId conn;
    uint32_t   session;
    uint8_t    token[TAK_NET_TOKEN_BYTES];
    char       name[TAK_NET_NAME_MAX];
    uint8_t    seat;
    int        started;
    uint32_t   seed;
    ToyWorld   world;
    uint32_t   done_turns;
    uint32_t   cmd_seq;
    uint64_t   next_cmd_ms;
    Bytes      inbox;  size_t in_off;
    Bytes      rec;    size_t sim_off;     /* the received turn stream */
    uint64_t   trace[TRACE_MAX];
    uint32_t   trace_len;
    TAK_MsgRoomState room;
    int        have_room;
    uint8_t    last_reject;
    int        resets;                      /* told to start over from turn 0 */
    int        stream_errors;
    int        waiting_seen;
    uint8_t    waiting_seat;
    int        lost_seen[TAK_NET_SEATS];
    int        catching_seen[TAK_NET_SEATS];
    int        left_seen[TAK_NET_SEATS];
    uint8_t    left_as[TAK_NET_SEATS];
    int        reclaim_seen[TAK_NET_SEATS];
    /* The script. Zero means never. */
    uint64_t   slow_from, slow_until, last_slow_ms;
    uint32_t   desync_at_tick;
    int        desync_done;
    uint64_t   mute_at, close_at, rejoin_at, resign_at;
    int        resigned;
} Client;

static TAK_Relay        relay;
static TAK_FakeNet      net;
static uint8_t          log_arena[TAK_RELAY_ROOMS_MAX * (512u << 10)];
static TAK_TurnLogEntry log_entries[TAK_RELAY_ROOMS_MAX * 32768u];
static Client           cl[N_MAX];
static int              ncl;
static uint64_t         g_now;
static uint8_t          tx[TAK_NET_FRAME_MAX];
static const uint8_t    MAP_FP[TAK_NET_FINGERPRINT_BYTES] = {
    0x9a, 0x11, 0x3c, 0x42, 0x07, 0xe5, 0x6b, 0x20, 0xd8, 0x55, 0x01, 0x7f,
    0xaa, 0x3e, 0x90, 0x12, 0x4c, 0x67, 0xb1, 0x08, 0x2d, 0xf0, 0x5e, 0x33,
    0x71, 0xc9, 0x0b, 0x86, 0x44, 0x1a, 0xee, 0x5d };

static Client *client_by_conn(TAK_ConnId c) {
    for (int i = 0; i < ncl; i++) if (cl[i].used && cl[i].conn == c) return &cl[i];
    return NULL;
}

static void up(Client *c, size_t n) {
    if (n) TAK_FakeNet_ClientSend(&net, c->conn, tx, n, g_now);
}

static void send_hello(Client *c, int rejoin) {
    TAK_MsgHello h;
    memset(&h, 0, sizeof(h));
    h.protocol_version = TAK_NET_PROTOCOL_VERSION;
    h.engine_build_id = BUILD;
    h.determinism_class = TAK_CLASS_TEST;
    h.client_kind = c->watcher ? TAK_CLIENT_WATCHER : TAK_CLIENT_PLAYER;
    h.flags = rejoin ? TAK_HELLOF_WANTS_REJOIN : 0;
    h.schema_hash = SCHEMA;
    h.content_hash = CONTENT;
    for (int g = 0; g < TAK_NET_GROUP_HASHES; g++) h.group_hash[g] = 100u + (uint64_t)g;
    memcpy(h.device_token, c->token, TAK_NET_TOKEN_BYTES);
    memcpy(h.name, c->name, TAK_NET_NAME_MAX);
    up(c, TAK_Msg_HelloEncode(&h, tx, sizeof(tx)));
}

static void send_edit(Client *c, uint8_t field, uint8_t seat, uint32_t value,
                      const uint8_t *fp) {
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof(e));
    e.field = field;
    e.seat = seat;
    e.value = value;
    if (fp) memcpy(e.fingerprint, fp, TAK_NET_FINGERPRINT_BYTES);
    up(c, TAK_Msg_RoomEditEncode(&e, tx, sizeof(tx)));
}

static void send_ack(Client *c, uint32_t last_turn, uint32_t tick, uint64_t hash) {
    TAK_MsgAck a;
    a.last_turn = last_turn;
    a.hash_tick = tick;
    a.state_hash = hash;
    up(c, TAK_Msg_AckEncode(&a, tx, sizeof(tx)));
}

static void send_cmd(Client *c) {
    uint8_t body[4];
    body[0] = (uint8_t)(c->seat + 1);
    body[1] = (uint8_t)(0u - c->seat);
    body[2] = (uint8_t)((c->seat + 3) & 7);
    body[3] = (uint8_t)(5 + c->seat);
    TAK_MsgCmd m;
    memset(&m, 0, sizeof(m));
    m.client_seq = ++c->cmd_seq;
    m.count = 1;
    m.cmd[0].data = body;
    m.cmd[0].len = sizeof(body);
    up(c, TAK_Msg_CmdEncode(&m, tx, sizeof(tx)));
}

/* One TURN frame into a world, live or replayed. Hashes every 60 ticks,
 * and a live client acknowledges each one with its hash. */
static int run_turns(ToyWorld *w, uint64_t *trace, uint32_t *trace_len,
                     uint32_t *done, const uint8_t *frame, uint32_t len,
                     Client *live) {
    TAK_NetFrame f;
    static TAK_MsgTurn t;
    if (TAK_Net_Split(frame, len, &f) || f.type != TAK_MSG_TURN ||
        TAK_Msg_TurnDecode(&t, f.payload, f.payload_len)) return -1;
    if (t.turn != *done) return -1;          /* a gap or a repeat */
    uint32_t turns = t.entry_count ? 1u : t.empty_run;
    for (uint32_t k = 0; k < turns; k++) {
        if (k == 0) {
            for (int e = 0; e < t.entry_count; e++) {
                const TAK_TurnEntry *en = &t.entry[e];
                for (int i = 0; i < en->count; i++) {
                    const uint8_t *b = en->cmd[i].data;
                    uint16_t n = en->cmd[i].len;
                    if (en->seat != TAK_NET_SEAT_SERVER) { toy_command(w, en->seat, b, n); continue; }
                    if (b[0] == TAK_SYS_PLAYER_LEFT && n >= 3) {
                        w->owner[b[1] & 7] = (uint8_t)(b[2] == TAK_LEFT_ARMY_REMOVED ? 2 : 1);
                        if (live) { live->left_seen[b[1] & 7] = 1; live->left_as[b[1] & 7] = b[2]; }
                    } else if (b[0] == TAK_SYS_SEAT_RECLAIM && n >= 6) {
                        w->owner[b[1] & 7] = 0;
                        if (live) live->reclaim_seen[b[1] & 7] = 1;
                    }
                }
            }
        }
        for (int i = 0; i < TAK_NET_TURN_TICKS; i++) {
            toy_step(w);
            if (live && live->desync_at_tick && !live->desync_done &&
                w->tick == live->desync_at_tick) {
                w->health[0] ^= 0x10;        /* one client's unit health flips */
                live->desync_done = 1;
            }
        }
        (*done)++;
        if (w->tick % TAK_NET_HASH_TICKS == 0) {
            uint32_t idx = w->tick / TAK_NET_HASH_TICKS - 1;
            uint64_t h = toy_hash(w);
            if (idx < TRACE_MAX) {
                trace[idx] = h;
                if (*trace_len < idx + 1) *trace_len = idx + 1;
            }
            if (live) send_ack(live, *done - 1, w->tick, h);
        }
    }
    return 0;
}

static void start_over(Client *c) {
    toy_reset(&c->world, c->seed);
    c->done_turns = 0;
    c->rec.len = 0;
    c->sim_off = 0;
    c->trace_len = 0;
}

static void client_handle(Client *c, const uint8_t *frame, uint32_t len) {
    TAK_NetFrame f;
    if (TAK_Net_Split(frame, len, &f)) { c->stream_errors++; return; }
    const uint8_t *p = f.payload;
    size_t n = f.payload_len;
    switch (f.type) {
    case TAK_MSG_WELCOME: {
        TAK_MsgWelcome m;
        if (!TAK_Msg_WelcomeDecode(&m, p, n)) c->session = m.session_id;
        break;
    }
    case TAK_MSG_REJECT: {
        TAK_MsgReject m;
        if (!TAK_Msg_RejectDecode(&m, p, n)) c->last_reject = m.reason;
        break;
    }
    case TAK_MSG_ROOM_STATE:
        if (TAK_Msg_RoomStateDecode(&c->room, p, n)) { c->stream_errors++; break; }
        c->have_room = 1;
        for (int i = 0; i < TAK_NET_SEATS; i++)
            if (c->room.slot[i].kind == TAK_NSLOT_HUMAN &&
                c->room.slot[i].client_id == c->session) c->seat = (uint8_t)i;
        break;
    case TAK_MSG_START_GAME: {
        TAK_MsgStartGame m;
        if (TAK_Msg_StartGameDecode(&m, p, n)) { c->stream_errors++; break; }
        c->seed = m.seed;
        c->seat = m.your_seat;
        start_over(c);
        TAK_MsgLoadProgress lp = { 100 };
        up(c, TAK_Msg_LoadProgressEncode(&lp, tx, sizeof(tx)));
        TAK_MsgLoaded ld;
        ld.world_hash = toy_hash(&c->world);
        up(c, TAK_Msg_LoadedEncode(&ld, tx, sizeof(tx)));
        break;
    }
    case TAK_MSG_GO: {
        TAK_MsgGo m;
        if (TAK_Msg_GoDecode(&m, p, n)) { c->stream_errors++; break; }
        if (m.first_turn == 0) {
            if (c->started) c->resets++;
            start_over(c);
        }
        c->started = 1;
        break;
    }
    case TAK_MSG_TURN:
        bytes_push(&c->rec, frame, len);
        break;
    case TAK_MSG_PACE: {
        TAK_MsgPace m;
        if (!TAK_Msg_PaceDecode(&m, p, n) &&
            m.reason == TAK_PACE_WAITING_FOR_PLAYER && !m.paused) {
            c->waiting_seen = 1;
            c->waiting_seat = m.seat;
        }
        break;
    }
    case TAK_MSG_PLAYER_STATUS: {
        TAK_MsgPlayerStatus m;
        if (TAK_Msg_PlayerStatusDecode(&m, p, n) || m.seat >= TAK_NET_SEATS) break;
        if (m.status == TAK_PSTATUS_LOST) c->lost_seen[m.seat] = 1;
        if (m.status == TAK_PSTATUS_CATCHING_UP) c->catching_seen[m.seat] = 1;
        break;
    }
    case TAK_MSG_PING: {
        TAK_MsgPing m;
        if (!TAK_Msg_PingDecode(&m, p, n))
            up(c, TAK_Msg_PingEncode(TAK_MSG_PONG, &m, tx, sizeof(tx)));
        break;
    }
    default:
        break;
    }
}

static void client_step(Client *c) {
    if (!c->alive) return;
    if (c->close_at && g_now >= c->close_at) {
        TAK_FakeNet_ClientClose(&net, c->conn, g_now);
        c->alive = 0;
        c->close_at = 0;
        return;
    }
    if (c->mute_at && g_now >= c->mute_at) {
        TAK_FakeNet_SetMuted(&net, c->conn, 1);    /* stops talking, stays open */
        c->mute_at = 0;
    }
    if (c->resign_at && c->started && g_now >= c->resign_at) {
        send_edit(c, TAK_EDIT_WATCH, c->seat, 1, NULL);
        c->resign_at = 0;
        c->resigned = 1;
    }
    while (c->in_off < c->inbox.len) {
        uint32_t n = tak_get_u32(c->inbox.p + c->in_off);
        const uint8_t *f = c->inbox.p + c->in_off + 4;
        c->in_off += 4u + n;
        client_handle(c, f, n);
    }
    c->inbox.len = 0;
    c->in_off = 0;
    if (!c->started) return;

    int simulated = 0;
    while (c->sim_off < c->rec.len) {
        int slow = g_now >= c->slow_from && g_now < c->slow_until;
        if (slow && g_now - c->last_slow_ms < 150) break;
        uint32_t n = tak_get_u32(c->rec.p + c->sim_off);
        if (run_turns(&c->world, c->trace, &c->trace_len, &c->done_turns,
                      c->rec.p + c->sim_off + 4, n, c)) c->stream_errors++;
        c->sim_off += 4u + n;
        simulated = 1;
        if (slow) { c->last_slow_ms = g_now; break; }
    }
    if (simulated && c->done_turns)
        send_ack(c, c->done_turns - 1, TAK_NET_NO_HASH, 0);
    if (!c->watcher && !c->resigned && c->seat < TAK_NET_SEATS &&
        g_now >= c->next_cmd_ms) {
        send_cmd(c);
        c->next_cmd_ms = g_now + 300u + 70u * c->seat;
    }
}

static void deliver(void *user, TAK_ConnId conn, int to_server,
                    const uint8_t *f, size_t n) {
    (void)user;
    if (to_server) {
        if (!f) TAK_Relay_OnClose(&relay, conn, g_now);
        else TAK_Relay_OnFrame(&relay, conn, f, n, g_now);
        return;
    }
    Client *c = client_by_conn(conn);
    if (c && f) bytes_push(&c->inbox, f, n);
}

static Client *new_client(int watcher) {
    Client *c = &cl[ncl];
    free(c->inbox.p);
    free(c->rec.p);
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->alive = 1;
    c->watcher = watcher;
    c->seat = TAK_NET_SEAT_NONE;
    for (int k = 0; k < TAK_NET_TOKEN_BYTES; k++) c->token[k] = (uint8_t)(0x40 + ncl * 7 + k);
    snprintf(c->name, sizeof(c->name), "P%d", ncl);
    ncl++;
    c->conn = TAK_FakeNet_Connect(&net);
    TAK_Relay_OnConnect(&relay, c->conn, g_now);
    send_hello(c, 0);
    return c;
}

/* Back on a fresh connection after a restart: nothing kept but the token. */
static void rejoin(Client *c) {
    c->rejoin_at = 0;
    c->alive = 1;
    c->started = 0;
    c->inbox.len = 0;
    c->in_off = 0;
    c->rec.len = 0;
    c->sim_off = 0;
    c->trace_len = 0;
    c->done_turns = 0;
    c->conn = TAK_FakeNet_Connect(&net);
    TAK_Relay_OnConnect(&relay, c->conn, g_now);
    send_hello(c, 1);
}

static void step(void) {
    TAK_FakeNet_Pump(&net, g_now, deliver, NULL);
    TAK_Relay_Tick(&relay, g_now);
    for (int i = 0; i < ncl; i++) {
        Client *c = &cl[i];
        if (!c->used) continue;
        if (c->rejoin_at && g_now >= c->rejoin_at) rejoin(c);
        client_step(c);
    }
    g_now += STEP_MS;
}

static void run_for(uint64_t ms) {
    uint64_t end = g_now + ms;
    while (g_now < end) step();
}

static void setup(uint32_t latency, uint32_t jitter, uint32_t seed) {
    for (int i = 0; i < N_MAX; i++) { free(cl[i].inbox.p); free(cl[i].rec.p); }
    memset(cl, 0, sizeof(cl));
    ncl = 0;
    g_now = 1000;
    TAK_FakeNet_Free(&net);
    TAK_FakeNetCfg nc;
    memset(&nc, 0, sizeof(nc));
    nc.latency_ms = latency;
    nc.jitter_ms = jitter;
    nc.seed = seed;
    TAK_FakeNet_Init(&net, &nc);
    TAK_RelayCfg rc;
    memset(&rc, 0, sizeof(rc));
    strcpy(rc.server_name, "loopback");
    rc.seed = seed * 2654435761u + 1u;
    TAK_Relay_Init(&relay, &rc, TAK_FakeNet_Server(&net), log_arena, sizeof(log_arena),
                   log_entries, (uint32_t)(sizeof(log_entries) / sizeof(log_entries[0])));
}

/* ── The lobby, as a player would go through it ───────────────────────── */

static int create_room(Client *host, uint32_t flags, uint16_t timeout) {
    TAK_MsgCreateRoom m;
    memset(&m, 0, sizeof(m));
    strcpy(m.name, "Loopback");
    strcpy(m.map_name, "Vain Blessings");
    memcpy(m.map_fingerprint, MAP_FP, sizeof(MAP_FP));
    m.flags = TAK_ROOMF_LISTED | flags;
    m.max_players = TAK_NET_SEATS;
    m.unit_cap = 500;
    m.timeout_secs = timeout;
    up(host, TAK_Msg_CreateRoomEncode(&m, tx, sizeof(tx)));
    run_for(800);
    return host->have_room;
}

static void join_code(Client *c, const char *code, int as_watcher) {
    TAK_MsgJoinRoom j;
    memset(&j, 0, sizeof(j));
    strcpy(j.code, code);
    j.as_watcher = (uint8_t)as_watcher;
    up(c, TAK_Msg_JoinRoomEncode(&j, tx, sizeof(tx)));
}

static void ready_up(int from, int to) {
    for (int i = from; i < to; i++) {
        send_edit(&cl[i], TAK_EDIT_HAVE_MAP, cl[i].seat, 0, MAP_FP);
        send_edit(&cl[i], TAK_EDIT_READY, cl[i].seat, 0, NULL);
    }
    run_for(800);
}

static int all_started(int from, int to) {
    for (int i = from; i < to; i++) if (!cl[i].started) return 0;
    return 1;
}

/* A host creates a room, the rest join by its code, and the match starts.
 * One joiner types the code in lower case, which has to work too. */
static int start_match(int players, uint32_t flags, uint16_t timeout) {
    Client *host = new_client(0);
    if (!create_room(host, flags, timeout)) return 0;
    char code[TAK_NET_CODE_MAX];
    strcpy(code, host->room.code);
    for (int i = 1; i < players; i++) {
        Client *c = new_client(0);
        char typed[TAK_NET_CODE_MAX];
        strcpy(typed, code);
        if (i == 2) for (int k = 0; typed[k]; k++)
            if (typed[k] >= 'A' && typed[k] <= 'Z') typed[k] = (char)(typed[k] + 32);
        join_code(c, typed, 0);
    }
    run_for(1000);
    for (int i = 0; i < players; i++) if (cl[i].seat == TAK_NET_SEAT_NONE) return 0;
    ready_up(0, players);
    up(host, TAK_Msg_EmptyEncode(TAK_MSG_START, tx, sizeof(tx)));
    for (int k = 0; k < 300 && !all_started(0, players); k++) step();
    return all_started(0, players);
}

static TAK_RelayRoom *the_room(void) {
    return TAK_Relay_FindRoom(&relay, cl[0].room.room_id);
}

/* ── Phase two: sequential replay and the comparison ─────────────────── */

static uint64_t rep[N_MAX][TRACE_MAX];

static uint32_t replay(const Client *c, uint64_t *out) {
    ToyWorld w;
    uint32_t done = 0, len = 0;
    toy_reset(&w, c->seed);
    for (size_t off = 0; off < c->rec.len;) {
        uint32_t n = tak_get_u32(c->rec.p + off);
        if (run_turns(&w, out, &len, &done, c->rec.p + off + 4, n, NULL)) return 0;
        off += 4u + n;
    }
    return len;
}

/* Every client's stream, replayed alone on a fresh world, gives the trace
 * it produced live, and every client's trace agrees with every other's.
 * Returns the number of hash ticks they share. */
static int traces_agree(void) {
    uint32_t common = TRACE_MAX;
    int first = -1;
    for (int i = 0; i < ncl; i++) {
        Client *c = &cl[i];
        if (!c->used || c->attacker || !c->started) continue;
        uint32_t len = replay(c, rep[i]);
        uint32_t live = c->trace_len < len ? c->trace_len : len;
        for (uint32_t k = 0; k < live; k++)
            if (rep[i][k] != c->trace[k]) {
                printf("\n    client %d: replay differs from live at hash tick %u\n", i, k);
                return -1;
            }
        if (len < common) common = len;
        if (first < 0) first = i;
    }
    if (first < 0) return -1;
    for (int i = first + 1; i < ncl; i++) {
        if (!cl[i].used || cl[i].attacker || !cl[i].started) continue;
        for (uint32_t k = 0; k < common; k++)
            if (rep[i][k] != rep[first][k]) {
                printf("\n    client %d disagrees with client %d at tick %u\n",
                       i, first, (k + 1) * TAK_NET_HASH_TICKS);
                return -1;
            }
    }
    return (int)common;
}

/* ── Scenarios ────────────────────────────────────────────────────────── */

TEST(eight_clients_agree_under_jitter_and_loss) {
    setup(30, 200, 77);
    ASSERT(start_match(8, TAK_ROOMF_AI_TAKES_OVER, 60));
    /* Faults switch on at GO. The lobby rides a reliable WebSocket, and
     * the loss models what an unreliable path would do to play. */
    net.cfg.loss_permille = 20;
    net.cfg.dup_permille = 10;
    net.cfg.reorder_permille = 10;
    run_for(60000);

    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    ASSERT_EQ_INT(0, (int)rr->clock.desync.count);
    ASSERT(rr->clock.head >= 1000);
    ASSERT(net.lost > 0);
    ASSERT(net.duplicated > 0);
    ASSERT(net.reordered > 0);
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_INT(0, cl[i].stream_errors);
        ASSERT(cl[i].done_turns >= 1000);
    }
    ASSERT(traces_agree() >= 50);
}

TEST(an_injected_desync_is_caught_within_60_ticks_and_the_outlier_named) {
    setup(20, 40, 5);
    ASSERT(start_match(8, TAK_ROOMF_AI_TAKES_OVER, 60));
    cl[5].desync_at_tick = 1234;
    run_for(40000);

    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    int sim5 = TAK_TurnClock_SimOf(&rr->clock, cl[5].session);
    ASSERT(sim5 >= 0);
    ASSERT_EQ_INT(1, (int)rr->clock.desync.count);
    ASSERT_EQ_INT(1260, (int)rr->clock.desync.tick);      /* the next hash tick */
    ASSERT_EQ_INT(1 << sim5, (int)rr->clock.desync.outliers);
    ASSERT_EQ_INT(0, rr->clock.desync.halted);             /* the majority played on */
    ASSERT_EQ_INT(1, cl[5].resets);                        /* and the outlier resynced */
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, rr->clock.sim[sim5].status);
    ASSERT(traces_agree() >= 30);
}

TEST(a_slow_client_engages_the_governor_and_everyone_still_agrees) {
    setup(20, 20, 9);
    ASSERT(start_match(4, TAK_ROOMF_AI_TAKES_OVER, 60));
    cl[3].slow_from = g_now + 5000;
    cl[3].slow_until = g_now + 10000;
    run_for(20000);
    ASSERT(cl[0].waiting_seen);
    ASSERT_EQ_INT(cl[3].seat, cl[0].waiting_seat);
    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    ASSERT_EQ_INT(TAK_NET_TURN_MS, TAK_TurnClock_Period(&rr->clock));  /* released */
    ASSERT_EQ_INT(0, (int)rr->clock.desync.count);
    ASSERT(traces_agree() >= 15);
}

TEST(a_reconnect_by_fast_forward_matches_the_live_trace) {
    setup(25, 50, 11);
    ASSERT(start_match(8, TAK_ROOMF_AI_TAKES_OVER, 60));
    uint8_t seat6 = cl[6].seat;
    cl[6].close_at = g_now + 20000;
    cl[6].rejoin_at = g_now + 24000;         /* inside the countdown */
    run_for(40000);

    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    ASSERT(cl[0].lost_seen[seat6]);
    ASSERT(cl[0].catching_seen[seat6]);
    ASSERT(!cl[0].left_seen[seat6]);         /* back in time, nobody left */
    ASSERT_EQ_INT(seat6, cl[6].seat);
    ASSERT_EQ_INT(0, cl[6].stream_errors);
    int sim6 = TAK_TurnClock_SimOf(&rr->clock, cl[6].session);
    ASSERT(sim6 >= 0);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, rr->clock.sim[sim6].status);
    ASSERT_EQ_INT(0, (int)rr->clock.desync.count);
    ASSERT(traces_agree() >= 30);
}

TEST(a_silent_player_is_replaced_by_the_computer_then_reclaims_the_seat) {
    setup(20, 20, 13);
    ASSERT(start_match(4, TAK_ROOMF_AI_TAKES_OVER, 30));
    uint8_t seat2 = cl[2].seat;
    cl[2].mute_at = g_now + 5000;            /* stops talking, connection open */
    cl[2].rejoin_at = g_now + 45000;         /* after the 30 second countdown */
    run_for(60000);

    ASSERT(cl[0].lost_seen[seat2]);
    ASSERT(cl[0].left_seen[seat2]);
    ASSERT_EQ_INT(TAK_LEFT_COMPUTER_TAKES_OVER, cl[0].left_as[seat2]);
    ASSERT(cl[0].reclaim_seen[seat2]);
    ASSERT(cl[2].reclaim_seen[seat2]);       /* the returning player saw it too */
    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    ASSERT_EQ_INT(0, (int)rr->clock.desync.count);
    /* The match stood still through the countdown, so the comparison has
     * to cover the turns actually played, which is every one of them. */
    ASSERT(traces_agree() >=
           (int)(rr->clock.head * TAK_NET_TURN_TICKS / TAK_NET_HASH_TICKS) - 3);
}

TEST(the_host_role_moves_in_the_lobby_and_in_game) {
    setup(20, 10, 17);
    Client *host = new_client(0);
    ASSERT(create_room(host, TAK_ROOMF_AI_TAKES_OVER, 60));
    char code[TAK_NET_CODE_MAX];
    strcpy(code, host->room.code);
    for (int i = 1; i < 4; i++) join_code(new_client(0), code, 0);
    run_for(1000);

    /* In the lobby: the host leaves, the room stays, seat 1 hosts. */
    up(host, TAK_Msg_EmptyEncode(TAK_MSG_LEAVE_ROOM, tx, sizeof(tx)));
    run_for(800);
    ASSERT_NOT_NULL(TAK_Relay_FindRoom(&relay, cl[1].room.room_id));
    ASSERT_EQ_INT((int)cl[1].session, (int)cl[1].room.host_client_id);
    ASSERT_EQ_INT((int)cl[1].session, (int)cl[3].room.host_client_id);

    /* The new host starts a match with the other two. */
    ready_up(1, 4);
    up(&cl[1], TAK_Msg_EmptyEncode(TAK_MSG_START, tx, sizeof(tx)));
    for (int k = 0; k < 300 && !all_started(1, 4); k++) step();
    ASSERT(all_started(1, 4));
    run_for(3000);

    /* In game: that host's connection drops. The lowest seat still
     * connected hosts now, and the dropped seat keeps its army. */
    cl[1].close_at = g_now;
    run_for(1500);
    TAK_RelayRoom *rr = TAK_Relay_FindRoom(&relay, cl[2].room.room_id);
    ASSERT_NOT_NULL(rr);
    ASSERT_EQ_INT((int)cl[2].session, (int)rr->room.host_client_id);
    ASSERT_EQ_INT((int)cl[2].session, (int)cl[3].room.host_client_id);
    ASSERT_EQ_INT(TAK_NSLOT_HUMAN, rr->room.slot[cl[1].seat].kind);
}

TEST(a_spectator_joins_mid_game_and_catches_up_without_a_pause) {
    setup(20, 30, 19);
    ASSERT(start_match(4, TAK_ROOMF_AI_TAKES_OVER | TAK_ROOMF_ALLOW_WATCHING, 60));
    run_for(20000);
    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    uint32_t head_before = rr->clock.head;
    Client *w = new_client(1);
    join_code(w, cl[0].room.code, 1);
    run_for(20000);

    ASSERT(w->started);
    ASSERT_EQ_INT(0, w->stream_errors);
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, w->seat);
    int ws = TAK_TurnClock_SimOf(&rr->clock, w->session);
    ASSERT(ws >= 0);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, rr->clock.sim[ws].status);
    /* Twenty seconds of play went on while the watcher caught up. */
    ASSERT(rr->clock.head >= head_before + 350);
    ASSERT(w->trace_len >= 35);
    ASSERT(traces_agree() >= 35);
}

TEST(a_resigning_player_leaves_the_game_and_keeps_watching) {
    setup(20, 20, 23);
    ASSERT(start_match(4, TAK_ROOMF_AI_TAKES_OVER, 60));
    uint8_t seat3 = cl[3].seat;
    cl[3].resign_at = g_now + 10000;
    run_for(20000);
    ASSERT(cl[0].left_seen[seat3]);
    ASSERT_EQ_INT(TAK_LEFT_RESIGNED, cl[0].left_as[seat3]);
    TAK_RelayRoom *rr = the_room();
    ASSERT_NOT_NULL(rr);
    int s3 = TAK_TurnClock_SimOf(&rr->clock, cl[3].session);
    ASSERT(s3 >= 0);
    ASSERT_EQ_INT(TAK_NET_SEAT_NONE, rr->clock.sim[s3].seat);
    ASSERT_EQ_INT(TAK_PSTATUS_CONNECTED, rr->clock.sim[s3].status);
    ASSERT(traces_agree() >= 15);
}

TEST(a_malformed_frame_closes_only_its_sender) {
    static uint8_t junk[70000];
    setup(20, 10, 29);
    ASSERT(start_match(2, TAK_ROOMF_AI_TAKES_OVER, 60));
    run_for(2000);

    Client *a[5];
    for (int i = 0; i < 5; i++) { a[i] = new_client(0); a[i]->attacker = 1; }
    run_for(500);
    uint32_t refused = relay.frames_refused;

    /* A frame whose length field promises more than it carries. */
    junk[0] = TAK_MSG_CHAT; tak_put_u16(junk + 1, 20);
    TAK_FakeNet_ClientSend(&net, a[0]->conn, junk, 8, g_now);
    /* One past the frame cap. */
    memset(junk, 0x41, sizeof(junk));
    junk[0] = TAK_MSG_CHAT;
    TAK_FakeNet_ClientSend(&net, a[1]->conn, junk, TAK_NET_FRAME_MAX + 1, g_now);
    /* A type from some future version: skipped, not fatal. */
    junk[0] = 200; tak_put_u16(junk + 1, 4);
    TAK_FakeNet_ClientSend(&net, a[2]->conn, junk, 7, g_now);
    /* A command message claiming 64 commands in one byte. */
    junk[0] = TAK_MSG_CMD; tak_put_u16(junk + 1, 6);
    tak_put_u32(junk + 3, 1); junk[7] = 64; junk[8] = 1;
    TAK_FakeNet_ClientSend(&net, a[3]->conn, junk, 9, g_now);
    /* A message only the server may send. */
    TAK_MsgGo go = { 0 };
    size_t n = TAK_Msg_GoEncode(&go, junk, sizeof(junk));
    TAK_FakeNet_ClientSend(&net, a[4]->conn, junk, n, g_now);
    run_for(2000);

    ASSERT(net.conn[a[0]->conn].closed_by_server);
    ASSERT(net.conn[a[1]->conn].closed_by_server);
    ASSERT(!net.conn[a[2]->conn].closed_by_server);
    ASSERT(net.conn[a[3]->conn].closed_by_server);
    ASSERT(net.conn[a[4]->conn].closed_by_server);
    ASSERT(relay.frames_refused >= refused + 5);
    /* The match never noticed. */
    ASSERT_EQ_INT(0, cl[0].stream_errors);
    ASSERT_EQ_INT(0, cl[1].stream_errors);
    ASSERT(traces_agree() >= 2);
}

int main(void) {
    TEST_SUITE("Relay loopback, eight clients in one process");
    RUN(eight_clients_agree_under_jitter_and_loss);
    RUN(an_injected_desync_is_caught_within_60_ticks_and_the_outlier_named);
    RUN(a_slow_client_engages_the_governor_and_everyone_still_agrees);
    RUN(a_reconnect_by_fast_forward_matches_the_live_trace);
    RUN(a_silent_player_is_replaced_by_the_computer_then_reclaims_the_seat);
    RUN(the_host_role_moves_in_the_lobby_and_in_game);
    RUN(a_spectator_joins_mid_game_and_catches_up_without_a_pause);
    RUN(a_resigning_player_leaves_the_game_and_keeps_watching);
    RUN(a_malformed_frame_closes_only_its_sender);
    TAK_FakeNet_Free(&net);
    for (int i = 0; i < N_MAX; i++) { free(cl[i].inbox.p); free(cl[i].rec.p); }
    TEST_REPORT();
}
