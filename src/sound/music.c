/*
 * music.c — TAK music streaming system (Phase 4)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  GAME AUDIO LESSON: Streaming vs. memory-loaded audio
 * ═══════════════════════════════════════════════════════════════════
 *
 * Sound effects (sword clashes, unit voices) are tiny: 8-15 KB each.
 * You load them entirely into RAM so playback is instant — the mixer
 * just reads from the buffer. This is "memory-loaded" audio.
 *
 * Music tracks are 3-5 MB each (20 tracks = 60-100 MB total). Loading
 * all of them into RAM is wasteful when only one plays at a time.
 * Instead, we STREAM: read small chunks (~64 KB) from disk into a ring
 * buffer as the mixer consumes them. miniaudio does this automatically
 * when you pass MA_SOUND_FLAG_STREAM to ma_sound_init_from_file.
 *
 * The trade-off: streaming costs a small amount of disk I/O each frame,
 * but saves potentially hundreds of MB of RAM. For music (which is
 * always playing continuously), streaming is the right choice.
 *
 * ═══════════════════════════════════════════════════════════════════
 *  MUSIC STATE MACHINE
 * ═══════════════════════════════════════════════════════════════════
 *
 * The original engine (MusicSys_Update at line 308579) runs a simple
 * state machine every frame:
 *
 *   1. Is a track currently playing?
 *   2. Has it finished? (check _AIL_stream_status_4)
 *   3. If finished:
 *      - SEQUENTIAL mode: advance to next track (wrap 20→1)
 *      - SHUFFLE mode: pick next from shuffled order
 *   4. If mode is OFF: stop anything playing
 *
 * The shuffle uses Fisher-Yates: at startup (or when all tracks have
 * played), generate a random permutation of [1..N]. Walk through it
 * in order. This guarantees every track plays before any repeats.
 */

#include "tak_music.h"
#include "tak_sound.h"    /* for TAK_Sound_IsInitialized */

/* We only need the miniaudio types — the implementation is in sound.c */
#include "miniaudio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>  /* rand, srand */

/* ── Configuration ───────────────────────────────────────────────── */

#define TAK_MAX_TRACKS 32

/* ── State ───────────────────────────────────────────────────────── */

static struct {
    /* Track paths (filesystem, not VFS — music is loose) */
    char   track_paths[TAK_MAX_TRACKS][512];
    int    track_numbers[TAK_MAX_TRACKS];
    int    track_count;

    /* The list to draw from, by track number, and its shuffled order.
     * pos is where in the order the playing track is, -1 before any. */
    int    list[TAK_MAX_TRACKS];
    int    list_count;
    int    order[TAK_MAX_TRACKS];
    int    pos;
    int    current_track;

    /* Playback state */
    ma_sound       stream;
    int            stream_active;   /* 1 if stream initialized */
    TAK_MusicMode  mode;
    int            volume;          /* 0-127 */
    int            current_index;   /* index into playlist (0-based) */
    int            paused;

    /* Shuffle order (Fisher-Yates permutation) */
    int            shuffle_order[TAK_MAX_TRACKS];

    /* Reference to miniaudio engine (from sound.c) */
    ma_engine     *engine;

    int            initialized;
} g_music;

/* Forward declaration — we need the engine from sound.c */
extern ma_engine *TAK_Sound_GetEngine(void);

#include "tak_hpi.h"
#include "tak_sides.h"
#include "tak_tdf.h"

/* ── Shuffle ─────────────────────────────────────────────────────── */

static void shuffle_tracks(void) {
    /* Fisher-Yates shuffle: for i from n-1 downto 1, swap [i] with
     * a random element [0..i]. Guarantees uniform distribution. */
    for (int i = 0; i < g_music.track_count; i++)
        g_music.shuffle_order[i] = i;

    for (int i = g_music.track_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = g_music.shuffle_order[i];
        g_music.shuffle_order[i] = g_music.shuffle_order[j];
        g_music.shuffle_order[j] = tmp;
    }
}

/* ── Stream management ───────────────────────────────────────────── */

static void close_stream(void) {
    if (g_music.stream_active) {
        ma_sound_stop(&g_music.stream);
        ma_sound_uninit(&g_music.stream);
        g_music.stream_active = 0;
    }
}

static int open_and_play(const char *path) {
    if (!g_music.engine) return -1;

    close_stream();

    /* MA_SOUND_FLAG_STREAM: don't load the whole file — read chunks
     * from disk on demand. This is the key difference from SFX. */
    ma_result r = ma_sound_init_from_file(
        g_music.engine, path,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION,
        NULL, NULL, &g_music.stream);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "Music: failed to open %s: %s\n",
                path, ma_result_description(r));
        return -1;
    }

    g_music.stream_active = 1;

    float vol = (float)g_music.volume / 127.0f;
    ma_sound_set_volume(&g_music.stream, vol);
    ma_sound_set_looping(&g_music.stream, MA_FALSE);
    ma_sound_start(&g_music.stream);
    fprintf(stderr, "Music: playing %s\n", path);   /* the web smoke waits for this */

    return 0;
}

/* ── Scan for music tracks ───────────────────────────────────────── */

static int scan_music_directory(const char *game_dir) {
    /* The original stores tracks as "Track1.wav" through "Track20.wav"
     * in the Music/ subdirectory of the game folder. */
    g_music.track_count = 0;

    for (int i = 1; i <= TAK_MAX_TRACKS; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/Music/track%d.wav", game_dir, i);

        /* Check if file exists (try to open it) */
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            /* Also try uppercase */
            snprintf(path, sizeof(path), "%s/Music/Track%d.wav", game_dir, i);
            fp = fopen(path, "rb");
        }
        if (!fp) continue;
        fclose(fp);

        strncpy(g_music.track_paths[g_music.track_count], path,
                sizeof(g_music.track_paths[0]) - 1);
        g_music.track_numbers[g_music.track_count] = i;
        g_music.track_count++;
    }

    return g_music.track_count;
}

/* ── Public API ──────────────────────────────────────────────────── */

int TAK_Music_Init(const char *game_dir) {
    if (g_music.initialized) return 0;
    if (!game_dir) return -1;
    if (!TAK_Sound_IsInitialized()) return -1;

    memset(&g_music, 0, sizeof(g_music));
    g_music.volume = 64;  /* 50% — original default was 0x40 */
    g_music.mode   = TAK_MUSIC_OFF;
    g_music.engine = TAK_Sound_GetEngine();

    int count = scan_music_directory(game_dir);
    fprintf(stderr, "Music: found %d tracks in %s/Music/\n", count, game_dir);

    if (count == 0) {
        fprintf(stderr, "Music: no tracks found, music disabled\n");
        return -1;
    }

    shuffle_tracks();
    g_music.initialized = 1;
    return 0;
}

void TAK_Music_Shutdown(void) {
    if (!g_music.initialized) return;
    close_stream();
    g_music.initialized = 0;
    fprintf(stderr, "Music: shutdown\n");
}

static const char *path_for_track(int number) {
    for (int i = 0; i < g_music.track_count; i++) {
        if (g_music.track_numbers[i] == number) return g_music.track_paths[i];
    }
    return NULL;
}

/* A random order of the list (legacy:308686-308700). */
static void shuffle_list(void) {
    for (int i = 0; i < g_music.list_count; i++) g_music.order[i] = g_music.list[i];
    for (int i = g_music.list_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = g_music.order[i];
        g_music.order[i] = g_music.order[j];
        g_music.order[j] = t;
    }
}

/* The next track of the list that the install has. 0 with none. */
static int next_of_list(void) {
    for (int tries = 0; tries < g_music.list_count; tries++) {
        g_music.pos++;
        if (g_music.pos >= g_music.list_count) {
            g_music.pos = 0;
            shuffle_list();
        }
        if (path_for_track(g_music.order[g_music.pos])) return g_music.order[g_music.pos];
    }
    return 0;
}

static void play_track_number(int number) {
    const char *path = path_for_track(number);
    if (!path) return;
    if (open_and_play(path) == 0) g_music.current_track = number;
}

void TAK_Music_SetTrackList(const int *tracks, int count) {
    if (!g_music.initialized) return;
    g_music.list_count = 0;
    for (int i = 0; tracks && i < count && g_music.list_count < TAK_MAX_TRACKS; i++) {
        if (tracks[i] > 0) g_music.list[g_music.list_count++] = tracks[i];
    }
    shuffle_list();
    g_music.pos = -1;
    /* The list changes with the screen, and the track with it
     * (legacy:243483 closes the stream as a battle begins). */
    close_stream();
    g_music.current_track = 0;
}

void TAK_Music_UseInterfaceList(void) {
    int tracks[TAK_MAX_TRACKS];
    int n = 0;
    if (VFS_IsInitialized()) {
        TDFFile *tdf = TDF_Open("gamedata/interface.tdf");
        if (tdf && TDF_Load(tdf) == 0 && TDF_PushSection(tdf, "InterfaceMusic") == 0) {
            const char *text = TDF_ReadString(tdf, "musictracks", "");
            const char *p = text;
            while (*p && n < TAK_MAX_TRACKS) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                int v = 0, any = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
                if (!any) { if (*p) p++; continue; }
                if (v > 0) tracks[n++] = v;
            }
            TDF_PopSection(tdf);
        }
        if (tdf) TDF_Close(tdf);
    }
    TAK_Music_SetTrackList(tracks, n);
}

void TAK_Music_UseSideList(int side) {
    const TakSideInfo *s = Sides_Get(side);
    if (!s || s->music_track_count <= 0) { TAK_Music_SetTrackList(NULL, 0); return; }
    TAK_Music_SetTrackList(s->music_tracks, s->music_track_count);
}

int TAK_Music_CurrentTrack(void) {
    return g_music.stream_active ? g_music.current_track : 0;
}

void TAK_Music_DebugSkip(void) {
    if (!g_music.initialized) return;
    close_stream();
    g_music.current_track = 0;
    TAK_Music_Update();
}

void TAK_Music_Update(void) {
    if (!g_music.initialized) return;
    if (g_music.mode == TAK_MUSIC_OFF) return;
    if (g_music.paused) return;
    if (g_music.track_count == 0) return;

    if (g_music.list_count > 0) {
        if (g_music.stream_active && !ma_sound_at_end(&g_music.stream)) return;
        if (g_music.stream_active) close_stream();
        int number = next_of_list();
        if (number > 0) play_track_number(number);
        return;
    }

    /* Check if current track has finished */
    if (g_music.stream_active && ma_sound_at_end(&g_music.stream)) {
        close_stream();

        /* Advance to next track */
        g_music.current_index++;
        if (g_music.current_index >= g_music.track_count) {
            g_music.current_index = 0;
            if (g_music.mode == TAK_MUSIC_SHUFFLE) {
                shuffle_tracks();  /* Re-shuffle for the next cycle */
            }
        }

        int track_idx = (g_music.mode == TAK_MUSIC_SHUFFLE)
                         ? g_music.shuffle_order[g_music.current_index]
                         : g_music.current_index;

        open_and_play(g_music.track_paths[track_idx]);
    }

    /* If nothing is playing, start the first track */
    if (!g_music.stream_active) {
        int track_idx = (g_music.mode == TAK_MUSIC_SHUFFLE)
                         ? g_music.shuffle_order[0]
                         : 0;
        g_music.current_index = 0;
        open_and_play(g_music.track_paths[track_idx]);
    }
}

void TAK_Music_SetMode(TAK_MusicMode mode) {
    if (!g_music.initialized) return;

    TAK_MusicMode old = g_music.mode;
    g_music.mode = mode;

    if (mode == TAK_MUSIC_OFF) {
        close_stream();
    } else if (old == TAK_MUSIC_OFF) {
        /* Turning music on — start from the beginning */
        g_music.current_index = 0;
        if (mode == TAK_MUSIC_SHUFFLE) shuffle_tracks();
    }
}

TAK_MusicMode TAK_Music_GetMode(void) {
    return g_music.mode;
}

void TAK_Music_SetVolume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    g_music.volume = vol;
    if (g_music.stream_active) {
        ma_sound_set_volume(&g_music.stream, (float)vol / 127.0f);
    }
}

int TAK_Music_GetVolume(void) {
    return g_music.volume;
}

void TAK_Music_PlayTrack(int track_number) {
    if (!g_music.initialized) return;
    play_track_number(track_number);
}

void TAK_Music_Pause(int pause) {
    if (!g_music.initialized) return;
    g_music.paused = pause;
    if (g_music.stream_active) {
        if (pause) ma_sound_stop(&g_music.stream);
        else       ma_sound_start(&g_music.stream);
    }
}

int TAK_Music_IsPlaying(void) {
    return g_music.stream_active &&
           !g_music.paused &&
           ma_sound_is_playing(&g_music.stream);
}

int TAK_Music_GetTrackCount(void) {
    return g_music.track_count;
}
