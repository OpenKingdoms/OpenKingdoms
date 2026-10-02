#ifndef TAK_HTTP_GET_H
#define TAK_HTTP_GET_H

#include <stddef.h>
#include <stdint.h>

/* One plain HTTP GET, for the desktop builds, which speak no TLS. The
 * relay answers on the port its WebSocket uses, so the address a player
 * plays on is where the mod registry and its downloads come from
 * (/api/mods, tak_mod_proxy.h). Blocking, so call it off the frame.
 *
 * address is ws://host[:port][/path], http://host[:port] or host[:port].
 * The body goes into *body, which the caller frees, at most cap bytes.
 * progress, when given, counts body bytes as they arrive. Returns 0 on
 * a 200, else -1 with why. The browser build has no use for this and
 * always fails. */
int TAK_HttpGet(const char *address, const char *path, size_t cap,
                uint8_t **body, size_t *len, volatile size_t *progress,
                char *why, size_t why_cap);

#endif /* TAK_HTTP_GET_H */
