/*
 * mod_fetch.c -- the relay's HTTP client for mod downloads.
 *
 * libcurl's multi interface, pumped once a pass of the server loop and
 * never waited on, so a slow mod host cannot stall a game's turns. Only
 * https is spoken, redirects included. A relay built without libcurl
 * has none of this and answers 501 to a download.
 */

#include "tak_mod_proxy.h"

#ifdef TAK_RELAY_CURL

#include <curl/curl.h>
#include <stdio.h>

static CURLM *g_multi;
static CURL  *g_easy[TAK_MODPROXY_SLOTS];
static TAK_ModProxy *g_proxy;

typedef struct { int slot; } Tag;
static Tag g_tag[TAK_MODPROXY_SLOTS];

int TAK_ModFetch_Available(void) {
    if (g_multi) return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
    g_multi = curl_multi_init();
    return g_multi != NULL;
}

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *user) {
    const Tag *t = (const Tag *)user;
    size_t len = size * nmemb;
    size_t took = TAK_ModProxy_Data(g_proxy, t->slot, ptr, len);
    if (took == TAK_MODPROXY_PAUSE) return CURL_WRITEFUNC_PAUSE;
    if (took == TAK_MODPROXY_ABORT) return 0;
    return took;
}

void TAK_ModFetch_Stop(int slot) {
    if (slot < 0 || slot >= TAK_MODPROXY_SLOTS || !g_easy[slot]) return;
    curl_multi_remove_handle(g_multi, g_easy[slot]);
    curl_easy_cleanup(g_easy[slot]);
    g_easy[slot] = NULL;
}

int TAK_ModFetch_Start(TAK_ModProxy *p, int slot) {
    const char *url = TAK_ModProxy_Url(p, slot);
    if (!g_multi || !url) return -1;
    TAK_ModFetch_Stop(slot);
    CURL *e = curl_easy_init();
    if (!e) return -1;
    g_proxy = p;
    g_tag[slot].slot = slot;
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &g_tag[slot]);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(e, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(e, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(e, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(e, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(e, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)TAK_ModProxy_Size(p, slot));
    curl_easy_setopt(e, CURLOPT_USERAGENT, "okrelay (OpenKingdoms mod registry)");
    curl_easy_setopt(e, CURLOPT_PRIVATE, &g_tag[slot]);
    if (curl_multi_add_handle(g_multi, e) != CURLM_OK) {
        curl_easy_cleanup(e);
        return -1;
    }
    g_easy[slot] = e;
    return 0;
}

void TAK_ModFetch_Pump(TAK_ModProxy *p) {
    if (!g_multi) return;
    g_proxy = p;
    int any = 0;
    for (int i = 0; i < TAK_MODPROXY_SLOTS; i++) {
        if (!g_easy[i]) continue;
        if (!TAK_ModProxy_Fetching(p, i)) { TAK_ModFetch_Stop(i); continue; }
        any = 1;
        if (TAK_ModProxy_Resume(p, i)) curl_easy_pause(g_easy[i], CURLPAUSE_CONT);
    }
    if (!any) return;
    int running = 0;
    curl_multi_perform(g_multi, &running);
    CURLMsg *m;
    int left = 0;
    while ((m = curl_multi_info_read(g_multi, &left)) != NULL) {
        if (m->msg != CURLMSG_DONE) continue;
        Tag *t = NULL;
        curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&t);
        if (!t) continue;
        CURLcode rc = m->data.result;
        char why[160];
        long code = 0;
        curl_easy_getinfo(m->easy_handle, CURLINFO_RESPONSE_CODE, &code);
        /* The status first: an error page can also be over the size. */
        if (rc != CURLE_OK && code >= 400) {
            snprintf(why, sizeof why, "the mod's host answered %ld", code);
        } else if (rc == CURLE_FILESIZE_EXCEEDED) {
            snprintf(why, sizeof why, "the download is larger than the registry says");
        } else {
            snprintf(why, sizeof why, "the mod's host could not be read: %s", curl_easy_strerror(rc));
        }
        if (rc != CURLE_OK) fprintf(stderr, "mod download %d: %s\n", t->slot, why);
        TAK_ModProxy_Done(p, t->slot, rc == CURLE_OK, why);
        TAK_ModFetch_Stop(t->slot);
    }
}

#else

int  TAK_ModFetch_Available(void) { return 0; }
int  TAK_ModFetch_Start(TAK_ModProxy *p, int slot) { (void)p; (void)slot; return -1; }
void TAK_ModFetch_Pump(TAK_ModProxy *p) { (void)p; }
void TAK_ModFetch_Stop(int slot) { (void)slot; }

#endif
