#ifndef TAK_VIEW_SHAKE_H
#define TAK_VIEW_SHAKE_H

#include <stdint.h>

/* The view shaken by a weapon that authors it (shakemagnitude and
 * shakeduration, read at legacy:250066). The original keeps a
 * magnitude, a length and what is left of it, and while any is left
 * moves the view each frame by a random step inside a box that shrinks
 * as the shake runs out (legacy:120568-120594). A second shake on top
 * of one that is running adds its magnitude and averages the two
 * lengths (legacy:120454-120470).
 *
 * It is what the player sees and nothing else: no simulation state, no
 * simulation random numbers, nothing in a save. */

/* magnitude in pixels, length in frames. Either at or below zero is no
 * shake. */
void ViewShake_Start(int magnitude, int frames);
void ViewShake_Reset(void);
int  ViewShake_Active(void);
/* This frame's offset, and one frame of the shake used up. 0,0 when
 * none is running. */
void ViewShake_Step(int32_t *out_dx, int32_t *out_dy);

#endif /* TAK_VIEW_SHAKE_H */
