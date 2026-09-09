#ifndef TAK_SOUNDCLASS_H
#define TAK_SOUNDCLASS_H

/* ══════════════════════════════════════════════════════════════════════
 *  Sound Class System — data-driven audio mapping
 *
 *  Maps unit actions and weapon hits to WAV filenames using TDF config
 *  files. Supports weighted random selection for audio variety.
 *
 *  Two subsystems:
 *
 *  1. UNIT SOUND CLASSES (185 TDFs in gamedata/soundclasses/)
 *     Each unit type has a soundclass (e.g. "ARAKNIGH"). Each class
 *     maps actions ("select", "move", "attack") to weighted sound pools.
 *     Example: [select] { TONEARA = 3.0; ARAKNIGHSEL3 = 1.0; }
 *     → 75% chance TONEARA, 25% chance ARAKNIGHSEL3
 *
 *  2. HIT SOUND CLASSES (soundclasses.tdf)
 *     Weapon types (sword, arrow, cannon...) × material types (flesh,
 *     armor, wood, scale, stone) → arrays of impact sound variants.
 *     Example: [sword][flesh] { sound0=SWRDFL01.wav; ... }
 *
 *  The original engine loads all of these at startup from:
 *    gamedata/soundclasses/ (all .tdf files)
 *  via a directory listing (the legacy reference line 221378).
 * ══════════════════════════════════════════════════════════════════════ */

/* Initialize: load all soundclass TDFs from the VFS. Call once at
 * startup after VFS_Init. Returns 0 on success, -1 on failure. */
int  SoundClass_LoadAll(void);

/* Free all loaded sound class data. */
void SoundClass_FreeAll(void);

/* Look up a unit sound class by name (case-insensitive).
 * Returns an opaque class ID (>= 0) or -1 if not found. */
int  SoundClass_Find(const char *class_name);

/* Given a class ID and action name (e.g. "select", "move", "attack"),
 * perform weighted random selection and return a sound name.
 * Returns NULL if the class has no sounds for that action.
 * The returned string points into internal storage — do not free. */
const char *SoundClass_SelectSound(int class_id, const char *action);

/* Look up a weapon hit sound for a given weapon type and material.
 * weapon_type: "sword", "arrow", "cannon", "fist", "hammer", etc.
 * material: "flesh", "armor", "wood", "scale", "stone", or "default"
 * Returns a randomly selected WAV filename, or NULL. */
const char *SoundClass_SelectHitSound(const char *weapon_type,
                                       const char *material);

/* Query how many unit sound classes are loaded. */
int SoundClass_GetCount(void);

#endif /* TAK_SOUNDCLASS_H */
