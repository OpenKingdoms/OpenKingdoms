/*
 * story.c -- Story / campaign screen (GAMESTATE_CAMPAIGN).
 *
 * The original "Book of Deeds" opens a player's book at the chapter
 * they have reached. Which books exist is a scan of camps\*.tdf named
 * through the translate table (legacy:141576, legacy:143362), the page
 * shows the chapter's localised title and the art that belongs to it
 * (legacy:144484-144540), and Play hands the mission to the normal
 * World_BeginLoad -> GAMESTATE_GAME_LOADING path.
 */

#include "tak_story.h"
#include "tak_battle_config.h"
#include "tak_blit.h"
#include "tak_dataset.h"
#include "tak_font.h"
#include "tak_gameloop.h"
#include "tak_gaf.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_hpi.h"
#include "tak_loading.h"
#include "tak_memory.h"
#include "tak_mission.h"
#include "tak_save_browser.h"
#include "tak_savegame.h"
#include "tak_settings.h"
#include "tak_sides.h"
#include "tak_tdf.h"
#include "tak_translate.h"
#include "tak_ui.h"
#include "tak_util.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORY_MAX_MISSIONS   64
#define STORY_MAX_CAMPAIGNS  16

/* The campaign the base game ships, and the one the expansion adds. */
#define STORY_DARIEN "book of darien.tdf"
#define STORY_PLAGUE "the iron plague.tdf"

/* Chapter art: Book of Darien's chapters start at frame 1 and The Iron
 * Plague's at 0x32, with 0x31 for a book that is neither
 * (legacy:144484-144513). */
#define STORY_PLAGUE_FIRST_FRAME 0x32
#define STORY_OTHER_FRAME        0x31

/* The chapter the expansion ends at, and what a shifted Play runs
 * instead (legacy:143903-143923). */
#define STORY_HIDDEN_FROM "takx25_dh"
#define STORY_HIDDEN_TO   "takx26_dh.ota"

#define STORY_CHEAT "wasabi"

/* Which book was open last, the way the original keeps a favourite
 * campaign beside the favourite user (legacy:144073). */
#define STORY_SETTING_BOOK "FavoriteCampaign"

typedef struct StoryMission {
    char file[80];
    char name[80];
    char stem[80];
} StoryMission;

typedef struct StoryCampaign {
    char file[80];        /* lower case, "book of darien.tdf" */
    char name[96];        /* shown, "Book of Darien"          */
    StoryMission missions[STORY_MAX_MISSIONS];
    int  mission_count;
    int  high_water;      /* furthest chapter reached, zero based */
} StoryCampaign;

/* The Change User dialog: the player's name and, when the install
 * offers more than one book, which book (legacy:142640). */
typedef struct StoryChooser {
    int         open;
    int         has_dialog;
    GUIDialog   dialog;
    GUIRuntime *rt;
    Font       *font;
    int         idx_list, idx_inc, idx_dec;
    int         scroll, sel;
    int         prev_mouse;
    char        name[32];
} StoryChooser;

static struct {
    int initialized;
    GUIDialog dialog;
    GUIRuntime *rt;
    Font *tooltip_font;

    StoryCampaign camps[STORY_MAX_CAMPAIGNS];
    int camp_count;
    int camp;
    int selected;
    unsigned camps_generation;   /* the mount the list was built from */
    int camps_loaded;

    TranslateTable tt;
    int tt_loaded;

    char player_name[32];
    char cheat[16];

    /* The chapter art, drawn straight from story1.gaf: a widget carries
     * at most GUI_MAX_FRAMES and the entry has dozens. */
    GAFFile *art;
    uint32_t art_rgba[256];
    int art_entry;
    int art_frames;
    int art_tried;
    uint32_t *art_px;
    int art_px_frame, art_w, art_h, art_ox, art_oy;

    int browser_open;
    StoryChooser chooser;
} story;

/* ── Small helpers ───────────────────────────────────────────────────── */

static void read_progress(void);

static void copy_bounded(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    while (n + 1 < cap && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static void lower_copy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    while (n + 1 < cap && src[n]) {
        char c = src[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[n++] = c;
    }
    dst[n] = '\0';
}

static void file_to_stem(char *dst, size_t cap, const char *file) {
    copy_bounded(dst, cap, file);
    size_t len = strlen(dst);
    if (len > 4 && tak_stricmp(dst + len - 4, ".ota") == 0) {
        dst[len - 4] = '\0';
    }
}

static const char *path_base(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

/* ── Translate tables ────────────────────────────────────────────────── */

/* Campaign names live in the expansion's guiexpansion.tdf, keyed by the
 * lower case file name, and chapter titles in missions.tdf and the
 * expansion's ipmissions.tdf. A base install carries neither expansion
 * file, and a miss hands back the key (legacy:267931). */
static void load_translate(void) {
    if (story.tt_loaded) return;
    story.tt_loaded = 1;
    Translate_Load(&story.tt, "english/translate/gui_text.tdf");
    Translate_Load(&story.tt, "english/translate/guiexpansion.tdf");
    Translate_Load(&story.tt, "english/translate/missions.tdf");
    Translate_Load(&story.tt, "english/translate/ipmissions.tdf");
}

static void free_translate(void) {
    if (!story.tt_loaded) return;
    Translate_Free(&story.tt);
    memset(&story.tt, 0, sizeof story.tt);
    story.tt_loaded = 0;
}

/* ── Campaign enumeration ────────────────────────────────────────────── */

static int read_campaign(StoryCampaign *c) {
    char path[160];
    snprintf(path, sizeof(path), "camps/%s", c->file);
    TDFFile *tdf = TDF_Open(path);
    if (!tdf || TDF_Load(tdf) != 0) {
        if (tdf) TDF_Close(tdf);
        return -1;
    }
    c->mission_count = 0;
    for (int i = 0; i < STORY_MAX_MISSIONS; i++) {
        char section[32];
        snprintf(section, sizeof(section), "MISSION%d", i);
        if (TDF_PushSection(tdf, section) != 0) break;
        const char *file = TDF_ReadString(tdf, "missionfile", "");
        const char *name = TDF_ReadString(tdf, "missionname", "");
        if (file && file[0] && name && name[0]) {
            StoryMission *m = &c->missions[c->mission_count++];
            copy_bounded(m->file, sizeof(m->file), file);
            copy_bounded(m->name, sizeof(m->name), name);
            file_to_stem(m->stem, sizeof(m->stem), file);
        }
        TDF_PopSection(tdf);
    }
    TDF_Close(tdf);
    return c->mission_count > 0 ? 0 : -1;
}

static int campaign_known(const char *file) {
    for (int i = 0; i < story.camp_count; i++) {
        if (tak_stricmp(story.camps[i].file, file) == 0) return 1;
    }
    return 0;
}

/* One camps\*.tdf entry. Every file the scan finds is a book, and the
 * blank entry is the only one the original drops (legacy:143366-143377).
 * The name is the translate table's, and a miss hands back the key
 * (legacy:267931), so a book no table names reads as its own file name.
 * The chapter art's catch-all frame exists for exactly those books. */
static void consider_campaign(const char *path) {
    char file[80];
    lower_copy(file, sizeof(file), path_base(path));
    size_t len = strlen(file);
    if (len <= 4 || tak_stricmp(file + len - 4, ".tdf") != 0) return;
    if (campaign_known(file)) return;
    if (story.camp_count >= STORY_MAX_CAMPAIGNS) return;

    /* -pretendnoexpansion cuts the list back to the one book
     * (legacy:141580-141710), as an install without the files does. */
    if (tak_stricmp(file, STORY_DARIEN) != 0 && !TAK_DataSet_HasIronPlague())
        return;

    StoryCampaign *c = &story.camps[story.camp_count];
    memset(c, 0, sizeof(*c));
    copy_bounded(c->file, sizeof(c->file), file);
    copy_bounded(c->name, sizeof(c->name), Translate_Lookup(&story.tt, file));
    if (read_campaign(c) != 0) return;
    story.camp_count++;
}

/* The chooser sorts on the shown name, case-insensitively
 * (legacy:261900-261990, legacy:143490). */
static int campaign_cmp(const void *a, const void *b) {
    return tak_stricmp(((const StoryCampaign *)a)->name,
                       ((const StoryCampaign *)b)->name);
}

/* The original scans the directory every time it builds the list. A
 * remount is the only thing that changes the answer, so the scan is
 * kept until the VFS generation moves. */
static void load_campaigns(void) {
    if (story.camps_loaded && story.camps_generation == VFS_Generation()) return;
    story.camp_count = 0;
    story.camp = 0;
    story.selected = 0;
    story.camps_loaded = 1;
    story.camps_generation = VFS_Generation();
    free_translate();
    if (!VFS_IsInitialized()) return;
    load_translate();

    /* Both spellings: the archives hold camps/, and a loose dev tree
     * holds it under the archive name it came from (hpi_vfs.c:526). */
    static const char *const patterns[2] = { "camps/*.tdf", "data/camps/*.tdf" };
    for (int p = 0; p < 2; p++) {
        char **paths = NULL;
        int n = 0;
        if (VFS_ListFiles(patterns[p], &paths, &n) != 0) continue;
        for (int i = 0; i < n; i++) {
            consider_campaign(paths[i]);
            tak_free(paths[i]);
        }
        tak_free(paths);
    }
    if (story.camp_count > 1) {
        qsort(story.camps, (size_t)story.camp_count, sizeof(StoryCampaign),
              campaign_cmp);
    }
    read_progress();
}

static StoryCampaign *current_campaign(void) {
    load_campaigns();
    if (story.camp < 0 || story.camp >= story.camp_count) return NULL;
    return &story.camps[story.camp];
}

static const StoryMission *current_mission(void) {
    StoryCampaign *c = current_campaign();
    if (!c || story.selected < 0 || story.selected >= c->mission_count) return NULL;
    return &c->missions[story.selected];
}

/* ── The open page ───────────────────────────────────────────────────── */

int Story_CampaignCount(void) {
    load_campaigns();
    return story.camp_count;
}

const char *Story_CampaignName(int index) {
    load_campaigns();
    if (index < 0 || index >= story.camp_count) return NULL;
    return story.camps[index].name;
}

const char *Story_CampaignFile(int index) {
    load_campaigns();
    if (index < 0 || index >= story.camp_count) return NULL;
    return story.camps[index].file;
}

int Story_SelectedCampaign(void) {
    load_campaigns();
    return story.camp;
}

void Story_SelectCampaign(int index) {
    load_campaigns();
    if (index < 0 || index >= story.camp_count) return;
    story.camp = index;
    story.selected = 0;
}

int Story_SelectedChapter(void) { return story.selected; }

void Story_SelectChapter(int chapter) {
    StoryCampaign *c = current_campaign();
    if (!c) return;
    if (chapter > c->high_water) chapter = c->high_water;
    if (chapter >= c->mission_count) chapter = c->mission_count - 1;
    if (chapter < 0) chapter = 0;
    story.selected = chapter;
}

int Story_ChapterCount(void) {
    StoryCampaign *c = current_campaign();
    return c ? c->mission_count : 0;
}

const char *Story_ChapterText(void) {
    const StoryMission *m = current_mission();
    if (!m) return "";
    load_translate();
    return Translate_Lookup(&story.tt, m->name);
}

int Story_HighWaterChapter(void) {
    StoryCampaign *c = current_campaign();
    return c ? c->high_water : 0;
}

/* ── Chapter art ─────────────────────────────────────────────────────── */

static void art_open(void) {
    if (story.art_tried) return;
    story.art_tried = 1;
    story.art_entry = -1;
    if (UI_LoadGAFWithPalette("data/anims/story1.gaf", "data/anims/story1.pcx",
                              &story.art, story.art_rgba) != 0 || !story.art) {
        story.art = NULL;
        return;
    }
    story.art_entry = GAF_FindSequence(story.art, "Story1");
    if (story.art_entry < 0) return;
    uint16_t frames = 0;
    memcpy(&frames, story.art->data + (uint32_t)story.art_entry, sizeof(frames));
    story.art_frames = frames;
}

int Story_ChapterImageFrame(void) {
    StoryCampaign *c = current_campaign();
    if (!c) return 0;
    int frame;
    if (tak_stricmp(c->file, STORY_PLAGUE) == 0) {
        frame = story.selected + STORY_PLAGUE_FIRST_FRAME;
    } else if (tak_stricmp(c->file, STORY_DARIEN) == 0) {
        frame = story.selected + 1;
    } else {
        frame = STORY_OTHER_FRAME;
    }
    art_open();
    if (story.art_frames > 0 && frame >= story.art_frames) frame = 0;
    return frame;
}

static void draw_chapter_art(SDL_Surface *off) {
    const GUIWidget *w = GUIDialog_FindByName(&story.dialog, "ChapterImage");
    if (!w || !off) return;
    art_open();
    if (!story.art || story.art_entry < 0) return;
    int frame = Story_ChapterImageFrame();
    if (frame != story.art_px_frame || !story.art_px) {
        tak_free(story.art_px);
        story.art_px = UI_DecodeFrame(story.art, story.art_entry, frame,
                                      story.art_rgba, &story.art_w, &story.art_h);
        story.art_px_frame = frame;
        story.art_ox = story.art_oy = 0;
        FrameHeader *fh = NULL;
        if (GAF_GetFrameInfo(story.art, (uint32_t)story.art_entry, frame, &fh) == 0
            && fh) {
            story.art_ox = fh->offset_x;
            story.art_oy = fh->offset_y;
        }
    }
    if (!story.art_px) return;
    Blit_RGBA(off, w->rect.x - story.art_ox, w->rect.y - story.art_oy,
              story.art_px, story.art_w, story.art_h);
}

/* ── Progress ────────────────────────────────────────────────────────── */

/* One settings line per player and book. The original keeps the same
 * fact in a highwater save under savedgames\<player>, written as a
 * mission is won (legacy:153802-153926) and read back when the book
 * opens (legacy:144384-144422). The port has no per player save
 * directory, and in the browser the settings file is restored before
 * the first frame where saved games are copied in lazily, so the line
 * lives there. docs/MANUAL_DEVIATIONS.md carries the note. */
static void highwater_key(char *out, size_t cap, const StoryCampaign *c) {
    char stem[48];
    copy_bounded(stem, sizeof(stem), c->file);
    size_t n = strlen(stem);
    if (n > 4 && tak_stricmp(stem + n - 4, ".tdf") == 0) stem[n - 4] = '\0';
    const char *who = Story_PlayerName();
    if (who && who[0]) snprintf(out, cap, "HighWater.%s.%s", who, stem);
    else               snprintf(out, cap, "HighWater.%s", stem);
}

/* Every book's furthest chapter, and which book was open last
 * (legacy:144073 reads FavoriteCampaign back). Called on a rebuild of
 * the list and whenever the player changes, because the lines are the
 * player's own. */
static void read_progress(void) {
    for (int i = 0; i < story.camp_count; i++) {
        StoryCampaign *c = &story.camps[i];
        char key[80];
        highwater_key(key, sizeof(key), c);
        int high = Settings_GetInt(key, 0);
        if (high > c->mission_count - 1) high = c->mission_count - 1;
        if (high < 0) high = 0;
        c->high_water = high;
    }
    const char *fav = Settings_GetStr(STORY_SETTING_BOOK, "");
    for (int i = 0; fav && fav[0] && i < story.camp_count; i++) {
        if (tak_stricmp(story.camps[i].file, fav) == 0) { story.camp = i; break; }
    }
    if (story.camp >= 0 && story.camp < story.camp_count)
        story.selected = story.camps[story.camp].high_water;
}

/* The write the original does as the mission ends. Settings_Save is
 * also what asks the browser to copy the file out of the tab. */
static void write_progress(const StoryCampaign *c) {
    char key[80];
    highwater_key(key, sizeof(key), c);
    Settings_SetInt(key, c->high_water);
    Settings_Save();
}

/* The cheat opens the book for this sitting only: the original writes
 * the highwater save on a won mission and nowhere else. */
void Story_UnlockAllChapters(void) {
    StoryCampaign *c = current_campaign();
    if (!c) return;
    c->high_water = c->mission_count - 1;
}

/* The original keeps every typed character in one buffer and checks its
 * tail against the cheat (legacy:144228-144232). This keeps only the
 * tail that is still a prefix of it, which answers the same. */
void Story_TypeText(const char *text) {
    if (!text) return;
    for (const char *p = text; *p; p++) {
        char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        size_t n = strlen(story.cheat);
        if (n + 2 > sizeof(story.cheat)) n = 0;
        story.cheat[n] = ch;
        story.cheat[n + 1] = '\0';
        while (story.cheat[0] &&
               strncmp(STORY_CHEAT, story.cheat, strlen(story.cheat)) != 0) {
            memmove(story.cheat, story.cheat + 1, strlen(story.cheat));
        }
        if (strcmp(story.cheat, STORY_CHEAT) == 0) {
            Story_UnlockAllChapters();
            story.cheat[0] = '\0';
        }
    }
}

void Story_MissionFinished(int won) {
    StoryCampaign *c = current_campaign();
    if (!c || !won) return;
    if (story.selected + 1 >= c->mission_count) return;
    if (story.selected + 1 > c->high_water) {
        c->high_water = story.selected + 1;
        write_progress(c);
    }
    story.selected = story.selected + 1;
}

/* ── The player ──────────────────────────────────────────────────────── */

const char *Story_PlayerName(void) {
    if (!story.player_name[0]) {
        copy_bounded(story.player_name, sizeof(story.player_name),
                     Settings_GetStr("PlayerName", ""));
    }
    return story.player_name;
}

void Story_SetPlayerName(const char *name) {
    copy_bounded(story.player_name, sizeof(story.player_name), name);
    Settings_SetStr("PlayerName", story.player_name);
}

/* ── Launching ───────────────────────────────────────────────────────── */

/* Load one mission. Each player takes the side its PlayerN line names
 * (legacy:169026-169095), applied as the game starts with nothing
 * turned back (legacy:177703-177749, legacy:206137). A line that names
 * no side leaves the player's default. */
static int story_begin(TAK_Platform *platform, const char *mission_file) {
    StoryMission m;
    memset(&m, 0, sizeof(m));
    copy_bounded(m.file, sizeof(m.file), mission_file);
    file_to_stem(m.stem, sizeof(m.stem), mission_file);

    BattleConfig cfg;
    BattleConfig_SetDefaults(&cfg);
    copy_bounded(cfg.map_name, sizeof(cfg.map_name), m.stem);

    char kingdom[32];
    kingdom[0] = '\0';
    MissionData mission;
    char path[160];
    snprintf(path, sizeof(path), "missions/missions/%s", m.file);
    if (Mission_LoadOTA(path, &mission) == 0) {
        lower_copy(kingdom, sizeof(kingdom), mission.kingdom);
        for (int p = 1; p <= TAK_MAX_PLAYERS && p < TAK_MISSION_PLAYER_LINES; p++) {
            int side = Sides_FindInText(mission.player_lines[p]);
            if (side >= 0)
                cfg.players[p - 1].side = Sides_Set(side, TAK_SIDES_CAMPAIGN, 0);
        }
        Mission_Free(&mission);
    }
    if (!kingdom[0]) copy_bounded(kingdom, sizeof(kingdom), "aramon");

    if (World_BeginLoad(platform, &cfg, m.stem, kingdom) != 0) {
        fprintf(stderr, "Story: failed to begin mission %s\n", m.stem);
        return GAMESTATE_CAMPAIGN;
    }
    fprintf(stderr, "Story: launching %s (%s) as side %d\n", m.stem,
            kingdom, cfg.players[0].side);
    return GAMESTATE_GAME_LOADING;
}

int Story_StartMission(TAK_Platform *platform, int mission_index) {
    StoryCampaign *c = current_campaign();
    if (!c) {
        fprintf(stderr, "Story: no campaign missions available\n");
        return GAMESTATE_CAMPAIGN;
    }
    if (mission_index < 0 || mission_index >= c->mission_count) {
        return GAMESTATE_CAMPAIGN;
    }
    return story_begin(platform, c->missions[mission_index].file);
}

int Story_StartMissionFile(TAK_Platform *platform, const char *mission_file) {
    if (!mission_file || !mission_file[0]) return GAMESTATE_CAMPAIGN;
    return story_begin(platform, mission_file);
}

int Story_StartChapter(TAK_Platform *platform, int shift_held) {
    const StoryMission *m = current_mission();
    if (!m) return GAMESTATE_CAMPAIGN;
    if (shift_held && tak_stricmp(m->name, STORY_HIDDEN_FROM) == 0) {
        return story_begin(platform, STORY_HIDDEN_TO);
    }
    return story_begin(platform, m->file);
}

/* ── Change User ─────────────────────────────────────────────────────── */

static int chooser_row_height(StoryChooser *ch) {
    const GUIWidget *t = GUIDialog_FindByName(&ch->dialog, "ListCampaignTemplate");
    return (t && t->rect.h > 0) ? t->rect.h : 19;
}

static int chooser_rows_visible(StoryChooser *ch) {
    if (ch->idx_list < 0) return 0;
    int h = chooser_row_height(ch);
    SDL_Rect r = ch->dialog.children[ch->idx_list].rect;
    return (h > 0 && r.h > 0) ? r.h / h : 1;
}

static void chooser_clamp(StoryChooser *ch) {
    int m = story.camp_count - chooser_rows_visible(ch);
    if (m < 0) m = 0;
    if (ch->scroll > m) ch->scroll = m;
    if (ch->scroll < 0) ch->scroll = 0;
}

static void chooser_close(StoryChooser *ch) {
    if (ch->rt) GUIRuntime_Destroy(ch->rt);
    if (ch->font) Font_Free(ch->font);
    if (ch->has_dialog) GUIDialog_Free(&ch->dialog);
    memset(ch, 0, sizeof(*ch));
    ch->idx_list = ch->idx_inc = ch->idx_dec = -1;
}

/* One book opens the plain player dialog, more than one the combined
 * player and campaign chooser (legacy:142640, legacy:143946-143970). */
static void chooser_open(StoryChooser *ch, int with_campaigns) {
    chooser_close(ch);
    const char *file = with_campaigns ? "data/guis/playercampaigndialogue.gui"
                                      : "data/guis/playerdialogue.gui";
    if (GUIDialog_Load(&ch->dialog, file) != 0) {
        fprintf(stderr, "Story: failed to load %s\n", file);
        return;
    }
    ch->has_dialog = 1;
    ch->rt = GUIRuntime_Create(&ch->dialog);
    if (!ch->rt) { GUIDialog_Free(&ch->dialog); ch->has_dialog = 0; return; }
    load_translate();
    Translate_Dialog(&story.tt, &ch->dialog);
    ch->font = Font_Load("data/fonts/b_times new roman (100b)", UI_RGBAFormat());
    for (int i = 0; i < ch->dialog.num_children; i++) {
        const GUIWidget *w = &ch->dialog.children[i];
        if (tak_stricmp(w->name, "CampaignList") == 0) ch->idx_list = i;
    }
    if (ch->idx_list >= 0) {
        SDL_Rect lr = ch->dialog.children[ch->idx_list].rect;
        for (int i = 0; i < ch->dialog.num_children; i++) {
            const GUIWidget *w = &ch->dialog.children[i];
            int beside = w->rect.x >= lr.x + lr.w - 40 &&
                         w->rect.x < lr.x + lr.w + 40 &&
                         w->rect.y >= lr.y - 4 && w->rect.y < lr.y + lr.h + 4;
            if (!beside) continue;
            if (tak_stricmp(w->name, "incbutton") == 0) ch->idx_inc = i;
            if (tak_stricmp(w->name, "decbutton") == 0) ch->idx_dec = i;
        }
    }
    /* The row templates are art the list draws over. */
    GUIRuntime_SetWidgetVisible(ch->rt, "ListCampaignTemplate", 0);
    GUIRuntime_SetWidgetVisible(ch->rt, "CampaignName", 0);
    GUIRuntime_SetWidgetVisible(ch->rt, "ListNameTemplate", 0);
    GUIRuntime_SetWidgetVisible(ch->rt, "PlayerName", 0);
    copy_bounded(ch->name, sizeof(ch->name), Story_PlayerName());
    GUIRuntime_SetWidgetText(ch->rt, "PlayerEditName", ch->name);
    ch->sel = story.camp;
    ch->open = 1;
}

static void chooser_press(StoryChooser *ch, const char *name) {
    if (tak_stricmp(name, "OK") == 0) {
        Story_SetPlayerName(ch->name);
        if (ch->idx_list >= 0) Story_SelectCampaign(ch->sel);
        /* Accepting the dialog is what the original remembers the
         * choice from (legacy:143142), and the book it opens is the
         * chosen player's, so their progress is re-read here. */
        if (story.camp >= 0 && story.camp < story.camp_count)
            Settings_SetStr(STORY_SETTING_BOOK, story.camps[story.camp].file);
        read_progress();
        Settings_Save();
        chooser_close(ch);
    } else if (tak_stricmp(name, "Cancel") == 0) {
        chooser_close(ch);
    }
}

static void chooser_type(StoryChooser *ch, TAK_Platform *platform) {
    if (!platform) return;
    size_t n = strlen(ch->name);
    if (platform->pressed_backspace && n > 0) ch->name[n - 1] = '\0';
    for (int i = 0; i < platform->text_in_len; i++) {
        n = strlen(ch->name);
        if (n + 1 >= sizeof(ch->name)) break;
        ch->name[n] = platform->text_in[i];
        ch->name[n + 1] = '\0';
    }
    GUIRuntime_SetWidgetText(ch->rt, "PlayerEditName", ch->name);
}

static void chooser_draw_rows(StoryChooser *ch, SDL_Surface *off) {
    if (ch->idx_list < 0 || !ch->font || !off) return;
    SDL_Rect lr = ch->dialog.children[ch->idx_list].rect;
    int h = chooser_row_height(ch);
    int vis = chooser_rows_visible(ch);
    for (int i = 0; i < vis; i++) {
        int row = ch->scroll + i;
        if (row >= story.camp_count) break;
        SDL_Rect r = { lr.x, lr.y + i * h, lr.w, h };
        if (row == ch->sel) {
            SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 90, 70, 40, 255));
        }
        Font_DrawString(ch->font, off, r.x + 10, r.y + 2, story.camps[row].name);
    }
}

static void chooser_tick(StoryChooser *ch, TAK_Platform *platform) {
    int mx = -1, my = -1, mouse_down = 0;
    if (platform && platform->has_focus)
        mouse_down = TAK_Platform_MouseThisFrame(platform, &mx, &my);

    /* The edge, not the key state: a held escape would otherwise close
     * this and leave the screen behind it on the next frame. */
    if (platform && platform->pressed_escape) { chooser_close(ch); return; }
    chooser_type(ch, platform);

    if (mouse_down && !ch->prev_mouse && ch->idx_list >= 0 && mx >= 0) {
        SDL_Rect lr = ch->dialog.children[ch->idx_list].rect;
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &lr)) {
            int row = ch->scroll + (my - lr.y) / chooser_row_height(ch);
            if (row >= 0 && row < story.camp_count) ch->sel = row;
        }
    }
    ch->prev_mouse = mouse_down;

    char clicked[64];
    int clicked_idx = -1;
    clicked[0] = '\0';
    (void)GUIRuntime_UpdateEx(ch->rt, mx, my, mouse_down, clicked,
                              sizeof(clicked), &clicked_idx);
    if (clicked[0]) {
        if (clicked_idx == ch->idx_inc)      { ch->scroll--; chooser_clamp(ch); }
        else if (clicked_idx == ch->idx_dec) { ch->scroll++; chooser_clamp(ch); }
        else chooser_press(ch, clicked);
        if (!ch->open) return;
    }

    GUIRuntime_Render(story.rt);
    GUIRuntime_Render(ch->rt);
    chooser_draw_rows(ch, UI_Offscreen());
    UI_Present(platform);
}

/* ── The screen ──────────────────────────────────────────────────────── */

static void update_story_labels(void) {
    if (!story.rt) return;
    StoryCampaign *c = current_campaign();
    if (!c) return;
    if (story.selected < 0) story.selected = 0;
    if (story.selected >= c->mission_count) story.selected = c->mission_count - 1;

    char chapter[32];
    snprintf(chapter, sizeof(chapter), "%d", story.selected + 1);
    /* "Book of" is authored beside it, and the name is the player's,
     * not the campaign's (legacy:144267). */
    GUIRuntime_SetWidgetText(story.rt, "BookName", Story_PlayerName());
    GUIRuntime_SetWidgetText(story.rt, "ChapterNumber", chapter);
    GUIRuntime_SetWidgetTextWrapped(story.rt, "ChapterText", Story_ChapterText());

    /* The page turners are dead where there is nowhere to turn: the
     * original parks one on frame 0 and puts it on frame 2 when the
     * page it points at exists (legacy:144085-144133). */
    int can_go_back = story.selected > 0;
    int can_go_on   = story.selected < c->high_water &&
                      story.selected + 1 < c->mission_count;
    GUIRuntime_SetFrameOverride(story.rt, "PreviousPage", can_go_back ? -1 : 0);
    GUIRuntime_SetFrameOverride(story.rt, "NextPage", can_go_on ? -1 : 0);
}

int Story_Init(TAK_Platform *platform) {
    (void)platform;
    Story_Shutdown();
    load_campaigns();
    if (story.camp_count <= 0) {
        fprintf(stderr, "Story: no campaigns found under camps/\n");
        return -1;
    }
    if (GUIDialog_Load(&story.dialog, "data/guis/bod.gui") != 0) {
        fprintf(stderr, "Story: failed to parse bod.gui\n");
        return -1;
    }
    story.rt = GUIRuntime_Create(&story.dialog);
    if (!story.rt) {
        GUIDialog_Free(&story.dialog);
        return -1;
    }
    load_translate();
    Translate_Dialog(&story.tt, &story.dialog);
    /* The art carries more frames than a widget can, so the screen
     * draws it and the widget only says where. */
    GUIRuntime_SetWidgetVisible(story.rt, "ChapterImage", 0);
    /* bod.gui authors the word Tooltip into the help strip
     * (bod.gui:114-119). The original fills the strip with the help
     * text of whatever the pointer is over and leaves it empty
     * otherwise, the way the main menu and the lobby do. */
    GUIRuntime_SetWidgetText(story.rt, "HelpText", "");
    story.tooltip_font = Font_Load("data/fonts/b_times new roman (100b)",
                                   UI_RGBAFormat());
    /* With more than one book the button changes the campaign too, and
     * the help strip says so (legacy:143798-143805). */
    if (story.camp_count > 1) {
        const char *help = Translate_Find(&story.tt,
                                          "CHANGE_USER_CAMPAIGN_BUTTON_HELP");
        for (int i = 0; help && i < story.dialog.num_children; i++) {
            if (tak_stricmp(story.dialog.children[i].name, "ChangeUser") == 0) {
                copy_bounded(story.dialog.children[i].tooltip,
                             sizeof(story.dialog.children[i].tooltip), help);
            }
        }
    }
    story.cheat[0] = '\0';
    story.initialized = 1;
    SDL_StartTextInput();
    update_story_labels();
    return 0;
}

GUIRuntime *Story_Runtime(void) { return story.initialized ? story.rt : NULL; }

static int story_click(TAK_Platform *platform, const char *clicked,
                       int shift_held) {
    if (tak_stricmp(clicked, "Play") == 0) {
        return Story_StartChapter(platform, shift_held);
    }
    if (tak_stricmp(clicked, "NextPage") == 0) {
        Story_SelectChapter(story.selected + 1);
        return GAMESTATE_CAMPAIGN;
    }
    if (tak_stricmp(clicked, "PreviousPage") == 0) {
        Story_SelectChapter(story.selected - 1);
        return GAMESTATE_CAMPAIGN;
    }
    if (tak_stricmp(clicked, "LoadGame") == 0) {
        if (SaveBrowser_Open(SAVEBROWSER_LOAD) == 0) story.browser_open = 1;
        return GAMESTATE_CAMPAIGN;
    }
    if (tak_stricmp(clicked, "ChangeUser") == 0) {
        chooser_open(&story.chooser, story.camp_count > 1);
        return GAMESTATE_CAMPAIGN;
    }
    if (tak_stricmp(clicked, "Previous") == 0 ||
        tak_stricmp(clicked, "Cancel") == 0 ||
        tak_stricmp(clicked, "Return") == 0) {
        return GAMESTATE_MENU;
    }
    fprintf(stderr, "Story: unhandled click '%s'\n", clicked);
    return GAMESTATE_CAMPAIGN;
}

/* A chosen save takes the same two lines Play takes: bring up the
 * battle the save names, then let the loading screen apply it. */
static int take_browser_result(SaveBrowserResult r, TAK_Platform *platform) {
    if (r == SAVEBROWSER_OPEN) return GAMESTATE_CAMPAIGN;
    if (r == SAVEBROWSER_LOAD_READY) {
        TAK_SaveGame *sg = SaveBrowser_TakeLoad();
        const TAK_SaveInfo *info = sg ? Save_Info(sg) : NULL;
        if (info && World_BeginLoad(platform, &info->cfg, info->map_name,
                                    info->map_kingdom) == 0) {
            World_SetRestoring(1);
            Loading_SetPendingSave(sg);
            SaveBrowser_Close();
            story.browser_open = 0;
            return GAMESTATE_GAME_LOADING;
        }
        if (sg) Save_ReadClose(sg);
        fprintf(stderr, "Story: could not begin the saved battle\n");
    }
    SaveBrowser_Close();
    story.browser_open = 0;
    return GAMESTATE_CAMPAIGN;
}

int Story_Tick(TAK_Platform *platform, float frame_dt) {
    (void)frame_dt;
    if (!story.initialized) return GAMESTATE_MENU;

    /* The load dialog and the chooser draw over this screen and own the
     * frame while they are up, the way the F1 menu owns Options. */
    if (story.browser_open) {
        return take_browser_result(SaveBrowser_Tick(platform), platform);
    }
    if (story.chooser.open) {
        chooser_tick(&story.chooser, platform);
        update_story_labels();
        return GAMESTATE_CAMPAIGN;
    }

    int wx = 0, wy = 0, mx = -1, my = -1;
    SDL_GetMouseState(&wx, &wy);
    if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
        mx = -1;
        my = -1;
    }
    int mouse_down = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT);

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int shift_held = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    if (platform) Story_TypeText(platform->text_in);

    char clicked[64];
    int next = GAMESTATE_CAMPAIGN;
    if (GUIRuntime_Update(story.rt, mx, my, mouse_down,
                          clicked, sizeof(clicked))) {
        next = story_click(platform, clicked, shift_held);
    }

    if (keys[SDL_SCANCODE_ESCAPE]) next = GAMESTATE_MENU;
    if (keys[SDL_SCANCODE_RETURN]) next = Story_StartChapter(platform, shift_held);

    update_story_labels();

    SDL_Surface *off = UI_Offscreen();
    SDL_Rect full = { 0, 0, 640, 480 };
    SDL_FillRect(off, &full, SDL_MapRGBA(off->format, 12, 12, 18, 255));
    GUIRuntime_Render(story.rt);
    draw_chapter_art(off);

    if (story.tooltip_font) {
        const GUIWidget *hw = GUIRuntime_HoveredWidget(story.rt);
        if (hw && hw->tooltip[0]) {
            const GUIWidget *help = GUIDialog_FindByName(&story.dialog, "HelpText");
            SDL_Rect r = help ? help->rect : (SDL_Rect){ 208, 452, 224, 30 };
            int tw = Font_MeasureString(story.tooltip_font, hw->tooltip);
            Font_DrawString(story.tooltip_font, off,
                            r.x + (r.w - tw) / 2, r.y + 4, hw->tooltip);
        }
    }

    UI_Present(platform);
    return next;
}

void Story_Shutdown(void) {
    chooser_close(&story.chooser);
    if (story.browser_open) SaveBrowser_Close();
    story.browser_open = 0;
    if (story.initialized) {
        if (story.rt) GUIRuntime_Destroy(story.rt);
        if (story.tooltip_font) Font_Free(story.tooltip_font);
        GUIDialog_Free(&story.dialog);
        SDL_StopTextInput();
    }
    story.rt = NULL;
    story.tooltip_font = NULL;
    story.initialized = 0;
    tak_free(story.art_px);
    story.art_px = NULL;
    story.art_px_frame = -1;
    if (story.art) GAF_Close(story.art);
    story.art = NULL;
    story.art_tried = 0;
    story.art_frames = 0;
    story.art_entry = -1;
}
