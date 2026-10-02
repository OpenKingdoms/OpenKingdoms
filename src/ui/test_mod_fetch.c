/*
 * test_mod_fetch.c -- Join in the desktop lobby on a game that plays a
 * mod the registry has: the first press looks, the second fetches,
 * checks and installs, and the next start is set to join.
 *
 * Data free. A small HTTP server on the loopback address plays the
 * relay's /api/mods and download routes, and everything is written in
 * scratch folders beside the binary.
 */

#include "test_framework.h"
#include "tak_mod_fetch.h"
#include "tak_mod_install.h"
#include "tak_modset.h"
#include "tak_settings.h"
#include "tak_sha256.h"
#include "net_socket.h"
#include "miniz.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define ROOT "test_modfetch_tmp"

static TakSocket      g_listen = TAK_INVALID_SOCKET;
static unsigned short g_port;
static SDL_atomic_t   g_stop;
static char           g_registry[2048];
static void          *g_zip;
static size_t         g_zip_len;
static int            g_tamper;

static void answer(TakSocket c, const char *type, const void *body, size_t len, int code) {
    char head[256];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Connection: close\r\n\r\n", code, type, (unsigned)len);
    send(c, head, n, 0);
    if (len) send(c, (const char *)body, (int)len, 0);
}

static int serve(void *unused) {
    (void)unused;
    while (!SDL_AtomicGet(&g_stop)) {
        TakSocket c = accept(g_listen, NULL, NULL);
        if (c == TAK_INVALID_SOCKET) { SDL_Delay(5); continue; }
        /* Windows and the BSDs hand on the listener's non blocking mode. */
#ifdef _WIN32
        u_long off = 0;
        ioctlsocket(c, FIONBIO, &off);
#else
        fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
#endif
        char req[1024];
        int got = 0;
        while (got < (int)sizeof req - 1) {
            int n = (int)recv(c, req + got, (int)sizeof req - 1 - got, 0);
            if (n <= 0) break;
            got += n;
            req[got] = '\0';
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[got > 0 ? got : 0] = '\0';
        if (strncmp(req, "GET /api/mods ", 14) == 0) {
            answer(c, "application/json", g_registry, strlen(g_registry), 200);
        } else if (strncmp(req, "GET /api/mods/tough-swords/download ", 36) == 0) {
            unsigned char *z = (unsigned char *)malloc(g_zip_len);
            memcpy(z, g_zip, g_zip_len);
            if (g_tamper) z[g_zip_len / 2] ^= 1;
            answer(c, "application/zip", z, g_zip_len, 200);
            free(z);
        } else {
            const char *e = "{\"error\":\"that mod is not in the registry\"}";
            answer(c, "application/json", e, strlen(e), 404);
        }
        TakNet_Close(c);
    }
    return 0;
}

static int start_server(void) {
    if (TakNet_Start() != 0) return -1;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only, no firewall prompt */
    socklen_t al = sizeof a;
    if (bind(g_listen, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_listen, 8) != 0 ||
        getsockname(g_listen, (struct sockaddr *)&a, &al) != 0) return -1;
    g_port = ntohs(a.sin_port);
    TakNet_SetNonBlocking(g_listen);
    SDL_Thread *t = SDL_CreateThread(serve, "testserver", NULL);
    if (!t) return -1;
    SDL_DetachThread(t);
    return 0;
}

/* The status line once it says want, or "" when it never does. */
static const char *wait_for(const char *want) {
    for (int i = 0; i < 1000; i++) {
        const char *t = ModFetch_Text();
        if (strstr(t, want)) return t;
        SDL_Delay(10);
    }
    printf("\n    last status: %s\n", ModFetch_Text());
    return "";
}

static void rm(const char *p) { remove(p); }

static void clean(void) {
    char why[128];
    TAK_ModInstall_Remove(ROOT, "tough-swords", why, sizeof why);
    rm(ROOT "/options.cfg");
}

static void setup(void) {
    static const char tdf[] = "[OKSTONE]\n{\nheight=12;\n}\n";
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    mz_zip_writer_init_heap(&z, 0, 0);
    mz_zip_writer_add_mem(&z, "Mods/Tough Swords/features/zz/okstone.tdf", tdf, strlen(tdf), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_heap_archive(&z, &g_zip, &g_zip_len);
    mz_zip_writer_end(&z);
    uint8_t d[32];
    char hex[65];
    TAK_Sha256_Hash(g_zip, g_zip_len, d);
    TAK_Sha256_ToHex(d, hex);
    snprintf(g_registry, sizeof g_registry,
             "{\"registry\":1,\"mods\":[{\"id\":\"tough-swords\",\"name\":\"Tough Swords\",\"version\":\"0.2\","
             "\"author\":\"Someone\",\"page\":\"https://example.org/ts\",\"modset\":\"tough-swords\","
             "\"url\":\"https://example.org/ts.zip\",\"size\":%u,\"sha256\":\"%s\",\"fingerprint\":\"5d0e44a1abcdef12\"},"
             "{\"id\":\"tne\",\"name\":\"The New Era\",\"version\":\"6.0\",\"author\":\"Sage\","
             "\"page\":\"https://example.org/tne\",\"manual\":\"by hand\"}]}",
             (unsigned)g_zip_len, hex);
}

static char g_address[64];

TEST(the_first_join_asks_and_the_second_fetches_installs_and_sets_the_join) {
    clean();
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "AAAA", "Tough Swords", "0.2", 0x5d0e44a1abcdef12ULL));
    ASSERT_NOT_NULL(strstr(wait_for("Press Join again"), "That game plays Tough Swords 0.2 by Someone"));
    ASSERT(ModFetch_Changed());
    ASSERT(!ModFetch_Changed());
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "AAAA", "Tough Swords", "0.2", 0x5d0e44a1abcdef12ULL));
    ASSERT(wait_for("is installed and checked")[0]);
    ASSERT_EQ_INT(1, TAK_ModInstall_Installed(ROOT, "tough-swords", NULL, 0));
    ASSERT_EQ_STR("tough-swords", Settings_GetStr("ModSet", ""));
    ASSERT_EQ_STR("AAAA", Settings_GetStr(MODFETCH_JOIN_SETTING, ""));
    clean();
}

TEST(a_tampered_download_is_refused_and_nothing_installed) {
    clean();
    Settings_SetStr("ModSet", "vanilla");
    g_tamper = 1;
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "BBBB", "Tough Swords", "0.2", 0x5d0e44a1abcdef12ULL));
    ASSERT(wait_for("Press Join again")[0]);
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "BBBB", "Tough Swords", "0.2", 0x5d0e44a1abcdef12ULL));
    ASSERT(wait_for("does not match the registry's fingerprint")[0]);
    g_tamper = 0;
    ASSERT_EQ_INT(0, TAK_ModInstall_Installed(ROOT, "tough-swords", NULL, 0));
    ASSERT_EQ_STR("vanilla", Settings_GetStr("ModSet", ""));
}

TEST(a_room_on_other_data_or_a_manual_mod_is_not_fetched) {
    clean();
    /* Same name and version, other fingerprint: the registry's would not
     * join, so it is not offered. */
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "CCCC", "Tough Swords", "0.2", 0x1234ULL));
    ASSERT(wait_for("plays other data than that game")[0]);
    ASSERT_EQ_INT(0, TAK_ModInstall_Installed(ROOT, "tough-swords", NULL, 0));
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "DDDD", "The New Era", "6.0", 0x77ULL));
    ASSERT_NOT_NULL(strstr(wait_for("installs by hand"), "https://example.org/tne"));
    /* Not in the registry: the lobby's own advice stands. */
    ASSERT_EQ_INT(1, ModFetch_Join(g_address, "EEEE", "Homebrew", "1", 0x88ULL));
    for (int i = 0; i < 500 && ModFetch_Join(g_address, "EEEE", "Homebrew", "1", 0x88ULL); i++) SDL_Delay(10);
    ASSERT_EQ_INT(0, ModFetch_Join(g_address, "EEEE", "Homebrew", "1", 0x88ULL));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    TEST_SUITE("Mod fetch: Join on a game that plays a registry mod");
    SDL_SetMainReady();
    if (SDL_Init(0) != 0 || start_server() != 0) {
        printf("could not start the loopback server\n");
        return 1;
    }
    snprintf(g_address, sizeof g_address, "ws://127.0.0.1:%u/relay", (unsigned)g_port);
#ifdef _WIN32
    _mkdir(ROOT);
#else
    mkdir(ROOT, 0755);
#endif
    Settings_SetDirectory(ROOT "/");
    TAK_ModSet_SetRoot(ROOT);
    setup();
    RUN(the_first_join_asks_and_the_second_fetches_installs_and_sets_the_join);
    RUN(a_tampered_download_is_refused_and_nothing_installed);
    RUN(a_room_on_other_data_or_a_manual_mod_is_not_fetched);
    SDL_AtomicSet(&g_stop, 1);
    SDL_Delay(50);
    clean();
    mz_free(g_zip);
    TEST_REPORT();
}
