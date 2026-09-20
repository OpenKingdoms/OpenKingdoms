#ifndef TAK_STORY_H
#define TAK_STORY_H

#include "tak_platform.h"
#include "tak_gui_render.h"

/* Story / campaign screen (GAMESTATE_CAMPAIGN) — the "Book of Deeds"
 * dialog (bod.gui) that the user reaches by clicking the bodgirl
 * (Story) button on the main menu. */

int  Story_Init(TAK_Platform *platform);
int  Story_Tick(TAK_Platform *platform, float frame_dt);
void Story_Shutdown(void);

/* The screen's widget runtime, for the tests. NULL before Init. */
GUIRuntime *Story_Runtime(void);

/* Start one mission of the selected campaign by zero-based index.
 * Primarily used by tests and the Story Play button. Returns
 * GAMESTATE_GAME_LOADING on success, GAMESTATE_CAMPAIGN on failure. */
int  Story_StartMission(TAK_Platform *platform, int mission_index);

/* Start one mission by its .ota file name ("takx03_ph.ota"), whichever
 * campaign it belongs to. Same returns as Story_StartMission. */
int  Story_StartMissionFile(TAK_Platform *platform, const char *mission_file);

/* Start the chapter the book is open at. `shift_held` is the hidden
 * Play: on The Iron Plague's last chapter it launches takx26_dh
 * instead (legacy:143903-143923). */
int  Story_StartChapter(TAK_Platform *platform, int shift_held);

/* ── Campaigns ───────────────────────────────────────────────────────
 *
 * The original scans camps\*.tdf and names each one through the
 * translate table, keyed by the file name (legacy:141576, 143362). With
 * more than one it offers a combined player and campaign chooser, with
 * one it offers the plain player dialog (legacy:142640). The expansion
 * adds The Iron Plague, and -pretendnoexpansion cuts the list back to
 * Book of Darien (legacy:141580-141710). */

/* How many campaigns this install offers, 1 or 2 in a shipped game. */
int  Story_CampaignCount(void);
/* The campaign's shown name, "Book of Darien". NULL when out of range. */
const char *Story_CampaignName(int index);
/* Its lower case file name, "book of darien.tdf". NULL out of range. */
const char *Story_CampaignFile(int index);
/* Which campaign the screen is on, and a chooser's pick. */
int  Story_SelectedCampaign(void);
void Story_SelectCampaign(int index);

/* The player whose book this is. The original writes it into BookName,
 * beside the authored "Book of" (legacy:144267). */
const char *Story_PlayerName(void);
void Story_SetPlayerName(const char *name);

/* ── The open page ───────────────────────────────────────────────────*/

/* Which chapter the book is open at, zero-based. The page turners and
 * a chooser set it, clamped to the furthest chapter reached. */
int  Story_SelectedChapter(void);
void Story_SelectChapter(int chapter);
/* How many chapters the selected campaign has. */
int  Story_ChapterCount(void);
/* The localised chapter title, from the translate table keyed by the
 * campaign's mission name (legacy:144520-144540). */
const char *Story_ChapterText(void);
/* The story1.gaf frame the chapter art draws: the chapter number for
 * Book of Darien, 0x32 above it for The Iron Plague, 0x31 for any
 * other campaign, and 0 when that is past the art's last frame
 * (legacy:144484-144513). */
int  Story_ChapterImageFrame(void);

/* ── Progress ────────────────────────────────────────────────────────*/

/* The highest chapter unlocked, zero-based. The page turners stop
 * here. */
int  Story_HighWaterChapter(void);

/* A campaign mission ended. Advances the chapter on a win and
 * leaves it alone on a loss. */
void Story_MissionFinished(int won);

/* Unlock every chapter of the selected campaign. Typing "wasabi" on
 * this screen does it (legacy:144228-144232). */
void Story_UnlockAllChapters(void);

/* Characters typed on the screen, for the cheat buffer. Story_Tick
 * feeds it what the platform composed this frame. */
void Story_TypeText(const char *text);

#endif
