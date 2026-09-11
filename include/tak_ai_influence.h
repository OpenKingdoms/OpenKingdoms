#ifndef TAK_AI_INFLUENCE_H
#define TAK_AI_INFLUENCE_H

#include <stdint.h>

typedef struct GameWorld GameWorld;
typedef struct UnitDef UnitDef;

/* Coarse per-player influence maps: one cell per 256 px, integers
 * only, rebuilt by the AI tick from the unit list. Each AI slot gets
 * its own set, gated by what that player can see. Allies count with
 * the owner, enemies against it. */
#define AI_INF_CELL_SHIFT 8
#define AI_INF_CELL_PX    (1 << AI_INF_CELL_SHIFT)
#define AI_INF_MAX_W      64
#define AI_INF_MAX_H      64

typedef enum {
    AI_INF_PRESENCE = 0,    /* own and allied combat value (legacy:19803) */
    AI_INF_THREAT,          /* seen enemy combat value */
    AI_INF_OWN_VALUE,       /* own and allied assets: monarch, structures, builders */
    AI_INF_ENEMY_VALUE,     /* seen enemy assets */
    AI_INF_WEALTH,          /* sacred sites and seen lodestones */
    AI_INF_LAYER_COUNT
} AiInfluenceLayer;

void    AI_Influence_Reset(void);
void    AI_Influence_Refresh(const GameWorld *world);
void    AI_Influence_Size(int *out_w, int *out_h);
/* 0 when no map has been built yet. Coordinates clamp to the grid. */
int     AI_Influence_CellOf(int32_t world_x, int32_t world_y,
                            int *out_cx, int *out_cy);
int32_t AI_Influence_Cell(int player_id, AiInfluenceLayer layer,
                          int cx, int cy);
int32_t AI_Influence_At(int player_id, AiInfluenceLayer layer,
                        int32_t world_x, int32_t world_y);
/* Where the enemy is weak and valuable: seen enemy assets less seen
 * enemy combat value. Negative where the enemy is strong. */
int32_t AI_Influence_Weakness(int player_id, int32_t world_x, int32_t world_y);
/* Where our own presence is thin and threatened: threat over presence
 * plus what stands there, 0 where nothing of ours is threatened. */
int32_t AI_Influence_Exposure(int player_id, int32_t world_x, int32_t world_y);

/* Per-type combat value, the original's summed threat unit
 * (legacy:19803). Asset value is ours: build cost per 50 mana, a
 * monarch counting extra, fighters counting as presence instead. */
int32_t AI_UnitCombatValue(const UnitDef *def);
int32_t AI_UnitAssetValue(const UnitDef *def, int monarch_expendable);

#endif /* TAK_AI_INFLUENCE_H */
