/*
 * hpi_cat -- print a file out of the install's archives.
 *
 *   hpi_cat <path in the archives> [more paths]
 *   hpi_cat --list <pattern>
 *   hpi_cat --only <archive.hpi> ...      one archive and no loose tree
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

static char g_only[128];

static int only_this(const char *file_name) {
    size_t n = strlen(g_only);
    size_t m = strlen(file_name);
    if (m < n) return 0;
    for (size_t i = 0; i < n; i++) {
        char a = g_only[i], b = file_name[m - n + i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hpi_cat <path>... | --list <pattern>\n");
        return 2;
    }
#ifdef _WIN32
    /* A file is bytes, not lines. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char *loose = TAK_DATA_DIR;
    if (argc > 3 && strcmp(argv[1], "--only") == 0) {
        snprintf(g_only, sizeof(g_only), "%s", argv[2]);
        VFS_SetMountFilter(only_this);
        loose = NULL;
        argv += 2;
        argc -= 2;
    }
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, loose) != 0) {
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
