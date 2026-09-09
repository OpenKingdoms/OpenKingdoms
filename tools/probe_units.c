/*
 * probe_units.c -- one-shot: list all *.fbi unit files in the VFS,
 * group by prefix, show a candidate "simple infantry" for kicking off
 * Phase C. Run once, discard.
 */

#include "tak_hpi.h"
#include "tak_memory.h"
#include <stdio.h>
#include <string.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

int main(void) {
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    char **paths = NULL;
    int n = 0;
    if (VFS_ListFiles("units/*.fbi", &paths, &n) != 0 || n == 0) {
        fprintf(stderr, "no unit fbis found\n");
        return 1;
    }
    printf("Total .fbi files: %d\n", n);
    for (int i = 0; i < n && i < 40; i++) {
        printf("  %s\n", paths[i]);
    }

    /* R1: distinguish canonical monarchs from alternates by dumping
     * the four candidates' UnitInfo + UnitName + Description fields. */
    {
        const char *fbis[] = {
            "units/araking.fbi",
            "units/tarnecro.fbi",
            "units/tarnecr2.fbi",
            "units/vermage.fbi",
            "units/zonhunt.fbi",
            "units/zonhurt.fbi",
        };
        for (size_t k = 0; k < sizeof(fbis)/sizeof(fbis[0]); k++) {
            void *b = NULL;
            uint32_t sz = 0;
            if (VFS_ReadFile(fbis[k], &b, &sz) != 0) {
                printf("\n=== %s: not found ===\n", fbis[k]);
                continue;
            }
            const char *src = (const char *)b;
            printf("\n=== %s ===\n", fbis[k]);
            /* Print the [UNITINFO] block only — first ~1000 bytes is enough. */
            uint32_t lim = sz < 1500 ? sz : 1500;
            fwrite(src, 1, lim, stdout);
            tak_free(b);
        }
    }

    /* Grep every FBI for `Category = ...MONARCH`. */
    for (int i = 0; i < n; i++) {
        void *b = NULL;
        uint32_t sz = 0;
        if (VFS_ReadFile(paths[i], &b, &sz) != 0) continue;
        const char *s = (const char *)b;
        /* Case-insensitive search for "monarch" as part of category. */
        for (uint32_t j = 0; j + 8 <= sz; j++) {
            if ((s[j]=='M'||s[j]=='m') && (s[j+1]=='O'||s[j+1]=='o') &&
                (s[j+2]=='N'||s[j+2]=='n') && (s[j+3]=='A'||s[j+3]=='a') &&
                (s[j+4]=='R'||s[j+4]=='r') && (s[j+5]=='C'||s[j+5]=='c') &&
                (s[j+6]=='H'||s[j+6]=='h')) {
                /* context: ~40 chars around the match */
                int start = (int)j - 20; if (start < 0) start = 0;
                int end = (int)j + 30; if ((uint32_t)end > sz) end = (int)sz;
                printf("%-32s @%u: '", paths[i], j);
                for (int k = start; k < end; k++) {
                    char c = s[k];
                    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
                    putchar(c);
                }
                printf("'\n");
                break;
            }
        }
        tak_free(b);
    }

    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
