/*
 * hpi_cat -- print a file out of the install's archives.
 *
 *   hpi_cat <path in the archives> [more paths]
 *   hpi_cat --list <pattern>
 *
 * The archives are the ones under TAK_GAME_DIR, with the loose tree
 * under TAK_DATA_DIR over them, the way the game mounts them.
 */

#include "tak_hpi.h"
#include "tak_memory.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hpi_cat <path>... | --list <pattern>\n");
        return 2;
    }
#ifdef _WIN32
    /* A file is bytes, not lines. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "hpi_cat: cannot mount %s\n", TAK_GAME_DIR);
        return 1;
    }
    int rc = 0;
    if (strcmp(argv[1], "--list") == 0 && argc > 2) {
        char **paths = NULL;
        int n = 0;
        if (VFS_ListFiles(argv[2], &paths, &n) == 0) {
            for (int i = 0; i < n; i++) { printf("%s\n", paths[i]); tak_free(paths[i]); }
            tak_free(paths);
        }
    } else {
        for (int i = 1; i < argc; i++) {
            void *data = NULL;
            uint32_t size = 0;
            if (VFS_ReadFile(argv[i], &data, &size) != 0 || !data) {
                fprintf(stderr, "hpi_cat: no %s\n", argv[i]);
                rc = 1;
                continue;
            }
            fwrite(data, 1, size, stdout);
            VFS_FreeBuffer(data);
        }
    }
    VFS_Shutdown();
    return rc;
}
