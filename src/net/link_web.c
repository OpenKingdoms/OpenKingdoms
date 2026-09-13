/*
 * link_web.c -- a link over the page's own WebSocket.
 *
 * A browser has a WebSocket already and it delivers whole messages, so
 * there is no framing to do and no socket to own. That is why this is
 * the shorter of the two halves by a long way: everything ws_conn does
 * for a native client, the browser has done before the bytes arrive.
 *
 * EM_JS is C calling out to the page, the same way paths.c reaches
 * origin private storage, so nothing has to be added to the exported
 * runtime methods.
 */

#ifdef __EMSCRIPTEN__

#include "tak_net_link.h"

#include <emscripten.h>
#include <string.h>

/* Whole messages the page has handed over, waiting for the next pump.
 * A browser delivers them on its own event loop and the game reads
 * them on its frame, so something has to hold them in between. One
 * turn bundle is a few hundred bytes and a lobby snapshot a few
 * hundred more, so this holds a long hiccup. */
#define WEB_IN_CAP (256u << 10)

static struct {
    TAK_LinkState state;
    const char   *why;
    uint8_t       in[WEB_IN_CAP];
    size_t        in_len;
    uint8_t       overflow;
} g_link;

/* The page calls these three. They are the only way into this file
 * from JS, and each one is a single job. */

EMSCRIPTEN_KEEPALIVE
void TAK_NetLink_WebOpened(void) {
    if (g_link.state == TAK_LINK_OPENING) g_link.state = TAK_LINK_OPEN;
}

EMSCRIPTEN_KEEPALIVE
void TAK_NetLink_WebClosed(void) {
    if (g_link.state == TAK_LINK_OPEN || g_link.state == TAK_LINK_OPENING) {
        g_link.state = TAK_LINK_FAILED;
        if (!g_link.why) g_link.why = "the connection closed";
    }
}

EMSCRIPTEN_KEEPALIVE
void TAK_NetLink_WebMessage(const uint8_t *data, int len) {
    if (len <= 0 || !data) return;
    if (g_link.in_len + (size_t)len > sizeof g_link.in) {
        /* The frame loop has not run for a long time, or the server is
         * sending faster than this machine can read. Either way, losing
         * a message quietly would leave a hole in the turn stream, and
         * a simulation cannot run over a hole. */
        g_link.overflow = 1;
        return;
    }
    memcpy(g_link.in + g_link.in_len, data, (size_t)len);
    g_link.in_len += (size_t)len;
}

/* HEAPU8, _malloc and the exported C functions are the glue's own
 * names and are in scope here. Reaching them through Module only works
 * when they are in EXPORTED_RUNTIME_METHODS, which they are not, and
 * the first browser run failed on exactly that: the socket opened and
 * every send threw on an undefined Module.HEAPU8. The socket itself is
 * parked on Module because it has to outlive one call. */
EM_JS(int, web_open, (const char *url), {
    try {
        if (Module.okSocket) { try { Module.okSocket.close(); } catch (e) {} }
        var s = new WebSocket(UTF8ToString(url));
        s.binaryType = 'arraybuffer';
        Module.okSocket = s;
        s.onopen = function () { _TAK_NetLink_WebOpened(); };
        s.onclose = function () { _TAK_NetLink_WebClosed(); };
        s.onerror = function () { _TAK_NetLink_WebClosed(); };
        s.onmessage = function (e) {
            var bytes = new Uint8Array(e.data);
            var p = _malloc(bytes.length);
            HEAPU8.set(bytes, p);
            _TAK_NetLink_WebMessage(p, bytes.length);
            _free(p);
        };
        return 0;
    } catch (e) {
        return -1;
    }
});

EM_JS(int, web_send, (const uint8_t *data, int len), {
    var s = Module.okSocket;
    if (!s || s.readyState !== 1) return -1;
    try {
        /* A copy, not a view: send on a view into the wasm heap can be
         * read after the heap has moved under it. */
        s.send(HEAPU8.slice(data, data + len));
        return 0;
    } catch (e) {
        return -1;
    }
});

EM_JS(void, web_close, (void), {
    var s = Module.okSocket;
    Module.okSocket = null;
    if (s) { try { s.close(); } catch (e) {} }
});

int TAK_NetLink_Open(const char *url) {
    memset(&g_link, 0, sizeof g_link);
    if (!url || !url[0]) {
        g_link.state = TAK_LINK_FAILED;
        g_link.why = "no address to open";
        return -1;
    }
    if (web_open(url) != 0) {
        g_link.state = TAK_LINK_FAILED;
        g_link.why = "the page would not open that address";
        return -1;
    }
    g_link.state = TAK_LINK_OPENING;
    return 0;
}

TAK_LinkState TAK_NetLink_State(void) { return g_link.state; }

const char *TAK_NetLink_Why(void) {
    return g_link.why ? g_link.why : "";
}

void TAK_NetLink_Close(void) {
    web_close();
    g_link.state = TAK_LINK_CLOSED;
    g_link.in_len = 0;
}

void TAK_NetLink_Pump(TAK_NetClient *c, uint64_t now_ms) {
    if (g_link.overflow) {
        g_link.why = "messages arrived faster than the frame loop read them";
        g_link.state = TAK_LINK_FAILED;
        g_link.in_len = 0;
        g_link.overflow = 0;
        TAK_NetClient_OnClose(c);
        return;
    }

    /* Whole messages, because the browser framed them. */
    size_t off = 0;
    while (off < g_link.in_len) {
        size_t whole = TAK_Net_PeekLen(g_link.in + off, g_link.in_len - off);
        if (whole == 0) break;
        if (TAK_NetClient_OnMessage(c, g_link.in + off, whole, now_ms) != 0) {
            g_link.why = "a message this build could not read";
            g_link.state = TAK_LINK_FAILED;
            g_link.in_len = 0;
            TAK_NetClient_OnClose(c);
            return;
        }
        off += whole;
    }
    if (off >= g_link.in_len) {
        g_link.in_len = 0;
    } else if (off > 0) {
        memmove(g_link.in, g_link.in + off, g_link.in_len - off);
        g_link.in_len -= off;
    }

    if (g_link.state != TAK_LINK_OPEN) return;

    uint8_t msg[TAK_NET_FRAME_MAX];
    size_t n;
    while ((n = TAK_NetClient_TakeMessage(c, msg, sizeof msg)) > 0) {
        if (web_send(msg, (int)n) != 0) {
            g_link.why = "the page would not send";
            g_link.state = TAK_LINK_FAILED;
            TAK_NetClient_OnClose(c);
            return;
        }
    }
}

#endif /* __EMSCRIPTEN__ */

/* Both halves are compiled everywhere and one of them is empty, which
 * ISO C does not allow a translation unit to be. */
typedef int tak_link_translation_unit_is_not_empty;

