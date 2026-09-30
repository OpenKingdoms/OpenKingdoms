#ifndef TAK_SAVELIST_H
#define TAK_SAVELIST_H

#include <stddef.h>
#include <stdint.h>

/* What the save and load dialogs put in their list box.
 *
 * The original enumerates a directory and shows the file names with the
 * extension stripped, in enumeration order, with no date column and no
 * sort key (legacy:159073-159085). It reads the archive's Summary entry
 * for the right hand panel, so selecting a row costs one short read
 * rather than a whole battle (legacy:159131-159160). This does the
 * same over the .oksave container.
 *
 * A file that will not open is still listed. The player has to be able
 * to see it and delete it, and the reason it refuses is the one thing
 * they can act on, so it is carried on the row and shown when the row
 * is selected rather than thrown away. */

#define TAK_SAVE_SLUG_MAX   64
#define TAK_SAVE_PATH_MAX 1200

typedef struct TAK_SaveEntry {
    char     slug[TAK_SAVE_SLUG_MAX];   /* file name without the extension */
    char     path[TAK_SAVE_PATH_MAX];   /* what to hand Save_Read          */
    char     map[96];                   /* the map it was played on        */
    char     side[32];                  /* the local player's kingdom      */
    char     game_time[16];             /* hh:mm:ss                        */
    uint64_t saved_at_utc;
    /* 0 when the save refuses to open. `refusal` says why, in the
     * container's own words, which name the map or the definition that
     * moved. */
    int      readable;
    char     refusal[256];
    /* A line more for the dialog's help strip. Replays name their
     * players here. */
    char     detail[160];
    /* The file's size, where the scan's reader filled it in. */
    uint32_t bytes;
} TAK_SaveEntry;

/* Every .oksave in the saved game directory. Returns the count and
 * hands back a heap array through `out`. A directory that holds none,
 * and one that cannot be opened at all, both come back as 0 with
 * `*out` NULL: an empty list is what the dialog shows either way.
 * Returns -1 only when `out` is NULL. */
int  SaveList_Scan(TAK_SaveEntry **out);
/* The same over any extension in the saved game directory, with `fill`
 * reading each row's panel from its file, and at most `max` rows. */
int  SaveList_ScanExt(const char *ext, void (*fill)(TAK_SaveEntry *e),
                      int max, TAK_SaveEntry **out);
void SaveList_Free(TAK_SaveEntry *list);

/* A tick count as the dialog shows it, hh:mm:ss (legacy:159150). */
void SaveList_FormatTime(uint32_t ticks, uint32_t hz, char *out, size_t cap);

/* Whether a typed game name can become a file name. The original
 * refuses an empty name and an invalid one with two different messages
 * (legacy:159247-159292), so the caller needs to tell them apart. */
typedef enum {
    TAK_SAVENAME_OK = 0,
    TAK_SAVENAME_EMPTY,
    TAK_SAVENAME_INVALID
} TAK_SaveNameVerdict;

TAK_SaveNameVerdict SaveList_CheckName(const char *name);

#endif /* TAK_SAVELIST_H */
