/*
 * net_session.c -- the one multiplayer session the game is in.
 *
 * See tak_net_session.h for why this sits between the screens and the
 * link rather than either owning the other.
 */

#include "tak_data_fingerprint.h"
#include "tak_net_session.h"
#include "tak_net_link.h"
#include "tak_settings.h"

#include <SDL.h>
#include <stdint.h>
#include <time.h>

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Which float environment this build simulates in. Every native build
 * used to claim one class, and the handshake then let two of them into
 * one room (docs/notes/2026-09-14-float-determinism.md). */
#define SESSION_DETERMINISM_CLASS TAK_Net_DeterminismClass()

uint8_t NetSession_DeterminismClass(void) { return SESSION_DETERMINISM_CLASS; }

static struct {
    NetSessionState state;
    TAK_NetClient   client;
    const char     *why;
    uint8_t         has_link;
} g_session;

#ifdef __EMSCRIPTEN__
/* The page's own origin as a WebSocket address. A page served over
 * https gets wss, which is what a browser insists on from a secure
 * page. Nothing about a particular deployment is written down here:
 * the page knows where it came from and this asks it. */
/* Where the page says the game server is.
 *
 * A relay is a process holding sockets open and this page is a
 * directory of files, so the two need not be the same host and on a
 * static file host they cannot be. The page reads the address out of a
 * relay.txt beside it and leaves it on Module.okRelayUrl. None of that
 * address is in this repository.
 *
 * With no such file the page's own origin is the guess, which is right
 * when the page really is served by the relay. When nothing answers
 * there the screen says so rather than looking broken. */
EM_JS(void, session_page_origin, (char *out, int cap), {
    try {
        var url = (typeof Module !== 'undefined' && Module.okRelayUrl)
                ? Module.okRelayUrl
                : ((location.protocol === 'https:' ? 'wss://' : 'ws://')
                   + location.host + '/relay');
        stringToUTF8(url, out, cap);
    } catch (e) {
        if (cap > 0) HEAPU8[out] = 0;
    }
});
#endif

/* What --relay named, if anything. Not stored with the player
 * settings: it is an argument for this run, and the settings file
 * carries integers only. */
static char g_preferred[160];

void NetSession_SetPreferredAddress(const char *address) {
    g_preferred[0] = '\0';
    if (!address || !address[0]) return;
    size_t n = strlen(address);
    if (n >= sizeof g_preferred) n = sizeof g_preferred - 1;
    memcpy(g_preferred, address, n);
    g_preferred[n] = '\0';
}

void NetSession_DefaultAddress(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (g_preferred[0]) {
        size_t n = strlen(g_preferred);
        if (n >= cap) n = cap - 1;
        memcpy(out, g_preferred, n);
        out[n] = '\0';
        return;
    }
#ifdef __EMSCRIPTEN__
    session_page_origin(out, (int)cap);
#endif
}

/* This machine's token, sixteen random bytes made once and kept with the
 * settings as hex, so a player who reloads or restarts is the same
 * player to the server and two players with one name are not. */
static void device_token(uint8_t out[TAK_NET_TOKEN_BYTES]) {
    const char *hex = Settings_GetStr("DeviceToken", "");
    int ok = hex && strlen(hex) == TAK_NET_TOKEN_BYTES * 2;
    for (int i = 0; ok && i < TAK_NET_TOKEN_BYTES; i++) {
        unsigned v = 0;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) ok = 0;
        out[i] = (uint8_t)v;
    }
    if (ok) return;
    /* Not the simulation's generator: this only has to differ between
     * machines and between runs. */
    uint64_t seed = (uint64_t)SDL_GetPerformanceCounter() ^ ((uint64_t)SDL_GetTicks64() << 32) ^
                    (uint64_t)(uintptr_t)out ^ (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull;
    char text[TAK_NET_TOKEN_BYTES * 2 + 1];
    for (int i = 0; i < TAK_NET_TOKEN_BYTES; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        out[i] = (uint8_t)(seed >> 24);
        snprintf(text + i * 2, 3, "%02x", out[i]);
    }
    Settings_SetStr("DeviceToken", text);
    Settings_Save();
}

static void fill_hello(TAK_MsgHello *h, const char *player_name) {
    memset(h, 0, sizeof *h);
    h->protocol_version = TAK_NET_PROTOCOL_VERSION;
    h->engine_build_id = TAK_NET_PROTOCOL_VERSION;
    h->determinism_class = SESSION_DETERMINISM_CLASS;
    h->client_kind = 0;
    /* The device token is what a rejoin is recognised by, and every
     * hello asks for one: a server with no match of ours to hand back
     * just says welcome. */
    size_t n = player_name ? strlen(player_name) : 0;
    device_token(h->device_token);
    h->flags |= TAK_HELLOF_WANTS_REJOIN;
    if (player_name && player_name[0]) {
        size_t cap = sizeof h->name - 1;
        size_t len = n < cap ? n : cap;
        memcpy(h->name, player_name, len);
        h->name[len] = '\0';
    }
    /* What the simulation reads, so the relay can keep apart two
     * players who would play different games (docs/MULTIPLAYER.md). */
    const TAK_DataFingerprint *fp = TAK_DataFingerprint_Get();
    if (fp) {
        h->schema_hash = fp->schema;
        h->content_hash = fp->content;
        for (int g = 0; g < TAK_NET_GROUP_HASHES && g < TAK_DATA_GROUP_COUNT; g++)
            h->group_hash[g] = fp->group[g];
    }
}

int NetSession_Connect(const char *address, const char *player_name) {
    NetSession_Disconnect();
    memset(&g_session, 0, sizeof g_session);

    if (!address || !address[0]) {
        g_session.state = NET_SESSION_FAILED;
        g_session.why = "No server address.";
        return -1;
    }
    if (TAK_NetLink_Open(address) != 0) {
        g_session.state = NET_SESSION_FAILED;
        g_session.why = TAK_NetLink_Why();
        return -1;
    }
    g_session.has_link = 1;

    TAK_MsgHello h;
    fill_hello(&h, player_name);
    TAK_NetClient_Init(&g_session.client, &h);
    g_session.state = NET_SESSION_CONNECTING;
    return 0;
}

void NetSession_BeginWithoutLink(const char *player_name) {
    NetSession_Disconnect();
    memset(&g_session, 0, sizeof g_session);
    TAK_MsgHello h;
    fill_hello(&h, player_name);
    TAK_NetClient_Init(&g_session.client, &h);
    g_session.has_link = 0;
    g_session.state = NET_SESSION_CONNECTING;
}

void NetSession_Disconnect(void) {
    if (g_session.has_link) TAK_NetLink_Close();
    memset(&g_session, 0, sizeof g_session);
    g_session.state = NET_SESSION_OFF;
}

NetSessionState NetSession_State(void) { return g_session.state; }

const char *NetSession_Why(void) {
    return g_session.why ? g_session.why : "";
}

TAK_NetClient *NetSession_Client(void) {
    return g_session.state == NET_SESSION_OFF ? NULL : &g_session.client;
}

void NetSession_Tick(uint64_t now_ms) {
    if (g_session.state == NET_SESSION_OFF ||
        g_session.state == NET_SESSION_FAILED) {
        return;
    }
    if (g_session.has_link) {
        TAK_NetLink_Pump(&g_session.client, now_ms);
        if (TAK_NetLink_State() == TAK_LINK_FAILED &&
            g_session.state != NET_SESSION_FAILED) {
            g_session.state = NET_SESSION_FAILED;
            g_session.why = TAK_NetLink_Why();
            return;
        }
    }

    switch (g_session.client.state) {
    case TAK_NC_LOBBY:
    case TAK_NC_ROOM:
    case TAK_NC_LOADING:
    case TAK_NC_PLAYING:
        g_session.state = NET_SESSION_READY;
        break;
    case TAK_NC_REFUSED:
        g_session.state = NET_SESSION_FAILED;
        /* The server's own words, which are the ones a player can act
         * on. The link is fine, so its reason would say nothing. */
        g_session.why = g_session.client.reject.text[0]
                      ? g_session.client.reject.text
                      : "The server would not let you in.";
        break;
    case TAK_NC_GONE:
        g_session.state = NET_SESSION_FAILED;
        if (!g_session.why) g_session.why = "The connection closed.";
        break;
    default:
        break;
    }
}
