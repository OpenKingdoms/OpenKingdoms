#ifndef TAK_HUD_INFO_H
#define TAK_HUD_INFO_H

/* What the selected-unit panel of the bottom strip describes. The owner
 * saw the original pick, in this order, the unit under the pointer, the
 * feature under it, the selected unit when exactly one is selected, and
 * otherwise nothing. Presentation only, no world read. */
typedef enum HUDInfoKind {
    HUD_INFO_NONE = 0,
    HUD_INFO_UNIT,
    HUD_INFO_FEATURE,
} HUDInfoKind;

typedef struct HUDInfoPick {
    HUDInfoKind kind;
    int         index;   /* unit handle or world->features index, -1 for none */
} HUDInfoPick;

/* hover_unit and hover_feature are -1 when the pointer is over neither. */
HUDInfoPick HUD_PickInfo(int hover_unit, int hover_feature,
                         const int *sel, int sel_count);

#endif /* TAK_HUD_INFO_H */
