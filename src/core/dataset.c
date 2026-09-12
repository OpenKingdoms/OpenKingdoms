/*
 * dataset.c -- Which game the mounted files make up.
 *
 * The original counts the expansion as present when both of its
 * campaign files open through the archive-aware file layer, whichever
 * executable is running (legacy:241744-241758). A patched base install
 * already carries the Creon .gui files from V3Rocket.hpi, so file
 * contents, not archive names, decide it.
 */

#include "tak_dataset.h"
#include "tak_hpi.h"

static int ds_pretend_no_expansion = 0;

void TAK_DataSet_SetPretendNoExpansion(int on) {
    ds_pretend_no_expansion = on ? 1 : 0;
}

int TAK_DataSet_HasIronPlague(void) {
    if (ds_pretend_no_expansion) return 0;   /* legacy:241742 */
    if (!VFS_IsInitialized()) return 0;
    return VFS_FileExists("camps/the iron plague.tdf") == 0 &&
           VFS_FileExists("camps/ipalt.tdf") == 0;
}
