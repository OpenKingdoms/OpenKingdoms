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
    GUIRuntime_SetWidgetText(mb.rt, "Message", mb.text);
    GUIRuntime_SetWidgetText(mb.rt, "HelpText", "");

    /* The box the original puts in the middle of the screen is authored
     * at 45,30 in a 640x480 screen, which is not the middle. Work the
     * offset out from the surface and the root rect, the way the Options
     * tabs place their sub-dialogs, rather than writing the numbers in:
     * a box of another size, from a mod, still lands in the middle. The
     * offset moves the hit tests with the art, so Ok stays clickable
     * where it is drawn. */
    SDL_Surface *screen = UI_Offscreen();
    if (screen) {
        mb.off_x = (screen->w - mb.dialog.root.rect.w) / 2
                 - mb.dialog.root.rect.x;
        mb.off_y = (screen->h - mb.dialog.root.rect.h) / 2
                 - mb.dialog.root.rect.y;
        GUIRuntime_SetOffset(mb.rt, mb.off_x, mb.off_y);
    }

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
    int top = 0, bottom = 0;
    if (Font_InkExtent(mb.font_help, hover->tooltip, &top, &bottom) != 0) return;
    SDL_Rect r = help->rect;
    Font_DrawString(mb.font_help, off,
                    r.x + mb.off_x + (r.w - tw) / 2,
                    r.y + mb.off_y + (r.h - (bottom - top)) / 2 - top,
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
