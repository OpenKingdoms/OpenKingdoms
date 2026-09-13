/*
 * ws.c -- WebSocket framing and the opening handshake, RFC 6455.
 *
 * Both ends live here because they are the same wire format read from
 * opposite sides, and a bug in one is a bug in the other.
 */

#include "tak_ws.h"

#include <stdio.h>
#include <string.h>

/* SHA-1. The handshake is defined in terms of it and nothing else uses
 * it. It proves neither identity nor integrity here: the accept value
 * only shows the peer read our key, which is all RFC 6455 asks. */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    size_t   have;
} Sha1;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->len = 0; s->have = 0;
}

static void sha1_update(Sha1 *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += (uint64_t)len * 8u;
    while (len > 0) {
        size_t take = 64 - s->have;
        if (take > len) take = len;
        memcpy(s->buf + s->have, p, take);
        s->have += take; p += take; len -= take;
        if (s->have == 64) { sha1_block(s, s->buf); s->have = 0; }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->len;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->have != 56) sha1_update(s, &pad, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, tail, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Four characters per three bytes, padded, not terminated. */
static size_t b64_encode(const uint8_t *in, size_t len, char *out) {
    size_t o = 0, i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                      (uint32_t)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    size_t rest = len - i;
    if (rest == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rest == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = '=';
    }
    return o;
}

static int ci_equal(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* The value of one header in the block, trimmed. NULL when absent. */
static const char *find_header(const char *h, size_t len, const char *name,
                               size_t *out_len) {
    size_t nlen = strlen(name);
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && h[eol] != '\n') eol++;
        size_t line_end = eol;
        if (line_end > i && h[line_end - 1] == '\r') line_end--;
        const char *colon = (const char *)memchr(h + i, ':', line_end - i);
        if (colon) {
            size_t klen = (size_t)(colon - (h + i));
            if (klen == nlen && ci_equal(h + i, klen, name)) {
                const char *v = colon + 1;
                while (v < h + line_end && (*v == ' ' || *v == '\t')) v++;
                const char *e = h + line_end;
                while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
                *out_len = (size_t)(e - v);
                return v;
            }
        }
        i = eol + 1;
    }
    return NULL;
}

/* Upgrade and Connection are comma lists the RFC lets other tokens
 * into, so the token is looked for rather than the whole value. */
static int header_list_has(const char *v, size_t vlen, const char *token) {
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < vlen && v[i] != ',') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e > s && ci_equal(v + s, e - s, token)) return 1;
    }
    return 0;
}

/* Where the header block ends, or 0 when it has not all arrived. */
static size_t header_block_end(const char *p, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (i + 3 < len && p[i] == '\r' && p[i + 1] == '\n' &&
            p[i + 2] == '\r' && p[i + 3] == '\n') return i + 4;
        if (p[i] == '\n' && p[i + 1] == '\n') return i + 2;
    }
    return 0;
}

void TAK_Ws_Accept(const char key[TAK_WS_KEY_CHARS],
                   char out[TAK_WS_ACCEPT_CHARS]) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 s;
    uint8_t digest[20];
    sha1_init(&s);
    sha1_update(&s, key, TAK_WS_KEY_CHARS);
    sha1_update(&s, GUID, sizeof(GUID) - 1);
    sha1_final(&s, digest);
    b64_encode(digest, sizeof(digest), out);
}

size_t TAK_Ws_ServerHandshake(const char *req, size_t req_len,
                              char *out, size_t cap, int *need_more) {
    if (need_more) *need_more = 0;
    if (!req || !out) return 0;
    size_t head = header_block_end(req, req_len);
    if (head == 0) {
        /* How long the caller waits for the rest is the caller's. */
        if (need_more) *need_more = 1;
        return 0;
    }
    if (req_len < 4 || memcmp(req, "GET ", 4) != 0) return 0;

    size_t vlen = 0;
    const char *v = find_header(req, head, "Upgrade", &vlen);
    if (!v || !header_list_has(v, vlen, "websocket")) return 0;
    v = find_header(req, head, "Connection", &vlen);
    if (!v || !header_list_has(v, vlen, "Upgrade")) return 0;
    v = find_header(req, head, "Sec-WebSocket-Version", &vlen);
    if (!v || !ci_equal(v, vlen, "13")) return 0;
    v = find_header(req, head, "Sec-WebSocket-Key", &vlen);
    if (!v || vlen != TAK_WS_KEY_CHARS) return 0;

    char accept[TAK_WS_ACCEPT_CHARS + 1];
    TAK_Ws_Accept(v, accept);
    accept[TAK_WS_ACCEPT_CHARS] = '\0';

    int n = snprintf(out, cap,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

size_t TAK_Ws_ClientRequest(const char *host, const char *path,
                            const uint8_t nonce[16],
                            char key_out[TAK_WS_KEY_CHARS],
                            char *out, size_t cap) {
    if (!host || !path || !nonce || !key_out || !out) return 0;
    b64_encode(nonce, 16, key_out);
    char key[TAK_WS_KEY_CHARS + 1];
    memcpy(key, key_out, TAK_WS_KEY_CHARS);
    key[TAK_WS_KEY_CHARS] = '\0';
    int n = snprintf(out, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     path, host, key);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

int TAK_Ws_CheckServerResponse(const char *resp, size_t resp_len,
                               const char key[TAK_WS_KEY_CHARS],
                               int *need_more) {
    if (need_more) *need_more = 0;
    if (!resp || !key) return 0;
    size_t head = header_block_end(resp, resp_len);
    if (head == 0) { if (need_more) *need_more = 1; return 0; }
    if (resp_len < 12 || memcmp(resp, "HTTP/1.1 101", 12) != 0) return 0;
    size_t vlen = 0;
    const char *v = find_header(resp, head, "Sec-WebSocket-Accept", &vlen);
    if (!v || vlen != TAK_WS_ACCEPT_CHARS) return 0;
    char want[TAK_WS_ACCEPT_CHARS];
    TAK_Ws_Accept(key, want);
    return memcmp(v, want, TAK_WS_ACCEPT_CHARS) == 0;
}

size_t TAK_Ws_Encode(uint8_t opcode, const void *payload, size_t len,
                     const uint8_t mask[4], void *out, size_t cap) {
    if (!out) return 0;
    if (len > TAK_WS_PAYLOAD_MAX) return 0;
    if (len > 0 && !payload) return 0;
    uint8_t *o = (uint8_t *)out;
    size_t hdr = 2;
    if (len > 125) hdr += (len > 0xFFFF) ? 8 : 2;
    if (mask) hdr += 4;
    if (cap < hdr + len) return 0;

    o[0] = (uint8_t)(0x80 | (opcode & 0x0F));   /* always the last frame */
    size_t i = 2;
    if (len <= 125) {
        o[1] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        o[1] = 126;
        o[i++] = (uint8_t)(len >> 8);
        o[i++] = (uint8_t)len;
    } else {
        o[1] = 127;
        for (int b = 7; b >= 0; b--) o[i++] = (uint8_t)(len >> (b * 8));
    }
    if (mask) {
        o[1] = (uint8_t)(o[1] | 0x80);
        memcpy(o + i, mask, 4);
        i += 4;
        for (size_t k = 0; k < len; k++) {
            o[i + k] = (uint8_t)(((const uint8_t *)payload)[k] ^ mask[k & 3]);
        }
    } else if (len > 0) {
        memcpy(o + i, payload, len);
    }
    return i + len;
}

int TAK_Ws_Parse(void *buf, size_t len, int expect_mask, TAK_WsFrame *out) {
    if (!buf || !out) return TAK_WS_PARSE_BAD;
    uint8_t *p = (uint8_t *)buf;
    if (len < 2) return TAK_WS_PARSE_PARTIAL;

    uint8_t b0 = p[0], b1 = p[1];
    if (b0 & 0x70) return TAK_WS_PARSE_BAD;   /* no extensions, so no rsv */
    uint8_t opcode = (uint8_t)(b0 & 0x0F);
    uint8_t fin = (uint8_t)((b0 & 0x80) != 0);
    int masked = (b1 & 0x80) != 0;
    if (masked != (expect_mask != 0)) return TAK_WS_PARSE_BAD;

    /* One whole message per frame is the rule above this layer, so a
     * continuation is a peer we do not speak to. A control frame that
     * is not final, or over 125 bytes, is an error in the RFC itself. */
    int control = (opcode & 0x08) != 0;
    if (opcode == TAK_WS_OP_CONT) return TAK_WS_PARSE_BAD;
    if (!control && opcode != TAK_WS_OP_TEXT && opcode != TAK_WS_OP_BINARY) {
        return TAK_WS_PARSE_BAD;
    }
    if (control && opcode != TAK_WS_OP_CLOSE && opcode != TAK_WS_OP_PING &&
        opcode != TAK_WS_OP_PONG) {
        return TAK_WS_PARSE_BAD;
    }
    if (!fin) return TAK_WS_PARSE_BAD;

    size_t i = 2;
    uint64_t plen = (uint64_t)(b1 & 0x7F);
    if (plen == 126) {
        if (len < i + 2) return TAK_WS_PARSE_PARTIAL;
        plen = ((uint64_t)p[i] << 8) | (uint64_t)p[i + 1];
        i += 2;
        if (plen < 126) return TAK_WS_PARSE_BAD;   /* not the shortest form */
    } else if (plen == 127) {
        if (len < i + 8) return TAK_WS_PARSE_PARTIAL;
        plen = 0;
        for (int b = 0; b < 8; b++) plen = (plen << 8) | (uint64_t)p[i + b];
        i += 8;
        if (plen <= 0xFFFF) return TAK_WS_PARSE_BAD;
    }
    if (control && plen > 125) return TAK_WS_PARSE_BAD;
    if (plen > (uint64_t)TAK_WS_PAYLOAD_MAX) return TAK_WS_PARSE_BAD;

    uint8_t mask[4] = { 0, 0, 0, 0 };
    if (masked) {
        if (len < i + 4) return TAK_WS_PARSE_PARTIAL;
        memcpy(mask, p + i, 4);
        i += 4;
    }
    if (len < i + (size_t)plen) return TAK_WS_PARSE_PARTIAL;
    if (masked) {
        for (size_t k = 0; k < (size_t)plen; k++) p[i + k] ^= mask[k & 3];
    }

    out->opcode = opcode;
    out->fin = fin;
    out->payload = p + i;
    out->payload_len = (size_t)plen;
    out->frame_len = i + (size_t)plen;
    return TAK_WS_PARSE_OK;
}
