#ifndef TAK_NET_SESSION_H
#define TAK_NET_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "tak_net_client.h"

/*
 * The one multiplayer session the game is in, and the thing the
 * screens talk to.
 *
 * Below this, nothing knows what a screen is: the client, the
 * connection pump and the relay core are all transport free and all
 * tested against each other with no network. Above it, no screen knows
 * what a socket is. This owns the link and the client together so
 * neither side has to.
 *
 * There is one of these because a player is in one session at a time,
 * the same reason there is one world.
 */

typedef enum {
    NET_SESSION_OFF = 0,
    NET_SESSION_CONNECTING,   /* the link is coming up, or HELLO is out */
    NET_SESSION_READY,        /* welcomed, in the lobby or in a room    */
    NET_SESSION_FAILED        /* it did not come up, or it went         */
} NetSessionState;

/* Where to connect when the player has not said. In a browser this is
 * the page's own origin, which is how the address stays out of the
 * source: the page already knows where it came from. On the desktop
 * there is no default and this writes an empty string, because a
 * desktop build has nothing to guess from and guessing at a host is
 * how a deployment detail ends up in a repository. */
void NetSession_DefaultAddress(char *out, size_t cap);

/* What --relay put on the command line, which wins over the default.
 * A link carrying one is how a player reaches a particular server
 * without typing its address. NULL when none was given. */
void NetSession_SetPreferredAddress(const char *address);

/* Open a session to `address` as `player_name`. Returns 0 when the
 * attempt started, which is not the same as connected. */
int NetSession_Connect(const char *address, const char *player_name);

/* Move bytes and messages. Call it every frame, from whichever screen
 * is up, because a lobby that stops pumping stops hearing. */
void NetSession_Tick(uint64_t now_ms);

void NetSession_Disconnect(void);

NetSessionState NetSession_State(void);

/* Why it failed, in words a player can read. Never NULL. */
const char *NetSession_Why(void);

/* The session client, for a screen to read the room list and the room
 * snapshot from, and to ask for things through. NULL when off. */
TAK_NetClient *NetSession_Client(void);

/* A session with no link under it, for a test. Everything above
 * behaves the same and messages are fed in with
 * TAK_NetClient_OnMessage, which is how the screens are tested with no
 * server in the room. */
void NetSession_BeginWithoutLink(const char *player_name);

#endif /* TAK_NET_SESSION_H */
