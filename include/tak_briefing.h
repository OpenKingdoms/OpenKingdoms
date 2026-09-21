#ifndef TAK_BRIEFING_H
#define TAK_BRIEFING_H

#include <stddef.h>

/* ── The campaign briefing ─────────────────────────────────────────
 *
 * A campaign mission opens paused under data/guis/briefing.gui: the
 * word Paused, the chapter, the mission's name, and what the mission
 * asks for, which is the text file beside the mission's .ota
 * (legacy:153077 opens it as the battle screen is built,
 * legacy:154523-154602 fills it, legacy:167978-168000 reads the text).
 * The panel has eight lines. The chapter and the name take the first
 * two and the text is wrapped over the six that are left. */

#define TAK_BRIEFING_LINES 8

/* Read missions\<stem>.txt with the space at its end trimmed. 1 and the
 * text when the mission has one. */
int  Briefing_LoadText(const char *stem, char *out, size_t cap);

/* Open the panel. chapter may be NULL outside a campaign book. -1 when
 * there is no text to show or the dialog will not load. */
int  Briefing_Open(const char *chapter, const char *title, const char *text);
void Briefing_Close(void);
int  Briefing_IsOpen(void);

/* One frame: draw it, and close it on a press of the mouse or on
 * dismiss_edge. Returns 1 on the frame it closes. */
int  Briefing_Tick(int mx, int my, int mouse_down, int dismiss_edge);

/* What line i of the panel holds, "" past the last. For tests. */
const char *Briefing_Line(int i);

#endif /* TAK_BRIEFING_H */
