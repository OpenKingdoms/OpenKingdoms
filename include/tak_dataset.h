#ifndef TAK_DATASET_H
#define TAK_DATASET_H

/* Which game the mounted files make up. The expansion counts as present
 * only when camps/the iron plague.tdf and camps/ipalt.tdf both open, and
 * the -pretendnoexpansion switch turns it off (legacy:241742-241758).
 * Both files ship only in IPData.hpi, so a base game install answers 0.
 * Every Creon rule hangs off this one answer, the save gate and the
 * multiplayer handshake included. */
int  TAK_DataSet_HasIronPlague(void);

/* The -pretendnoexpansion switch (legacy:251988-251990). */
void TAK_DataSet_SetPretendNoExpansion(int on);

#endif /* TAK_DATASET_H */
