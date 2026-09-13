#ifndef TAK_NET_LINK_H
#define TAK_NET_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_client.h"

/*
 * The one thing above the client that does know what a socket is.
 *
 * The client, the connection pump and the relay core are all transport
 * free, which is what let them be tested against each other with no
 * network in the room. Something still has to open the connection, and
 * that something is different in a browser and on a desktop:
 *
 *   browser  the page already has a WebSocket, and it delivers whole
 *            messages. There is no framing to do and no socket to own.
 *   native   a TCP socket with ws_conn over it doing the framing.
 *
 * One API over both, so the screens and the game loop never find out
 * which they are on.
 */

typedef enum {
    TAK_LINK_CLOSED = 0,
    TAK_LINK_OPENING,     /* connecting, or mid handshake             */
    TAK_LINK_OPEN,        /* messages flow                            */
    TAK_LINK_FAILED       /* it did not come up, or it went           */
} TAK_LinkState;

/* Open a link to `url`. A browser takes the whole ws:// or wss:// URL
 * and hands it to the page. A native build parses out the host, port
 * and path, and refuses wss:// because it terminates no TLS of its
 * own: a desktop client reaches a secure server through the same
 * reverse proxy a browser does, which is a later piece of work.
 *
 * Returns 0 when the attempt started, which is not the same as
 * connected. Poll TAK_NetLink_State until it leaves OPENING. */
int TAK_NetLink_Open(const char *url);

TAK_LinkState TAK_NetLink_State(void);

/* Why it failed, for a log line or a dialog. Never NULL. */
const char *TAK_NetLink_Why(void);

/* Move bytes both ways and hand whole messages to the client. Call it
 * every frame. `now_ms` is the caller's clock, the same one the rest
 * of the session is stamped with. */
void TAK_NetLink_Pump(TAK_NetClient *c, uint64_t now_ms);

void TAK_NetLink_Close(void);

#endif /* TAK_NET_LINK_H */
