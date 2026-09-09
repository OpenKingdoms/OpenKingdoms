#ifndef TAK_CAMERA_H
#define TAK_CAMERA_H

/* Camera input/scroll configuration. Owned by the in-game screen as a
 * module-global static so a future options UI can tweak it without
 * threading a pointer through every caller. Defaults are set once at
 * program startup via Camera_ResetDefaults() — do that before the
 * first InGame_Tick. */
typedef struct CameraConfig {
    /* Keyboard and mouse-edge scroll speed in map pixels per second
     * at the default zoom. Both inputs share this value and add to
     * each other when combined (holding A at the left edge = 2x).   */
    float scroll_px_per_sec;

    /* Multiplier applied while Shift is held. Typical RTS convention
     * is 2×–4×; 3× gives a quick overview scroll on big maps.       */
    float boost_multiplier;

    /* Distance (in window pixels, not canvas) within which the mouse
     * triggers edge scroll. 0 disables edge scroll entirely.        */
    int   edge_scroll_margin_px;
} CameraConfig;

/* Initialise the module-global config with sensible defaults. Safe
 * to call repeatedly — last call wins. */
void                Camera_ResetDefaults(void);

/* Read-only access to the current config. Never returns NULL. */
const CameraConfig *Camera_GetConfig(void);

/* Update the config from an options UI, config file, or CLI flag.
 * Pass a fully-populated struct; the module copies it by value. */
void                Camera_SetConfig(const CameraConfig *cfg);

#endif /* TAK_CAMERA_H */
