#ifndef TAK_MOD_FETCH_H
#define TAK_MOD_FETCH_H

#include <stdint.h>

/* ── Fetching a room's mod from the lobby ────────────────────────────
 *
 * Join on a game whose data differs from ours asks the mod registry for
 * the mod set it plays. The first Join looks, and the status line says
 * what the registry has. A second Join on the same game fetches it,
 * checks it against the registry and the room, and installs it under
 * the mod root. The mod set is mounted at startup, so the game is
 * joined after a restart, which the install arranges.
 *
 * On the desktop the registry and the download come from the server
 * the lobby is connected to, over plain HTTP, on a thread of their own.
 * In the browser the page does all of it (web/mods.js) and reloads
 * into the game. */

/* A Join on a room whose data differs. Returns 1 when the registry
 * question took the press and ModFetch_Text says what happened, 0 when
 * the registry has nothing for the room and the usual advice stands. */
int ModFetch_Join(const char *address, const char *code, const char *mod,
                  const char *version, uint64_t content);

/* The line for the status bar, and whether it changed since asked. */
const char *ModFetch_Text(void);
int         ModFetch_Changed(void);

/* Settings written by an install, read once at startup. */
#define MODFETCH_JOIN_SETTING "JoinAfterRestart"

/* --registry, --install-mod <id> and --remove-mod <id>: what = "list",
 * "install" or "remove". Prints what happened and returns the exit
 * status. address is the server's, root the mod root. */
int ModFetch_Command(const char *what, const char *id, const char *address,
                     const char *root);

#endif /* TAK_MOD_FETCH_H */
