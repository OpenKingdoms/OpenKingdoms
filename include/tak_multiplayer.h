#ifndef TAK_MULTIPLAYER_H
#define TAK_MULTIPLAYER_H

#include "tak_platform.h"
#include "tak_gui_render.h"

/* Multiplayer battle room (GAMESTATE_MULTIPLAYER), battlemenumulti.gui,
 * reached from the main menu's Multiplayer (multiknight) button. */

int  Multiplayer_Init(TAK_Platform *platform);
int  Multiplayer_Tick(TAK_Platform *platform, float frame_dt);
void Multiplayer_Shutdown(void);

/* Inspection and drive points for tests. */

/* The room's widget runtime, NULL before Init. */
GUIRuntime *Multiplayer_Runtime(void);

/* Choose the battle's map by its .ota base name. Returns 0 when the map
 * exists and is now the room's map. */
int Multiplayer_SelectMap(const char *key);

/* The host's Allow Creon choice. The room allows Creon only when the
 * expansion is present as well. */
void Multiplayer_SetAllowCreon(int allow);

/* 1 when this room allows Creon: the expansion is present and the host
 * allowed it. What a join handshake would carry. */
int  Multiplayer_CreonAllowed(void);

/* Press the host row's PlayerSide cell, and read the side it holds. */
void Multiplayer_CycleHostSide(void);
int  Multiplayer_HostSide(void);

/* Press a widget by name and index, the way the runtime would on a
 * click. A test uses this to press a row without working out where the
 * mouse would have to be. Returns 1 when the room consumed it. */
int  Multiplayer_HandleClick(const char *name, int widget_index);

/* The map chooser, choosemap.gui for the host and viewmap.gui for
 * everyone else (legacy:136832-136851). It sits over the room, takes
 * the frame while it is up, and OK on the host's tells the server the
 * map and its fingerprint. */
void Multiplayer_OpenMapChooser(int as_host);
int  Multiplayer_MapChooserOpen(void);
int  Multiplayer_MapChooserTick(TAK_Platform *platform, float dt);
void Multiplayer_CloseMapChooser(void);
/* The chat, for the tests: the line being typed, sending it, and the
 * log as the room shows it. */
const char *Multiplayer_ChatTyping(void);
void        Multiplayer_ChatSend(void);
int         Multiplayer_ChatLineCount(void);
const char *Multiplayer_ChatLine(int index);
/* For the tests: the rows and the selection, and OK by name. */
int         Multiplayer_MapChooserRowCount(void);
const char *Multiplayer_MapChooserRowKey(int row);
void        Multiplayer_MapChooserSelect(int row);
void        Multiplayer_MapChooserPress(const char *name);
/* Test seams for the chooser's list and its bar: the top row showing,
 * the rows that fit, a pointer event as the tick would feed it, the
 * bar's thumb and track as drawn, and whether a named widget is hidden. */
int         Multiplayer_MapChooserScroll(void);
int         Multiplayer_MapChooserRowsVisible(void);
void        Multiplayer_MapChooserPointer(int x, int y, int down);
int         Multiplayer_MapChooserThumbRect(SDL_Rect *out);
int         Multiplayer_MapChooserTrackRect(SDL_Rect *out);
int         Multiplayer_MapChooserWidgetHidden(const char *name);

/* Build the world a START_GAME describes and seat this machine in it.
 * The battle room calls it for a new match, Select Game for one it is
 * rejoining. 0 on success. */
struct TAK_MsgStartGame;
int MP_BeginMatchWorld(TAK_Platform *platform, const struct TAK_MsgStartGame *sg);

#endif
