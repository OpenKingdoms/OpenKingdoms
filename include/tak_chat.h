#ifndef TAK_CHAT_H
#define TAK_CHAT_H

#include "tak_net_protocol.h"
#include <SDL.h>
#include <stddef.h>
#include <stdint.h>

/* ── The in battle chat console ───────────────────────────────────────
 *
 * Enter opens a one line edit box over the bottom of the screen, Enter
 * again sends what is in it, Escape throws it away (the running battle
 * key switch at legacy:242892-242913, the console's own Escape at
 * legacy:154417-154425). The console exists in every mode, single
 * player included (legacy:243251), so a skirmish player still sees
 * their own line.
 *
 * Sent and received lines land in a ring of thirty entries that draws
 * down the top left corner of the screen in absolute screen pixels,
 * not anchored to the play viewport (legacy:205958-206051). The first
 * row of the block sits at (10, 23) and each further row is 18 px
 * below. Every message is word wrapped to the screen width less the
 * 128 px sidebar and a 10 px margin.
 *
 * Nothing here touches the simulation. The console never pauses the
 * battle, never pushes a command, and no simulation state reads it.
 * Pausing on a chat line would stop the clock and desync a networked
 * match, which is the one way this must not behave like the F1 menu. */

/* The ring is thirty slots and one is always the gap between head and
 * tail, so twenty nine messages can be held (legacy:205785-205809). */
#define CHAT_RING_SLOTS   30
/* The original's stored line, 0x12a bytes of payload inside a 0x132
 * byte entry. */
#define CHAT_TEXT_MAX     298
/* console.gui's SingleEdit declares "21 3 20 256": a 256 character
 * line, plus the NUL. */
#define CHAT_INPUT_MAX    256

/* Message types, the original's own numbers. Your own line is stored
 * with 4, a received line with 8, and the draw filter passes an entry
 * only when the ChatLevel option is on or the type is 8
 * (legacy:206003-206004). So with Unit Chat On switched off you see
 * everyone else and never yourself. 2 is ours, for a local notice that
 * never went anywhere. */
#define CHAT_TYPE_NOTICE  2
#define CHAT_TYPE_MINE    4
#define CHAT_TYPE_THEIRS  8

/* The owner the original gives a system line, drawn with no banner. */
#define CHAT_OWNER_SYSTEM 10

/* Layout, in canvas pixels, from the original's draw (legacy:205958-206051)
 * and the console dialog's own bottom align (legacy:154381-154391). */
#define CHAT_LIST_X       10
#define CHAT_LIST_TOP     23
#define CHAT_LINE_PITCH   18
#define CHAT_RIGHT_INSET  138   /* the 128 px sidebar plus a 10 px margin */
#define CHAT_INPUT_X      9
#define CHAT_INPUT_W      512
#define CHAT_INPUT_H      32
#define CHAT_INPUT_INSET  10    /* console.gui puts the edit at x 19 */

/* ── Options ──────────────────────────────────────────────────────────
 * The three the display reads, under the original's own config names
 * (legacy:131691-131696). Their factory values cannot be read out of
 * the reference, so we ship ChatLevel on, TextScrollTime at 5 and
 * MaxTextLines at 8: a MaxTextLines of 0 stores and draws nothing at
 * all, and ChatLevel off hides your own line, so either default would
 * read as chat being broken. */
int  Chat_ChatLevel(void);       /* Unit Chat On                        */
int  Chat_TextScrollTime(void);  /* Text Delay, 0 to 20                 */
int  Chat_MaxTextLines(void);    /* Text Lines, 0 to 20                 */

/* ── Lifecycle ────────────────────────────────────────────────────── */

/* Load the font and clear the ring. Safe to call twice. */
int  Chat_Init(void);
void Chat_Shutdown(void);
/* Empty the ring and close the line, leaving the font alone. */
void Chat_Reset(void);

/* Who the local player is on the wire and in the "<name>: <text>"
 * prefix. Defaults to slot 1 and "Player", because nothing in the
 * repo carries a player name yet. */
void Chat_SetLocalPlayer(int slot, const char *name);
int  Chat_LocalSlot(void);

/* ── The ring ─────────────────────────────────────────────────────── */

/* Store one message. Drops the oldest when the ring is full, and does
 * nothing at all when MaxTextLines is 0 (legacy:205792). Returns 1 when
 * the message was stored. */
int  Chat_Push(const char *text, int type, int owner_slot, uint32_t now_ms);

/* One expiry pass. Looks only at the oldest entry and drops it when it
 * has outlived (TextScrollTime + 1) seconds, so a new message never
 * pushes an older one off (legacy:205907-205910). */
void Chat_Expire(uint32_t now_ms);

/* How many entries the ring holds, oldest first at index 0. */
int  Chat_Count(void);
const char *Chat_EntryText(int index);
int  Chat_EntryType(int index);
int  Chat_EntryOwner(int index);

/* ── The input line ───────────────────────────────────────────────── */

int  Chat_IsOpen(void);
/* Clear the line and take the keyboard (legacy:242899-242912). */
void Chat_Open(void);
/* Escape: close and throw the line away (legacy:154417-154425). */
void Chat_Cancel(void);
/* Enter: send and close. An empty or whitespace only line just closes.
 * Returns 1 when a message was sent. */
int  Chat_Submit(uint32_t now_ms);

/* Editing. Text arrives as the platform collected it, already filtered
 * to printable ASCII. */
void Chat_TypeText(const char *text);
void Chat_Backspace(void);
void Chat_Delete(void);
void Chat_CaretLeft(void);
void Chat_CaretRight(void);
void Chat_CaretHome(void);
void Chat_CaretEnd(void);

const char *Chat_Line(void);
int  Chat_Caret(void);

/* ── Drawing ──────────────────────────────────────────────────────── */

/* The message block, top left of the whole screen. */
void Chat_Draw(SDL_Surface *off);
/* The edit box, bottom aligned the way the console lays itself out. */
void Chat_DrawInput(SDL_Surface *off);

/* Rows the block would paint, and the text of one of them. Drawing and
 * this walk the same list, so a test can read what a player sees
 * without a font. */
int  Chat_VisibleRows(void);
const char *Chat_VisibleRow(int row);

/* ── The wire ─────────────────────────────────────────────────────────
 * One path, the multiplayer protocol's own TAK_MSG_CHAT. Submit encodes
 * a frame and hands it to whatever the session registered. With no
 * session registered the line is still stored locally, which is what
 * the original does in single player: its send loop finds no other
 * active player and nothing leaves the machine. */
typedef void (*Chat_SendFn)(const void *frame, size_t len, void *user);
void Chat_SetSender(Chat_SendFn fn, void *user);

/* A TAK_MSG_CHAT that arrived. Stores it as a received line, type 8. */
void Chat_OnMessage(const TAK_MsgChat *m, uint32_t now_ms);

/* The frame Submit last handed to the sender, for tests. len is 0 when
 * nothing has been sent. */
const void *Chat_LastSentFrame(size_t *out_len);

#endif /* TAK_CHAT_H */
