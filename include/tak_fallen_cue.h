#ifndef TAK_FALLEN_CUE_H
#define TAK_FALLEN_CUE_H

#include <stddef.h>
#include <stdint.h>

/* The fallen player cue as a WAV image (D-043). Returns the bytes
 * written, or 0 when `cap` is short of TAK_FallenCue_WavBytes(). */
size_t TAK_FallenCue_Wav(uint8_t *out, size_t cap);
size_t TAK_FallenCue_WavBytes(void);

#endif
