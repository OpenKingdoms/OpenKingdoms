/*
 * room.c -- the battle room's rules, with nothing underneath them.
 *
 * No sockets, no clock, no allocation. Every entry point either applies a
 * change and bumps the revision, or refuses with a protocol reject reason
 * and leaves the room exactly as it was.
 *
 * The rules and the deviations from the original are in tak_net_room.h.
 */

#include "tak_net_room.h"
#include "tak_battle_config.h"

#include <string.h>

/* ── Small helpers ──────────────────────────────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int slot_occupied(const TAK_NetSlot *s) {
    return s->kind == TAK_NSLOT_HUMAN || s->kind == TAK_NSLOT_COMPUTER;
}

static void touch(TAK_Room *r) { r->revision++; }

static int fingerprint_set(const uint8_t *fp) {
    for (int i = 0; i < TAK_NET_FINGERPRINT_BYTES; i++) if (fp[i]) return 1;
    return 0;
}

/* A new map: nobody has confirmed it yet except the host, whose client
 * computed the fingerprint the room now carries. */
static void reset_map_flags(TAK_Room *r) {
    for (int i = 0; i < TAK_NET_SEATS; i++)
        r->slot[i].flags = (uint8_t)(r->slot[i].flags & ~TAK_SLOTF_HAS_MAP);
    uint8_t host = TAK_Room_SeatOf(r, r->host_client_id);
    if (host != TAK_NET_SEAT_NONE && fingerprint_set(r->cfg.map_fingerprint))
        r->slot[host].flags |= TAK_SLOTF_HAS_MAP;
}

/* Ready belongs to people. A computer player is always ready. */
static void clear_all_ready(TAK_Room *r, TAK_RoomEffect *fx) {
    for (int i = 0; i < TAK_NET_SEATS; i++)
        if (r->slot[i].kind == TAK_NSLOT_HUMAN) r->slot[i].ready = 0;
    if (fx) fx->ready_cleared = 1;
}

/* ── Invite codes ───────────────────────────────────────────────────── */

void TAK_Room_MakeCode(uint32_t seed, char out[TAK_NET_CODE_MAX]) {
    static const char alphabet[] = TAK_ROOM_CODE_ALPHABET;
    /* Mix first, so room 1 and room 2 do not differ by one character. */
    uint32_t x = seed + 0x9e3779b9u;
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    for (int i = 0; i < TAK_NET_CODE_LEN; i++) {
        out[i] = alphabet[x & 31u];
        x >>= 5;
        if ((i & 3) == 3) {            /* 30 bits used, refill */
            x = (x + seed) * 0x85ebca6bu;
            x ^= x >> 13;
        }
    }
    out[TAK_NET_CODE_LEN] = '\0';
}

/* ── Colours ────────────────────────────────────────────────────────────
 * The next free colour comes from the engine's own rule so the lobby and
 * the skirmish screen cannot drift apart. */

static int next_free_colour(const TAK_Room *r, int seat, int from) {
    BattleConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        cfg.players[i].kind = slot_occupied(&r->slot[i]) ? TAK_SLOT_HUMAN
                                                         : TAK_SLOT_CLOSED;
        cfg.players[i].color = r->slot[i].colour;
    }
    return BattleConfig_NextFreeColor(&cfg, seat, from);
}

/* ── Setup ──────────────────────────────────────────────────────────── */

static void clamp_cfg(TAK_RoomCfg *c) {
    if (c->max_players < 2) c->max_players = 2;
    if (c->max_players > TAK_NET_SEATS) c->max_players = TAK_NET_SEATS;
    if (c->unit_cap < TAK_UNITS_PER_SIDE_MIN) c->unit_cap = TAK_UNITS_PER_SIDE_MIN;
    if (c->unit_cap > TAK_UNITS_PER_SIDE_MAX) c->unit_cap = TAK_UNITS_PER_SIDE_MAX;
    if (c->timeout_secs < TAK_ROOM_TIMEOUT_MIN) c->timeout_secs = TAK_ROOM_TIMEOUT_MIN;
    if (c->timeout_secs > TAK_ROOM_TIMEOUT_MAX) c->timeout_secs = TAK_ROOM_TIMEOUT_MAX;
    if (c->password[0]) c->flags |= TAK_ROOMF_PASSWORD;
    else                c->flags &= ~TAK_ROOMF_PASSWORD;
}

void TAK_Room_Init(TAK_Room *r, uint32_t id, uint32_t code_seed,
                   const TAK_RoomCfg *cfg,
                   uint32_t host_client_id, const char *host_name,
                   uint32_t engine_build_id, uint8_t determinism_class) {
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->revision = 1;
    TAK_Room_MakeCode(code_seed, r->code);
    if (cfg) r->cfg = *cfg;
    clamp_cfg(&r->cfg);
    r->status = TAK_ROOM_OPEN;
    r->host_client_id = host_client_id;
    r->engine_build_id = engine_build_id;
    r->determinism_class = determinism_class;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        r->slot[i].kind = TAK_NSLOT_EMPTY;
        r->slot[i].colour = (uint8_t)i;
        r->slot[i].side = (uint8_t)(i % TAK_ROOM_SIDES);
    }
    r->slot[0].kind = TAK_NSLOT_HUMAN;
    r->slot[0].client_id = host_client_id;
    r->slot[0].connected = 1;
    copy_str(r->slot[0].name, TAK_NET_NAME_MAX, host_name);
    reset_map_flags(r);
}

/* ── Lookups ────────────────────────────────────────────────────────── */

uint8_t TAK_Room_SeatOf(const TAK_Room *r, uint32_t client_id) {
    if (client_id == 0) return TAK_NET_SEAT_NONE;
    for (int i = 0; i < TAK_NET_SEATS; i++)
        if (r->slot[i].kind == TAK_NSLOT_HUMAN &&
            r->slot[i].client_id == client_id) return (uint8_t)i;
    return TAK_NET_SEAT_NONE;
}

int TAK_Room_IsWatcher(const TAK_Room *r, uint32_t client_id) {
    for (int i = 0; i < r->watcher_count; i++)
        if (r->watcher[i].client_id == client_id) return 1;
    return 0;
}

int TAK_Room_HumanCount(const TAK_Room *r) {
    int n = 0;
    for (int i = 0; i < TAK_NET_SEATS; i++)
        if (r->slot[i].kind == TAK_NSLOT_HUMAN) n++;
    return n;
}

int TAK_Room_OccupiedCount(const TAK_Room *r) {
    int n = 0;
    for (int i = 0; i < TAK_NET_SEATS; i++)
        if (slot_occupied(&r->slot[i])) n++;
    return n;
}

void TAK_Room_SetPing(TAK_Room *r, uint32_t client_id, uint16_t ping_ms) {
    uint8_t seat = TAK_Room_SeatOf(r, client_id);
    if (seat != TAK_NET_SEAT_NONE) { r->slot[seat].ping_ms = ping_ms; return; }
    for (int i = 0; i < r->watcher_count; i++)
        if (r->watcher[i].client_id == client_id) r->watcher[i].ping_ms = ping_ms;
}

/* ── Joining ────────────────────────────────────────────────────────── */

int TAK_Room_Compatible(const TAK_Room *r, uint32_t engine_build_id,
                        uint8_t determinism_class) {
    if (determinism_class != r->determinism_class)
        return TAK_REJECT_DETERMINISM_CLASS;
    if (engine_build_id != r->engine_build_id)
        return TAK_REJECT_NEEDS_NEWER;
    return 0;
}

static int free_seat(const TAK_Room *r) {
    for (int i = 0; i < r->cfg.max_players; i++)
        if (r->slot[i].kind == TAK_NSLOT_EMPTY) return i;
    return -1;
}

int TAK_Room_Join(TAK_Room *r, uint32_t client_id, const char *name,
                  const char *password, int as_watcher,
                  uint32_t engine_build_id, uint8_t determinism_class,
                  uint8_t *out_seat) {
    if (out_seat) *out_seat = TAK_NET_SEAT_NONE;
    if (client_id == 0) return TAK_REJECT_NOT_ALLOWED;
    if (!name || !name[0]) return TAK_REJECT_NAME_REQUIRED;

    /* The password is compared before anything else can be learned about
     * the room, and always over the whole field. */
    int pass_ok = TAK_Net_SecretEqual(r->cfg.password, password,
                                      TAK_NET_PASSWORD_MAX);
    if (!pass_ok) return TAK_REJECT_WRONG_PASSWORD;

    int compat = TAK_Room_Compatible(r, engine_build_id, determinism_class);
    if (compat) return compat;

    /* A watcher may join a match under way, N-008. A player may not,
     * because a seat is a simulation slot fixed when the match starts. */
    if (r->status != TAK_ROOM_OPEN &&
        !(as_watcher && r->status == TAK_ROOM_IN_PROGRESS))
        return TAK_REJECT_GAME_CLOSED;
    if (TAK_Room_SeatOf(r, client_id) != TAK_NET_SEAT_NONE ||
        TAK_Room_IsWatcher(r, client_id)) return TAK_REJECT_NOT_ALLOWED;

    if (as_watcher) {
        if (!(r->cfg.flags & TAK_ROOMF_ALLOW_WATCHING))
            return TAK_REJECT_NO_WATCHING;
        if (r->watcher_count >= TAK_NET_WATCHERS_MAX) return TAK_REJECT_GAME_FULL;
        TAK_RoomWatcher *w = &r->watcher[r->watcher_count++];
        w->client_id = client_id;
        w->ping_ms = 0;
        copy_str(w->name, TAK_NET_NAME_MAX, name);
        touch(r);
        return 0;
    }

    int seat = free_seat(r);
    if (seat < 0) return TAK_REJECT_GAME_FULL;
    TAK_NetSlot *s = &r->slot[seat];
    s->kind = TAK_NSLOT_HUMAN;
    s->client_id = client_id;
    s->connected = 1;
    s->ready = 0;
    s->load_percent = 0;
    s->flags = 0;
    copy_str(s->name, TAK_NET_NAME_MAX, name);
    s->colour = (uint8_t)next_free_colour(r, seat, s->colour);
    if (out_seat) *out_seat = (uint8_t)seat;
    touch(r);
    return 0;
}

/* ── Leaving, and the host moving ───────────────────────────────────── */

static void drop_watcher(TAK_Room *r, uint32_t client_id) {
    for (int i = 0; i < r->watcher_count; i++) {
        if (r->watcher[i].client_id != client_id) continue;
        for (int k = i; k + 1 < r->watcher_count; k++)
            r->watcher[k] = r->watcher[k + 1];
        r->watcher_count--;
        memset(&r->watcher[r->watcher_count], 0, sizeof(r->watcher[0]));
        return;
    }
}

void TAK_Room_Leave(TAK_Room *r, uint32_t client_id, TAK_RoomLeave *out) {
    TAK_RoomLeave local;
    if (!out) out = &local;
    memset(out, 0, sizeof(*out));
    out->seat = TAK_NET_SEAT_NONE;
    out->new_host_client_id = r->host_client_id;

    uint8_t seat = TAK_Room_SeatOf(r, client_id);
    if (seat == TAK_NET_SEAT_NONE) {
        drop_watcher(r, client_id);
        touch(r);
        return;
    }

    out->seat = seat;
    out->was_host = (client_id == r->host_client_id);
    memset(&r->slot[seat], 0, sizeof(r->slot[seat]));
    r->slot[seat].kind = TAK_NSLOT_EMPTY;
    r->slot[seat].colour = seat;
    r->slot[seat].side = (uint8_t)(seat % TAK_ROOM_SIDES);

    if (out->was_host) {
        /* N-002: the lowest occupied human seat takes over. The server
         * decides it and tells everyone, so no two clients can disagree.
         * A room of computer players alone is finished. */
        uint32_t next = 0;
        for (int i = 0; i < TAK_NET_SEATS; i++) {
            if (r->slot[i].kind == TAK_NSLOT_HUMAN) { next = r->slot[i].client_id; break; }
        }
        r->host_client_id = next;
        out->new_host_client_id = next;
        out->room_finished = (next == 0);
    } else if (TAK_Room_HumanCount(r) == 0) {
        out->room_finished = 1;
        r->host_client_id = 0;
        out->new_host_client_id = 0;
    }
    touch(r);
}

/* ── Edits ──────────────────────────────────────────────────────────── */

static int is_host(const TAK_Room *r, uint32_t client_id) {
    return client_id != 0 && client_id == r->host_client_id;
}

/* The host's changes that alter how the battle plays. N-007: they put
 * everyone back to not ready, so nobody starts under rules they did not
 * see. Opening a seat, changing the drop timeout or allowing watchers do
 * not change the rules of play, so they leave ready alone. */
static int changes_the_rules(uint8_t field) {
    switch (field) {
    case TAK_EDIT_MAP:
    case TAK_EDIT_OPTIONS:
    case TAK_EDIT_UNIT_CAP:
    case TAK_EDIT_ADD_COMPUTER:
    case TAK_EDIT_REMOVE_COMPUTER:
    case TAK_EDIT_BLOCK_SLOT:
        return 1;
    default:
        return 0;
    }
}

static int own_row_field(uint8_t field) {
    switch (field) {
    case TAK_EDIT_SIDE:
    case TAK_EDIT_COLOUR:
    case TAK_EDIT_TEAM:
    case TAK_EDIT_NAME:
    case TAK_EDIT_WATCH:
    case TAK_EDIT_READY:
    case TAK_EDIT_HAVE_MAP:
        return 1;
    default:
        return 0;
    }
}

int TAK_Room_Edit(TAK_Room *r, uint32_t client_id,
                  const TAK_MsgRoomEdit *e, TAK_RoomEffect *fx) {
    TAK_RoomEffect scratch;
    if (!fx) fx = &scratch;
    memset(fx, 0, sizeof(*fx));
    fx->seat = TAK_NET_SEAT_NONE;

    if (r->status != TAK_ROOM_OPEN) return TAK_REJECT_GAME_CLOSED;

    uint8_t mine = TAK_Room_SeatOf(r, client_id);
    int host = is_host(r, client_id);

    if (own_row_field(e->field)) {
        if (mine == TAK_NET_SEAT_NONE) return TAK_REJECT_NOT_ALLOWED;
        if (e->seat != mine) return TAK_REJECT_NOT_ALLOWED;
    } else if (e->field != TAK_EDIT_ADD_COMPUTER && !host) {
        /* N-003 leaves adding a computer player open to anyone, as the
         * original did. Everything else below is the host's. */
        return TAK_REJECT_NOT_ALLOWED;
    }

    switch (e->field) {
    case TAK_EDIT_SIDE: {
        if (e->value >= TAK_ROOM_SIDES) return TAK_REJECT_NOT_ALLOWED;
        r->slot[mine].side = (uint8_t)e->value;
        r->slot[mine].ready = 0;
        fx->seat = mine;
        break;
    }
    case TAK_EDIT_COLOUR: {
        if (e->value >= TAK_ROOM_COLOURS) return TAK_REJECT_NOT_ALLOWED;
        /* A colour is a request. The server hands back the next free one. */
        r->slot[mine].colour = (uint8_t)next_free_colour(r, mine, (int)e->value);
        r->slot[mine].ready = 0;
        fx->seat = mine;
        break;
    }
    case TAK_EDIT_TEAM: {
        if (e->value > TAK_ROOM_TEAMS) return TAK_REJECT_NOT_ALLOWED;
        r->slot[mine].team = (uint8_t)e->value;
        r->slot[mine].ready = 0;
        fx->seat = mine;
        break;
    }
    case TAK_EDIT_NAME: {
        if (!e->text[0]) return TAK_REJECT_NAME_REQUIRED;
        copy_str(r->slot[mine].name, TAK_NET_NAME_MAX, e->text);
        r->slot[mine].ready = 0;
        fx->seat = mine;
        break;
    }
    case TAK_EDIT_READY: {
        /* Go toggles, it does not latch (legacy:135767-135790). */
        r->slot[mine].ready = (uint8_t)(!r->slot[mine].ready);
        fx->seat = mine;
        break;
    }
    case TAK_EDIT_HAVE_MAP: {
        /* The original would not start until every player had the map. A
         * name is not enough, so this compares the fingerprint the
         * player's own client computed with the host's. */
        int match = fingerprint_set(r->cfg.map_fingerprint) &&
                    memcmp(e->fingerprint, r->cfg.map_fingerprint,
                           TAK_NET_FINGERPRINT_BYTES) == 0;
        if (match) r->slot[mine].flags |= TAK_SLOTF_HAS_MAP;
        else r->slot[mine].flags = (uint8_t)(r->slot[mine].flags & ~TAK_SLOTF_HAS_MAP);
        fx->seat = mine;
        break;
    }
    case TAK_EDIT_WATCH: {
        if (!(r->cfg.flags & TAK_ROOMF_ALLOW_WATCHING))
            return TAK_REJECT_NO_WATCHING;
        if (r->watcher_count >= TAK_NET_WATCHERS_MAX) return TAK_REJECT_GAME_FULL;
        if (is_host(r, client_id) && TAK_Room_HumanCount(r) == 1)
            return TAK_REJECT_NOT_ALLOWED;   /* nobody would be left to host */
        TAK_RoomWatcher *w = &r->watcher[r->watcher_count++];
        w->client_id = client_id;
        w->ping_ms = r->slot[mine].ping_ms;
        copy_str(w->name, TAK_NET_NAME_MAX, r->slot[mine].name);
        fx->seat = mine;
        memset(&r->slot[mine], 0, sizeof(r->slot[mine]));
        r->slot[mine].kind = TAK_NSLOT_EMPTY;
        r->slot[mine].colour = mine;
        r->slot[mine].side = (uint8_t)(mine % TAK_ROOM_SIDES);
        if (is_host(r, client_id)) {
            for (int i = 0; i < TAK_NET_SEATS; i++)
                if (r->slot[i].kind == TAK_NSLOT_HUMAN) {
                    r->host_client_id = r->slot[i].client_id;
                    break;
                }
        }
        break;
    }
    case TAK_EDIT_MAP: {
        if (!e->text[0]) return TAK_REJECT_NOT_ALLOWED;
        copy_str(r->cfg.map_name, TAK_NET_MAP_NAME_MAX, e->text);
        /* The fingerprint travels beside the name, computed from the map
         * itself. A host that sends a name with no fingerprint leaves the
         * room unable to start, which TAK_Room_CanStart reports rather
         * than starting on a guess. */
        memcpy(r->cfg.map_fingerprint, e->fingerprint,
               TAK_NET_FINGERPRINT_BYTES);
        reset_map_flags(r);
        break;
    }
    case TAK_EDIT_OPTIONS: {
        r->cfg.options = e->value;
        break;
    }
    case TAK_EDIT_UNIT_CAP: {
        if (e->value < TAK_UNITS_PER_SIDE_MIN ||
            e->value > TAK_UNITS_PER_SIDE_MAX) return TAK_REJECT_NOT_ALLOWED;
        r->cfg.unit_cap = (uint16_t)e->value;
        break;
    }
    case TAK_EDIT_TIMEOUT: {
        if (e->value < TAK_ROOM_TIMEOUT_MIN ||
            e->value > TAK_ROOM_TIMEOUT_MAX) return TAK_REJECT_NOT_ALLOWED;
        r->cfg.timeout_secs = (uint16_t)e->value;
        break;
    }
    case TAK_EDIT_ALLOW_WATCHING: {
        if (e->value) r->cfg.flags |= TAK_ROOMF_ALLOW_WATCHING;
        else          r->cfg.flags &= ~TAK_ROOMF_ALLOW_WATCHING;
        break;
    }
    case TAK_EDIT_ADD_COMPUTER: {
        if (e->seat >= r->cfg.max_players) return TAK_REJECT_NOT_ALLOWED;
        if (r->slot[e->seat].kind != TAK_NSLOT_EMPTY) return TAK_REJECT_NOT_ALLOWED;
        if (mine == TAK_NET_SEAT_NONE && !host) return TAK_REJECT_NOT_ALLOWED;
        TAK_NetSlot *s = &r->slot[e->seat];
        s->kind = TAK_NSLOT_COMPUTER;
        s->client_id = 0;            /* N-003: it belongs to the room */
        s->connected = 1;
        s->ready = 1;                /* a computer player is always ready */
        copy_str(s->name, TAK_NET_NAME_MAX, e->text[0] ? e->text : "Computer");
        s->colour = (uint8_t)next_free_colour(r, e->seat, s->colour);
        fx->seat = e->seat;
        break;
    }
    case TAK_EDIT_REMOVE_COMPUTER: {
        if (e->seat >= TAK_NET_SEATS) return TAK_REJECT_NOT_ALLOWED;
        if (r->slot[e->seat].kind != TAK_NSLOT_COMPUTER) return TAK_REJECT_NOT_ALLOWED;
        memset(&r->slot[e->seat], 0, sizeof(r->slot[0]));
        r->slot[e->seat].kind = TAK_NSLOT_EMPTY;
        r->slot[e->seat].colour = e->seat;
        fx->seat = e->seat;
        break;
    }
    case TAK_EDIT_BLOCK_SLOT: {
        if (e->seat >= TAK_NET_SEATS) return TAK_REJECT_NOT_ALLOWED;
        if (r->slot[e->seat].kind != TAK_NSLOT_EMPTY) return TAK_REJECT_NOT_ALLOWED;
        r->slot[e->seat].kind = TAK_NSLOT_BLOCKED;
        fx->seat = e->seat;
        break;
    }
    case TAK_EDIT_UNBLOCK_SLOT: {
        if (e->seat >= TAK_NET_SEATS) return TAK_REJECT_NOT_ALLOWED;
        if (r->slot[e->seat].kind != TAK_NSLOT_BLOCKED) return TAK_REJECT_NOT_ALLOWED;
        r->slot[e->seat].kind = TAK_NSLOT_EMPTY;
        fx->seat = e->seat;
        break;
    }
    case TAK_EDIT_KICK: {
        if (e->seat >= TAK_NET_SEATS) return TAK_REJECT_NOT_ALLOWED;
        if (r->slot[e->seat].kind != TAK_NSLOT_HUMAN) return TAK_REJECT_NOT_ALLOWED;
        if (r->slot[e->seat].client_id == r->host_client_id)
            return TAK_REJECT_NOT_ALLOWED;      /* the host cannot remove itself */
        fx->removed_client_id = r->slot[e->seat].client_id;
        fx->seat = e->seat;
        break;                                   /* the relay calls Leave */
    }
    default:
        return TAK_REJECT_NOT_ALLOWED;
    }

    if (changes_the_rules(e->field)) clear_all_ready(r, fx);
    touch(r);
    return 0;
}

/* ── Starting ───────────────────────────────────────────────────────── */

int TAK_Room_CanStart(const TAK_Room *r, uint32_t client_id) {
    if (!is_host(r, client_id)) return TAK_REJECT_NOT_ALLOWED;
    if (r->status != TAK_ROOM_OPEN) return TAK_REJECT_GAME_CLOSED;

    /* The original needs at least one other human in the room. One person
     * against computer players is what the skirmish screen is for. */
    if (TAK_Room_HumanCount(r) < 2) return TAK_REJECT_NEEDS_HUMAN;

    /* A map, and a fingerprint for it, so every client can agree on which
     * map this is. The fingerprint comes from the map itself, not from its
     * name, because two installs can hold different maps under one name. */
    if (!r->cfg.map_name[0]) return TAK_REJECT_MAP_MISSING;
    if (!fingerprint_set(r->cfg.map_fingerprint)) return TAK_REJECT_MAP_MISSING;

    /* Not everyone on one team. Team 0 means every player for themselves,
     * so a room of zeroes is a free for all and starts. */
    int teamed = 0, same = 1;
    uint8_t first = 0;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        if (!slot_occupied(&r->slot[i])) continue;
        if (!teamed) { first = r->slot[i].team; teamed = 1; continue; }
        if (r->slot[i].team != first) { same = 0; break; }
    }
    if (teamed && same && first != 0) return TAK_REJECT_ONE_TEAM;

    /* Every human has this exact map, by fingerprint. Computer players run
     * on every client, so they need nothing of their own. */
    for (int i = 0; i < TAK_NET_SEATS; i++)
        if (r->slot[i].kind == TAK_NSLOT_HUMAN &&
            !(r->slot[i].flags & TAK_SLOTF_HAS_MAP))
            return TAK_REJECT_MAP_MISSING;

    for (int i = 0; i < TAK_NET_SEATS; i++)
        if (r->slot[i].kind == TAK_NSLOT_HUMAN && !r->slot[i].ready)
            return TAK_REJECT_NOT_READY;

    return 0;
}

int TAK_Room_Start(TAK_Room *r, uint32_t client_id) {
    int why = TAK_Room_CanStart(r, client_id);
    if (why) return why;
    r->status = TAK_ROOM_LOADING;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        r->slot[i].load_percent = 0;
        r->slot[i].flags = (uint8_t)(r->slot[i].flags & ~TAK_SLOTF_LOADED);
    }
    touch(r);
    return 0;
}

void TAK_Room_Go(TAK_Room *r) {
    if (r->status != TAK_ROOM_LOADING) return;
    r->status = TAK_ROOM_IN_PROGRESS;
    touch(r);
}

void TAK_Room_Abort(TAK_Room *r) {
    if (r->status != TAK_ROOM_LOADING && r->status != TAK_ROOM_IN_PROGRESS)
        return;
    r->status = TAK_ROOM_OPEN;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        if (r->slot[i].kind == TAK_NSLOT_HUMAN) r->slot[i].ready = 0;
        r->slot[i].load_percent = 0;
        r->slot[i].flags = (uint8_t)(r->slot[i].flags & ~TAK_SLOTF_LOADED);
    }
    touch(r);
}

uint32_t TAK_Room_SetConnected(TAK_Room *r, uint32_t client_id, int connected) {
    uint8_t seat = TAK_Room_SeatOf(r, client_id);
    if (seat == TAK_NET_SEAT_NONE) return r->host_client_id;
    r->slot[seat].connected = (uint8_t)(connected ? 1 : 0);
    if (!connected && client_id == r->host_client_id) {
        for (int i = 0; i < TAK_NET_SEATS; i++) {
            const TAK_NetSlot *s = &r->slot[i];
            if (s->kind == TAK_NSLOT_HUMAN && s->connected && s->client_id != client_id) {
                r->host_client_id = s->client_id;
                break;
            }
        }
    }
    touch(r);
    return r->host_client_id;
}

/* ── Views ──────────────────────────────────────────────────────────── */

void TAK_Room_Snapshot(const TAK_Room *r, TAK_MsgRoomState *out) {
    memset(out, 0, sizeof(*out));
    out->room_id = r->id;
    out->revision = r->revision;
    copy_str(out->code, TAK_NET_CODE_MAX, r->code);
    copy_str(out->name, TAK_NET_ROOM_NAME_MAX, r->cfg.name);
    copy_str(out->map_name, TAK_NET_MAP_NAME_MAX, r->cfg.map_name);
    memcpy(out->map_fingerprint, r->cfg.map_fingerprint,
           TAK_NET_FINGERPRINT_BYTES);
    out->host_client_id = r->host_client_id;
    out->flags = r->cfg.flags;
    out->options = r->cfg.options;
    out->status = r->status;
    out->watchers = r->watcher_count;
    out->unit_cap = r->cfg.unit_cap;
    out->timeout_secs = r->cfg.timeout_secs;
    out->seat_count = TAK_NET_SEATS;
    for (int i = 0; i < TAK_NET_SEATS; i++) out->slot[i] = r->slot[i];
}

void TAK_Room_Summary(const TAK_Room *r, uint32_t viewer_build,
                      uint8_t viewer_class, TAK_RoomSummary *out) {
    memset(out, 0, sizeof(*out));
    out->room_id = r->id;
    copy_str(out->code, TAK_NET_CODE_MAX, r->code);
    copy_str(out->name, TAK_NET_ROOM_NAME_MAX, r->cfg.name);
    copy_str(out->map_name, TAK_NET_MAP_NAME_MAX, r->cfg.map_name);
    memcpy(out->map_fingerprint, r->cfg.map_fingerprint,
           TAK_NET_FINGERPRINT_BYTES);
    uint8_t host_seat = TAK_Room_SeatOf(r, r->host_client_id);
    if (host_seat != TAK_NET_SEAT_NONE)
        copy_str(out->host_name, TAK_NET_NAME_MAX, r->slot[host_seat].name);
    out->players = (uint8_t)TAK_Room_OccupiedCount(r);
    out->max_players = r->cfg.max_players;
    out->watchers = r->watcher_count;
    out->status = r->status;
    out->flags = r->cfg.flags;
    out->engine_build_id = r->engine_build_id;
    out->determinism_class = r->determinism_class;
    /* Greyed with a reason rather than hidden, which is what the original
     * got wrong: it dropped mismatched sessions from the list silently. */
    int why = TAK_Room_Compatible(r, viewer_build, viewer_class);
    if (!why && r->status != TAK_ROOM_OPEN) why = TAK_REJECT_GAME_CLOSED;
    if (!why && out->players >= out->max_players) why = TAK_REJECT_GAME_FULL;
    out->compat = (uint8_t)why;
}
