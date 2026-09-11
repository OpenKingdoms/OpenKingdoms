/*
 * test_sound.c: the parts of the sound layer that need no device and
 * no game data: the viewport volume and pan rule, channel stealing,
 * and the debug recorder tests lean on.
 */

#include "test_framework.h"
#include "tak_sound.h"
#include "tak_game_sound.h"
#include "tak_memory.h"
#include "tak_hpi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#define tsnd_mkdir(p) _mkdir(p)
#define tsnd_rmdir(p) _rmdir(p)
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#define tsnd_mkdir(p) mkdir((p), 0755)
#define tsnd_rmdir(p) rmdir(p)
#endif

/* Inside the viewport a source plays at 0x7f, anywhere outside at
 * 0x40, with no distance term (legacy:221181-221190). */
TEST(spatialize_volume_is_two_step) {
    int vol = 0, pan = 0;
    TAK_Sound_Spatialize(500, 300, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x7f, vol);
    TAK_Sound_Spatialize(1001, 300, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, vol);
    /* Just past the bottom edge is as quiet as far across the map. */
    TAK_Sound_Spatialize(500, 601, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, vol);
    TAK_Sound_Spatialize(500, 9000, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, vol);
    /* The camera offset moves the window with it. */
    TAK_Sound_Spatialize(2500, 2300, 2000, 2000, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x7f, vol);
}

/* Pan is 64 plus 64 times the offset from the viewport centre over the
 * whole viewport width, clamped (legacy:221191-221194). */
TEST(spatialize_pan_runs_across_viewport_width) {
    int vol = 0, pan = 0;
    TAK_Sound_Spatialize(500, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x40, pan);
    TAK_Sound_Spatialize(1000, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x60, pan);          /* right edge: half way to hard right */
    TAK_Sound_Spatialize(0, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x20, pan);
    TAK_Sound_Spatialize(-600, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0, pan);             /* clamped */
    TAK_Sound_Spatialize(3000, 0, 0, 0, 1000, 600, &vol, &pan);
    ASSERT_EQ_INT(0x7f, pan);
    /* No viewport at all: centre. */
    TAK_Sound_Spatialize(3000, 0, 0, 0, 0, 0, &vol, &pan);
    ASSERT_EQ_INT(0x40, pan);
}

/* Only a strictly lower priority can be stolen, the lowest first and
 * the oldest among equals (legacy:308165-308203). */
TEST(choose_victim_steals_strictly_lower_priority_only) {
    int      active[4]   = { 1, 1, 1, 0 };
    int      priority[4] = { 4, 2, 2, 0 };
    uint32_t serial[4]   = { 10, 7, 3, 1 };
    /* A priority 4 request: both 2s qualify, the older (serial 3) goes. */
    ASSERT_EQ_INT(2, TAK_Sound_ChooseVictim(4, active, priority, serial, 4));
    /* A priority 2 request finds nothing strictly lower. */
    ASSERT_EQ_INT(-1, TAK_Sound_ChooseVictim(4, active, priority, serial, 2));
    /* A priority 7 request takes the lowest, not the oldest. */
    ASSERT_EQ_INT(2, TAK_Sound_ChooseVictim(4, active, priority, serial, 7));
    priority[0] = 1;
    ASSERT_EQ_INT(0, TAK_Sound_ChooseVictim(4, active, priority, serial, 7));
    /* A free slot is never a victim even at priority 0. */
    ASSERT_EQ_INT(-1, TAK_Sound_ChooseVictim(4, active, priority, serial, 0));
}

/* The recorder keeps the resolved name, the spatial result and whether
 * the wav was found. Without a VFS nothing loads, which is the point:
 * the trigger is still observable. */
TEST(debug_recorder_keeps_events_in_order) {
    GameSound_Init();
    GameSound_DebugRecord(1);
    GameSound_DebugClear();
    GameSound_PlayUI("oktobuild");
    GameSound_PlayWorldWav("CHITGRND.wav", 4, 100, 100, 0, 0, 640, 480);
    GameSound_PlayWorldWav("CHITGRND.wav", 4, 5000, 100, 0, 0, 640, 480);
    ASSERT_EQ_INT(3, GameSound_DebugCount());
    const GameSoundEvent *ev = GameSound_DebugEvent(0);
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ_STR("oktobuild", ev->name);
    ASSERT_EQ_INT(0, ev->positional);
    ASSERT_EQ_INT(0x7f, ev->volume);
    ASSERT_EQ_INT(7, ev->priority);
    ASSERT_EQ_INT(0, ev->loaded);
    ev = GameSound_DebugEvent(1);
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ_INT(1, ev->positional);
    ASSERT_EQ_INT(0x7f, ev->volume);
    ASSERT_EQ_INT(4, ev->priority);
    ev = GameSound_DebugEvent(2);
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ_INT(0x40, ev->volume);
    ASSERT_EQ_INT(0x7f, ev->pan);
    ASSERT_EQ_INT(2, GameSound_DebugCountPrefix("chitgrnd"));
    ASSERT_EQ_INT(2, GameSound_DebugFindPrefix("CHITGRND"));
    ASSERT_EQ_INT(-1, GameSound_DebugFindPrefix("AHIT"));
    GameSound_DebugClear();
    ASSERT_EQ_INT(0, GameSound_DebugCount());
    GameSound_DebugRecord(0);
    GameSound_PlayUI("oktobuild");
    ASSERT_EQ_INT(0, GameSound_DebugCount());
    GameSound_Shutdown();
}

static void put_le(uint8_t *p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* A short silent 16-bit mono wav, built here so the test needs no
 * game data. */
static int write_silent_wav(const char *path, uint32_t frames) {
    uint8_t h[44];
    uint32_t data_bytes = frames * 2;
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + data_bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);          /* PCM */
    put_le(h + 22, 1, 2);          /* mono */
    put_le(h + 24, 22050, 4);
    put_le(h + 28, 44100, 4);      /* bytes per second */
    put_le(h + 32, 2, 2);          /* block align */
    put_le(h + 34, 16, 2);         /* bits per sample */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data_bytes, 4);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(h, 1, sizeof(h), f);
    for (uint32_t i = 0; i < data_bytes; i++) fputc(0, f);
    return fclose(f) == 0 ? 0 : -1;
}

#define CACHE_TEST_ROOT "test_sound_vfs"
#define CACHE_TEST_WAV  CACHE_TEST_ROOT "/sounds/cacheprobe.wav"

static void cache_test_cleanup(void) {
    VFS_Shutdown();
    remove(CACHE_TEST_WAV);
    tsnd_rmdir(CACHE_TEST_ROOT "/sounds");
    tsnd_rmdir(CACHE_TEST_ROOT);
}

/* Every name asked for stays cached, found or not. After six hundred
 * missing names a real wav decodes once, a second play allocates
 * nothing, and shutdown frees it. The cache used to stop at 512, so a
 * later name decoded again on every play and was never freed. */
TEST(wav_cache_keeps_every_name) {
    tsnd_mkdir(CACHE_TEST_ROOT);
    tsnd_mkdir(CACHE_TEST_ROOT "/sounds");
    ASSERT_EQ_INT(0, write_silent_wav(CACHE_TEST_WAV, 64));
    ASSERT_EQ_INT(0, VFS_Init(CACHE_TEST_ROOT, CACHE_TEST_ROOT));
    GameSound_Init();
    char name[32];
    for (int i = 0; i < 600; i++) {
        snprintf(name, sizeof(name), "nosuchwav%03d", i);
        GameSound_PlayUI(name);
    }
    GameSound_DebugRecord(1);
    GameSound_DebugClear();
    TakMemStats before, first, second, end;
    tak_mem_get_stats(&before);
    GameSound_PlayUI("cacheprobe");
    tak_mem_get_stats(&first);
    GameSound_PlayUI("cacheprobe");
    tak_mem_get_stats(&second);
    GameSound_DebugRecord(0);
    GameSound_Shutdown();
    tak_mem_get_stats(&end);
    int loaded = GameSound_DebugCount() == 2 &&
                 GameSound_DebugEvent(0)->loaded &&
                 GameSound_DebugEvent(1)->loaded;
    cache_test_cleanup();
    ASSERT(loaded);
    ASSERT_EQ_INT((int)first.live_alloc_count, (int)second.live_alloc_count);
    ASSERT((int)end.live_alloc_count <= (int)before.live_alloc_count);
}

/* A name the cache cannot keep is never handed out. Only cached
 * entries are freed at shutdown, so an effect handed out after a
 * failed insert is one nothing will ever free. The seam fails the
 * next insert, which is what a full cache used to do at 512 names. */
TEST(wav_cache_drops_what_it_cannot_keep) {
    tsnd_mkdir(CACHE_TEST_ROOT);
    tsnd_mkdir(CACHE_TEST_ROOT "/sounds");
    ASSERT_EQ_INT(0, write_silent_wav(CACHE_TEST_WAV, 64));
    ASSERT_EQ_INT(0, VFS_Init(CACHE_TEST_ROOT, CACHE_TEST_ROOT));
    GameSound_Init();
    GameSound_DebugRecord(1);
    GameSound_DebugClear();
    /* One miss first, so the cache's own array is already allocated
     * and the counts compare like for like. */
    GameSound_PlayUI("nosuchwav");
    TakMemStats before, refused, end;
    tak_mem_get_stats(&before);

    GameSound_DebugFailCacheInsertOnce();
    GameSound_PlayUI("cacheprobe");
    tak_mem_get_stats(&refused);
    int handed_out = GameSound_DebugEvent(GameSound_DebugCount() - 1)->loaded;

    /* The next play stores it and does hand it out. */
    GameSound_PlayUI("cacheprobe");
    int second = GameSound_DebugEvent(GameSound_DebugCount() - 1)->loaded;

    GameSound_DebugRecord(0);
    GameSound_Shutdown();
    tak_mem_get_stats(&end);
    cache_test_cleanup();
    ASSERT_EQ_INT(0, handed_out);
    ASSERT_EQ_INT((int)before.live_alloc_count, (int)refused.live_alloc_count);
    ASSERT_EQ_INT(1, second);
    ASSERT((int)end.live_alloc_count <= (int)before.live_alloc_count);
}

#ifndef TAK_SOURCE_DIR
#define TAK_SOURCE_DIR "."
#endif

/* Code points a cp1252 byte 0x80..0xBF decodes to. UTF-8 read as
 * cp1252 and saved again as UTF-8 turns each continuation byte into
 * one of these. */
static int is_cp1252_cont(uint32_t c) {
    static const uint16_t specials[] = {
        0x20AC, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6,
        0x2030, 0x0160, 0x2039, 0x0152, 0x017D, 0x2018, 0x2019, 0x201C,
        0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
        0x0153, 0x017E, 0x0178
    };
    if (c >= 0x80 && c <= 0xBF) return 1;
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++)
        if (c == specials[i]) return 1;
    return 0;
}

static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *i) {
    unsigned char b = s[*i];
    int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3
            : (b >> 3) == 30 ? 4 : 0;
    if (len == 0 || *i + (size_t)len > n) { (*i)++; return 0xFFFD; }
    uint32_t c = len == 1 ? b : len == 2 ? (b & 0x1Fu)
               : len == 3 ? (b & 0x0Fu) : (b & 0x07u);
    for (int k = 1; k < len; k++) c = (c << 6) | (s[*i + (size_t)k] & 0x3Fu);
    *i += (size_t)len;
    return c;
}

/* 1-based line of the first double-encoded character, or 0. Two-byte
 * originals come back as A-circumflex or A-tilde plus one such code
 * point, three-byte ones as a U+00E0..U+00EF lead plus two. */
static int first_double_encoded_line(const unsigned char *s, size_t n) {
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    int line = 1;
    size_t i = 0;
    while (i < n) {
        uint32_t c = utf8_next(s, n, &i);
        w0 = w1; w1 = w2; w2 = c;
        if ((w1 == 0xC2 || w1 == 0xC3) && is_cp1252_cont(w2)) return line;
        if (w0 >= 0xE0 && w0 <= 0xEF && is_cp1252_cont(w1) && is_cp1252_cont(w2))
            return line;
        if (c == '\n') line++;
    }
    return 0;
}

typedef struct { char path[512]; int line; int files; } EncodingScan;

static void scan_source_file(const char *path, EncodingScan *r) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = len > 0 ? (unsigned char *)malloc((size_t)len) : NULL;
    size_t got = data ? fread(data, 1, (size_t)len, f) : 0;
    fclose(f);
    if (!data) return;
    r->files++;
    int line = first_double_encoded_line(data, got);
    free(data);
    if (line && !r->path[0]) {
        snprintf(r->path, sizeof(r->path), "%s", path);
        r->line = line;
    }
}

static int is_c_source(const char *name) {
    size_t n = strlen(name);
    return n > 2 && name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'h');
}

static void scan_source_tree(const char *dir, EncodingScan *r) {
    char path[512];
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof(path), "%s/*", dir);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan_source_tree(path, r);
        else if (is_c_source(fd.cFileName)) scan_source_file(path, r);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_source_tree(path, r);
        else if (is_c_source(e->d_name)) scan_source_file(path, r);
    }
    closedir(d);
#endif
}

/* No source carries text saved through a cp1252 round trip. The sound
 * change once re-saved a header that way and turned its box-drawing,
 * arrow, section and dash characters into mojibake. */
TEST(sources_carry_no_double_encoded_utf8) {
    EncodingScan r;
    memset(&r, 0, sizeof(r));
    scan_source_tree(TAK_SOURCE_DIR "/include", &r);
    scan_source_tree(TAK_SOURCE_DIR "/src", &r);
    if (r.path[0]) printf("(%s:%d) ", r.path, r.line);
    ASSERT(r.files > 50);
    ASSERT(r.path[0] == '\0');
}

int main(void) {
    TEST_SUITE("sound");
    tak_mem_init();
    RUN(spatialize_volume_is_two_step);
    RUN(spatialize_pan_runs_across_viewport_width);
    RUN(choose_victim_steals_strictly_lower_priority_only);
    RUN(debug_recorder_keeps_events_in_order);
    RUN(wav_cache_keeps_every_name);
    RUN(wav_cache_drops_what_it_cannot_keep);
    RUN(sources_carry_no_double_encoded_utf8);
    TEST_REPORT();
}
