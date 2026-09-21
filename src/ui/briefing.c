/*
 * briefing.c -- the panel a campaign mission opens under.
 *
 * See tak_briefing.h. The text is wrapped to the width of the panel's
 * third line in that line's font and dealt out a line to a label, the
 * way the original does it (legacy:154585-154596).
 */

#include "tak_briefing.h"

#include "tak_gui.h"
#include "tak_gui_render.h"
#include "tak_hpi.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>

#define BRIEFING_GUI "data/guis/briefing.gui"
#define LINE_CAP 160

static struct {
    GUIDialog   dialog;
    int         has_dialog;
    GUIRuntime *rt;
    int         open;
    int         prev_down;
    int         lines;
    char        line[TAK_BRIEFING_LINES][LINE_CAP];
} br;

int Briefing_LoadText(const char *stem, char *out, size_t cap) {
    if (!stem || !stem[0] || !out || cap == 0) return 0;
    out[0] = 0;
    char path[256];
    snprintf(path, sizeof(path), "missions/missions/%s.txt", stem);
    void *data = NULL;
    uint32_t size = 0;
    if (VFS_ReadFile(path, &data, &size) != 0 || !data) return 0;
    size_t n = size < cap - 1 ? size : cap - 1;
    memcpy(out, data, n);
    out[n] = 0;
    VFS_FreeBuffer(data);
    /* The space at the end goes (legacy:167994-168000). */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n' ||
                     out[n - 1] == '\r' || out[n - 1] == '\t')) out[--n] = 0;
    return n > 0;
}

static void put_line(const char *text) {
    if (br.lines >= TAK_BRIEFING_LINES) return;
    char name[16];
    snprintf(name, sizeof(name), "Line%d", br.lines);
    /* A panel with fewer labels than lines just shows fewer. */
    if (br.has_dialog && !GUIDialog_FindByName(&br.dialog, name)) return;
    snprintf(br.line[br.lines], LINE_CAP, "%s", text);
    if (br.rt) GUIRuntime_SetWidgetText(br.rt, name, br.line[br.lines]);
    br.lines++;
}

/* One paragraph of the text, broken at spaces to the width of a line. */
static void put_wrapped(const char *para, int width) {
    char cur[LINE_CAP];
    size_t n = 0;
    cur[0] = 0;
    const char *p = para;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        char word[LINE_CAP];
        size_t wl = 0;
        while (*p && *p != ' ' && *p != '\t' && wl + 1 < sizeof(word)) word[wl++] = *p++;
        word[wl] = 0;
        if (!wl) break;
        char trial[LINE_CAP * 2];
        snprintf(trial, sizeof(trial), "%s%s%s", cur, n ? " " : "", word);
        int fits = strlen(trial) < LINE_CAP &&
                   (width <= 0 || !br.rt ||
                    GUIRuntime_MeasureWidgetText(br.rt, "Line2", trial) <= width);
        if (n && !fits) {
            put_line(cur);
            snprintf(cur, sizeof(cur), "%s", word);
        } else {
            snprintf(cur, sizeof(cur), "%s", trial);
        }
        n = strlen(cur);
    }
    if (n) put_line(cur);
}

int Briefing_Open(const char *chapter, const char *title, const char *text) {
    Briefing_Close();
    if (!text || !text[0]) return -1;
    if (GUIDialog_Load(&br.dialog, BRIEFING_GUI) != 0) return -1;
    br.has_dialog = 1;
    br.rt = GUIRuntime_Create(&br.dialog);
    if (!br.rt) { Briefing_Close(); return -1; }
    for (int i = 0; i < TAK_BRIEFING_LINES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Line%d", i);
        GUIRuntime_SetWidgetText(br.rt, name, "");
    }
    if (chapter && chapter[0]) put_line(chapter);
    if (title && title[0]) put_line(title);

    const GUIWidget *cell = GUIDialog_FindByName(&br.dialog, "Line2");
    int width = cell ? cell->rect.w : 0;
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", text);
    for (char *para = copy; para && *para; ) {
        char *end = strpbrk(para, "\r\n");
        if (end) *end++ = 0;
        put_wrapped(para, width);
        para = end;
    }
    br.open = 1;
    /* A press that is already down when the panel opens is not a
     * press on the panel. */
    br.prev_down = 1;
    return 0;
}

void Briefing_Close(void) {
    if (br.rt) GUIRuntime_Destroy(br.rt);
    if (br.has_dialog) GUIDialog_Free(&br.dialog);
    memset(&br, 0, sizeof(br));
}

int Briefing_IsOpen(void) { return br.open; }

const char *Briefing_Line(int i) {
    return (i >= 0 && i < br.lines) ? br.line[i] : "";
}

int Briefing_Tick(int mx, int my, int mouse_down, int dismiss_edge) {
    if (!br.open) return 0;
    char clicked[64];
    if (br.rt) {
        (void)GUIRuntime_Update(br.rt, mx, my, mouse_down, clicked, sizeof(clicked));
        GUIRuntime_Render(br.rt);
    }
    int pressed = mouse_down && !br.prev_down;
    br.prev_down = mouse_down;
    if (pressed || dismiss_edge) {
        Briefing_Close();
        return 1;
    }
    return 0;
}
