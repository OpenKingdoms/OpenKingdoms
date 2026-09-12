/*
 * story.c -- Story / campaign screen (GAMESTATE_CAMPAIGN).
 *
 * The original "Book of Deeds" is a campaign launcher over
 * data/camps/book of darien.tdf. This port keeps the legacy bod.gui
 * shell, overlays the active mission metadata, and routes Play into the
 * normal World_BeginLoad -> GAMESTATE_GAME_LOADING path.
 */

#include "tak_story.h"
#include "tak_battle_config.h"
#include "tak_blit.h"
#include "tak_font.h"
#include "tak_gameloop.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_mission.h"
#include "tak_sides.h"
#include "tak_tdf.h"
#include "tak_ui.h"
#include "tak_util.h"
#include "tak_world.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define STORY_MAX_MISSIONS 64

typedef struct StoryMission {
    char file[80];
    char name[80];
    char stem[80];
    char kingdom[32];
} StoryMission;

static struct {
    int initialized;
    GUIDialog dialog;
    GUIRuntime *rt;
    Font *tooltip_font;
    StoryMission missions[STORY_MAX_MISSIONS];
    int mission_count;
    int selected;
} story;

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

static void resolve_mission_kingdom(StoryMission *m) {
    MissionData mission;
    char path[160];
    if (!m) return;
    m->kingdom[0] = '\0';
    snprintf(path, sizeof(path), "missions/missions/%s", m->file);
    if (Mission_LoadOTA(path, &mission) == 0) {
        lower_copy(m->kingdom, sizeof(m->kingdom), mission.kingdom);
        Mission_Free(&mission);
    }
    if (!m->kingdom[0]) {
        copy_bounded(m->kingdom, sizeof(m->kingdom), "aramon");
    }
}

static int load_campaign_index(void) {
    TDFFile *tdf = TDF_Open("data/camps/book of darien.tdf");
    if (!tdf || TDF_Load(tdf) != 0) {
        if (tdf) TDF_Close(tdf);
        return -1;
    }

    story.mission_count = 0;
    for (int i = 0; i < STORY_MAX_MISSIONS; i++) {
        char section[32];
        snprintf(section, sizeof(section), "MISSION%d", i);
        if (TDF_PushSection(tdf, section) != 0) break;

        const char *file = TDF_ReadString(tdf, "missionfile", "");
        const char *name = TDF_ReadString(tdf, "missionname", "");
        if (file && file[0] && name && name[0]) {
            StoryMission *m = &story.missions[story.mission_count++];
            copy_bounded(m->file, sizeof(m->file), file);
            copy_bounded(m->name, sizeof(m->name), name);
            file_to_stem(m->stem, sizeof(m->stem), file);
            resolve_mission_kingdom(m);
        }
        TDF_PopSection(tdf);
    }

    TDF_Close(tdf);
    return story.mission_count > 0 ? 0 : -1;
}

static void update_story_labels(void) {
    if (!story.rt || story.mission_count <= 0) return;
    if (story.selected < 0) story.selected = 0;
    if (story.selected >= story.mission_count) {
        story.selected = story.mission_count - 1;
    }

    const StoryMission *m = &story.missions[story.selected];
    char chapter[32];
    char text[160];
    snprintf(chapter, sizeof(chapter), "%d", story.selected + 1);
    snprintf(text, sizeof(text), "%s", m->name);
    GUIRuntime_SetWidgetText(story.rt, "BookName", "Book of Darien");
    GUIRuntime_SetWidgetText(story.rt, "ChapterNumber", chapter);
    GUIRuntime_SetWidgetText(story.rt, "ChapterText", text);
}

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

    MissionData mission;
    char path[160];
    snprintf(path, sizeof(path), "missions/missions/%s", m.file);
    if (Mission_LoadOTA(path, &mission) == 0) {
        lower_copy(m.kingdom, sizeof(m.kingdom), mission.kingdom);
        for (int p = 1; p <= TAK_MAX_PLAYERS && p < TAK_MISSION_PLAYER_LINES; p++) {
            int side = Sides_FindInText(mission.player_lines[p]);
            if (side >= 0)
                cfg.players[p - 1].side = Sides_Set(side, TAK_SIDES_CAMPAIGN, 0);
        }
        Mission_Free(&mission);
    }
    if (!m.kingdom[0]) copy_bounded(m.kingdom, sizeof(m.kingdom), "aramon");

    if (World_BeginLoad(platform, &cfg, m.stem, m.kingdom) != 0) {
        fprintf(stderr, "Story: failed to begin mission %s\n", m.stem);
        return GAMESTATE_CAMPAIGN;
    }
    fprintf(stderr, "Story: launching %s (%s) as side %d\n", m.stem,
            m.kingdom, cfg.players[0].side);
    return GAMESTATE_GAME_LOADING;
}

int Story_StartMission(TAK_Platform *platform, int mission_index) {
    if (story.mission_count <= 0 && load_campaign_index() != 0) {
        fprintf(stderr, "Story: no campaign missions available\n");
        return GAMESTATE_CAMPAIGN;
    }
    if (mission_index < 0 || mission_index >= story.mission_count) {
        return GAMESTATE_CAMPAIGN;
    }

    return story_begin(platform, story.missions[mission_index].file);
}

int Story_StartMissionFile(TAK_Platform *platform, const char *mission_file) {
    if (!mission_file || !mission_file[0]) return GAMESTATE_CAMPAIGN;
    return story_begin(platform, mission_file);
}

int Story_Init(TAK_Platform *platform) {
    (void)platform;
    memset(&story, 0, sizeof(story));
    if (load_campaign_index() != 0) {
        fprintf(stderr, "Story: failed to load Book of Darien index\n");
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
    story.tooltip_font = Font_Load("data/fonts/b_times new roman (100b)",
                                   UI_RGBAFormat());
    story.initialized = 1;
    update_story_labels();
    return 0;
}

int Story_Tick(TAK_Platform *platform, float frame_dt) {
    (void)frame_dt;
    if (!story.initialized) return GAMESTATE_MENU;

    int wx = 0, wy = 0, mx = -1, my = -1;
    SDL_GetMouseState(&wx, &wy);
    if (!TAK_Platform_MapMouseToCanvas(platform, wx, wy, &mx, &my)) {
        mx = -1;
        my = -1;
    }
    int mouse_down = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT);

    char clicked[64];
    int next = GAMESTATE_CAMPAIGN;
    if (GUIRuntime_Update(story.rt, mx, my, mouse_down,
                          clicked, sizeof(clicked))) {
        if (tak_stricmp(clicked, "Play") == 0) {
            next = Story_StartMission(platform, story.selected);
        } else if (tak_stricmp(clicked, "NextPage") == 0) {
            if (story.selected + 1 < story.mission_count) story.selected++;
        } else if (tak_stricmp(clicked, "PreviousPage") == 0) {
            if (story.selected > 0) story.selected--;
        } else if (tak_stricmp(clicked, "Previous") == 0 ||
                   tak_stricmp(clicked, "Cancel") == 0 ||
                   tak_stricmp(clicked, "Return") == 0) {
            next = GAMESTATE_MENU;
        } else {
            fprintf(stderr, "Story: unhandled click '%s'\n", clicked);
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_ESCAPE]) next = GAMESTATE_MENU;
    if (keys[SDL_SCANCODE_RETURN]) next = Story_StartMission(platform, story.selected);

    update_story_labels();

    SDL_Surface *off = UI_Offscreen();
    SDL_Rect full = { 0, 0, 640, 480 };
    SDL_FillRect(off, &full, SDL_MapRGBA(off->format, 12, 12, 18, 255));
    GUIRuntime_Render(story.rt);

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
    if (!story.initialized) return;
    if (story.rt) GUIRuntime_Destroy(story.rt);
    if (story.tooltip_font) Font_Free(story.tooltip_font);
    GUIDialog_Free(&story.dialog);
    memset(&story, 0, sizeof(story));
}
