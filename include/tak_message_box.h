#ifndef TAK_MESSAGE_BOX_H
#define TAK_MESSAGE_BOX_H

#include "tak_platform.h"

/*
 * The shipped one button box, data/guis/ok.gui: a Message label, an Ok
 * button and the accelerators "#Enter#Ok#Esc#Ok". The original puts
 * every refusal a player has to read through it, so the save dialogs
 * and the loading screen share one of these rather than each growing
 * its own.
 *
 * It draws over whatever is already on the offscreen surface and never
 * presents. The screen hosting it owns the frame.
 */

/* Put a message up. Returns 0 on success, or -1 when the art will not
 * load, in which case the text is still held and MessageBox_Text
 * returns it: a message that cannot be drawn still has to be read
 * before the screen underneath takes another press. */
int  MessageBox_Open(const char *text);
void MessageBox_Close(void);
int  MessageBox_IsOpen(void);
const char *MessageBox_Text(void);

/* For tests: the box's runtime, and where a widget of it (the whole box
 * for NULL) sits on the canvas. -1 when there is no such widget. */
struct GUIRuntime *MessageBox_Runtime(void);
int  MessageBox_Rect(const char *name, SDL_Rect *out);

/* Draw it and take a press. `mx`/`my` are canvas coordinates, -1 when
 * the pointer is outside. Returns 1 when the player has read it, and
 * the box is closed by then. */
int  MessageBox_Tick(int mx, int my, int mouse_down,
                     int enter_edge, int esc_edge);

/* Draw it without taking any input, for a frame where the screen
 * underneath is still being painted. */
void MessageBox_Render(void);

#endif /* TAK_MESSAGE_BOX_H */
