#ifndef TAK_PATHS_H
#define TAK_PATHS_H

#include <stddef.h>

/* One resolver for every file the game writes on the player's behalf:
 * options.cfg, saved games, and anything later that outlives a session.
 *
 * The preference directory is SDL's, built from "OpenKingdoms" as both
 * the organisation and the application, so it is doubled on disk. That
 * is left alone on purpose: changing it would orphan the options.cfg
 * players already have. See docs/notes/paths.md.
 *
 * Separators are never assumed. SDL hands back a backslash path on
 * Windows and a forward slash path everywhere else, so anything this
 * module appends reuses whichever separator the directory already
 * carries and a composed path never mixes the two. */

/* The preference directory, always with a trailing separator. */
const char *Paths_PrefDir(void);

/* The saved game directory under it, created on first use. */
const char *Paths_SaveDir(void);

/* Full path of one save. Returns 0 on success, -1 when the slug is
 * unusable or the buffer is too small. A slug carrying a separator, a
 * drive colon or a parent reference is refused: the display name lives
 * inside the file, so the slug never has to be anything but a plain
 * file name. */
int Paths_SaveFile(const char *slug, char *out, size_t cap);

/* Point every path at `dir` instead of the platform default. NULL or
 * an empty string restores the default. Tests use this, and
 * Settings_SetDirectory is a thin wrapper over it. */
void Paths_SetOverride(const char *dir);

/* Tell the host that something under the preference directory changed.
 * A no-op on the desktop, where the write already reached the disk. In
 * the browser the write landed in a filesystem that dies with the tab,
 * so this asks the page to copy it out to origin private storage. */
void Paths_NotifyPrefWritten(void);

/* Bring the saved games this machine holds into the filesystem the
 * game reads, and say whether that is still happening.
 *
 * On a desktop they are already there: the first does nothing and the
 * second always answers no. In a browser they live in origin private
 * storage, which can only be read a promise at a time, so the copy
 * runs while the dialog is open and the dialog waits for it.
 *
 * The page used to do this at boot, which charged every player the
 * price of every save they had ever made before the main menu drew,
 * whether or not they were going to load one. */
void Paths_BeginSaveSync(void);
int  Paths_SavesPending(void);

/* Answer the question above with `pending` instead of asking the host,
 * so a desktop test can drive the case only a browser reaches. -1 puts
 * it back to asking. */
void Paths_PretendSavesArePending(int pending);

#endif
