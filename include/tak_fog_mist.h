#ifndef TAK_FOG_MIST_H
#define TAK_FOG_MIST_H

#include <stdint.h>

struct GameWorld;
struct TAK_Platform;

/* The hardware mode's fog (D-053) in rgb with `side`'s set (a sidedata name,
 * NULL for the plain one). clock_ms comes from the view so captures repeat. */
void FogMist_Render(const struct GameWorld *world, struct TAK_Platform *plat,
                    uint32_t clock_ms, const char *side, const uint8_t rgb[3]);

/* Free the texture while its renderer is still up. */
void FogMist_Shutdown(struct TAK_Platform *plat);

#endif /* TAK_FOG_MIST_H */
