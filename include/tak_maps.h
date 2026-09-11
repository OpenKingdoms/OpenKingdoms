#ifndef TAK_MAPS_H
#define TAK_MAPS_H

#include <stddef.h>

/* Where a map came from. A map pack (Maps/<name>.kmp) keeps its files
 * under kmap/ and wins over a map of the same name in the maps folder,
 * which is the order the original looks in (legacy:168765). */
typedef enum {
    TAK_MAP_SOURCE_PACK = 0,   /* Maps/<name>.kmp */
    TAK_MAP_SOURCE_MAPS = 1    /* the maps folder, in an archive or loose */
} TAK_MapSource;

typedef struct TAK_MapEntry {
    char key[96];   /* file stem, the name the loader takes */
    int  source;    /* TAK_MapSource */
} TAK_MapEntry;

/* Every map a skirmish can choose: the maps folder and the map packs,
 * never the missions folder. The original builds its list from
 * Maps\*.ota and Maps\*.kmp alone (legacy:167671), so which maps are
 * offered follows from where they live, not from anything in the .ota.
 *
 * Fills a heap array the caller releases with TAK_Maps_Free. Entries are
 * unique by name and sorted by it. Returns 0 on success. */
int  TAK_Maps_Scan(TAK_MapEntry **out_entries, int *out_count);
void TAK_Maps_Free(TAK_MapEntry *entries);

/* VFS path of one of a map's files (ext is "ota", "tnt", "crt", "tdf").
 * Searches the map pack first, then the maps folder, then the missions
 * folder so campaign maps resolve through the same call. Returns 0 when
 * the file exists, -1 otherwise, and always leaves a usable path in out. */
int  TAK_Maps_FindFile(const char *key, const char *ext,
                       char *out, size_t out_cap);

#endif /* TAK_MAPS_H */
