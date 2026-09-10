#ifndef TAK_SETTINGS_H
#define TAK_SETTINGS_H

/* Player settings that outlive a session: the Options dialogs write
 * them and the engine reads them at start. The original keeps these in
 * the registry under per-page keys; here they live in one text file,
 * "options.cfg" under the platform's preferences directory, as
 * key=value lines. Keys use the original's names so a note that cites
 * one (DisplayDamageBars, DrawShadows) reads the same. */

/* Read the file. Missing file is not an error. Returns 0 on success. */
int  Settings_Load(void);

/* Write every known key. Returns 0 on success. */
int  Settings_Save(void);

int  Settings_GetInt(const char *key, int default_value);
void Settings_SetInt(const char *key, int value);

/* Tests point the store at a scratch directory. NULL restores the
 * platform default. */
void Settings_SetDirectory(const char *dir);
const char *Settings_FilePath(void);

#endif
