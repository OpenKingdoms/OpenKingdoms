/*
 * mod_proxy.c -- the relay's mod download stream, without sockets.
 *
 * See tak_mod_proxy.h. The answer's head goes out only once the first
 * byte has arrived, so an upstream that fails at once is a 502 the page
 * can read rather than a 200 that stops short.
 */

#include "tak_mod_proxy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { SLOT_FREE = 0, SLOT_FETCHING, SLOT_FETCHED, SLOT_FAILED };

#define CORS "Access-Control-Allow-Origin: *\r\n" \
             "Cache-Control: no-store\r\n" \
             "Connection: close\r\n"

void TAK_ModProxy_Init(TAK_ModProxy *p, const TAK_ModRegistry *reg, int can_fetch) {
    memset(p, 0, sizeof *p);
    p->reg = reg;
    p->can_fetch = can_fetch;
}

/* The id out of "GET /api/mods/<id>/download". 1 for a download. */
static int download_id(const uint8_t *req, size_t len, char *id, size_t cap, int *head) {
    const char *s = (const char *)req;
    size_t at;
    if (len >= 4 && memcmp(s, "GET ", 4) == 0) { at = 4; *head = 0; }
    else if (len >= 5 && memcmp(s, "HEAD ", 5) == 0) { at = 5; *head = 1; }
    else return 0;
    static const char pre[] = "/api/mods/";
    if (len - at < sizeof pre - 1 || memcmp(s + at, pre, sizeof pre - 1) != 0) return 0;
    at += sizeof pre - 1;
    size_t n = 0;
    while (at < len && s[at] != '/' && s[at] != ' ' && s[at] != '?') {
        if (n + 1 >= cap) return 0;
        id[n++] = s[at++];
    }
    id[n] = '\0';
    static const char post[] = "/download";
    if (n == 0 || len - at < sizeof post - 1 || memcmp(s + at, post, sizeof post - 1) != 0) return 0;
    at += sizeof post - 1;
    return at < len && (s[at] == ' ' || s[at] == '?');
}

int TAK_ModProxy_IsDownload(const uint8_t *req, size_t len) {
    char id[64];
    int head = 0;
    return download_id(req, len, id, sizeof id, &head);
}

static size_t error_answer(char *out, size_t cap, int code, const char *status, const char *why) {
    char clean[200], body[256];
    size_t k = 0;
    for (const char *w = why; *w && k + 1 < sizeof clean; w++)
        clean[k++] = (*w == '"' || *w == '\\' || (unsigned char)*w < 0x20) ? '\'' : *w;
    clean[k] = '\0';
    int bn = snprintf(body, sizeof body, "{\"error\":\"%s\"}", clean);
    if (bn < 0 || (size_t)bn >= sizeof body) bn = 0;
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %d\r\n" CORS "\r\n%s",
                     code, status, bn, bn ? body : "");
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

static size_t ok_head(char *out, size_t cap, uint64_t size) {
    int n = snprintf(out, cap,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/zip\r\n"
                     "Content-Length: %llu\r\n" CORS "\r\n",
                     (unsigned long long)size);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

int TAK_ModProxy_Begin(TAK_ModProxy *p, uint32_t conn, const uint8_t *req, size_t len,
                       char *out, size_t cap, size_t *out_len) {
    char id[64];
    int head = 0;
    *out_len = 0;
    if (!download_id(req, len, id, sizeof id, &head)) {
        *out_len = error_answer(out, cap, 404, "Not Found", "not found");
        return -1;
    }
    const TAK_ModEntry *e = TAK_ModRegistry_Find(p->reg, id);
    if (!e) {
        *out_len = error_answer(out, cap, 404, "Not Found", "that mod is not in the registry");
        return -1;
    }
    if (!TAK_ModEntry_OneClick(e)) {
        *out_len = error_answer(out, cap, 404, "Not Found",
                                "that mod installs by hand, see its page");
        return -1;
    }
    if (head) {
        *out_len = ok_head(out, cap, e->size);
        return -1;
    }
    if (!p->can_fetch) {
        *out_len = error_answer(out, cap, 501, "Not Implemented",
                                "this server was built without downloads");
        return -1;
    }
    for (int i = 0; i < TAK_MODPROXY_SLOTS; i++) {
        TAK_ModProxySlot *s = &p->slot[i];
        if (s->state != SLOT_FREE) continue;
        memset(s, 0, offsetof(TAK_ModProxySlot, stage));
        s->state = SLOT_FETCHING;
        s->conn = conn;
        s->entry = e;
        return i;
    }
    *out_len = error_answer(out, cap, 503, "Service Unavailable",
                            "the server is busy with other downloads, try again in a minute");
    return -1;
}

static TAK_ModProxySlot *live(TAK_ModProxy *p, int slot) {
    if (slot < 0 || slot >= TAK_MODPROXY_SLOTS || p->slot[slot].state == SLOT_FREE) return NULL;
    return &p->slot[slot];
}

const char *TAK_ModProxy_Url(const TAK_ModProxy *p, int slot) {
    const TAK_ModProxySlot *s = live((TAK_ModProxy *)p, slot);
    return s ? s->entry->url : NULL;
}

uint64_t TAK_ModProxy_Size(const TAK_ModProxy *p, int slot) {
    const TAK_ModProxySlot *s = live((TAK_ModProxy *)p, slot);
    return s ? s->entry->size : 0;
}

static void fail(TAK_ModProxySlot *s, const char *why) {
    if (s->state != SLOT_FETCHING) return;
    s->state = SLOT_FAILED;
    /* Nothing has gone out, so the page can still be told why. */
    if (s->got == 0)
        s->head_len = error_answer(s->head, sizeof s->head, 502, "Bad Gateway", why);
}

size_t TAK_ModProxy_Data(TAK_ModProxy *p, int slot, const void *bytes, size_t len) {
    TAK_ModProxySlot *s = live(p, slot);
    if (!s || s->state != SLOT_FETCHING) return TAK_MODPROXY_ABORT;
    if (len == 0) return 0;
    if (s->got + len > s->entry->size) {
        fail(s, "the download is larger than the registry says");
        return TAK_MODPROXY_ABORT;
    }
    if (len > TAK_MODPROXY_STAGE) {
        fail(s, "the download came in pieces too large to pass on");
        return TAK_MODPROXY_ABORT;
    }
    if (s->stage_len + len > TAK_MODPROXY_STAGE) {
        s->paused = 1;
        return TAK_MODPROXY_PAUSE;
    }
    if (s->got == 0) s->head_len = ok_head(s->head, sizeof s->head, s->entry->size);
    s->moved_ms = 0;
    memcpy(s->stage + s->stage_len, bytes, len);
    s->stage_len += len;
    s->got += len;
    return len;
}

void TAK_ModProxy_Done(TAK_ModProxy *p, int slot, int ok, const char *why) {
    TAK_ModProxySlot *s = live(p, slot);
    if (!s || s->state != SLOT_FETCHING) return;
    if (ok && s->got == s->entry->size) { s->state = SLOT_FETCHED; return; }
    fail(s, ok ? "the download is smaller than the registry says"
               : (why && why[0] ? why : "the download failed"));
}

int TAK_ModProxy_Resume(TAK_ModProxy *p, int slot) {
    TAK_ModProxySlot *s = live(p, slot);
    if (!s || !s->paused || s->state != SLOT_FETCHING ||
        s->stage_len > TAK_MODPROXY_STAGE / 2) return 0;
    s->paused = 0;
    return 1;
}

void TAK_ModProxy_Drain(TAK_ModProxy *p, TAK_ModProxyWrite write, void *ctx,
                        uint64_t now_ms) {
    for (int i = 0; i < TAK_MODPROXY_SLOTS; i++) {
        TAK_ModProxySlot *s = &p->slot[i];
        if (s->state == SLOT_FREE) continue;
        if (!s->moved_ms) s->moved_ms = now_ms;
        if (now_ms - s->moved_ms >= TAK_MODPROXY_STALL_MS) {
            /* A reader or a host that stopped: close with what went out. */
            s->state = SLOT_FAILED;
            s->stage_len = 0;
            s->head_len = s->head_off;
        }
        if (s->head_off < s->head_len) {
            size_t n = write(ctx, s->conn, s->head + s->head_off, s->head_len - s->head_off, 0);
            if (n) s->moved_ms = now_ms;
            s->head_off += n;
            if (s->head_off < s->head_len) continue;
        }
        if (s->stage_len) {
            size_t n = write(ctx, s->conn, s->stage, s->stage_len, 0);
            if (n > s->stage_len) n = s->stage_len;
            if (n) s->moved_ms = now_ms;
            memmove(s->stage, s->stage + n, s->stage_len - n);
            s->stage_len -= n;
            if (s->stage_len) continue;
        }
        if (s->state == SLOT_FETCHING) continue;
        /* Fetched, or failed: a failure after the head went out closes
         * short of the length promised, which the reader sees. */
        write(ctx, s->conn, "", 0, 1);
        s->state = SLOT_FREE;
    }
}

int TAK_ModProxy_Fetching(const TAK_ModProxy *p, int slot) {
    return slot >= 0 && slot < TAK_MODPROXY_SLOTS && p->slot[slot].state == SLOT_FETCHING;
}

int TAK_ModProxy_Cancel(TAK_ModProxy *p, uint32_t conn) {
    for (int i = 0; i < TAK_MODPROXY_SLOTS; i++) {
        if (p->slot[i].state != SLOT_FREE && p->slot[i].conn == conn) {
            p->slot[i].state = SLOT_FREE;
            return i;
        }
    }
    return -1;
}
