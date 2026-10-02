#ifndef TAK_MOD_PROXY_H
#define TAK_MOD_PROXY_H

#include <stddef.h>
#include <stdint.h>

#include "tak_mod_registry.h"

/* ── The relay's mod download stream ────────────────────────────────
 *
 *   GET /api/mods/<id>/download
 *
 * streams a registry entry's download with Access-Control-Allow-Origin,
 * because the hosts mods live on mostly send none and a page cannot
 * read their answer without it. The request names a registry id and
 * never a URL, so only what the registry lists is ever fetched: this is
 * no open proxy. A download is cut off past the size the registry
 * lists, and at most TAK_MODPROXY_SLOTS run at once.
 *
 * No sockets and no HTTP client here. The relay's fetcher feeds bytes
 * in with Data and Done, and the server takes them out with Drain, so a
 * test drives both sides by hand. */

#define TAK_MODPROXY_SLOTS  4
#define TAK_MODPROXY_STAGE  (256u << 10)

/* What Data answers besides len: offer the same bytes again later, or
 * give up on the transfer. */
#define TAK_MODPROXY_PAUSE  ((size_t)-1)
#define TAK_MODPROXY_ABORT  ((size_t)-2)

typedef struct TAK_ModProxySlot {
    int      state;             /* 0 free, 1 fetching, 2 fetched, 3 failed */
    uint32_t conn;
    const TAK_ModEntry *entry;
    uint64_t got;               /* bytes taken from upstream */
    int      paused;
    uint64_t moved_ms;          /* when a byte last came in or went out */
    char     head[640];         /* the status line and headers, or a whole error answer */
    size_t   head_len, head_off;
    size_t   stage_len;
    uint8_t  stage[TAK_MODPROXY_STAGE];
} TAK_ModProxySlot;

typedef struct TAK_ModProxy {
    const TAK_ModRegistry *reg;
    int can_fetch;
    TAK_ModProxySlot slot[TAK_MODPROXY_SLOTS];
} TAK_ModProxy;

/* can_fetch is 0 on a relay built without an HTTP client, which then
 * answers 501 to every download. */
void TAK_ModProxy_Init(TAK_ModProxy *p, const TAK_ModRegistry *reg, int can_fetch);

/* Whether a plain request is a GET or HEAD of /api/mods/<id>/download. */
int TAK_ModProxy_IsDownload(const uint8_t *req, size_t len);

/* Starts a download for conn. Returns the slot it streams through, or
 * -1 with a whole answer in out and its length in *out_len: 404 for an
 * id the registry does not list or lists as manual, 503 with every slot
 * busy, 501 when this relay cannot fetch. HEAD answers at once. */
int TAK_ModProxy_Begin(TAK_ModProxy *p, uint32_t conn, const uint8_t *req, size_t len,
                       char *out, size_t cap, size_t *out_len);

const char *TAK_ModProxy_Url(const TAK_ModProxy *p, int slot);
uint64_t    TAK_ModProxy_Size(const TAK_ModProxy *p, int slot);

/* The fetcher's side. Data takes all of len, or answers PAUSE when the
 * stage is full or ABORT past the listed size or for a slot that went.
 * Done ends the transfer, and ok with fewer bytes than listed fails. */
size_t TAK_ModProxy_Data(TAK_ModProxy *p, int slot, const void *bytes, size_t len);
void   TAK_ModProxy_Done(TAK_ModProxy *p, int slot, int ok, const char *why);
/* 1 once a paused fetch's stage has room again, and the pause is over. */
int    TAK_ModProxy_Resume(TAK_ModProxy *p, int slot);

/* The connection's side. write takes up to len bytes for conn and says
 * how many it took. last means nothing follows, so close behind them.
 * A download where nothing has moved for TAK_MODPROXY_STALL_MS ends. */
#define TAK_MODPROXY_STALL_MS 120000u
typedef size_t (*TAK_ModProxyWrite)(void *ctx, uint32_t conn, const void *bytes,
                                    size_t len, int last);
void TAK_ModProxy_Drain(TAK_ModProxy *p, TAK_ModProxyWrite write, void *ctx,
                        uint64_t now_ms);

/* Whether the slot still wants bytes, so a fetcher can drop the rest. */
int  TAK_ModProxy_Fetching(const TAK_ModProxy *p, int slot);

/* conn went away. Frees its slot and returns it so the fetcher can drop
 * the transfer, or -1 when it had none. */
int  TAK_ModProxy_Cancel(TAK_ModProxy *p, uint32_t conn);

/* The relay's fetcher, mod_fetch.c: libcurl where the build found it,
 * else nothing, and the proxy answers 501. */
int  TAK_ModFetch_Available(void);
int  TAK_ModFetch_Start(TAK_ModProxy *p, int slot);
void TAK_ModFetch_Pump(TAK_ModProxy *p);
void TAK_ModFetch_Stop(int slot);

#endif /* TAK_MOD_PROXY_H */
