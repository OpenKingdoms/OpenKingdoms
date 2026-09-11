/*
 * relay.c -- connections, rooms and matches behind one transport.
 *
 * Every frame is split and decoded through the bounded codec before it is
 * acted on. A frame that does not parse closes its connection, because a
 * client that sends one is broken or hostile. A type this build does not
 * know is skipped, so an older server tolerates a newer client. The shape
 * is in tak_net_relay.h.
 */

#include "tak_net_relay.h"
#include "tak_bytes.h"

#include <string.h>

/* ── Small helpers ────────────────────────────────────────────────────── */

static uint32_t next_rand(TAK_Relay *r) {
    uint32_t x = r->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->rng = x;
    return x;
}

static TAK_RelayClient *client_by_conn(TAK_Relay *r, TAK_ConnId c) {
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++)
        if (r->client[i].in_use && r->client[i].conn == c) return &r->client[i];
    return NULL;
}

TAK_RelayClient *TAK_Relay_FindClient(TAK_Relay *r, uint32_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++)
        if (r->client[i].in_use && r->client[i].id == id) return &r->client[i];
    return NULL;
}

TAK_RelayRoom *TAK_Relay_FindRoom(TAK_Relay *r, uint32_t room_id) {
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++)
        if (r->room[i].in_use && r->room[i].room.id == room_id) return &r->room[i];
    return NULL;
}

static int room_index(const TAK_Relay *r, const TAK_RelayRoom *rr) {
    return (int)(rr - r->room);
}

static void send_frame(TAK_Relay *r, TAK_RelayClient *cl,
                       const uint8_t *f, size_t n) {
    if (cl && cl->in_use && n) r->tx.send(r->tx.ctx, cl->conn, f, n);
}

static int member_count(const TAK_Relay *r, const TAK_RelayRoom *rr) {
    int idx = room_index(r, rr), n = 0;
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++)
        if (r->client[i].in_use && r->client[i].room == idx) n++;
    return n;
}

static void room_broadcast(TAK_Relay *r, TAK_RelayRoom *rr,
                           const uint8_t *f, size_t n) {
    int idx = room_index(r, rr);
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++)
        if (r->client[i].in_use && r->client[i].room == idx)
            send_frame(r, &r->client[i], f, n);
}

static void send_room_state(TAK_Relay *r, TAK_RelayRoom *rr) {
    TAK_MsgRoomState s;
    TAK_Room_Snapshot(&rr->room, &s);
    room_broadcast(r, rr, r->out, TAK_Msg_RoomStateEncode(&s, r->out, sizeof(r->out)));
}

static void send_reject(TAK_Relay *r, TAK_RelayClient *cl, uint8_t reason,
                        uint8_t detail) {
    TAK_MsgReject m;
    memset(&m, 0, sizeof(m));
    m.reason = reason;
    m.detail = detail;
    strncpy(m.text, TAK_Net_RejectText(reason), TAK_NET_TEXT_MAX - 1);
    send_frame(r, cl, r->out, TAK_Msg_RejectEncode(&m, r->out, sizeof(r->out)));
}

static int token_set(const uint8_t *t) {
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) if (t[i]) return 1;
    return 0;
}

/* The turn clock's way out, into this room's connections. */
static void clock_send(void *user, int sim, const uint8_t *f, size_t n) {
    TAK_RelayRoom *rr = (TAK_RelayRoom *)user;
    TAK_Relay *r = rr->relay;
    if (sim == TAK_TURN_SIM_ALL) { room_broadcast(r, rr, f, n); return; }
    TAK_RelayClient *cl = TAK_Relay_FindClient(r, rr->clock.sim[sim].client_id);
    if (cl && cl->room == room_index(r, rr)) send_frame(r, cl, f, n);
}

/* ── Leaving and closing ──────────────────────────────────────────────── */

static void free_room(TAK_Relay *r, TAK_RelayRoom *rr, uint8_t tell) {
    int idx = room_index(r, rr);
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++) {
        TAK_RelayClient *cl = &r->client[i];
        if (!cl->in_use || cl->room != idx) continue;
        if (tell) send_reject(r, cl, tell, 0);
        cl->room = -1;
    }
    rr->in_use = 0;
}

static void leave_room(TAK_Relay *r, TAK_RelayClient *cl, int on_purpose) {
    TAK_RelayRoom *rr = &r->room[cl->room];
    uint32_t id = cl->id;
    cl->room = -1;

    if (rr->room.status == TAK_ROOM_IN_PROGRESS) {
        int sim = TAK_TurnClock_SimOf(&rr->clock, id);
        if (sim >= 0) {
            uint8_t seat = rr->clock.sim[sim].seat;
            TAK_TurnClock_Disconnect(&rr->clock, sim, r->now);
            /* Leaving on purpose skips the countdown, which exists for a
             * connection that dropped by accident. */
            if (on_purpose && seat != TAK_NET_SEAT_NONE)
                (void)TAK_TurnClock_Reject(&rr->clock, seat, r->now);
        }
        if (TAK_Room_IsWatcher(&rr->room, id)) TAK_Room_Leave(&rr->room, id, NULL);
        else TAK_Room_SetConnected(&rr->room, id, 0);
        send_room_state(r, rr);
        return;
    }

    /* Leaving while the worlds load abandons the start. */
    if (rr->room.status == TAK_ROOM_LOADING) TAK_Room_Abort(&rr->room);

    TAK_RoomLeave lv;
    TAK_Room_Leave(&rr->room, id, &lv);
    if (lv.room_finished) {
        /* No human left to host. Whoever is still watching is told the
         * way the original told them. */
        free_room(r, rr, TAK_REJECT_CREATOR_LEFT);
        return;
    }
    send_room_state(r, rr);
}

static void forget_client(TAK_Relay *r, TAK_RelayClient *cl) {
    if (cl->room >= 0) leave_room(r, cl, 0);
    memset(cl, 0, sizeof(*cl));
    cl->room = -1;
}

/* Close from our side, for a broken or refused client. */
static void drop_client(TAK_Relay *r, TAK_RelayClient *cl) {
    r->clients_dropped++;
    r->tx.close(r->tx.ctx, cl->conn);
    forget_client(r, cl);
}

/* ── Setup and connections ────────────────────────────────────────────── */

void TAK_Relay_Init(TAK_Relay *r, const TAK_RelayCfg *cfg, TAK_NetTransport tx,
                    uint8_t *log_arena, size_t arena_bytes,
                    TAK_TurnLogEntry *log_entries, uint32_t entry_count) {
    memset(r, 0, sizeof(*r));
    if (cfg) r->cfg = *cfg;
    r->tx = tx;
    r->rng = r->cfg.seed ? r->cfg.seed : 0x6d2b79f5u;
    size_t arena_each = arena_bytes / TAK_RELAY_ROOMS_MAX;
    uint32_t entries_each = entry_count / TAK_RELAY_ROOMS_MAX;
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++) {
        TAK_RelayRoom *rr = &r->room[i];
        rr->relay = r;
        rr->log_arena = log_arena ? log_arena + (size_t)i * arena_each : NULL;
        rr->log_arena_cap = arena_each;
        rr->log_entries = log_entries ? log_entries + (size_t)i * entries_each : NULL;
        rr->log_entry_cap = entries_each;
    }
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++) r->client[i].room = -1;
}

void TAK_Relay_OnConnect(TAK_Relay *r, TAK_ConnId conn, uint64_t now_ms) {
    r->now = now_ms;
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++) {
        TAK_RelayClient *cl = &r->client[i];
        if (cl->in_use) continue;
        memset(cl, 0, sizeof(*cl));
        cl->in_use = 1;
        cl->conn = conn;
        cl->room = -1;
        cl->opened_ms = now_ms;
        return;
    }
    r->tx.close(r->tx.ctx, conn);   /* full */
}

void TAK_Relay_OnClose(TAK_Relay *r, TAK_ConnId conn, uint64_t now_ms) {
    r->now = now_ms;
    TAK_RelayClient *cl = client_by_conn(r, conn);
    if (cl) forget_client(r, cl);
}

/* ── The match ────────────────────────────────────────────────────────── */

static void send_start_game(TAK_Relay *r, TAK_RelayRoom *rr, TAK_RelayClient *cl) {
    TAK_MsgStartGame m;
    const TAK_Room *room = &rr->room;
    memset(&m, 0, sizeof(m));
    m.match_id = rr->match_id;
    m.seed = rr->seed;
    m.your_seat = TAK_Room_SeatOf(room, cl->id);
    m.turn_ticks = TAK_NET_TURN_TICKS;
    m.schema_hash = rr->schema_hash;
    m.content_hash = rr->content_hash;
    memcpy(m.map_name, room->cfg.map_name, TAK_NET_MAP_NAME_MAX);
    memcpy(m.map_fingerprint, room->cfg.map_fingerprint, TAK_NET_FINGERPRINT_BYTES);
    m.options = room->cfg.options;
    m.unit_cap = room->cfg.unit_cap;
    m.timeout_secs = room->cfg.timeout_secs;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        m.slot[i].kind = room->slot[i].kind;
        m.slot[i].side = room->slot[i].side;
        m.slot[i].colour = room->slot[i].colour;
        m.slot[i].team = room->slot[i].team;
        memcpy(m.slot[i].name, room->slot[i].name, TAK_NET_NAME_MAX);
    }
    send_frame(r, cl, r->out, TAK_Msg_StartGameEncode(&m, r->out, sizeof(r->out)));
}

static void send_load_state(TAK_Relay *r, TAK_RelayRoom *rr) {
    TAK_MsgLoadState m;
    memset(&m, 0, sizeof(m));
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        const TAK_NetSlot *s = &rr->room.slot[i];
        if (s->kind != TAK_NSLOT_HUMAN) continue;
        m.entry[m.count].seat = (uint8_t)i;
        m.entry[m.count].percent = s->load_percent;
        m.entry[m.count].loaded = (uint8_t)((s->flags & TAK_SLOTF_LOADED) ? 1 : 0);
        m.count++;
    }
    room_broadcast(r, rr, r->out, TAK_Msg_LoadStateEncode(&m, r->out, sizeof(r->out)));
}

static void begin_match(TAK_Relay *r, TAK_RelayRoom *rr) {
    rr->match_id = ++r->next_match_id;
    rr->seed = next_rand(r);
    rr->loaded = 0;
    memset(rr->world_hash, 0, sizeof(rr->world_hash));
    memset(rr->sim_token, 0, sizeof(rr->sim_token));
    TAK_TurnLog_Init(&rr->log, rr->log_arena, rr->log_arena_cap,
                     rr->log_entries, rr->log_entry_cap);

    TAK_TurnClockCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.turn_ms = TAK_NET_TURN_MS;
    cfg.speed = TAK_NET_SPEED_NORMAL;
    cfg.timeout_secs = rr->room.cfg.timeout_secs;
    cfg.left_as = (rr->room.cfg.flags & TAK_ROOMF_AI_TAKES_OVER)
                ? TAK_LEFT_COMPUTER_TAKES_OVER : TAK_LEFT_ARMY_REMOVED;
    cfg.host_paces_only = (uint8_t)((rr->room.cfg.flags & TAK_ROOMF_HOST_PACES_ONLY) ? 1 : 0);
    TAK_TurnClock_Init(&rr->clock, &cfg, &rr->log, clock_send, rr);

    int idx = room_index(r, rr);
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++) {
        TAK_RelayClient *cl = &r->client[i];
        if (!cl->in_use || cl->room != idx) continue;
        uint8_t seat = TAK_Room_SeatOf(&rr->room, cl->id);
        int sim = TAK_TurnClock_AddSim(&rr->clock, cl->id, seat, r->now);
        if (sim >= 0)
            memcpy(rr->sim_token[sim], cl->hello.device_token, TAK_NET_TOKEN_BYTES);
        send_start_game(r, rr, cl);
    }
}

/* Every world is built. The host's is the reference, because the host's
 * data is what the room was made with. A world that differs is a data
 * mismatch caught before the first tick, while it is still an error
 * message rather than a desync ten minutes in. */
static void worlds_loaded(TAK_Relay *r, TAK_RelayRoom *rr) {
    int host_sim = TAK_TurnClock_SimOf(&rr->clock, rr->room.host_client_id);
    if (host_sim < 0) return;
    uint64_t reference = rr->world_hash[host_sim];
    uint32_t bad = 0;
    for (int s = 0; s < TAK_TURN_SIMS_MAX; s++)
        if (rr->clock.sim[s].in_use && rr->world_hash[s] != reference) bad |= 1u << s;
    if (bad) {
        for (int s = 0; s < TAK_TURN_SIMS_MAX; s++) {
            if (!(bad & (1u << s))) continue;
            TAK_RelayClient *cl = TAK_Relay_FindClient(r, rr->clock.sim[s].client_id);
            if (cl) send_reject(r, cl, TAK_REJECT_DATA_MISMATCH, TAK_NET_SEAT_NONE);
        }
        TAK_Room_Abort(&rr->room);
        send_room_state(r, rr);
        return;
    }
    TAK_Room_Go(&rr->room);
    send_room_state(r, rr);
    TAK_TurnClock_Start(&rr->clock, r->now);
}

/* A returning player, recognised by the device token they joined with. */
static TAK_RelayRoom *find_rejoin(TAK_Relay *r, const uint8_t *token, int *out_sim) {
    if (!token_set(token)) return NULL;
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++) {
        TAK_RelayRoom *rr = &r->room[i];
        if (!rr->in_use || rr->room.status != TAK_ROOM_IN_PROGRESS) continue;
        for (int s = 0; s < TAK_TURN_SIMS_MAX; s++) {
            if (!rr->clock.sim[s].in_use) continue;
            if (memcmp(rr->sim_token[s], token, TAK_NET_TOKEN_BYTES) != 0) continue;
            *out_sim = s;
            return rr;
        }
    }
    return NULL;
}

/* ── Handlers ─────────────────────────────────────────────────────────── */

static void on_hello(TAK_Relay *r, TAK_RelayClient *cl, const TAK_MsgHello *h) {
    if (h->protocol_version < TAK_NET_PROTOCOL_MIN ||
        h->protocol_version > TAK_NET_PROTOCOL_VERSION) {
        send_reject(r, cl, TAK_REJECT_PROTOCOL_VERSION, 0);
        drop_client(r, cl);
        return;
    }
    if (r->cfg.access_key[0] &&
        !TAK_Net_SecretEqual(r->cfg.access_key, h->access_key, TAK_NET_KEY_MAX)) {
        send_reject(r, cl, TAK_REJECT_SERVER_PASSWORD, 0);
        drop_client(r, cl);
        return;
    }
    if (!h->name[0]) {
        send_reject(r, cl, TAK_REJECT_NAME_REQUIRED, 0);
        drop_client(r, cl);
        return;
    }
    cl->hello = *h;
    cl->welcomed = 1;
    cl->id = ++r->next_client_id;

    int sim = -1;
    TAK_RelayRoom *back = (h->flags & TAK_HELLOF_WANTS_REJOIN)
                        ? find_rejoin(r, h->device_token, &sim) : NULL;
    if (back) {
        /* Take over the old session, closing a stale connection first. */
        uint32_t old_id = back->clock.sim[sim].client_id;
        TAK_RelayClient *old = TAK_Relay_FindClient(r, old_id);
        if (old && old != cl) {
            r->tx.close(r->tx.ctx, old->conn);
            memset(old, 0, sizeof(*old));
            old->room = -1;
        }
        cl->id = old_id;
        cl->room = room_index(r, back);
    }

    TAK_MsgWelcome w;
    memset(&w, 0, sizeof(w));
    w.session_id = cl->id;
    w.flags = r->cfg.flags;
    w.protocol_min = TAK_NET_PROTOCOL_MIN;
    w.protocol_max = TAK_NET_PROTOCOL_VERSION;
    w.newest_build = r->cfg.newest_build;
    memcpy(w.server_name, r->cfg.server_name, TAK_NET_SERVER_NAME_MAX);
    memcpy(w.motd, r->cfg.motd, TAK_NET_MOTD_MAX);
    send_frame(r, cl, r->out, TAK_Msg_WelcomeEncode(&w, r->out, sizeof(r->out)));

    if (back) {
        send_start_game(r, back, cl);
        TAK_Room_SetConnected(&back->room, cl->id, 1);
        if (TAK_TurnClock_Reconnect(&back->clock, sim, 0, r->now) != 0) {
            cl->room = -1;
            send_reject(r, cl, TAK_REJECT_GAME_CLOSED, 0);
            return;
        }
        send_room_state(r, back);
    }
}

static void on_list(TAK_Relay *r, TAK_RelayClient *cl) {
    TAK_MsgRoomList m;
    memset(&m, 0, sizeof(m));
    m.flags = TAK_ROOMLISTF_FULL;
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX && m.count < TAK_NET_ROOMS_PER_LIST; i++) {
        TAK_RelayRoom *rr = &r->room[i];
        if (!rr->in_use || !(rr->room.cfg.flags & TAK_ROOMF_LISTED)) continue;
        TAK_Room_Summary(&rr->room, cl->hello.engine_build_id,
                         cl->hello.determinism_class, &m.room[m.count]);
        /* A data mismatch greys the row too, with the reason. */
        if (!m.room[m.count].compat &&
            (cl->hello.content_hash != rr->content_hash ||
             cl->hello.schema_hash != rr->schema_hash))
            m.room[m.count].compat = TAK_REJECT_DATA_MISMATCH;
        m.count++;
    }
    send_frame(r, cl, r->out, TAK_Msg_RoomListEncode(&m, r->out, sizeof(r->out)));
}

static int code_in_use(const TAK_Relay *r, const char *code) {
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++)
        if (r->room[i].in_use && strcmp(r->room[i].room.code, code) == 0) return 1;
    return 0;
}

static void on_create(TAK_Relay *r, TAK_RelayClient *cl, const TAK_MsgCreateRoom *m) {
    if (cl->room >= 0) { send_reject(r, cl, TAK_REJECT_NOT_ALLOWED, 0); return; }
    TAK_RelayRoom *rr = NULL;
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++)
        if (!r->room[i].in_use) { rr = &r->room[i]; break; }
    if (!rr) { send_reject(r, cl, TAK_REJECT_RATE_LIMITED, 0); return; }

    TAK_RoomCfg c;
    memset(&c, 0, sizeof(c));
    memcpy(c.name, m->name, sizeof(c.name));
    memcpy(c.password, m->password, sizeof(c.password));
    memcpy(c.map_name, m->map_name, sizeof(c.map_name));
    memcpy(c.map_fingerprint, m->map_fingerprint, sizeof(c.map_fingerprint));
    c.flags = m->flags;
    c.options = m->options;
    c.max_players = m->max_players;
    c.unit_cap = m->unit_cap;
    c.timeout_secs = m->timeout_secs;

    /* Invite codes are drawn until one is not already live. */
    uint32_t id = ++r->next_room_id;
    for (int tries = 0; tries < 16; tries++) {
        TAK_Room_Init(&rr->room, id, next_rand(r), &c, cl->id, cl->hello.name,
                      cl->hello.engine_build_id, cl->hello.determinism_class);
        if (!code_in_use(r, rr->room.code)) break;
    }
    rr->in_use = 1;
    rr->relay = r;
    rr->schema_hash = cl->hello.schema_hash;
    rr->content_hash = cl->hello.content_hash;
    memcpy(rr->group_hash, cl->hello.group_hash, sizeof(rr->group_hash));
    cl->room = room_index(r, rr);
    send_room_state(r, rr);
}

static TAK_RelayRoom *room_by_code(TAK_Relay *r, const char *code) {
    char up[TAK_NET_CODE_MAX];
    for (int i = 0; i < TAK_NET_CODE_MAX; i++) {
        char ch = code[i];
        up[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        if (!ch) break;
    }
    up[TAK_NET_CODE_MAX - 1] = '\0';
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++)
        if (r->room[i].in_use && strcmp(r->room[i].room.code, up) == 0) return &r->room[i];
    return NULL;
}

static void on_join(TAK_Relay *r, TAK_RelayClient *cl, const TAK_MsgJoinRoom *m) {
    if (cl->room >= 0) { send_reject(r, cl, TAK_REJECT_NOT_ALLOWED, 0); return; }
    TAK_RelayRoom *rr = m->room_id ? TAK_Relay_FindRoom(r, m->room_id)
                                   : room_by_code(r, m->code);
    if (!rr) { send_reject(r, cl, TAK_REJECT_NO_SUCH_ROOM, 0); return; }

    /* The password first, so a stranger learns nothing else. */
    if (!TAK_Net_SecretEqual(rr->room.cfg.password, m->password, TAK_NET_PASSWORD_MAX)) {
        send_reject(r, cl, TAK_REJECT_WRONG_PASSWORD, 0);
        return;
    }
    /* Then the data, naming the group that differs. */
    if (cl->hello.schema_hash != rr->schema_hash ||
        cl->hello.content_hash != rr->content_hash) {
        uint8_t group = TAK_NET_SEAT_NONE;
        for (int g = 0; g < TAK_NET_GROUP_HASHES; g++)
            if (cl->hello.group_hash[g] != rr->group_hash[g]) { group = (uint8_t)g; break; }
        send_reject(r, cl, TAK_REJECT_DATA_MISMATCH, group);
        return;
    }
    uint8_t seat = TAK_NET_SEAT_NONE;
    int rc = TAK_Room_Join(&rr->room, cl->id, cl->hello.name, m->password,
                           m->as_watcher, cl->hello.engine_build_id,
                           cl->hello.determinism_class, &seat);
    if (rc) { send_reject(r, cl, (uint8_t)rc, 0); return; }
    cl->room = room_index(r, rr);
    send_room_state(r, rr);

    if (rr->room.status == TAK_ROOM_IN_PROGRESS) {
        /* A watcher arriving mid game builds the world and replays the
         * log, without pausing anyone. */
        int sim = TAK_TurnClock_AddSim(&rr->clock, cl->id, TAK_NET_SEAT_NONE, r->now);
        if (sim < 0) { send_reject(r, cl, TAK_REJECT_GAME_FULL, 0); return; }
        memcpy(rr->sim_token[sim], cl->hello.device_token, TAK_NET_TOKEN_BYTES);
        send_start_game(r, rr, cl);
        (void)TAK_TurnClock_Reconnect(&rr->clock, sim, 0, r->now);
    }
}

static void on_edit(TAK_Relay *r, TAK_RelayClient *cl, const TAK_MsgRoomEdit *m) {
    if (cl->room < 0) { send_reject(r, cl, TAK_REJECT_NOT_ALLOWED, 0); return; }
    TAK_RelayRoom *rr = &r->room[cl->room];

    if (rr->room.status == TAK_ROOM_IN_PROGRESS) {
        int rc = TAK_REJECT_GAME_CLOSED;
        if (m->field == TAK_EDIT_WATCH) {
            /* Resigning, then staying on to watch as a defeated player. */
            rc = TAK_TurnClock_Resign(&rr->clock, TAK_TurnClock_SimOf(&rr->clock, cl->id));
        } else if (m->field == TAK_EDIT_KICK) {
            /* The host rejecting a lost player before the countdown ends. */
            rc = (cl->id == rr->room.host_client_id)
               ? TAK_TurnClock_Reject(&rr->clock, m->seat, r->now)
               : TAK_REJECT_NOT_ALLOWED;
        }
        if (rc) send_reject(r, cl, (uint8_t)rc, 0);
        return;
    }

    TAK_RoomEffect fx;
    int rc = TAK_Room_Edit(&rr->room, cl->id, m, &fx);
    if (rc) { send_reject(r, cl, (uint8_t)rc, 0); return; }
    if (fx.removed_client_id) {
        TAK_RelayClient *gone = TAK_Relay_FindClient(r, fx.removed_client_id);
        TAK_Room_Leave(&rr->room, fx.removed_client_id, NULL);
        if (gone) {
            send_reject(r, gone, TAK_REJECT_REMOVED_BY_HOST, 0);
            gone->room = -1;
        }
    }
    send_room_state(r, rr);
}

static void on_chat(TAK_Relay *r, TAK_RelayClient *cl, TAK_MsgChat *m) {
    if (cl->room < 0 || m->scope == TAK_CHAT_SYSTEM) return;
    TAK_RelayRoom *rr = &r->room[cl->room];
    uint8_t seat = TAK_Room_SeatOf(&rr->room, cl->id);
    int watcher = TAK_Room_IsWatcher(&rr->room, cl->id);
    if (!watcher && rr->room.status == TAK_ROOM_IN_PROGRESS) {
        /* A player who resigned stays on as a watcher. */
        int sim = TAK_TurnClock_SimOf(&rr->clock, cl->id);
        if (sim >= 0 && rr->clock.sim[sim].seat == TAK_NET_SEAT_NONE) watcher = 1;
    }
    memcpy(m->name, cl->hello.name, TAK_NET_NAME_MAX);
    m->from_seat = watcher ? TAK_NET_SEAT_NONE : seat;
    m->turn = rr->clock.started ? rr->clock.head : 0;
    /* Watcher chat reaches only watchers, as in the original. */
    if (watcher) m->scope = TAK_CHAT_WATCHERS;
    size_t n = TAK_Msg_ChatEncode(m, r->out, sizeof(r->out));

    int idx = room_index(r, rr);
    for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++) {
        TAK_RelayClient *to = &r->client[i];
        if (!to->in_use || to->room != idx) continue;
        uint8_t to_seat = TAK_Room_SeatOf(&rr->room, to->id);
        int to_watcher = (to_seat == TAK_NET_SEAT_NONE);
        int deliver = 0;
        switch (m->scope) {
        case TAK_CHAT_ROOM:
        case TAK_CHAT_ALL:      deliver = 1; break;
        case TAK_CHAT_WATCHERS: deliver = to_watcher || to == cl; break;
        case TAK_CHAT_TEAM:
            deliver = to == cl || (!to_watcher && seat != TAK_NET_SEAT_NONE &&
                                   rr->room.slot[to_seat].team == rr->room.slot[seat].team);
            break;
        case TAK_CHAT_DIRECT:   deliver = to == cl || to_seat == m->to_seat; break;
        default:                deliver = 0; break;
        }
        if (deliver) send_frame(r, to, r->out, n);
    }
}

static void on_start(TAK_Relay *r, TAK_RelayClient *cl) {
    if (cl->room < 0) { send_reject(r, cl, TAK_REJECT_NOT_ALLOWED, 0); return; }
    TAK_RelayRoom *rr = &r->room[cl->room];
    int rc = TAK_Room_Start(&rr->room, cl->id);
    if (rc) { send_reject(r, cl, (uint8_t)rc, 0); return; }
    send_room_state(r, rr);
    begin_match(r, rr);
}

static void on_load_progress(TAK_Relay *r, TAK_RelayClient *cl, uint8_t percent) {
    if (cl->room < 0) return;
    TAK_RelayRoom *rr = &r->room[cl->room];
    if (rr->room.status != TAK_ROOM_LOADING) return;
    uint8_t seat = TAK_Room_SeatOf(&rr->room, cl->id);
    if (seat == TAK_NET_SEAT_NONE) return;
    rr->room.slot[seat].load_percent = percent > 100 ? 100 : percent;
    send_load_state(r, rr);
}

static void on_loaded(TAK_Relay *r, TAK_RelayClient *cl, uint64_t world_hash) {
    if (cl->room < 0) return;
    TAK_RelayRoom *rr = &r->room[cl->room];
    if (rr->room.status != TAK_ROOM_LOADING) return;
    int sim = TAK_TurnClock_SimOf(&rr->clock, cl->id);
    if (sim < 0) return;
    rr->world_hash[sim] = world_hash;
    rr->loaded |= 1u << sim;
    uint8_t seat = TAK_Room_SeatOf(&rr->room, cl->id);
    if (seat != TAK_NET_SEAT_NONE) {
        rr->room.slot[seat].load_percent = 100;
        rr->room.slot[seat].flags |= TAK_SLOTF_LOADED;
    }
    send_load_state(r, rr);
    uint32_t want = 0;
    for (int s = 0; s < TAK_TURN_SIMS_MAX; s++)
        if (rr->clock.sim[s].in_use) want |= 1u << s;
    if ((rr->loaded & want) == want) worlds_loaded(r, rr);
}

static TAK_RelayRoom *match_of(TAK_Relay *r, TAK_RelayClient *cl, int *sim) {
    if (cl->room < 0) return NULL;
    TAK_RelayRoom *rr = &r->room[cl->room];
    if (rr->room.status != TAK_ROOM_IN_PROGRESS) return NULL;
    *sim = TAK_TurnClock_SimOf(&rr->clock, cl->id);
    return *sim >= 0 ? rr : NULL;
}

static void on_pace(TAK_Relay *r, TAK_RelayClient *cl, const TAK_MsgPace *m) {
    int sim = -1;
    TAK_RelayRoom *rr = match_of(r, cl, &sim);
    if (!rr) return;
    /* A PACE from a client is a request. The server decides and tells
     * everyone, so pause and speed never reach a simulation. */
    uint8_t host_seat = TAK_Room_SeatOf(&rr->room, rr->room.host_client_id);
    int rc = 0;
    if (m->paused != rr->clock.paused)
        rc = TAK_TurnClock_SetPaused(&rr->clock, sim, m->paused, host_seat, r->now);
    if (!rc && m->speed_level && m->speed_level != rr->clock.cfg.speed)
        rc = TAK_TurnClock_SetSpeed(&rr->clock, sim, m->speed_level, host_seat, r->now);
    if (rc) send_reject(r, cl, (uint8_t)rc, 0);
}

/* ── Dispatch ─────────────────────────────────────────────────────────── */

void TAK_Relay_OnFrame(TAK_Relay *r, TAK_ConnId conn,
                       const uint8_t *frame, size_t len, uint64_t now_ms) {
    r->now = now_ms;
    r->frames_in++;
    TAK_RelayClient *cl = client_by_conn(r, conn);
    if (!cl) return;

    TAK_NetFrame f;
    if (TAK_Net_Split(frame, len, &f) != 0) {
        r->frames_refused++;
        drop_client(r, cl);
        return;
    }
    if (!cl->welcomed && f.type != TAK_MSG_HELLO && f.type != TAK_MSG_PING) {
        r->frames_refused++;
        drop_client(r, cl);
        return;
    }

    /* Anything at all from a player in a match counts as being heard. */
    int sim = -1;
    TAK_RelayRoom *mr = cl->welcomed ? match_of(r, cl, &sim) : NULL;
    if (mr) TAK_TurnClock_Heard(&mr->clock, sim, now_ms);

    const uint8_t *p = f.payload;
    size_t n = f.payload_len;
    int bad = 0;
    switch (f.type) {
    case TAK_MSG_HELLO: {
        TAK_MsgHello m;
        if (cl->welcomed || TAK_Msg_HelloDecode(&m, p, n)) { bad = 1; break; }
        on_hello(r, cl, &m);
        return;                       /* on_hello may drop the client */
    }
    case TAK_MSG_PING: {
        TAK_MsgPing m;
        if (TAK_Msg_PingDecode(&m, p, n)) { bad = 1; break; }
        send_frame(r, cl, r->out, TAK_Msg_PingEncode(TAK_MSG_PONG, &m, r->out, sizeof(r->out)));
        break;
    }
    case TAK_MSG_PONG: {
        TAK_MsgPing m;
        if (TAK_Msg_PingDecode(&m, p, n)) { bad = 1; break; }
        if (cl->room >= 0 && m.sent_ms <= now_ms) {
            uint64_t rtt = now_ms - m.sent_ms;
            TAK_Room_SetPing(&r->room[cl->room].room, cl->id,
                             (uint16_t)(rtt > 0xffffu ? 0xffffu : rtt));
        }
        break;
    }
    case TAK_MSG_LIST_ROOMS: {
        TAK_MsgListRooms m;
        if (TAK_Msg_ListRoomsDecode(&m, p, n)) { bad = 1; break; }
        on_list(r, cl);
        break;
    }
    case TAK_MSG_CREATE_ROOM: {
        TAK_MsgCreateRoom m;
        if (TAK_Msg_CreateRoomDecode(&m, p, n)) { bad = 1; break; }
        on_create(r, cl, &m);
        break;
    }
    case TAK_MSG_JOIN_ROOM: {
        TAK_MsgJoinRoom m;
        if (TAK_Msg_JoinRoomDecode(&m, p, n)) { bad = 1; break; }
        on_join(r, cl, &m);
        break;
    }
    case TAK_MSG_LEAVE_ROOM:
        if (n) { bad = 1; break; }
        if (cl->room >= 0) leave_room(r, cl, 1);
        break;
    case TAK_MSG_ROOM_EDIT: {
        TAK_MsgRoomEdit m;
        if (TAK_Msg_RoomEditDecode(&m, p, n)) { bad = 1; break; }
        on_edit(r, cl, &m);
        break;
    }
    case TAK_MSG_CHAT: {
        TAK_MsgChat m;
        if (TAK_Msg_ChatDecode(&m, p, n)) { bad = 1; break; }
        on_chat(r, cl, &m);
        break;
    }
    case TAK_MSG_START:
        if (n) { bad = 1; break; }
        on_start(r, cl);
        break;
    case TAK_MSG_LOAD_PROGRESS: {
        TAK_MsgLoadProgress m;
        if (TAK_Msg_LoadProgressDecode(&m, p, n)) { bad = 1; break; }
        on_load_progress(r, cl, m.percent);
        break;
    }
    case TAK_MSG_LOADED: {
        TAK_MsgLoaded m;
        if (TAK_Msg_LoadedDecode(&m, p, n)) { bad = 1; break; }
        on_loaded(r, cl, m.world_hash);
        break;
    }
    case TAK_MSG_CMD: {
        TAK_MsgCmd m;
        if (TAK_Msg_CmdDecode(&m, p, n)) { bad = 1; break; }
        if (!mr) { send_reject(r, cl, TAK_REJECT_GAME_CLOSED, 0); break; }
        int rc = TAK_TurnClock_Command(&mr->clock, sim, &m);
        if (rc) send_reject(r, cl, (uint8_t)rc, 0);
        break;
    }
    case TAK_MSG_ACK: {
        TAK_MsgAck m;
        if (TAK_Msg_AckDecode(&m, p, n)) { bad = 1; break; }
        if (mr && TAK_TurnClock_Ack(&mr->clock, sim, &m, now_ms) != 0) r->frames_refused++;
        break;
    }
    case TAK_MSG_PACE: {
        TAK_MsgPace m;
        if (TAK_Msg_PaceDecode(&m, p, n)) { bad = 1; break; }
        on_pace(r, cl, &m);
        break;
    }
    /* Server to client only. A client sending one of these is broken. */
    case TAK_MSG_WELCOME: case TAK_MSG_REJECT: case TAK_MSG_ROOM_LIST:
    case TAK_MSG_ROOM_STATE: case TAK_MSG_START_GAME: case TAK_MSG_LOAD_STATE:
    case TAK_MSG_GO: case TAK_MSG_TURN: case TAK_MSG_PLAYER_STATUS:
        bad = 1;
        break;
    default:
        r->frames_refused++;          /* newer than us: skipped by length */
        break;
    }
    if (bad) {
        r->frames_refused++;
        drop_client(r, cl);
    }
}

/* ── Time ─────────────────────────────────────────────────────────────── */

void TAK_Relay_Tick(TAK_Relay *r, uint64_t now_ms) {
    r->now = now_ms;
    for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++) {
        TAK_RelayRoom *rr = &r->room[i];
        if (!rr->in_use || rr->room.status != TAK_ROOM_IN_PROGRESS) continue;
        TAK_TurnClock_Advance(&rr->clock, now_ms);
        /* A match nobody can come back to is over. */
        int live = 0;
        for (int s = 0; s < TAK_TURN_SIMS_MAX; s++)
            if (rr->clock.sim[s].in_use && rr->clock.sim[s].status != TAK_PSTATUS_DROPPED)
                live = 1;
        if (!live && member_count(r, rr) == 0) rr->in_use = 0;
    }

    if (now_ms >= r->next_ping_ms) {
        r->next_ping_ms = now_ms + TAK_RELAY_PING_MS;
        for (int i = 0; i < TAK_RELAY_CLIENTS_MAX; i++) {
            TAK_RelayClient *cl = &r->client[i];
            if (!cl->in_use) continue;
            if (!cl->welcomed) {
                if (now_ms - cl->opened_ms >= TAK_RELAY_HELLO_MS) drop_client(r, cl);
                continue;
            }
            TAK_MsgPing m;
            m.seq = ++cl->ping_seq;
            m.sent_ms = now_ms;
            send_frame(r, cl, r->out, TAK_Msg_PingEncode(TAK_MSG_PING, &m, r->out, sizeof(r->out)));
        }
        /* The Ping column refreshes with the heartbeat. */
        for (int i = 0; i < TAK_RELAY_ROOMS_MAX; i++)
            if (r->room[i].in_use && r->room[i].room.status == TAK_ROOM_OPEN)
                send_room_state(r, &r->room[i]);
    }
}
