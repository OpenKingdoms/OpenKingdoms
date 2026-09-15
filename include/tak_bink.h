#ifndef TAK_BINK_H
#define TAK_BINK_H

#include <stdint.h>
#include <SDL.h>

/*
 * Bink video player using FFmpeg's libavcodec Bink decoder.
 *
 * Used for the main menu character animations. Each character has
 * 4 .bik video clips (numbered 4-7) in Movies/Gui/:
 *   machine4.bik - machine7.bik  (skirmish mage)
 *   GIRL4.BIK    - GIRL7.BIK     (story girl)
 *   KNIGHT4.BIK  - KNIGHT7.BIK   (multiplayer knight)
 *   SNORT4.BIK   - SNORT7.BIK    (credits creature)
 */

typedef struct BinkPlayer BinkPlayer;

/* Open a .bik file. Returns NULL on failure. */
BinkPlayer *BinkPlayer_Open(const char *path);

/* Open a clip by its path under the resolved game directory, whatever
 * case the install spells the file name in. NULL when it is not there
 * or this build has no decoder. */
BinkPlayer *BinkPlayer_OpenClip(const char *rel_path);

/* Is there a clip at that path this build could play? 0 without a
 * decoder, so a reel that is optional is not even requested. */
int BinkPlayer_ClipExists(const char *rel_path);

/* Close and free a player. */
void BinkPlayer_Close(BinkPlayer *bp);

/* Advance to the next frame. Call once per game frame (or at the video's
 * native frame rate). Returns 1 if a new frame was decoded, 0 if the
 * video has ended. */
int BinkPlayer_NextFrame(BinkPlayer *bp);

/* Get the current decoded frame as RGBA pixels.
 * The returned pointer is owned by the player (do NOT free it).
 * Returns NULL if no frame has been decoded yet. */
const uint32_t *BinkPlayer_GetPixels(BinkPlayer *bp);

/* Get video dimensions. */
int BinkPlayer_GetWidth(BinkPlayer *bp);
int BinkPlayer_GetHeight(BinkPlayer *bp);

/* Get the video's native frame duration in seconds. */
double BinkPlayer_GetFrameDuration(BinkPlayer *bp);

/* Reset playback to the beginning. */
void BinkPlayer_Rewind(BinkPlayer *bp);

/* Check if playback has finished. */
int BinkPlayer_IsFinished(BinkPlayer *bp);

/* Total number of decoded frames in the clip. */
int BinkPlayer_GetFrameCount(BinkPlayer *bp);

/* Seek to a specific frame index (clamped to [0, count-1]). Clears the
 * finished flag so NextFrame advances normally again. */
void BinkPlayer_SeekTo(BinkPlayer *bp, int frame);

/* Fold dt seconds in and step to the next frame once it is due. One
 * step at most per call, and at most one frame of debt is carried, so
 * a stall repays one frame on the next call and the clip then runs at
 * its rate. Returns 1 when the frame changed. */
int BinkPlayer_Advance(BinkPlayer *bp, double dt);

/* The frame on show, -1 without a player. */
int BinkPlayer_CurrentFrame(BinkPlayer *bp);

/* Successful opens since the process started. */
int BinkPlayer_OpenCount(void);

#endif /* TAK_BINK_H */
