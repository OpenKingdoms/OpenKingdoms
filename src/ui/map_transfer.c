/*
 * map_transfer.c -- the room's map on its way, both ends of it. See
 * tak_map_transfer.h. None of it touches the simulation.
 */

#include "tak_map_transfer.h"
#include "tak_map_share.h"
#include "tak_map_fingerprint.h"
#include "tak_net_link.h"
#include "tak_memory.h"
#include "tak_paths.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Pieces asked for at once, how long one may take before it is asked
 * again, and how long the whole may go quiet, past the relay's minute. */
#define MT_WINDOW        4
#define MT_ASK_AGAIN_MS  15000u
#define MT_STALL_MS      75000u
/* A host answers only while its link has this little waiting, so the
 * room's own traffic is never buried behind a map. */
#define MT_HOST_BACKLOG  (64u * 1024u)
#define MT_LINES         4

typedef enum {
    MT_IDLE = 0,
    MT_HAVE,          /* this install has the room's map */
    MT_WAITING,       /* without it, the host has not offered it */
    MT_RECEIVING,
    MT_DONE,          /* it came, checked, and is mounted */
    MT_HOST_SAID_NO,  /* the host's map may not be sent */
    MT_REFUSED,       /* it may not come here */
    MT_FAILED         /* it came wrong or stopped coming */
} MtState;

static struct {
    /* The map this end is working on. */
    uint32_t room_id;
    uint8_t  fp[TAK_NET_FINGERPRINT_BYTES];
    char     name[TAK_NET_MAP_NAME_MAX];
    int      state;
    /* Coming down: from which host, what it said, the next piece and
     * the ones asked for. */
    uint32_t host_id;
    uint8_t  share_seen;
    uint32_t next;
    uint8_t  asking[MT_WINDOW];
    uint32_t asked[MT_WINDOW];
    uint64_t asked_ms[MT_WINDOW];
    int      reported;
    char     text[256];
    unsigned serial;
    /* As host: the map said of, what was said, and its bundle. */
    int      declared;
    uint64_t declared_ms;
    uint32_t declared_room;
    uint8_t  declared_fp[TAK_NET_FINGERPRINT_BYTES];
    uint8_t  share;
    int      weighed;
    uint8_t  weighed_fp[TAK_NET_FINGERPRINT_BYTES];
    uint8_t *bundle;
    uint32_t bundle_len;
    /* Each seat as last seen, for the room's chat lines. */
    uint32_t seat_client[TAK_NET_SEATS];
    uint8_t  seat_state[TAK_NET_SEATS];
    uint8_t  seat_has[TAK_NET_SEATS];
    uint8_t  seat_receiving[TAK_NET_SEATS];
    char     lines[MT_LINES][192];
    int      line_count;
} mt;

static int fp_set(const uint8_t *fp) {
    for (int i = 0; i < TAK_NET_FINGERPRINT_BYTES; i++) if (fp[i]) return 1;
    return 0;
}

static void say(const char *fmt, ...) {
    char text[sizeof mt.text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (strcmp(text, mt.text) == 0) return;
    memcpy(mt.text, text, sizeof text);
    mt.serial++;
}

static void line(const char *fmt, ...) {
    if (mt.line_count >= MT_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mt.lines[mt.line_count], sizeof mt.lines[0], fmt, ap);
    va_end(ap);
    mt.line_count++;
}

static void drop_bundle(void) {
    tak_free(mt.bundle);
    mt.bundle = NULL;
    mt.bundle_len = 0;
}

void MapTransfer_Reset(void) {
    drop_bundle();
    unsigned serial = mt.serial;
    memset(&mt, 0, sizeof mt);
    mt.serial = serial + 1;
}

const char *MapTransfer_Text(void) { return mt.text; }
unsigned    MapTransfer_TextSerial(void) { return mt.serial; }

int MapTransfer_TakeLine(char *out, size_t cap) {
    if (mt.line_count == 0 || !out || !cap) return 0;
    snprintf(out, cap, "%s", mt.lines[0]);
    mt.line_count--;
    memmove(mt.lines[0], mt.lines[1], sizeof mt.lines[0] * (size_t)mt.line_count);
    return 1;
}

/* How far it has come, for the host and everyone else to see. */
static void report(TAK_NetClient *c, int value) {
    if (c->seat == TAK_NET_SEAT_NONE || value == mt.reported) return;
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof e);
    e.field = TAK_EDIT_MAP_STATE;
    e.value = (uint32_t)value;
    if (TAK_NetClient_EditRoom(c, &e) == 0) mt.reported = value;
}

static void fail(TAK_NetClient *c, const char *why) {
    TAK_NetClient_MapDrop(c);
    mt.state = MT_FAILED;
    report(c, TAK_MAPSTATE_FAILED);
    say("%s", why);
}

/* ── The host's end ─────────────────────────────────────────────────── */

static void host_tick(TAK_NetClient *c, uint64_t now_ms) {
    const TAK_MsgRoomState *rs = &c->room;
    if (!fp_set(rs->map_fingerprint)) return;
    /* Said and not heard, as when the word crossed a map change. */
    if (mt.declared && rs->map_share == TAK_MAPSHARE_UNKNOWN && now_ms - mt.declared_ms > 3000)
        mt.declared = 0;
    if (!mt.declared || mt.declared_room != rs->room_id ||
        memcmp(mt.declared_fp, rs->map_fingerprint, sizeof mt.declared_fp) != 0) {
        /* Worked out once a map, however often the word has to be sent. */
        if (!mt.weighed || memcmp(mt.weighed_fp, rs->map_fingerprint, sizeof mt.weighed_fp) != 0) {
            drop_bundle();
            mt.share = (uint8_t)TAK_MapShare_Status(rs->map_name, rs->map_fingerprint);
            mt.weighed = 1;
            memcpy(mt.weighed_fp, rs->map_fingerprint, sizeof mt.weighed_fp);
        }
        TAK_MsgRoomEdit e;
        memset(&e, 0, sizeof e);
        e.field = TAK_EDIT_MAP_SHARE;
        e.seat = TAK_NET_SEAT_NONE;
        e.value = mt.share;
        memcpy(e.fingerprint, rs->map_fingerprint, sizeof e.fingerprint);
        if (TAK_NetClient_EditRoom(c, &e) != 0) return;
        mt.declared = 1;
        mt.declared_ms = now_ms;
        mt.declared_room = rs->room_id;
        memcpy(mt.declared_fp, rs->map_fingerprint, sizeof mt.declared_fp);
    }
    TAK_MsgMapRequest a;
    while (TAK_NetClient_MapCanSend(c) && TAK_NetLink_Backlog() < MT_HOST_BACKLOG &&
           TAK_NetClient_MapTakeAsk(c, &a)) {
        if (memcmp(a.fingerprint, mt.declared_fp, sizeof a.fingerprint) != 0) continue;
        TAK_MsgMapData d;
        memset(&d, 0, sizeof d);
        d.client_id = a.client_id;
        memcpy(d.fingerprint, a.fingerprint, sizeof d.fingerprint);
        d.offset = a.offset;
        /* Built for the first player who asks, not for every map picked. */
        if (mt.share == TAK_MAPSHARE_OFFERED && !mt.bundle) {
            int rc = TAK_MapShare_PackInstalled(rs->map_name, mt.declared_fp,
                                                &mt.bundle, &mt.bundle_len);
            if (rc != 0) {
                mt.share = rc == -2 ? TAK_MAPSHARE_TOO_BIG : TAK_MAPSHARE_UNREADABLE;
                mt.declared = 0;
            }
        }
        if (mt.share == TAK_MAPSHARE_OFFERED && mt.bundle && a.offset < mt.bundle_len) {
            uint32_t left = mt.bundle_len - a.offset;
            d.total = mt.bundle_len;
            d.len = (uint16_t)(left < TAK_NET_MAP_CHUNK ? left : TAK_NET_MAP_CHUNK);
            d.data = mt.bundle + a.offset;
        }
        (void)TAK_NetClient_MapSend(c, &d);
    }
}

/* ── The end without the map ────────────────────────────────────────── */

static void begin(TAK_NetClient *c, uint64_t now_ms) {
    const TAK_MsgRoomState *rs = &c->room;
    char why[256];
    int shipped = TAK_MapShare_Shipped(mt.fp);
    if (!shipped && TAK_MapShare_ShippedName(mt.name)) shipped = TAK_MAPSHARE_SHIPPED_NAME;
    if (shipped) {
        TAK_MapShare_Advice(shipped, NULL, mt.name, why, sizeof why);
        mt.state = MT_REFUSED;
        say("%s", why);
        return;
    }
    mt.share_seen = rs->map_share;
    if (rs->map_share != TAK_MAPSHARE_OFFERED) {
        TAK_MapShare_Advice(rs->map_share, NULL, mt.name, why, sizeof why);
        if (rs->map_share != TAK_MAPSHARE_UNKNOWN) mt.state = MT_HOST_SAID_NO;
        say("%s", why);
        return;
    }
    if (!TAK_MapShare_NameOk(mt.name)) {
        mt.state = MT_REFUSED;
        report(c, TAK_MAPSTATE_FAILED);
        say("%s cannot be kept under that name, so it was not sent.", mt.name);
        return;
    }
    if (TAK_MapShare_HasOwn(mt.name)) {
        mt.state = MT_REFUSED;
        report(c, TAK_MAPSTATE_CONFLICT);
        say("You have another map named %s. Move yours out of the game's folders "
            "to take the host's.", mt.name);
        return;
    }
    TAK_NetClient_MapBegin(c, mt.fp, now_ms);
    mt.host_id = rs->host_client_id;
    mt.next = 0;
    memset(mt.asking, 0, sizeof mt.asking);
    mt.state = MT_RECEIVING;
    report(c, 1);
    say("Receiving %s from the host.", mt.name);
}

static void finish(TAK_NetClient *c) {
    TAK_MapShareFiles f;
    int rc = TAK_MapShare_Unpack(c->map_in, c->map_in_total, mt.fp, &f);
    TAK_NetClient_MapDrop(c);
    char why[256];
    if (rc == -2) {
        snprintf(why, sizeof why, "What came from the host is not the room's %s, so it "
                 "was not used.", mt.name);
        fail(c, why);
        return;
    }
    if (rc != 0) {
        snprintf(why, sizeof why, "The %s that came from the host could not be read.", mt.name);
        fail(c, why);
        return;
    }
    rc = TAK_MapShare_Keep(Paths_ReceivedMapDir(), mt.name, &f);
    TAK_MapShare_FreeFiles(&f);
    if (rc != 0) {
        snprintf(why, sizeof why, "%s came, but could not be kept in %s.", mt.name,
                 Paths_ReceivedMapDir());
        fail(c, why);
        return;
    }
    uint8_t mine[TAK_NET_FINGERPRINT_BYTES];
    if (TAK_MapFingerprint_FromName(mt.name, mine) != 0 ||
        memcmp(mine, mt.fp, sizeof mine) != 0) {
        snprintf(why, sizeof why, "%s came, but another copy of it is read first.", mt.name);
        fail(c, why);
        return;
    }
    TAK_MsgRoomEdit e;
    memset(&e, 0, sizeof e);
    e.field = TAK_EDIT_HAVE_MAP;
    memcpy(e.fingerprint, mt.fp, sizeof e.fingerprint);
    (void)TAK_NetClient_EditRoom(c, &e);
    mt.reported = 0;
    mt.state = MT_DONE;
    say("%s has come from the host.", mt.name);
}

static void receive(TAK_NetClient *c, uint64_t now_ms) {
    const TAK_MsgRoomState *rs = &c->room;
    /* A new host sends its own copy, from the start. */
    if (rs->host_client_id != mt.host_id || rs->map_share != TAK_MAPSHARE_OFFERED) {
        TAK_NetClient_MapDrop(c);
        mt.state = MT_WAITING;
        return;
    }
    char why[256];
    if (c->map_in_refused) {
        snprintf(why, sizeof why, "The host could not send %s.", mt.name);
        fail(c, why);
        return;
    }
    if (c->map_in_bad) {
        snprintf(why, sizeof why, "%s came in pieces that do not agree, so it was not used.",
                 mt.name);
        fail(c, why);
        return;
    }
    if (now_ms > c->map_in_heard_ms && now_ms - c->map_in_heard_ms > MT_STALL_MS) {
        snprintf(why, sizeof why, "The host stopped sending %s.", mt.name);
        fail(c, why);
        return;
    }
    uint32_t total = c->map_in ? c->map_in_total : 0;
    for (int k = 0; k < MT_WINDOW; k++) {
        if (!mt.asking[k]) continue;
        if (TAK_NetClient_MapHave(c, mt.asked[k])) { mt.asking[k] = 0; continue; }
        if (now_ms - mt.asked_ms[k] > MT_ASK_AGAIN_MS &&
            TAK_NetClient_MapAsk(c, mt.asked[k]) == 0) mt.asked_ms[k] = now_ms;
    }
    for (int k = 0; k < MT_WINDOW; k++) {
        if (mt.asking[k]) continue;
        while (total && mt.next < total && TAK_NetClient_MapHave(c, mt.next))
            mt.next += TAK_NET_MAP_CHUNK;
        /* Until the first piece says how big it is, only the first. */
        if ((!total && mt.next > 0) || (total && mt.next >= total)) break;
        if (TAK_NetClient_MapAsk(c, mt.next) != 0) break;
        mt.asking[k] = 1;
        mt.asked[k] = mt.next;
        mt.asked_ms[k] = now_ms;
        mt.next += TAK_NET_MAP_CHUNK;
    }
    if (total && c->map_in_got >= total) { finish(c); return; }
    if (total) {
        int pct = (int)((uint64_t)c->map_in_got * 100u / total);
        if (pct < 1) pct = 1;
        if (pct > 99) pct = 99;
        if (pct >= mt.reported + 10) report(c, pct);
        say("Receiving %s from the host, %d per cent.", mt.name, pct);
    }
}

static void player_tick(TAK_NetClient *c, uint64_t now_ms) {
    const TAK_MsgRoomState *rs = &c->room;
    if (!fp_set(rs->map_fingerprint) || !rs->map_name[0]) return;
    if (mt.room_id != rs->room_id || memcmp(mt.fp, rs->map_fingerprint, sizeof mt.fp) != 0) {
        /* Another map. The relay forgets each seat's word on the last. */
        TAK_NetClient_MapDrop(c);
        mt.room_id = rs->room_id;
        memcpy(mt.fp, rs->map_fingerprint, sizeof mt.fp);
        snprintf(mt.name, sizeof mt.name, "%s", rs->map_name);
        mt.reported = 0;
        say("%s", "");
        uint8_t mine[TAK_NET_FINGERPRINT_BYTES];
        mt.state = (TAK_MapFingerprint_FromName(mt.name, mine) == 0 &&
                    memcmp(mine, mt.fp, sizeof mine) == 0) ? MT_HAVE : MT_WAITING;
    }
    switch (mt.state) {
    case MT_HOST_SAID_NO:
        /* A new host may say otherwise of its own copy. */
        if (rs->map_share != mt.share_seen) mt.state = MT_WAITING;
        if (mt.state == MT_WAITING) begin(c, now_ms);
        break;
    case MT_WAITING:
        begin(c, now_ms);
        break;
    case MT_RECEIVING:
        receive(c, now_ms);
        break;
    default:
        break;
    }
}

/* ── What the room hears ────────────────────────────────────────────── */

static void seat_lines(const TAK_NetClient *c) {
    const TAK_MsgRoomState *rs = &c->room;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        const TAK_NetSlot *s = &rs->slot[i];
        uint8_t has = (uint8_t)((s->flags & TAK_SLOTF_HAS_MAP) ? 1 : 0);
        if (s->kind != TAK_NSLOT_HUMAN || s->client_id != mt.seat_client[i]) {
            mt.seat_client[i] = s->kind == TAK_NSLOT_HUMAN ? s->client_id : 0;
            mt.seat_state[i] = s->map_state;
            mt.seat_has[i] = has;
            mt.seat_receiving[i] = 0;
            continue;
        }
        uint8_t was = mt.seat_state[i], now = s->map_state;
        if (i != c->seat) {
            if (now >= 1 && now <= 100 && !has && !mt.seat_receiving[i])
                line("%s is receiving the map from the host.", s->name);
            if (has && !mt.seat_has[i] && mt.seat_receiving[i])
                line("%s has the map now.", s->name);
            if (now == TAK_MAPSTATE_FAILED && was != now)
                line("%s could not take the map.", s->name);
            if (now == TAK_MAPSTATE_CONFLICT && was != now)
                line("%s has another map of that name, so the host's was not sent.", s->name);
        }
        if (now >= 1 && now <= 100 && !has) mt.seat_receiving[i] = 1;
        if (has) mt.seat_receiving[i] = 0;
        mt.seat_state[i] = now;
        mt.seat_has[i] = has;
    }
}

void MapTransfer_Tick(TAK_NetClient *c, uint64_t now_ms) {
    if (!c || c->state != TAK_NC_ROOM || c->room.room_id == 0) {
        if (mt.room_id || mt.declared || mt.weighed || mt.text[0]) MapTransfer_Reset();
        return;
    }
    if (c->room.status != TAK_ROOM_OPEN) return;
    int host = c->room.host_client_id == c->session_id;
    if (host) {
        /* Hosting now: whatever was coming to this end has no sender. */
        if (c->map_in_on) TAK_NetClient_MapDrop(c);
        host_tick(c, now_ms);
    } else {
        if (mt.declared || mt.weighed) { mt.declared = mt.weighed = 0; drop_bundle(); }
        c->map_ask_count = 0;
        player_tick(c, now_ms);
    }
    seat_lines(c);
}

void MapTransfer_WhyNotStart(const TAK_MsgRoomState *rs, char *out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", TAK_Net_RejectText(TAK_REJECT_MAP_MISSING));
    if (!rs) return;
    int shipped = TAK_MapShare_Shipped(rs->map_fingerprint);
    int share = shipped ? shipped : rs->map_share;
    for (int i = 0; i < TAK_NET_SEATS; i++) {
        const TAK_NetSlot *s = &rs->slot[i];
        if (s->kind != TAK_NSLOT_HUMAN || (s->flags & TAK_SLOTF_HAS_MAP)) continue;
        if (s->map_state >= 1 && s->map_state <= 100)
            snprintf(out, cap, "Waiting for %s to receive the map, %u per cent.", s->name,
                     (unsigned)s->map_state);
        else if (s->map_state == TAK_MAPSTATE_FAILED)
            snprintf(out, cap, "%s could not take the map.", s->name);
        else if (s->map_state == TAK_MAPSTATE_CONFLICT)
            snprintf(out, cap, "%s has another map named %s, so the host's was not sent.",
                     s->name, rs->map_name);
        else if (share == TAK_MAPSHARE_EXPANSION)
            snprintf(out, cap, "%s does not have %s, which comes with The Iron Plague.",
                     s->name, rs->map_name);
        else if (share == TAK_MAPSHARE_MAP_PACK)
            snprintf(out, cap, "%s does not have %s, one of Cavedog's map packs.",
                     s->name, rs->map_name);
        else if (share == TAK_MAPSHARE_SHIPPED_NAME)
            snprintf(out, cap, "%s does not have %s, which is named as a shipped map and "
                     "so is not sent.", s->name, rs->map_name);
        else if (share == TAK_MAPSHARE_OFFERED)
            snprintf(out, cap, "%s does not have %s yet.", s->name, rs->map_name);
        else
            snprintf(out, cap, "%s does not have %s.", s->name, rs->map_name);
        return;
    }
}
