/*
 * save_browser.c -- the original's save and load dialogs.
 *
 * data/guis/savegame.gui and data/guis/loadgame.gui, picked by which
 * one the caller asked for (legacy:158806-158813). Both are 486x344
 * and share a GameList list box, a ScrollBars.gaf scrollbar, a
 * GameNameTemplate row, a RadarView panel, Side/Map/GameTime labels, a
 * DeleteGame button, Cancel and HelpText. The save file adds a
 * GameName edit box and an OKButton named SaveGame; the load file adds
 * a LoadGame button and a taller list, because it needs no name field.
 *
 * The list is a plain directory enumeration with the extension stripped
 * (legacy:159073-159085). Clicking a row in the save dialog copies its
 * name into the edit box, which is how the original offers to overwrite
 * (legacy:159300-159356), and OK writes straight over any existing file:
 * there is no overwrite confirmation string anywhere in the shipped
 * data (legacy:159247-159292). Delete deletes at once, with no
 * confirmation either (legacy:159241-159244).
 *
 * Everything that can go wrong goes through data/guis/ok.gui with the
 * message the container or english/translate/messages.tdf gives, so a
 * player is told what to do rather than watching nothing happen.
 */

#include "tak_save_browser.h"
#include "tak_dataset.h"

#include "tak_blit.h"
#include "tak_font.h"
#include "tak_game_sound.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_memory.h"
#include "tak_message_box.h"
#include "tak_minimap.h"
#include "tak_paths.h"
#include "tak_savegame.h"
#include "tak_savelist.h"
#include "tak_translate.h"
#include "tak_ui.h"
#include "tak_util.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB_NAME_MAX 48   /* what one row of the list can show */

static struct {
    int              open;
    SaveBrowserMode  mode;

    GUIDialog        dialog;
    int              has_dialog;
    GUIRuntime      *rt;
    char             path[128];
    char             enter_widget[32];
    char             esc_widget[32];

    /* A refusal that arrived before there was anything to show closes
     * the whole dialog once it is read (legacy:158733-158758). */
    int              message_closes;

    /* The saved games have been asked for and have not all arrived.
     * Only a browser is ever in this state. */
    int              waiting;

    Font            *font_row;
    Font            *font_help;

    TAK_SaveEntry   *rows;
    int              row_count;
    int              selected;
    int              scroll;

    char             name[SB_NAME_MAX + 1];

    int              idx_list;
    int              idx_track;
    int              idx_thumb;
    int              idx_inc;
    int              idx_dec;
    /* The load dialog's picture panel, and the selected save's picture
     * decoded into it. Not idx_thumb above, which is the scrollbar's
     * "sbutton" and a different thing entirely. */
    int              idx_radar;
    uint32_t        *radar_px;
    int              radar_w, radar_h;
    int              dragging;
    int              grab_dy;

    int              prev_enter;
    int              prev_esc;
    int              prev_back;
    int              prev_mouse;
    int              prev_click_row;
    uint32_t         prev_click_ms;

    TAK_SaveGame    *loaded;
    char             last_sound[64];
} sb;

static void set_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

/* -- dialog plumbing ----------------------------------------------- */

/* Root tooltip "#Enter#SaveGame#Esc#Cancel": key, widget pairs. */
static void parse_accelerators(const char *spec) {
    sb.enter_widget[0] = sb.esc_widget[0] = '\0';
    if (!spec) return;
    char buf[128];
    set_text(buf, sizeof(buf), spec);
    char *p = buf;
    while (*p == '#') p++;
    while (*p) {
        char *key = p;
        char *sep = strchr(key, '#');
        if (!sep) break;
        *sep = '\0';
        char *widget = sep + 1;
        char *next = strchr(widget, '#');
        if (next) *next = '\0';
        if (tak_stricmp(key, "Enter") == 0)
            set_text(sb.enter_widget, sizeof(sb.enter_widget), widget);
        if (tak_stricmp(key, "Esc") == 0)
            set_text(sb.esc_widget, sizeof(sb.esc_widget), widget);
        if (!next) break;
        p = next + 1;
    }
}

static void free_message(void) {
    MessageBox_Close();
    sb.message_closes = 0;
}

/* Put a message in front of the dialog, in the shipped one button
 * box data/guis/ok.gui. */
static void show_message(const char *text, int closes) {
    (void)MessageBox_Open(text);
    sb.message_closes = closes;
}

/* The translated message for a key, shown in the box. */
static void show_message_key(const char *key, int closes) {
    char text[256];
    Translate_Message(key, text, sizeof(text));
    show_message(text, closes);
}

static const char *dialog_file(SaveBrowserMode mode) {
    return mode == SAVEBROWSER_LOAD ? "data/guis/loadgame.gui"
                                    : "data/guis/savegame.gui";
}

/* The scrollbar parts carry the art's generic names, and each file
 * authors exactly one set, so the name is enough here. */
static void cache_indices(void) {
    sb.idx_list = sb.idx_track = sb.idx_thumb = sb.idx_inc = sb.idx_dec = -1;
    sb.idx_radar = -1;
    for (int i = 0; i < sb.dialog.num_children; i++) {
        const GUIWidget *w = &sb.dialog.children[i];
        if (tak_stricmp(w->name, "GameList")  == 0) sb.idx_list  = i;
        if (tak_stricmp(w->name, "slider")    == 0) sb.idx_track = i;
        if (tak_stricmp(w->name, "sbutton")   == 0) sb.idx_thumb = i;
        if (tak_stricmp(w->name, "incbutton") == 0) sb.idx_inc   = i;
        if (tak_stricmp(w->name, "decbutton") == 0) sb.idx_dec   = i;
        if (tak_stricmp(w->name, "RadarView")  == 0) sb.idx_radar = i;
    }
}

/* -- list geometry ------------------------------------------------- */

/* A row is one GameNameTemplate tall: the original clones that template
 * once per file (legacy:159073-159085). */
static int row_height(void) {
    const GUIWidget *t = GUIDialog_FindByName(&sb.dialog, "GameNameTemplate");
    return (t && t->rect.h > 0) ? t->rect.h : 19;
}

/* The GameList rect, trimmed at the scrollbar so the dialogue art
 * stays visible beside the rows. */
static SDL_Rect list_rect(void) {
    SDL_Rect r = { 58, 152, 179, 129 };
    if (sb.idx_list >= 0) r = sb.dialog.children[sb.idx_list].rect;
    if (sb.idx_track >= 0) {
        const GUIWidget *t = &sb.dialog.children[sb.idx_track];
        if (t->rect.x > r.x) r.w = t->rect.x - r.x;
    }
    return r;
}

static int rows_visible(void) {
    int n = list_rect().h / row_height();
    return n > 0 ? n : 1;
}

static int max_scroll(void) {
    int m = sb.row_count - rows_visible();
    return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
    int m = max_scroll();
    if (sb.scroll > m) sb.scroll = m;
    if (sb.scroll < 0)  sb.scroll = 0;
}

static void widget_draw_rect(int index, SDL_Rect *out) {
    out->x = out->y = 0;
    out->w = out->h = 1;
    if (index < 0 || index >= sb.dialog.num_children) return;
    if (GUIRuntime_WidgetDrawRect(sb.rt, index, out) != 0)
        *out = sb.dialog.children[index].rect;
}

static void thumb_travel(SDL_Rect *track, int *thumb_h) {
    SDL_Rect thumb;
    widget_draw_rect(sb.idx_track, track);
    widget_draw_rect(sb.idx_thumb, &thumb);
    *thumb_h = thumb.h > 0 ? thumb.h : 1;
}

static void sync_thumb(void) {
    if (sb.idx_thumb < 0 || sb.idx_track < 0) return;
    if (sb.dragging) return;
    SDL_Rect track, thumb;
    int th = 1;
    thumb_travel(&track, &th);
    widget_draw_rect(sb.idx_thumb, &thumb);
    int travel = track.h - th;
    if (travel < 0) travel = 0;
    int m = max_scroll();
    GUIWidget *w = &sb.dialog.children[sb.idx_thumb];
    int rel = m > 0 ? (travel * sb.scroll) / m : 0;
    w->rect.y = track.y + rel + (w->rect.y - thumb.y);
}

/* -- the list itself ----------------------------------------------- */

/* The selected save's picture, decoded once per selection change
 * rather than per frame: it means opening the file, and the list can
 * be walked with the arrow keys. A save that carries none, or one that
 * will not open, leaves the panel as the art authored it. */
static void sync_radar(void) {
    tak_free(sb.radar_px);
    sb.radar_px = NULL;
    sb.radar_w = sb.radar_h = 0;
    if (sb.idx_radar < 0) return;
    if (sb.selected < 0 || sb.selected >= sb.row_count) return;
    if (!sb.rows[sb.selected].readable) return;

    char err[TAK_SAVE_ERR_MAX];
    TAK_SaveGame *sg = Save_Read(sb.rows[sb.selected].path, err, sizeof err);
    if (!sg) return;
    int w = 0, h = 0;
    const uint8_t *rgb = Save_Thumbnail(sg, &w, &h);
    if (rgb && w > 0 && h > 0) {
        uint32_t *px = (uint32_t *)tak_malloc((size_t)w * (size_t)h * sizeof *px);
        if (px) {
            SDL_PixelFormat *fmt = UI_RGBAFormat();
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                px[i] = SDL_MapRGBA(fmt, rgb[i * 3], rgb[i * 3 + 1],
                                    rgb[i * 3 + 2], 255);
            }
            sb.radar_px = px;
            sb.radar_w = w;
            sb.radar_h = h;
        }
    }
    Save_ReadClose(sg);
}

/* Over the panel art, at the panel's own size. */
static void draw_radar(void) {
    if (!sb.radar_px || sb.idx_radar < 0) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    SDL_Rect r;
    widget_draw_rect(sb.idx_radar, &r);
    if (r.w <= 0 || r.h <= 0) return;
    Blit_RGBA_Scaled(off, r, sb.radar_px, sb.radar_w, sb.radar_h);
}

int SaveBrowser_RadarViewRect(SDL_Rect *out) {
    if (!out || !sb.open || !sb.rt || sb.idx_radar < 0) return 0;
    widget_draw_rect(sb.idx_radar, out);
    return (out->w > 0 && out->h > 0);
}

static void sync_details(void) {
    const char *side = "";
    const char *map  = "";
    const char *time = "";
    if (sb.selected >= 0 && sb.selected < sb.row_count) {
        const TAK_SaveEntry *e = &sb.rows[sb.selected];
        side = e->side;
        map  = e->map;
        time = e->game_time;
    }
    if (!sb.rt) return;
    GUIRuntime_SetWidgetText(sb.rt, "Side", side);
    GUIRuntime_SetWidgetText(sb.rt, "Map", map);
    GUIRuntime_SetWidgetText(sb.rt, "GameTime", time);
    /* The row template carries the art placeholder text, never a row. */
    GUIRuntime_SetWidgetText(sb.rt, "FileName", "");
    sync_radar();
}

void SaveBrowser_SelectRow(int row) {
    if (row < 0 || row >= sb.row_count) return;
    sb.selected = row;
    /* Clicking a row in the save dialog copies the name into the edit
     * box, which is how the original offers to overwrite
     * (legacy:159300-159356). */
    if (sb.mode == SAVEBROWSER_SAVE)
        set_text(sb.name, sizeof(sb.name), sb.rows[row].slug);
    sync_details();
}

static void rescan(void) {
    SaveList_Free(sb.rows);
    sb.rows = NULL;
    sb.row_count = SaveList_Scan(&sb.rows);
    if (sb.row_count < 0) sb.row_count = 0;
    if (sb.selected >= sb.row_count) sb.selected = sb.row_count - 1;
    if (sb.row_count == 0) sb.selected = -1;
    clamp_scroll();
    sync_details();
}

/* The list, once the saved games are in hand. Split out of Open
 * because in a browser they arrive a few frames later. */
static void finish_open(void) {
    GUIRuntime_SetWidgetText(sb.rt, "HelpText", "");
    rescan();
    if (sb.row_count > 0) SaveBrowser_SelectRow(0);

    /* The load dialog opened over an empty directory says so and goes
     * (legacy:158733-158758 and again at legacy:159086-159100). */
    if (sb.mode == SAVEBROWSER_LOAD && sb.row_count == 0)
        show_message_key("NO_SAVED_GAMES", 1);
}

/* -- open and close ------------------------------------------------ */

int SaveBrowser_Open(SaveBrowserMode mode) {
    SaveBrowser_Close();
    memset(&sb, 0, sizeof(sb));
    sb.selected = -1;
    sb.mode = mode;

    const char *file = dialog_file(mode);
    if (GUIDialog_Load(&sb.dialog, file) != 0) {
        fprintf(stderr, "SaveBrowser: failed to load %s\n", file);
        return -1;
    }
    sb.has_dialog = 1;
    sb.rt = GUIRuntime_Create(&sb.dialog);
    if (!sb.rt) { GUIDialog_Free(&sb.dialog); sb.has_dialog = 0; return -1; }
    set_text(sb.path, sizeof(sb.path), file);
    parse_accelerators(sb.dialog.root.tooltip);
    cache_indices();

    sb.font_row  = Font_Load("data/fonts/b_times new roman (100)",
                             UI_RGBAFormat());
    sb.font_help = Font_Load("data/fonts/b_times new roman (100b)",
                             UI_RGBAFormat());

    GUIRuntime_SetWidgetText(sb.rt, "HelpText", "");
    sb.open = 1;

    /* Where the saved games live is the host's business. On a desktop
     * they are already in the directory. In a browser they are in
     * storage that can only be read a promise at a time, so the dialog
     * asks for them here and shows the list when they arrive. Deciding
     * the directory is empty before that is how a player with saves
     * gets told they have none. */
    Paths_BeginSaveSync();
    if (Paths_SavesPending()) {
        sb.waiting = 1;
        GUIRuntime_SetWidgetText(sb.rt, "HelpText", "Reading your saved games.");
    } else {
        finish_open();
    }

    /* Typed characters only arrive between these two calls, so nothing
     * collects any until the save dialog asks. */
    if (mode == SAVEBROWSER_SAVE) SDL_StartTextInput();
    return 0;
}

void SaveBrowser_Close(void) {
    if (sb.open && sb.mode == SAVEBROWSER_SAVE) SDL_StopTextInput();
    free_message();
    if (sb.rt) GUIRuntime_Destroy(sb.rt);
    if (sb.has_dialog) GUIDialog_Free(&sb.dialog);
    if (sb.font_row)  Font_Free(sb.font_row);
    if (sb.font_help) Font_Free(sb.font_help);
    SaveList_Free(sb.rows);
    tak_free(sb.radar_px);
    /* A save nobody took is closed here rather than leaked. */
    if (sb.loaded) Save_ReadClose(sb.loaded);
    memset(&sb, 0, sizeof(sb));
    sb.selected = -1;
}

int SaveBrowser_IsOpen(void) { return sb.open; }

TAK_SaveGame *SaveBrowser_TakeLoad(void) {
    TAK_SaveGame *sg = sb.loaded;
    sb.loaded = NULL;
    return sg;
}

/* -- the buttons --------------------------------------------------- */

static void play_widget_sound(const GUIWidget *w) {
    if (!w || !w->sound[0]) return;
    set_text(sb.last_sound, sizeof(sb.last_sound), w->sound);
    GameSound_PlayUI(w->sound);
}

static SaveBrowserResult do_save(void) {
    switch (SaveList_CheckName(sb.name)) {
    case TAK_SAVENAME_EMPTY:
        show_message_key("YOU_MUST_ENTER_A_GAME_NAME", 0);
        return SAVEBROWSER_OPEN;
    case TAK_SAVENAME_INVALID:
        show_message_key("ENTER_VALID_FILENAME", 0);
        return SAVEBROWSER_OPEN;
    default:
        break;
    }

    char path[TAK_SAVE_PATH_MAX];
    if (Paths_SaveFile(sb.name, path, sizeof(path)) != 0) {
        show_message_key("ENTER_VALID_FILENAME", 0);
        return SAVEBROWSER_OPEN;
    }

    /* No overwrite prompt: the original writes straight over an
     * existing file and the shipped data carries no confirmation
     * string (legacy:159247-159292). */
    char err[256];
    err[0] = '\0';
    /* The picture the load dialog will show for this save. The
     * container knows about bytes and nothing about maps, so the
     * screen that has the renderer draws it and hands it over. A
     * battle with no overview image to draw from simply has none. */
    {
        size_t n = (size_t)TAK_THMB_W * (size_t)TAK_THMB_H * 3u;
        uint8_t *rgb = (uint8_t *)tak_malloc(n);
        if (rgb && Minimap_RenderThumbnail(rgb, (int)TAK_THMB_W,
                                           (int)TAK_THMB_H) == 0) {
            Save_SetThumbnail(rgb, (int)TAK_THMB_W, (int)TAK_THMB_H);
        } else {
            Save_SetThumbnail(NULL, 0, 0);
        }
        tak_free(rgb);
    }

    if (Save_Write(path, err, sizeof(err)) != 0) {
        /* A full disk, a directory that cannot be written and a path
         * that is too long all arrive here already worded. */
        show_message(err[0] ? err : "The game could not be saved.", 0);
        rescan();
        return SAVEBROWSER_OPEN;
    }
    /* In the browser the write landed in a filesystem that dies with
     * the tab, so ask the page to copy it out. */
    Paths_NotifyPrefWritten();
    return SAVEBROWSER_SAVED;
}

/* Expansion content will not come up without the expansion. The
 * original refuses a save whose summary names the Iron Plague campaign
 * and a skirmish save carrying [CreonUnits] (legacy:159443,
 * legacy:159603). Creon on a seat is what both of those record, and
 * the Crusades balance is the expansion's rule set. */
static int save_needs_expansion(const TAK_SaveGame *sg) {
    const TAK_SaveInfo *info = sg ? Save_Info(sg) : NULL;
    if (!info) return 0;
    if (info->cfg.crusades_balance) return 1;
    for (int i = 0; i < TAK_MAX_PLAYERS; i++) {
        if (info->cfg.players[i].kind == TAK_SLOT_CLOSED) continue;
        if (info->cfg.players[i].side == TAK_SIDE_CREON) return 1;
    }
    return 0;
}

static SaveBrowserResult do_load(void) {
    if (sb.selected < 0 || sb.selected >= sb.row_count) {
        show_message_key("NO_SAVED_GAMES", 0);
        return SAVEBROWSER_OPEN;
    }
    const TAK_SaveEntry *e = &sb.rows[sb.selected];
    if (!e->readable) {
        /* The container already named the map, the definition that
         * moved or the damage. That is the sentence a player can act
         * on, so it is the one shown. */
        show_message(e->refusal, 0);
        return SAVEBROWSER_OPEN;
    }
    char err[256];
    err[0] = '\0';
    TAK_SaveGame *sg = Save_Read(e->path, err, sizeof(err));
    if (!sg) {
        show_message(err[0] ? err : "The saved game file is corrupted.", 0);
        rescan();
        return SAVEBROWSER_OPEN;
    }
    if (!TAK_DataSet_HasIronPlague() && save_needs_expansion(sg)) {
        Save_ReadClose(sg);
        show_message_key("NEED_EXPANSION_TO_PLAY", 0);
        return SAVEBROWSER_OPEN;
    }
    if (sb.loaded) Save_ReadClose(sb.loaded);
    sb.loaded = sg;
    return SAVEBROWSER_LOAD_READY;
}

static SaveBrowserResult do_delete(void) {
    if (sb.selected < 0 || sb.selected >= sb.row_count) return SAVEBROWSER_OPEN;
    /* Deleted at once, with nothing asked (legacy:159241-159244). */
    if (remove(sb.rows[sb.selected].path) != 0) {
        show_message("That saved game could not be deleted.", 0);
        return SAVEBROWSER_OPEN;
    }
    Paths_NotifyPrefWritten();
    int was = sb.selected;
    rescan();
    if (sb.row_count > 0) {
        SaveBrowser_SelectRow(was < sb.row_count ? was : sb.row_count - 1);
    } else {
        sb.name[0] = '\0';
    }
    return SAVEBROWSER_OPEN;
}

static SaveBrowserResult press_named(const char *name) {
    if (!sb.open || !name) return SAVEBROWSER_OPEN;
    const GUIWidget *w = GUIDialog_FindByName(&sb.dialog, name);
    if (!w) return SAVEBROWSER_OPEN;
    play_widget_sound(w);
    if (tak_stricmp(name, "Cancel") == 0)     return SAVEBROWSER_CANCELLED;
    if (tak_stricmp(name, "SaveGame") == 0)   return do_save();
    if (tak_stricmp(name, "LoadGame") == 0)   return do_load();
    if (tak_stricmp(name, "DeleteGame") == 0) return do_delete();
    return SAVEBROWSER_OPEN;
}

SaveBrowserResult SaveBrowser_DismissMessage(void) {
    if (!MessageBox_IsOpen()) return SAVEBROWSER_OPEN;
    int closes = sb.message_closes;
    free_message();
    return closes ? SAVEBROWSER_CANCELLED : SAVEBROWSER_OPEN;
}

SaveBrowserResult SaveBrowser_Press(const char *name) {
    if (MessageBox_IsOpen()) return SaveBrowser_DismissMessage();
    return press_named(name);
}

SaveBrowserResult SaveBrowser_PressKey(const char *key) {
    if (!sb.open || !key) return SAVEBROWSER_OPEN;
    if (MessageBox_IsOpen()) return SaveBrowser_DismissMessage();
    const char *widget = NULL;
    if (tak_stricmp(key, "Enter") == 0) widget = sb.enter_widget;
    if (tak_stricmp(key, "Esc") == 0)   widget = sb.esc_widget;
    if (!widget || !widget[0]) return SAVEBROWSER_OPEN;
    return press_named(widget);
}

/* -- typing -------------------------------------------------------- */

static void take_typing(TAK_Platform *platform, int backspace_edge) {
    if (sb.mode != SAVEBROWSER_SAVE) return;
    if (backspace_edge) {
        size_t n = strlen(sb.name);
        if (n) sb.name[n - 1] = '\0';
    }
    if (!platform || platform->text_in_len <= 0) return;
    for (int i = 0; i < platform->text_in_len; i++) {
        size_t n = strlen(sb.name);
        if (n + 1 >= sizeof(sb.name)) break;
        sb.name[n] = platform->text_in[i];
        sb.name[n + 1] = '\0';
    }
}

/* -- drawing ------------------------------------------------------- */

static void draw_rows(void) {
    SDL_Surface *off = UI_Offscreen();
    if (!off || !sb.font_row) return;
    SDL_Rect r = list_rect();
    int rh = row_height();
    SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 16, 12, 8, 255));
    int rows = rows_visible();
    for (int i = 0; i < rows; i++) {
        int idx = sb.scroll + i;
        if (idx < 0 || idx >= sb.row_count) break;
        if (idx == sb.selected) {
            SDL_Rect sel = { r.x, r.y + i * rh, r.w, rh };
            SDL_FillRect(off, &sel, SDL_MapRGBA(off->format, 60, 50, 35, 255));
        }
        Font_DrawString(sb.font_row, off, r.x + 6, r.y + i * rh + 2,
                        sb.rows[idx].slug);
    }
}

static void draw_name_field(void) {
    if (sb.mode != SAVEBROWSER_SAVE) return;
    SDL_Surface *off = UI_Offscreen();
    const GUIWidget *w = GUIDialog_FindByName(&sb.dialog, "GameName");
    if (!off || !w || !sb.font_row) return;
    SDL_Rect r = w->rect;
    SDL_FillRect(off, &r, SDL_MapRGBA(off->format, 16, 12, 8, 255));
    char shown[SB_NAME_MAX + 2];
    snprintf(shown, sizeof(shown), "%s_", sb.name);
    Font_DrawString(sb.font_row, off, r.x + 4, r.y + 3, shown);
}

/* The hovered button help string, in the dialog HelpText label
 * (legacy:46918). */
static void draw_help_strip(void) {
    if (!sb.font_help || !sb.rt) return;
    const GUIWidget *hover = GUIRuntime_HoveredWidget(sb.rt);
    const GUIWidget *help = GUIDialog_FindByName(&sb.dialog, "HelpText");
    if (!hover || !help || !hover->tooltip[0]) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    int tw = Font_MeasureString(sb.font_help, hover->tooltip);
    int top = 0, bottom = 0;
    if (Font_InkExtent(sb.font_help, hover->tooltip, &top, &bottom) != 0) return;
    SDL_Rect r = help->rect;
    Font_DrawString(sb.font_help, off, r.x + (r.w - tw) / 2,
                    r.y + (r.h - (bottom - top)) / 2 - top, hover->tooltip);
}

static void render_all(void) {
    sync_thumb();
    GUIRuntime_Render(sb.rt);
    draw_radar();
    draw_rows();
    draw_name_field();
    draw_help_strip();
    MessageBox_Render();
}

/* -- the frame ----------------------------------------------------- */

SaveBrowserResult SaveBrowser_Tick(TAK_Platform *platform) {
    if (!sb.open || !sb.rt) return SAVEBROWSER_OPEN;

    /* Still arriving. The dialog draws with an empty list and its help
     * line saying so, and nothing is decided about what is in the
     * directory until it is all there. */
    if (sb.waiting) {
        if (Paths_SavesPending()) {
            render_all();
            return SAVEBROWSER_OPEN;
        }
        sb.waiting = 0;
        finish_open();
    }

    int focus = platform && platform->has_focus;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int enter = focus && (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]);
    int esc   = focus && keys[SDL_SCANCODE_ESCAPE];
    int back  = focus && keys[SDL_SCANCODE_BACKSPACE];
    int enter_edge = enter && !sb.prev_enter;
    int esc_edge   = esc && !sb.prev_esc;
    int back_edge  = back && !sb.prev_back;
    sb.prev_enter = enter;
    sb.prev_esc = esc;
    sb.prev_back = back;

    int mx = -1, my = -1, mouse_down = 0;
    if (focus) mouse_down = TAK_Platform_MouseThisFrame(platform, &mx, &my);

    /* A message box owns the keys and the mouse until it is read, the
     * same rule an inner dialog follows (legacy:243003-243004). The
     * text is what makes it modal, not the art: a message whose
     * dialog would not load still has to be read before the dialog
     * underneath takes another press. */
    if (MessageBox_IsOpen()) {
        /* A message that takes the dialog with it stands on its own: the
         * browser never came up for the player to see. */
        if (!sb.message_closes) {
            sync_thumb();
            GUIRuntime_Render(sb.rt);
            draw_radar();
            draw_rows();
            draw_name_field();
        }
        int read = MessageBox_Tick(mx, my, mouse_down, enter_edge, esc_edge);
        sb.prev_mouse = mouse_down;
        if (read) {
            int closes = sb.message_closes;
            sb.message_closes = 0;
            return closes ? SAVEBROWSER_CANCELLED : SAVEBROWSER_OPEN;
        }
        return SAVEBROWSER_OPEN;
    }

    take_typing(platform, back_edge);

    /* Thumb drag. The thumb travels only inside the bar art, which the
     * hotspot places between the two nubs (legacy:45228). */
    if (sb.idx_thumb >= 0 && sb.idx_track >= 0) {
        SDL_Point pt = { mx, my };
        SDL_Rect thumb_draw;
        widget_draw_rect(sb.idx_thumb, &thumb_draw);
        if (mouse_down && !sb.dragging && SDL_PointInRect(&pt, &thumb_draw)) {
            sb.dragging = 1;
            sb.grab_dy = my - thumb_draw.y;
        }
        if (!mouse_down) sb.dragging = 0;
        if (sb.dragging) {
            SDL_Rect track;
            int th = 1;
            thumb_travel(&track, &th);
            int travel = track.h - th;
            int m = max_scroll();
            if (travel > 0 && m > 0) {
                int rel = my - sb.grab_dy - track.y;
                if (rel < 0) rel = 0;
                if (rel > travel) rel = travel;
                sb.scroll = (rel * m + travel / 2) / travel;
                clamp_scroll();
                GUIWidget *tw = &sb.dialog.children[sb.idx_thumb];
                tw->rect.y = track.y + rel + (tw->rect.y - thumb_draw.y);
            }
        }
    }

    char clicked[64];
    int clicked_idx = -1;
    int got = GUIRuntime_UpdateEx(sb.rt, mx, my, mouse_down,
                                   clicked, sizeof(clicked), &clicked_idx);
    SaveBrowserResult result = SAVEBROWSER_OPEN;
    if (got && !sb.dragging) {
        if (clicked_idx == sb.idx_inc)      { sb.scroll--; clamp_scroll(); }
        else if (clicked_idx == sb.idx_dec) { sb.scroll++; clamp_scroll(); }
        else result = press_named(clicked);
    }

    /* Row clicks, and the double click that loads straight away
     * (legacy:159372-159378). The list is not a button widget, so this
     * is a rect test on the release edge, the way the map list is. */
    if (result == SAVEBROWSER_OPEN && !mouse_down && sb.prev_mouse && !sb.dragging) {
        SDL_Rect lr = list_rect();
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &lr)) {
            int row = sb.scroll + (my - lr.y) / row_height();
            if (row >= 0 && row < sb.row_count) {
                uint32_t now = SDL_GetTicks();
                int again = (row == sb.prev_click_row) &&
                            (now - sb.prev_click_ms < 400u);
                SaveBrowser_SelectRow(row);
                sb.prev_click_row = row;
                sb.prev_click_ms = now;
                if (again && sb.mode == SAVEBROWSER_LOAD) result = do_load();
            }
        } else {
            SDL_Rect track, thumb_draw;
            int th = 1;
            thumb_travel(&track, &th);
            widget_draw_rect(sb.idx_thumb, &thumb_draw);
            if (sb.idx_track >= 0 && SDL_PointInRect(&pt, &track)) {
                int vis = rows_visible();
                if (my < thumb_draw.y) sb.scroll -= vis;
                else if (my >= thumb_draw.y + th) sb.scroll += vis;
                clamp_scroll();
            }
        }
    }
    sb.prev_mouse = mouse_down;

    /* The wheel over the list, the way the lobby map list takes it. */
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT,
                          SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) > 0) {
        SDL_Rect lr = list_rect();
        SDL_Point pt = { mx, my };
        if (SDL_PointInRect(&pt, &lr)) { sb.scroll -= e.wheel.y; clamp_scroll(); }
    }

    if (result == SAVEBROWSER_OPEN && enter_edge) result = SaveBrowser_PressKey("Enter");
    if (result == SAVEBROWSER_OPEN && esc_edge)   result = SaveBrowser_PressKey("Esc");

    render_all();
    return result;
}

/* -- Introspection ------------------------------------------------- */

const char *SaveBrowser_DialogPath(void) { return sb.open ? sb.path : ""; }

int SaveBrowser_HasWidget(const char *name) {
    if (!sb.open || !name) return 0;
    return GUIDialog_FindByName(&sb.dialog, name) != NULL;
}

const char *SaveBrowser_Accelerators(void) {
    return sb.open ? sb.dialog.root.tooltip : "";
}

int SaveBrowser_RowCount(void) { return sb.open ? sb.row_count : 0; }

const char *SaveBrowser_RowName(int row) {
    if (!sb.open || row < 0 || row >= sb.row_count) return "";
    return sb.rows[row].slug;
}

int SaveBrowser_SelectedRow(void) { return sb.open ? sb.selected : -1; }

static const char *detail(int which) {
    if (!sb.open || sb.selected < 0 || sb.selected >= sb.row_count) return "";
    const TAK_SaveEntry *e = &sb.rows[sb.selected];
    return which == 0 ? e->side : which == 1 ? e->map : e->game_time;
}

const char *SaveBrowser_DetailSide(void) { return detail(0); }
const char *SaveBrowser_DetailMap(void)  { return detail(1); }
const char *SaveBrowser_DetailTime(void) { return detail(2); }

const char *SaveBrowser_Message(void) { return MessageBox_Text(); }

const char *SaveBrowser_Name(void) { return sb.name; }

void SaveBrowser_SetName(const char *name) {
    set_text(sb.name, sizeof(sb.name), name);
}
