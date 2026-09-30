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
/* The whole map with no fog, for a watcher. Fog_Viewer is 0 meanwhile,
 * a viewer with no layer, which every query reads as visible. */
void Fog_SetSeeAll(int on);
int  Fog_SeesAll(void);
int  Fog_Viewer(void);
/* Start every explored map over from the Mapping option and stamp each
 * seat's sight again, as the original does when a console command
 * turns Line of Sight or Mapping. */
void Fog_Refresh(struct GameWorld *world);
/* True when a unit owned by owner reveals ground for viewer. */
int  Fog_SharesSight(const struct GameWorld *world, int viewer, int owner);
int  Fog_StateAtForPlayer(const struct GameWorld *world, int player_id,
                          int32_t world_x, int32_t world_y);
int  Fog_IsVisibleForPlayer(const struct GameWorld *world, int player_id,
                            int32_t world_x, int32_t world_y);
int  Fog_StateAt(const struct GameWorld *world, int32_t world_x, int32_t world_y);
int  Fog_IsVisible(const struct GameWorld *world, int32_t world_x, int32_t world_y);
/* What a seat sees at a point, the original's one visibility test:
 * current sight with Line of Sight on, explored ground with it off.
 * Its screen reads it in both modes, its units with Line of Sight on. */
int  Fog_SeatSeesAt(const struct GameWorld *world, int player_id,
                    int32_t world_x, int32_t world_y);
/* Fog_SeatSeesAt for the seat the screen shows. */
int  Fog_ShowsAt(const struct GameWorld *world, int32_t world_x, int32_t world_y);
void Fog_RenderOverlay(const struct GameWorld *world, struct TAK_Platform *plat);
/* The black the classic overlay lays on the fog cell holding a world
 * point, for the local viewer: 0 clear, 0x78 dimmed, 0xFF black. */
uint8_t Fog_OverlayAlphaAt(const struct GameWorld *world, int32_t world_x, int32_t world_y);
/* Draws the overlay has taken since the process began, for a test. */
uint32_t Fog_DebugOverlayDraws(void);

#endif
