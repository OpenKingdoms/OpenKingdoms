/*
 * transport_fake.c -- an in process network for the relay's tests.
 *
 * Virtual time, one sorted queue of frames in flight, and faults applied
 * on send. Test code, so it allocates a packet per frame. The relay core
 * it feeds does not. Shape and fault model in tak_net_transport.h.
 */

#include "tak_net_transport.h"

#include <stdlib.h>
#include <string.h>

struct TAK_FakePacket {
    TAK_FakePacket *next;
    uint64_t        due;
    uint64_t        order;
    TAK_ConnId      conn;
    uint8_t         to_server;
    uint8_t         is_close;
    size_t          len;
    uint8_t        *data;
};

static uint32_t roll(TAK_FakeNet *n, uint32_t bound) {
    uint32_t x = n->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    n->rng = x;
    return bound ? x % bound : 0;
}

static int valid(const TAK_FakeNet *n, TAK_ConnId c) {
    return c >= 1 && c <= TAK_FAKE_CONNS_MAX;
}

static void enqueue(TAK_FakeNet *n, TAK_ConnId c, int to_server, int is_close,
                    const uint8_t *data, size_t len, uint64_t due) {
    TAK_FakePacket *p = (TAK_FakePacket *)malloc(sizeof(*p) + len);
    if (!p) return;
    p->data = (uint8_t *)(p + 1);
    if (len) memcpy(p->data, data, len);
    p->len = len;
    p->conn = c;
    p->to_server = (uint8_t)to_server;
    p->is_close = (uint8_t)is_close;
    p->due = due;
    p->order = n->order++;
    TAK_FakePacket **pp = &n->head;
    while (*pp && ((*pp)->due < due || ((*pp)->due == due && (*pp)->order < p->order)))
        pp = &(*pp)->next;
    p->next = *pp;
    *pp = p;
    n->in_flight++;
}

static uint64_t delay(TAK_FakeNet *n) {
    return (uint64_t)n->cfg.latency_ms +
           (n->cfg.jitter_ms ? roll(n, n->cfg.jitter_ms + 1u) : 0u);
}

/* ── The server's side ────────────────────────────────────────────────── */

static int server_send(void *ctx, TAK_ConnId c, const uint8_t *f, size_t len) {
    TAK_FakeNet *n = (TAK_FakeNet *)ctx;
    if (!valid(n, c) || !n->conn[c].open) return -1;
    uint64_t due = n->now + delay(n);
    if (due < n->conn[c].last_to_client) due = n->conn[c].last_to_client;
    n->conn[c].last_to_client = due;
    enqueue(n, c, 0, 0, f, len, due);
    return 0;
}

static void server_close(void *ctx, TAK_ConnId c) {
    TAK_FakeNet *n = (TAK_FakeNet *)ctx;
    if (!valid(n, c) || !n->conn[c].open) return;
    n->conn[c].open = 0;
    n->conn[c].closed_by_server = 1;
    /* The client hears the close after everything sent before it. */
    uint64_t due = n->now + n->cfg.latency_ms;
    if (due < n->conn[c].last_to_client) due = n->conn[c].last_to_client;
    enqueue(n, c, 0, 1, NULL, 0, due);
}

TAK_NetTransport TAK_FakeNet_Server(TAK_FakeNet *n) {
    TAK_NetTransport t;
    t.ctx = n;
    t.send = server_send;
    t.close = server_close;
    return t;
}

/* ── Setup ────────────────────────────────────────────────────────────── */

void TAK_FakeNet_Init(TAK_FakeNet *n, const TAK_FakeNetCfg *cfg) {
    memset(n, 0, sizeof(*n));
    if (cfg) n->cfg = *cfg;
    n->rng = n->cfg.seed ? n->cfg.seed : 0x2545f491u;
}

void TAK_FakeNet_Free(TAK_FakeNet *n) {
    while (n->head) {
        TAK_FakePacket *p = n->head;
        n->head = p->next;
        free(p);
    }
    n->in_flight = 0;
}

TAK_ConnId TAK_FakeNet_Connect(TAK_FakeNet *n) {
    if (n->next_id >= TAK_FAKE_CONNS_MAX) return 0;
    TAK_ConnId c = ++n->next_id;
    memset(&n->conn[c], 0, sizeof(n->conn[c]));
    n->conn[c].open = 1;
    return c;
}

int TAK_FakeNet_IsOpen(const TAK_FakeNet *n, TAK_ConnId c) {
    return valid(n, c) && n->conn[c].open;
}

void TAK_FakeNet_SetMuted(TAK_FakeNet *n, TAK_ConnId c, int muted) {
    if (valid(n, c)) n->conn[c].muted = (uint8_t)(muted ? 1 : 0);
}

/* ── The client's side ────────────────────────────────────────────────── */

void TAK_FakeNet_ClientSend(TAK_FakeNet *n, TAK_ConnId c,
                            const uint8_t *frame, size_t len, uint64_t now) {
    if (!valid(n, c) || !n->conn[c].open || n->conn[c].muted) return;
    n->sent_to_server++;
    if (n->cfg.loss_permille && roll(n, 1000) < n->cfg.loss_permille) {
        n->lost++;
        return;
    }
    int copies = 1;
    if (n->cfg.dup_permille && roll(n, 1000) < n->cfg.dup_permille) {
        copies = 2;
        n->duplicated++;
    }
    for (int k = 0; k < copies; k++) {
        uint64_t due = now + delay(n);
        if (n->cfg.reorder_permille && roll(n, 1000) < n->cfg.reorder_permille) {
            /* Held back, so frames sent after it overtake it. */
            due += 3u * (uint64_t)n->cfg.latency_ms + 1u;
            n->reordered++;
        } else {
            if (due < n->conn[c].last_to_server) due = n->conn[c].last_to_server;
            n->conn[c].last_to_server = due;
        }
        enqueue(n, c, 1, 0, frame, len, due);
    }
}

void TAK_FakeNet_ClientClose(TAK_FakeNet *n, TAK_ConnId c, uint64_t now) {
    if (!valid(n, c) || !n->conn[c].open) return;
    n->conn[c].open = 0;
    n->conn[c].client_gone = 1;
    uint64_t due = now + n->cfg.latency_ms;
    if (due < n->conn[c].last_to_server) due = n->conn[c].last_to_server;
    enqueue(n, c, 1, 1, NULL, 0, due);
}

/* ── Delivery ─────────────────────────────────────────────────────────── */

void TAK_FakeNet_Pump(TAK_FakeNet *n, uint64_t now,
                      TAK_FakeDeliver deliver, void *user) {
    n->now = now;
    while (n->head && n->head->due <= now) {
        TAK_FakePacket *p = n->head;
        n->head = p->next;
        n->in_flight--;
        int gone = !p->to_server && n->conn[p->conn].client_gone;
        if (!gone && deliver) {
            if (p->is_close) deliver(user, p->conn, p->to_server, NULL, 0);
            else deliver(user, p->conn, p->to_server, p->data, p->len);
        }
        free(p);
    }
}
