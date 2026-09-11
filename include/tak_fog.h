#ifndef TAK_FOG_H
#define TAK_FOG_H

#include "tak_types.h"

struct GameWorld;
struct TAK_Platform;

#define TAK_FOG_UNEXPLORED 0
#define TAK_FOG_EXPLORED   1
#define TAK_FOG_VISIBLE    2

int  Fog_Init(struct GameWorld *world);
void Fog_Free(struct GameWorld *world);
void Fog_Update(struct GameWorld *world, int player_id);
/* True when a unit owned by owner reveals ground for viewer. */
int  Fog_SharesSight(const struct GameWorld *world, int viewer, int owner);
int  Fog_StateAtForPlayer(const struct GameWorld *world, int player_id,
                          int32_t world_x, int32_t world_y);
int  Fog_IsVisibleForPlayer(const struct GameWorld *world, int player_id,
                            int32_t world_x, int32_t world_y);
int  Fog_StateAt(const struct GameWorld *world, int32_t world_x, int32_t world_y);
int  Fog_IsVisible(const struct GameWorld *world, int32_t world_x, int32_t world_y);
void Fog_RenderOverlay(const struct GameWorld *world, struct TAK_Platform *plat);

#endif
