#ifndef TAK_SETTINGS_H
#define TAK_SETTINGS_H

/* Player settings that outlive a session: the Options dialogs write
 * them and the engine reads them at start. The original keeps these in
 * the registry under per-page keys; here they live in one text file,
 * "options.cfg" under the platform's preferences directory, as
 * key=value lines. Keys use the original's names so a note that cites
 * one (DisplayDamageBars, DrawShadows) reads the same. */

/* The battle's scale, "original" or "fit" (the Visual page's
 * Resolution slider). Under Original, the screen pixels to a game pixel
 * in a browser (0 follows the page) and the desktop window's size in
 * the original's own key names. */
#define TAK_SETTING_SCALE      "BattleScale"
#define TAK_SETTING_PIXEL_SIZE "PixelSize"
#define TAK_SETTING_SCREEN_W   "InGameScreenWidth"
#define TAK_SETTING_SCREEN_H   "InGameScreenHeight"

/* Read the file. Missing file is not an error. Returns 0 on success. */
int  Settings_Load(void);

/* Write every known key. Returns 0 on success. */
int  Settings_Save(void);

int  Settings_GetInt(const char *key, int default_value);
void Settings_SetInt(const char *key, int value);

/* The same store for text. The file has always been key=value lines,
 * so this changes nothing about its shape. A value is written on one
 * line, so anything past a newline is refused rather than writing a
 * file that reads back as two settings.
 *
 * Reconnect waits on this too: the device token a rejoin is recognised
 * by has to survive a restart, and the store could not hold one. */
const char *Settings_GetStr(const char *key, const char *default_value);
void        Settings_SetStr(const char *key, const char *value);

/* Tests point the store at a scratch directory. NULL restores the
 * platform default. */
void Settings_SetDirectory(const char *dir);
const char *Settings_FilePath(void);

#endif
