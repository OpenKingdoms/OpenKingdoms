#ifndef TAK_GAME_SOUND_H
#define TAK_GAME_SOUND_H

/* ══════════════════════════════════════════════════════════════════════
 *  Game-level sound dispatcher
 *
 *  Bridges gameplay events to the low-level sound API. Handles:
 *    - On-demand WAV loading with caching (sounds loaded once, reused)
 *    - Unit voice playback via sound class weighted random selection
 *    - Weapon impact sounds with material-specific variants
 *    - UI button click sounds
 *    - Positional playback for in-game sounds
 *
 *  The original engine's game-level sound functions are at
 *  the legacy reference lines 220920-221891.
 * ══════════════════════════════════════════════════════════════════════ */

/* Initialize the game sound layer. Loads no sounds at startup — they
 * are loaded on demand and cached. Call after TAK_Sound_Init and
 * SoundClass_LoadAll. */
int  GameSound_Init(void);
void GameSound_Shutdown(void);

/* Play a unit action sound. Looks up the unit's soundclass, performs
 * weighted random selection, loads the WAV if needed, and plays it
 * at the unit's position.
 *
 * soundclass_name: from the unit's FBI "soundclass" field (e.g. "ARAKNIGH")
 * action: "select", "move", "attack", "guard", "patrol", "default"
 * volume: 0-127
 * world_x/y: unit's position in world coords
 * cam_x/y: camera position, viewport_w/h: screen size (for pan/attenuation)
 *
 * Original: Sound_Play3DAtPos at line 221204 */
void GameSound_UnitAction(const char *soundclass_name, const char *action,
                           int volume, int world_x, int world_y,
                           int cam_x, int cam_y,
                           int viewport_w, int viewport_h);

/* Play a weapon hit sound at an impact position.
 * hitclass: from the weapon's "soundhitclass" (e.g. "sword", "arrow")
 * material: target body type (e.g. "flesh", "armor", "wood")
 *
 * Original: dispatched from combat resolution code */
void GameSound_WeaponHit(const char *hitclass, const char *material,
                          int volume, int world_x, int world_y,
                          int cam_x, int cam_y,
                          int viewport_w, int viewport_h);

/* Play a UI sound (non-positional, center-panned).
 * wav_name: VFS path relative to Sounds/ (e.g. "MENUBUTTON" — .wav
 *           extension is added automatically if missing) */
void GameSound_PlayUI(const char *wav_name);

/* Play a named WAV positionally in the world (COB play-sound host,
 * weapon fire/impact sounds — anything that arrives as a bare wav
 * name rather than a soundclass action). */
void GameSound_PlayWorldWav(const char *wav_name, int volume,
                            int world_x, int world_y,
                            int cam_x, int cam_y,
                            int viewport_w, int viewport_h);

#endif /* TAK_GAME_SOUND_H */
