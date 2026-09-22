#ifndef TAK_CLIP_AUDIO_H
#define TAK_CLIP_AUDIO_H

#include <stdint.h>

/* ── A clip's soundtrack ──────────────────────────────────────────
 *
 * The clip player decodes a reel's audio as it goes and hands the
 * samples here. They wait in a ring the mixer drains at the clip's
 * own rate, so the reel's picture and its sound stay together and the
 * player never touches the audio device. A ring that runs dry plays
 * silence rather than ending, since the next frame's samples are on
 * their way. */

typedef struct ClipAudio ClipAudio;

/* Open a track of so many channels at so many samples a second. NULL
 * when the sound system is off or not up, in which case the reel
 * plays without sound and nothing else changes. */
ClipAudio *ClipAudio_Open(int channels, int sample_rate);
void       ClipAudio_Close(ClipAudio *ca);

/* Queue interleaved 16 bit samples. Frames that do not fit are
 * dropped, and the count kept is returned. */
int  ClipAudio_Push(ClipAudio *ca, const int16_t *pcm, int frames);
/* Frames waiting to be played. */
int  ClipAudio_Queued(const ClipAudio *ca);
/* Frames the ring can hold at most. */
int  ClipAudio_Capacity(const ClipAudio *ca);
/* Start the mixer on the ring. Called once the first samples are in,
 * so the sound does not begin with a stretch of nothing. */
int  ClipAudio_Start(ClipAudio *ca);
/* Throw away what is queued, for a rewind. */
void ClipAudio_Flush(ClipAudio *ca);
/* Frames the mixer has taken so far, for tests and the drift check. */
int64_t ClipAudio_Played(const ClipAudio *ca);

#endif /* TAK_CLIP_AUDIO_H */
