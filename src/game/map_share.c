/*
 * map_share.c -- a room's map as one bundle, and the folder it is kept in.
 * A bundle is "OKMS" u8 version u8 count, then u8 part u32 size bytes per
 * part, compressed whole behind "OKMZ" and the size it inflates to.
 */

#include "tak_map_share.h"
#include "tak_net_protocol.h"
#include "tak_bytes.h"
#include "tak_hpi.h"
#include "tak_maps.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "miniz.h"

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum { PART_TNT = 0, PART_OTA, PART_CRT, PART_TDF, PART_TXT, PARTS };
static const char *const part_ext[PARTS] = { "tnt", "ota", "crt", "tdf", "txt" };

#define BUNDLE_VERSION 1u
#define BUNDLE_HEAD    6u    /* "OKMS", version, count */
#define PART_HEAD      5u    /* part, size */
#define WIRE_HEAD      8u    /* "OKMZ", inflated size */

static void parts_of(const TAK_MapShareFiles *f, const void *p[PARTS], size_t n[PARTS]) {
    p[PART_TNT] = f->map.tnt; n[PART_TNT] = f->map.tnt_size;
    p[PART_OTA] = f->map.ota; n[PART_OTA] = f->map.ota_size;
    p[PART_CRT] = f->map.crt; n[PART_CRT] = f->map.crt_size;
    p[PART_TDF] = f->map.tdf; n[PART_TDF] = f->map.tdf_size;
    p[PART_TXT] = f->txt;     n[PART_TXT] = f->txt_size;
    for (int k = 0; k < PARTS; k++) if (!p[k]) n[k] = 0;
}

/* ── The bundle ─────────────────────────────────────────────────────── */

int TAK_MapShare_Pack(const TAK_MapShareFiles *f, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    if (!f) return -1;
    const void *p[PARTS];
    size_t n[PARTS];
    parts_of(f, p, n);
    if (!p[PART_TNT] || !p[PART_OTA]) return -1;
    if (n[PART_OTA] > TAK_MAP_SHARE_TDF_MAX || n[PART_TDF] > TAK_MAP_SHARE_TDF_MAX) return -1;
    if (n[PART_TXT] > TAK_MAP_SHARE_TXT_MAX) { p[PART_TXT] = NULL; n[PART_TXT] = 0; }

    size_t raw = BUNDLE_HEAD;
    int count = 0;
    for (int k = 0; k < PARTS; k++) {
        if (!p[k]) continue;
        if (n[k] > TAK_MAP_SHARE_RAW_MAX) return -2;
        raw += PART_HEAD + n[k];
        count++;
    }
    if (raw > TAK_MAP_SHARE_RAW_MAX) return -2;

    uint8_t *bundle = (uint8_t *)tak_malloc(raw);
    if (!bundle) return -1;
    memcpy(bundle, "OKMS", 4);
    bundle[4] = (uint8_t)BUNDLE_VERSION;
    bundle[5] = (uint8_t)count;
    size_t at = BUNDLE_HEAD;
    for (int k = 0; k < PARTS; k++) {
        if (!p[k]) continue;
        bundle[at] = (uint8_t)k;
        tak_put_u32(bundle + at + 1, (uint32_t)n[k]);
        if (n[k]) memcpy(bundle + at + PART_HEAD, p[k], n[k]);
        at += PART_HEAD + n[k];
    }

    mz_ulong bound = mz_compressBound((mz_ulong)raw);
    uint8_t *wire = (uint8_t *)tak_malloc(WIRE_HEAD + (size_t)bound);
    if (!wire) { tak_free(bundle); return -1; }
    mz_ulong got = bound;
    int rc = mz_compress2(wire + WIRE_HEAD, &got, bundle, (mz_ulong)raw, MZ_DEFAULT_COMPRESSION);
    tak_free(bundle);
    if (rc != MZ_OK) { tak_free(wire); return -1; }
    if (WIRE_HEAD + (size_t)got > TAK_NET_MAP_MAX) { tak_free(wire); return -2; }
    memcpy(wire, "OKMZ", 4);
    tak_put_u32(wire + 4, (uint32_t)raw);
    *out = wire;
    *out_len = (uint32_t)(WIRE_HEAD + got);
    return 0;
}

void TAK_MapShare_FreeFiles(TAK_MapShareFiles *f) {
    if (!f) return;
    tak_free(f->owned);
    memset(f, 0, sizeof(*f));
}

int TAK_MapShare_Unpack(const uint8_t *wire, uint32_t len,
                        const uint8_t want[TAK_MAP_FINGERPRINT_BYTES],
                        TAK_MapShareFiles *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!wire || !want || len < WIRE_HEAD || len > TAK_NET_MAP_MAX ||
        memcmp(wire, "OKMZ", 4) != 0) return -1;
    uint32_t raw = tak_get_u32(wire + 4);
    if (raw < BUNDLE_HEAD || raw > TAK_MAP_SHARE_RAW_MAX) return -1;
    uint8_t *bundle = (uint8_t *)tak_malloc(raw);
    if (!bundle) return -1;
    mz_ulong got = raw;
    if (mz_uncompress(bundle, &got, wire + WIRE_HEAD, (mz_ulong)(len - WIRE_HEAD)) != MZ_OK ||
        got != raw) {
        tak_free(bundle);
        return -1;
    }

    const void *p[PARTS] = { NULL, NULL, NULL, NULL, NULL };
    size_t n[PARTS] = { 0, 0, 0, 0, 0 };
    int ok = memcmp(bundle, "OKMS", 4) == 0 && bundle[4] == BUNDLE_VERSION &&
             bundle[5] >= 2 && bundle[5] <= PARTS;
    size_t at = BUNDLE_HEAD;
    for (int i = 0; ok && i < bundle[5]; i++) {
        if (raw - at < PART_HEAD) { ok = 0; break; }
        uint8_t k = bundle[at];
        uint32_t size = tak_get_u32(bundle + at + 1);
        at += PART_HEAD;
        if (k >= PARTS || p[k] || size > raw - at ||
            (k == PART_TXT && size > TAK_MAP_SHARE_TXT_MAX) ||
            ((k == PART_OTA || k == PART_TDF) && size > TAK_MAP_SHARE_TDF_MAX)) { ok = 0; break; }
        p[k] = bundle + at;
        n[k] = size;
        at += size;
    }
    if (!ok || at != raw || !p[PART_TNT] || !p[PART_OTA]) {
        tak_free(bundle);
        return -1;
    }
    out->map.tnt = p[PART_TNT]; out->map.tnt_size = n[PART_TNT];
    out->map.ota = p[PART_OTA]; out->map.ota_size = n[PART_OTA];
    out->map.crt = p[PART_CRT]; out->map.crt_size = n[PART_CRT];
    out->map.tdf = p[PART_TDF]; out->map.tdf_size = n[PART_TDF];
    out->txt = p[PART_TXT];     out->txt_size = n[PART_TXT];
    out->owned = bundle;

    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    if (TAK_MapFingerprint_FromFiles(&out->map, fp) != 0 ||
        memcmp(fp, want, sizeof fp) != 0) {
        TAK_MapShare_FreeFiles(out);
        return -2;
    }
    return 0;
}

/* ── From the installed map ─────────────────────────────────────────── */

/* The game folder's own .hpi, and which release: an IP prefix is The
 * Iron Plague's, as the page reads it (web/shell.html). */
static int game_archive_share(const char *archive) {
    return tak_strnicmp(archive, "ip", 2) == 0 ? TAK_MAPSHARE_EXPANSION : TAK_MAPSHARE_GAME;
}

/* The installed map's parts as its fingerprint reads them, in buffers
 * the caller frees with VFS_FreeBuffer. 0 when they are the map want. */
static int read_installed(const char *map_key, const uint8_t want[TAK_MAP_FINGERPRINT_BYTES],
                          void *data[PARTS], uint32_t size[PARTS], TAK_MapShareFiles *f) {
    for (int k = 0; k < PARTS; k++) { data[k] = NULL; size[k] = 0; }
    for (int k = 0; k < PARTS; k++) {
        char path[512], archive[128];
        if (TAK_Maps_FindFile(map_key, part_ext[k], path, sizeof path) != 0) continue;
        /* The blurb of a map the game ships is not the host's to send. */
        if (k == PART_TXT && VFS_FileOrigin(path, archive, sizeof archive) == VFS_ORIGIN_GAME)
            continue;
        if (VFS_ReadFile(path, &data[k], &size[k]) != 0) { data[k] = NULL; size[k] = 0; }
    }
    memset(f, 0, sizeof *f);
    f->map.tnt = data[PART_TNT]; f->map.tnt_size = size[PART_TNT];
    f->map.ota = data[PART_OTA]; f->map.ota_size = size[PART_OTA];
    f->map.crt = data[PART_CRT]; f->map.crt_size = size[PART_CRT];
    f->map.tdf = data[PART_TDF]; f->map.tdf_size = size[PART_TDF];
    if (size[PART_TXT] <= TAK_MAP_SHARE_TXT_MAX) { f->txt = data[PART_TXT]; f->txt_size = size[PART_TXT]; }
    uint8_t fp[TAK_MAP_FINGERPRINT_BYTES];
    if (TAK_MapFingerprint_FromFiles(&f->map, fp) != 0) return -1;
    return memcmp(fp, want, sizeof fp) == 0 ? 0 : -3;
}

int TAK_MapShare_PackInstalled(const char *map_key,
                               const uint8_t want[TAK_MAP_FINGERPRINT_BYTES],
                               uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    if (!map_key || !want) return -1;
    void *data[PARTS];
    uint32_t size[PARTS];
    TAK_MapShareFiles f;
    int rc = read_installed(map_key, want, data, size, &f);
    if (rc == 0) rc = TAK_MapShare_Pack(&f, out, out_len);
    for (int k = 0; k < PARTS; k++) VFS_FreeBuffer(data[k]);
    return rc;
}

int TAK_MapShare_Status(const char *map_key, const uint8_t fp[TAK_MAP_FINGERPRINT_BYTES]) {
    if (!map_key || !fp) return TAK_MAPSHARE_UNREADABLE;
    int shipped = TAK_MapShare_Shipped(fp);
    if (shipped) return shipped;
    if (TAK_MapShare_ShippedName(map_key)) return TAK_MapShare_ShippedName(map_key);
    /* Out of the game's own archives is the game's, whatever its print. */
    for (int k = PART_TNT; k <= PART_TDF; k++) {
        char path[512], archive[128];
        if (TAK_Maps_FindFile(map_key, part_ext[k], path, sizeof path) != 0) continue;
        if (VFS_FileOrigin(path, archive, sizeof archive) == VFS_ORIGIN_GAME)
            return game_archive_share(archive);
    }
    if (!TAK_MapShare_NameOk(map_key)) return TAK_MAPSHARE_UNREADABLE;
    void *data[PARTS];
    uint32_t size[PARTS];
    TAK_MapShareFiles f;
    int rc = read_installed(map_key, fp, data, size, &f);
    uint64_t raw = 0;
    for (int k = 0; k < PARTS; k++) raw += size[k];
    int text_ok = size[PART_OTA] <= TAK_MAP_SHARE_TDF_MAX && size[PART_TDF] <= TAK_MAP_SHARE_TDF_MAX;
    for (int k = 0; k < PARTS; k++) VFS_FreeBuffer(data[k]);
    if (rc != 0 || !text_ok) return TAK_MAPSHARE_UNREADABLE;
    /* Compressed it may still be too big, which the first piece finds. */
    return raw > TAK_MAP_SHARE_RAW_MAX ? TAK_MAPSHARE_TOO_BIG : TAK_MAPSHARE_OFFERED;
}

/* ── Where a received map is kept ───────────────────────────────────── */

int TAK_MapShare_NameOk(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n == 0 || n >= TAK_NET_MAP_NAME_MAX) return 0;
    if (name[0] == ' ' || name[0] == '.' || name[n - 1] == ' ' || name[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 (c != 0 && strchr(" '-_.,()!&+#", c) != NULL);
        if (!ok) return 0;
    }
    /* The names Windows keeps for devices, with or without a suffix. */
    static const char *const devices[] = { "con", "prn", "aux", "nul", "com", "lpt" };
    for (size_t d = 0; d < sizeof devices / sizeof devices[0]; d++) {
        if (tak_strnicmp(name, devices[d], 3) != 0) continue;
        const char *rest = name + 3;
        if (d >= 4 && *rest >= '0' && *rest <= '9') rest++;
        if (*rest == '\0' || *rest == '.' || *rest == ' ') return 0;
    }
    return 1;
}

int TAK_MapShare_HasOwn(const char *map_key) {
    static const char *const look[] = { "ota", "tnt" };
    for (size_t i = 0; i < 2; i++) {
        char path[512];
        if (TAK_Maps_FindFile(map_key, look[i], path, sizeof path) != 0) continue;
        int origin = VFS_FileOrigin(path, NULL, 0);
        if (origin != VFS_ORIGIN_NONE && origin != VFS_ORIGIN_RECEIVED) return 1;
    }
    return 0;
}

/* One directory, kmap, with each part's file in it, uncompressed. */
int TAK_MapShare_WritePack(const char *path, const char *map_key,
                           const TAK_MapShareFiles *files, uint32_t date) {
    if (!path || !map_key || !files || !TAK_MapShare_NameOk(map_key)) return -1;
    const void *p[PARTS];
    size_t n[PARTS];
    parts_of(files, p, n);
    if (!p[PART_TNT] || !p[PART_OTA]) return -1;

    char names[PARTS][TAK_NET_MAP_NAME_MAX + 8];
    uint32_t name_at[PARTS], data_at[PARTS];
    int count = 0;
    uint32_t data = (uint32_t)(sizeof(HPIVersion) + sizeof(HPIHeader_V2)), data_size = 0;
    uint8_t name_block[2 + 5 + PARTS * (TAK_NET_MAP_NAME_MAX + 8) + 32];
    uint32_t name_size = 0;
    name_block[name_size++] = '\0';                       /* the root */
    uint32_t kmap_at = name_size;
    memcpy(name_block + name_size, "kmap", 5);
    name_size += 5;
    for (int k = 0; k < PARTS; k++) {
        if (!p[k]) continue;
        int w = snprintf(names[k], sizeof names[k], "%s.%s", map_key, part_ext[k]);
        if (w <= 0 || (size_t)w >= sizeof names[k]) return -1;
        name_at[k] = name_size;
        memcpy(name_block + name_size, names[k], (size_t)w + 1);
        name_size += (uint32_t)w + 1;
        data_at[k] = data + data_size;
        if (n[k] > TAK_MAP_SHARE_RAW_MAX) return -1;
        data_size += (uint32_t)n[k];
        count++;
    }
    /* The reader looks 19 bytes into a block for a compression mark. */
    while (name_size < 32) name_block[name_size++] = '\0';

    uint8_t dir_block[2 * 20 + PARTS * 24 + 32];
    uint32_t dir_size = 0;
    memset(dir_block, 0, sizeof dir_block);
    uint32_t files_at = 2 * 20;
    /* The root: no files of its own, one directory. */
    tak_put_u32(dir_block + 0, 0);
    tak_put_u32(dir_block + 4, 20);
    tak_put_u32(dir_block + 8, 1);
    tak_put_u32(dir_block + 12, files_at);
    tak_put_u32(dir_block + 16, 0);
    /* kmap and its files. */
    tak_put_u32(dir_block + 20, kmap_at);
    tak_put_u32(dir_block + 24, 0);
    tak_put_u32(dir_block + 28, 0);
    tak_put_u32(dir_block + 32, files_at);
    tak_put_u32(dir_block + 36, (uint32_t)count);
    dir_size = files_at;
    for (int k = 0; k < PARTS; k++) {
        if (!p[k]) continue;
        uint8_t *e = dir_block + dir_size;
        tak_put_u32(e + 0, name_at[k]);
        tak_put_u32(e + 4, data_at[k]);
        tak_put_u32(e + 8, (uint32_t)n[k]);
        tak_put_u32(e + 12, 0);                        /* stored, not compressed */
        tak_put_u32(e + 16, date);
        tak_put_u32(e + 20, 0);
        dir_size += 24;
    }
    while (dir_size < 32) dir_block[dir_size++] = 0;

    uint8_t head[sizeof(HPIVersion) + sizeof(HPIHeader_V2)];
    tak_put_u32(head + 0, HPI_MAGIC);
    tak_put_u32(head + 4, HPI_VERSION_V2);
    uint32_t name_block_at = data + data_size;
    tak_put_u32(head + 8, name_block_at + name_size);    /* dir_block */
    tak_put_u32(head + 12, dir_size);
    tak_put_u32(head + 16, name_block_at);               /* name_block */
    tak_put_u32(head + 20, name_size);
    tak_put_u32(head + 24, data);
    tak_put_u32(head + 28, 0);

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int ok = fwrite(head, 1, sizeof head, fp) == sizeof head;
    for (int k = 0; ok && k < PARTS; k++)
        if (p[k] && n[k]) ok = fwrite(p[k], 1, n[k], fp) == n[k];
    ok = ok && fwrite(name_block, 1, name_size, fp) == name_size;
    ok = ok && fwrite(dir_block, 1, dir_size, fp) == dir_size;
    if (fclose(fp) != 0) ok = 0;
    return ok ? 0 : -1;
}

#ifdef __EMSCRIPTEN__
/* The page copies it out to the browser storage it keeps the game's files
 * in, so it is there after a reload. */
EM_JS(void, share_keep_in_browser, (const char *rel), {
    if (typeof Module !== 'undefined' && Module.okKeepReceived)
        Module.okKeepReceived(UTF8ToString(rel));
});
#endif

int TAK_MapShare_Keep(const char *dir, const char *map_key, const TAK_MapShareFiles *files) {
    if (!dir || !TAK_MapShare_NameOk(map_key) || !files) return -1;
    char final_path[1024], part_path[1040];
    int w = snprintf(final_path, sizeof final_path, "%s%s.kmp", dir, map_key);
    if (w <= 0 || (size_t)w >= sizeof final_path) return -1;
    snprintf(part_path, sizeof part_path, "%s.part", final_path);
    /* Dated 0, so any other copy of that name a player adds wins a tie. */
    if (TAK_MapShare_WritePack(part_path, map_key, files, 0) != 0) {
        remove(part_path);
        return -1;
    }
    /* Out of the mount first: an open file cannot be replaced everywhere. */
    (void)VFS_DropMapPack(final_path);
    remove(final_path);
    if (rename(part_path, final_path) != 0) { remove(part_path); return -1; }
    if (VFS_AddMapPack(final_path) != 0) return -1;
#ifdef __EMSCRIPTEN__
    char rel[TAK_NET_MAP_NAME_MAX + 16];
    snprintf(rel, sizeof rel, "Received/%s.kmp", map_key);
    share_keep_in_browser(rel);
#endif
    return 0;
}

/* ── What a player without it is told ───────────────────────────────── */

void TAK_MapShare_Advice(int share, const char *shown, const char *map_key,
                         char *out, size_t cap) {
    if (!out || !cap) return;
    const char *name = shown && shown[0] ? shown : map_key ? map_key : "this map";
    switch (share) {
    case TAK_MAPSHARE_OFFERED:
        snprintf(out, cap, "Receiving %s from the host.", name);
        break;
    case TAK_MAPSHARE_GAME:
        snprintf(out, cap, "%s comes with the game, and your game files are missing it "
                 "or hold another version of it.", name);
        break;
    case TAK_MAPSHARE_EXPANSION:
        snprintf(out, cap, "%s comes with The Iron Plague. Add the expansion's files "
                 "to play it.", name);
        break;
    case TAK_MAPSHARE_MAP_PACK:
        snprintf(out, cap, "%s is one of Cavedog's map packs. Put %s.kmp in your game's "
                 "Maps folder to play it.", name, map_key ? map_key : name);
        break;
    case TAK_MAPSHARE_TOO_BIG:
        snprintf(out, cap, "The host's %s is too big to send.", name);
        break;
    case TAK_MAPSHARE_UNREADABLE:
        snprintf(out, cap, "The host's %s cannot be sent.", name);
        break;
    default:
        snprintf(out, cap, "You do not have %s.", name);
        break;
    }
}
