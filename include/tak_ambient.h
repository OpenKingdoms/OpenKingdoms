#ifndef TAK_AMBIENT_H
#define TAK_AMBIENT_H

/* Ambient feature sounds: map features with a SoundClass (the noise
 * emitters authored on maps) play their class on a randomised timer
 * while they sit in the viewport and in sight (legacy:128619-128712). */

struct GameWorld;

/* Forget every timer. Call when a world starts and when it ends. */
void Ambient_Reset(void);

/* Advance the timers one simulation tick and play what is due. */
void Ambient_Tick(const struct GameWorld *world);

#endif /* TAK_AMBIENT_H */
