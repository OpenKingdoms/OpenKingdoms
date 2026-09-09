/*
 * probe_hud_gaf.c -- list all entries in an in-game HUD GAF file with
 * dimensions so we can see what each one is. The previous probe_cob
 * tool covers .cob files; this is for HUD art GAFs in data/anims/.
 */

#include "tak_hpi.h"
#include "tak_gaf.h"
#include "tak_palette.h"
#include "tak_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

const char *Palette_LookupForGAF(const char *gaf_path);

/* Minimal hand-rolled BMP writer — easier than wiring stb_image_write
 * into the probe build. 24-bit BGR, no compression, top-down rows
 * via negative biHeight so loaders show the image right-side-up. */
static int dump_bmp(const char *path, const uint32_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row_pad = (4 - (w * 3) % 4) % 4;
    uint32_t image_size = (w * 3 + row_pad) * h;
    uint32_t file_size = 14 + 40 + image_size;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=file_size & 0xFF; hdr[3]=(file_size>>8)&0xFF;
    hdr[4]=(file_size>>16)&0xFF; hdr[5]=(file_size>>24)&0xFF;
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=w & 0xFF; hdr[19]=(w>>8)&0xFF;
    hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
    int neg_h = -h;
    hdr[22]=neg_h & 0xFF; hdr[23]=(neg_h>>8)&0xFF;
    hdr[24]=(neg_h>>16)&0xFF; hdr[25]=(neg_h>>24)&0xFF;
    hdr[26]=1;
    hdr[28]=24;
    fwrite(hdr, 1, 54, f);
    uint8_t pad[3] = {0};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = rgba[y*w + x];
            uint8_t r = p & 0xFF;
            uint8_t g = (p>>8) & 0xFF;
            uint8_t b = (p>>16) & 0xFF;
            uint8_t bgr[3] = { b, g, r };
            fwrite(bgr, 1, 3, f);
        }
        if (row_pad) fwrite(pad, 1, row_pad, f);
    }
    fclose(f);
    return 0;
}

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif
#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

int main(int argc, char **argv) {
    tak_mem_init();
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        fprintf(stderr, "VFS_Init failed\n");
        return 1;
    }

    const char *path = (argc > 1) ? argv[1] : "data/anims/araingame.gaf";
    int dump_entry = (argc > 2) ? atoi(argv[2]) : -1;
    int dump_frame = (argc > 3) ? atoi(argv[3]) : 0;

    GAFFile *gaf = NULL;
    if (GAF_Open(&gaf, path) != 0 || !gaf) {
        fprintf(stderr, "Failed to open %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    printf("entries: %u\n", gaf->num_entries);
    for (uint32_t i = 0; i < gaf->num_entries; i++) {
        uint32_t entry_off = *(const uint32_t *)(gaf->data + 12 + i * 4);
        if (entry_off + 40 > gaf->data_size) continue;
        const char *name = (const char *)(gaf->data + entry_off + 8);
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(gaf, entry_off, 0, &fh) != 0 || !fh) {
            printf("  [%2u] '%s' (frame info missing)\n", i, name);
            continue;
        }
        printf("  [%2u] '%-24s'  %dx%d  off=(%d,%d)  comp=%u  trans=0x%02x\n",
               i, name, fh->width, fh->height,
               fh->offset_x, fh->offset_y,
               fh->compressed, fh->transparency_index);
    }

    /* Optional: dump a specific entry to BMP for visual inspection. */
    if (dump_entry >= 0 && (uint32_t)dump_entry < gaf->num_entries) {
        /* Try sibling .pcx first; then guipal fallback. */
        char sibling[256];
        size_t L = strlen(path);
        if (L > 4 && L < sizeof(sibling)) {
            memcpy(sibling, path, L);
            sibling[L] = '\0';
            sibling[L-3]='p'; sibling[L-2]='c'; sibling[L-1]='x';
        } else sibling[0]='\0';

        Palette pal;
        int pal_ok = (sibling[0] && Palette_LoadPCX(&pal, sibling) == 0);
        if (!pal_ok) {
            const char *p = Palette_LookupForGAF(path);
            if (!p) p = "guipal.pcx";
            char pp[256];
            snprintf(pp, sizeof(pp), "data/palettes/%s", p);
            pal_ok = (Palette_LoadPCX(&pal, pp) == 0);
        }
        if (!pal_ok) {
            fprintf(stderr, "could not load palette for %s\n", path);
        } else {
            uint32_t rgba_table[256];
            for (int i = 0; i < 256; i++) {
                rgba_table[i] = (uint32_t)pal.entries[i].r
                              | ((uint32_t)pal.entries[i].g << 8)
                              | ((uint32_t)pal.entries[i].b << 16)
                              | (0xFFu << 24);
            }
            /* Trans index: most TAK GAFs use 9. */
            rgba_table[9] = 0;
            uint32_t entry_off = *(const uint32_t *)(gaf->data + 12 + dump_entry * 4);
            FrameHeader *fh = NULL;
            if (GAF_GetFrameInfo(gaf, entry_off, dump_frame, &fh) == 0 && fh) {
                uint32_t *pix = GAF_DecodeFrameRGBA(gaf, fh, rgba_table);
                if (pix) {
                    char outname[64];
                    snprintf(outname, sizeof(outname), "entry_%d.bmp", dump_entry);
                    dump_bmp(outname, pix, fh->width, fh->height);
                    /* ASCII heatmap so we can SEE the pixels in stdout. */
                    printf("\n");
                    for (int y = 0; y < fh->height; y++) {
                        printf("  ");
                        for (int x = 0; x < fh->width; x++) {
                            uint32_t p = pix[y*fh->width + x];
                            uint32_t a = (p >> 24) & 0xFF;
                            uint32_t r = p & 0xFF;
                            uint32_t g = (p >> 8) & 0xFF;
                            uint32_t b = (p >> 16) & 0xFF;
                            uint32_t lum = (r+g+b)/3;
                            if (a == 0) putchar('.');
                            else if (lum < 60)  putchar('#');
                            else if (lum < 130) putchar('+');
                            else if (lum < 200) putchar('o');
                            else                putchar(' ');
                        }
                        putchar('\n');
                    }
                    int nontrans = 0;
                    for (int i = 0; i < fh->width * fh->height; i++) {
                        if ((pix[i] >> 24) & 0xFF) nontrans++;
                    }
                    printf("dumped entry %d (%dx%d, %d non-transparent px) -> %s\n",
                           dump_entry, fh->width, fh->height, nontrans, outname);
                    free(pix);
                }
            }
        }
    }

    GAF_Close(gaf);
    VFS_Shutdown();
    tak_mem_shutdown();
    return 0;
}
