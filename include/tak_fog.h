#ifndef TAK_FOG_H
#define TAK_FOG_H

#include "tak_types.h"

struct GameWorld;
struct TAK_Platform;

#define TAK_FOG_UNEXPLORED 0
#define TAK_FOG_EXPLORED   1
#define TAK_FOG_VISIBLE    2

/* Legacy fog cells are 32 px, half map-cell resolution and the same
 * grid as the 32 px graphic tiles (legacy:130169). The stamp and the
 * reveal anchor both measure in these cells. */
#define TAK_FOG_CELL_PX 32

int  Fog_Init(struct GameWorld *world);
void Fog_Free(struct GameWorld *world);
void Fog_Update(struct GameWorld *world, int player_id);
/* The seat the screen shows the fog for, which Fog_StateAt,
 * Fog_IsVisible and the overlay read. Presentation only. */
void Fog_SetViewer(int player_id);
int  Fog_Viewer(void);
/* True when a unit owned by owner reveals ground for viewer. */
int  Fog_SharesSight(const struct GameWorld *world, int viewer, int owner);
int  Fog_StateAtForPlayer(const struct GameWorld *world, int player_id,
                          int32_t world_x, int32_t world_y);
int  Fog_IsVisibleForPlayer(const struct GameWorld *world, int player_id,
                            int32_t world_x, int32_t world_y);
int  Fog_StateAt(const struct GameWorld *world, int32_t world_x, int32_t world_y);
int  Fog_IsVisible(const struct GameWorld *world, int32_t world_x, int32_t world_y);
/* The local player's draw test: current sight with Line of Sight on,
 * explored ground with it off. Presentation only. */
int  Fog_ShowsAt(const struct GameWorld *world, int32_t world_x, int32_t world_y);
void Fog_RenderOverlay(const struct GameWorld *world, struct TAK_Platform *plat);

#endif
