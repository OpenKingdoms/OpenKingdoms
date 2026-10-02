/*
 * message_box.c -- the shipped one button box, data/guis/ok.gui.
 *
 * A Message label, an Ok button and the root accelerators
 * "#Enter#Ok#Esc#Ok". The original shows every refusal a player has to
 * act on through this file, so the save and load dialogs and the
 * loading screen share one of these.
 *
 * The text is what makes it modal, not the art. A message whose dialog
 * will not load still has to be read before the screen underneath
 * takes another press, or a broken install turns a refusal into a
 * button that does nothing.
 */

#include "tak_message_box.h"

#include "tak_font.h"
#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_hud.h"
#include "tak_ui.h"
#include "tak_util.h"

#include <string.h>

static struct {
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    Font       *font_help;
    int         off_x;
    int         off_y;
    char        text[256];
} mb;

int MessageBox_IsOpen(void) { return mb.text[0] != '\0'; }

const char *MessageBox_Text(void) { return mb.text; }

struct GUIRuntime *MessageBox_Runtime(void) { return mb.rt; }

int MessageBox_Rect(const char *name, SDL_Rect *out) {
    if (!mb.has_dialog || !out) return -1;
    const GUIWidget *w = name ? GUIDialog_FindByName(&mb.dialog, name)
                              : &mb.dialog.root;
    if (!w) return -1;
    *out = w->rect;
    out->x += mb.off_x;
    out->y += mb.off_y;
    return 0;
}

static int widget_index(const char *name) {
    for (int i = 0; i < GUIRuntime_NumWidgets(mb.rt); i++)
        if (tak_stricmp(GUIRuntime_WidgetAt(mb.rt, i)->name, name) == 0) return i;
    return -1;
}

/* The canvas rows the message's ink covers, from the renderer's box for
 * its first line. */
static int message_rows(int idx, int lines, int line_h, int *top, int *bottom) {
    SDL_Rect ink;
    if (GUIRuntime_TextDrawRect(mb.rt, idx, &ink) != 0) return -1;
    *top = ink.y;
    *bottom = ink.y + ink.h + (lines - 1) * line_h;
    return 0;
}

/* Wrapped to the Message cell and centred in it, as the original's wrap
 * flag does (legacy:146815, legacy:335570-335588). Too tall a block may
 * run down to the Ok button, and lines past that are dropped. */
static void set_message(void) {
    GUIRuntime_SetWidgetTextWrapped(mb.rt, "Message", mb.text);
    GUIWidget *cell = GUIDialog_FindByName(&mb.dialog, "Message");
    Font *f = GUIRuntime_WidgetFont(mb.rt, "Message");
    int idx = widget_index("Message");
    if (!cell || !f || idx < 0) return;

    int lines = 1;
    for (const char *p = cell->display_text; *p; p++) if (*p == '\n') lines++;
    int line_h = Font_LineHeight(f);
    int foot = mb.dialog.root.rect.y + mb.dialog.root.rect.h;
    int ok = widget_index("Ok");
    SDL_Rect art;
    if (ok >= 0 && GUIRuntime_WidgetDrawRect(mb.rt, ok, &art) == 0 &&
        art.y - mb.off_y < foot)
        foot = art.y - mb.off_y;

    for (int grown = 0;; ) {
        int top, bottom;
        if (message_rows(idx, lines, line_h, &top, &bottom) != 0) return;
        int cell_top = cell->rect.y + mb.off_y;
        if (top >= cell_top && bottom <= cell_top + cell->rect.h) return;
        if (!grown) {
            grown = 1;
            if (foot > cell->rect.y + cell->rect.h)
                cell->rect.h = foot - cell->rect.y;
            continue;
        }
        char *cut = strrchr(cell->display_text, '\n');
        if (!cut) return;
        *cut = '\0';
        lines--;
    }
}

void MessageBox_Close(void) {
    if (mb.rt) { GUIRuntime_Destroy(mb.rt); mb.rt = NULL; }
    if (mb.has_dialog) { GUIDialog_Free(&mb.dialog); mb.has_dialog = 0; }
    if (mb.font_help) { Font_Free(mb.font_help); mb.font_help = NULL; }
    mb.off_x = 0;
    mb.off_y = 0;
    mb.text[0] = '\0';
}

int MessageBox_Open(const char *text) {
    MessageBox_Close();
    strncpy(mb.text, text ? text : "", sizeof(mb.text) - 1);
    mb.text[sizeof(mb.text) - 1] = '\0';
    if (!mb.text[0]) return -1;
    if (GUIDialog_Load(&mb.dialog, "data/guis/ok.gui") != 0) return -1;
    mb.has_dialog = 1;
    mb.rt = GUIRuntime_Create(&mb.dialog);
    if (!mb.rt) { GUIDialog_Free(&mb.dialog); mb.has_dialog = 0; return -1; }
    GUIRuntime_SetWidgetText(mb.rt, "HelpText", "");

    /* The box is authored at 45,30, which is nowhere in particular. The
     * original stands it in the middle of the play area in a battle and
     * of the screen everywhere else. The offset carries the hit tests
     * with the art, so Ok stays clickable where it is drawn. */
    SDL_Rect area;
    HUD_DialogArea(&area);
    GUI_CenterOffset(&mb.dialog, area, &mb.off_x, &mb.off_y);
    GUIRuntime_SetOffset(mb.rt, mb.off_x, mb.off_y);
    set_message();

    mb.font_help = Font_Load("data/fonts/b_times new roman (100b)",
                             UI_RGBAFormat());
    return 0;
}

/* The hovered button help string, in the dialog HelpText label
 * (legacy:46918), the same strip the save and load dialogs fill. */
static void draw_help_strip(void) {
    if (!mb.font_help || !mb.rt) return;
    const GUIWidget *hover = GUIRuntime_HoveredWidget(mb.rt);
    const GUIWidget *help = GUIDialog_FindByName(&mb.dialog, "HelpText");
    if (!hover || !help || !hover->tooltip[0]) return;
    SDL_Surface *off = UI_Offscreen();
    if (!off) return;
    int tw = Font_MeasureString(mb.font_help, hover->tooltip);
    SDL_Rect r = help->rect;
    Font_DrawString(mb.font_help, off,
                    r.x + mb.off_x + (r.w - tw) / 2,
                    Font_CenterY(mb.font_help, r.y + mb.off_y, r.h),
                    hover->tooltip);
}

void MessageBox_Render(void) {
    if (!mb.rt) return;
    GUIRuntime_Render(mb.rt);
    draw_help_strip();
}

int MessageBox_Tick(int mx, int my, int mouse_down,
                    int enter_edge, int esc_edge) {
    if (!MessageBox_IsOpen()) return 0;
    char clicked[64];
    int got = mb.rt ? GUIRuntime_Update(mb.rt, mx, my, mouse_down,
                                        clicked, sizeof(clicked))
                    : 0;
    MessageBox_Render();
    if (got || enter_edge || esc_edge) {
        MessageBox_Close();
        return 1;
    }
    return 0;
}
