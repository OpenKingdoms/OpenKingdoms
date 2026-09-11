/*
 * test_gui_loader.c — Unit tests for the .gui parser.
 *
 * Tests run against synthetic buffers for specific widget types plus the
 * real mainmenu.gui / battlemenusingle.gui files extracted under
 * data/extracted/data/guis.
 */

#include "test_framework.h"
#include "tak_gui.h"
#include "tak_hpi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef TAK_GAME_DIR
#define TAK_GAME_DIR "C:/GOG Games/Total Annihilation Kingdoms"
#endif

#ifndef TAK_DATA_DIR
#define TAK_DATA_DIR "data/extracted"
#endif

/* ── Synthetic buffers — one widget each ─────────────────────────────── */

/* Root window with no children (trailing 0 child count). */
static const char *ROOT_ONLY =
    "2 1 "
    "2 0 0 640 480 1 1 0 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "4 Root 1 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "0 ";

/* Root + one Button (same format as the mainmenu Skirmish button). */
static const char *ROOT_PLUS_BUTTON =
    /* Root window */
    "2 1 "
    "2 0 0 640 480 1 1 0 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "4 Root 1 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "1 "
    /* Child button */
    "4 1 "
    "1 "
    "2 40 192 149 188 1 0 0 0 "
    "3 255 48 48 48 "
    "1 0 0 "
    "1 0 "
    "12 PlayComputer 2 3 "
    "3 "
    "1 17 singlemachine.gaf 14 SingleMachine0 0 18 "
    "1 17 singlemachine.gaf 14 SingleMachine0 1 18 "
    "1 17 singlemachine.gaf 14 SingleMachine0 2 18 "
    "3 "
    "2 0 0 2 0 0 2 0 0 3 "
    "1 7 Default "
    "1 12 skirmish.wav "
    "1 7 Default "
    "2 0 16 Play the Machine "
    "0 ";

/* A label widget (no frames, just a font reference and tooltip). */
static const char *LABEL_ONLY =
    "19 1 "
    "1 "
    "2 174 408 295 14 1 0 0 0 "
    "3 255 48 48 48 "
    "1 0 0 "
    "1 26 times new roman (100b).gaf "
    "7 Version 1 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "0 ";

/* Root + a progress bar + a label, laid out the way loadscreen.gui
 * authors them. The label is here to prove the lexer is still in sync
 * after the bar: a wrong record shape swallows it. */
static const char *ROOT_PROGRESS_LABEL =
    /* Root window with two children */
    "2 1 "
    "2 0 0 640 480 1 1 1 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "4 Root 0 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "2 "
    /* Progress bar: version 2, value 50, step 1, max 100, flag 0 */
    "16 2 50 1 100 0 "
    "1 "
    "2 19 444 597 10 1 0 0 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "12 MainProgress 1 4 "
    "4 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "4 "
    "2 0 0 2 0 0 2 0 0 2 0 0 4 "
    "1 0 "
    "1 0 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "0 "
    /* Label that has to survive the bar */
    "19 1 "
    "1 "
    "2 8 4 102 20 1 0 0 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "8 AfterBar 1 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "0 ";

/* Root + a SingleEdit + a label, the way battlemenumulti.gui authors its
 * chat input. The label proves the lexer is still in sync after the box. */
static const char *ROOT_EDIT_LABEL =
    /* Root window with two children */
    "2 1 "
    "2 0 0 640 480 1 1 1 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "4 Root 0 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "2 "
    /* SingleEdit: version 3, then 18 and a 256 maximum, empty text, five
     * values, a style, four flags and two colours */
    "21 3 18 256 "
    "0  0 0 0 0 5 16 1 1 0 0 "
    "3 255 192 192 192 "
    "3 255 128 128 128 "
    "1 "
    "2 66 369 505 20 1 1 1 0 "
    "3 0 0 0 0 "
    "1 0 0 "
    "1 25 times new roman (100).gaf "
    "4 Chat 1 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "0 "
    /* Label that has to survive the box */
    "19 1 "
    "1 "
    "2 8 4 102 20 1 0 0 0 "
    "3 255 255 255 255 "
    "1 0 0 "
    "1 0 "
    "8 AfterBox 1 2 "
    "2 "
    "1 0 0 0 0 "
    "1 0 0 0 0 "
    "2 "
    "2 0 0 2 0 0 2 "
    "1 0 "
    "1 0 "
    "2 0 0 "
    "0 ";

/* ── Tests ───────────────────────────────────────────────────────────── */

TEST(load_root_only) {
    GUIDialog d;
    int rc = GUIDialog_LoadFromBuffer(&d, ROOT_ONLY, strlen(ROOT_ONLY));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT((int)GUI_WT_WINDOW, (int)d.root.type);
    ASSERT_EQ_INT(0, d.root.rect.x);
    ASSERT_EQ_INT(0, d.root.rect.y);
    ASSERT_EQ_INT(640, d.root.rect.w);
    ASSERT_EQ_INT(480, d.root.rect.h);
    ASSERT_EQ_STR("Root", d.root.name);
    ASSERT_EQ_INT(0, d.num_children);
    GUIDialog_Free(&d);
}

TEST(load_root_plus_button) {
    GUIDialog d;
    int rc = GUIDialog_LoadFromBuffer(&d, ROOT_PLUS_BUTTON, strlen(ROOT_PLUS_BUTTON));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(1, d.num_children);

    GUIWidget *btn = &d.children[0];
    ASSERT_EQ_INT((int)GUI_WT_BUTTON, (int)btn->type);
    ASSERT_EQ_STR("PlayComputer", btn->name);
    ASSERT_EQ_INT(40,  btn->rect.x);
    ASSERT_EQ_INT(192, btn->rect.y);
    ASSERT_EQ_INT(149, btn->rect.w);
    ASSERT_EQ_INT(188, btn->rect.h);
    ASSERT_EQ_INT(3, btn->num_frames);
    ASSERT_EQ_STR("singlemachine.gaf", btn->frames[0].gaf);
    ASSERT_EQ_STR("SingleMachine0",    btn->frames[0].sequence);
    ASSERT_EQ_INT(0, btn->frames[0].frame_index);
    ASSERT_EQ_INT(1, btn->frames[1].frame_index);
    ASSERT_EQ_INT(2, btn->frames[2].frame_index);
    ASSERT_EQ_STR("Play the Machine", btn->tooltip);
    GUIDialog_Free(&d);
}

TEST(find_by_name_matches_case_insensitive) {
    GUIDialog d;
    ASSERT_EQ_INT(0, GUIDialog_LoadFromBuffer(&d, ROOT_PLUS_BUTTON, strlen(ROOT_PLUS_BUTTON)));
    ASSERT_NOT_NULL(GUIDialog_FindByName(&d, "PlayComputer"));
    ASSERT_NOT_NULL(GUIDialog_FindByName(&d, "playcomputer"));
    ASSERT_NOT_NULL(GUIDialog_FindByName(&d, "PLAYCOMPUTER"));
    ASSERT_NULL(GUIDialog_FindByName(&d, "NotThere"));
    GUIDialog_Free(&d);
}

TEST(label_captures_font_and_tooltip) {
    GUIDialog d;
    int rc = GUIDialog_LoadFromBuffer(&d, LABEL_ONLY, strlen(LABEL_ONLY));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT((int)GUI_WT_LABEL, (int)d.root.type);
    ASSERT_EQ_STR("Version", d.root.name);
    ASSERT_EQ_STR("times new roman (100b).gaf", d.root.font);
    GUIDialog_Free(&d);
}

/* A progress bar carries a version and three values before the record
 * every widget shares, so a reader that guesses one field loses every
 * widget behind it (legacy:328245, legacy:328267). loadscreen.gui puts
 * its stained glass behind eight of them. */
TEST(progress_bar_keeps_the_lexer_in_sync) {
    GUIDialog d;
    int rc = GUIDialog_LoadFromBuffer(&d, ROOT_PROGRESS_LABEL,
                                      strlen(ROOT_PROGRESS_LABEL));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(2, d.num_children);

    ASSERT_EQ_INT((int)GUI_WT_PROGRESS, (int)d.children[0].type);
    ASSERT_EQ_STR("MainProgress", d.children[0].name);
    ASSERT_EQ_INT(19,  d.children[0].rect.x);
    ASSERT_EQ_INT(444, d.children[0].rect.y);
    ASSERT_EQ_INT(597, d.children[0].rect.w);
    ASSERT_EQ_INT(10,  d.children[0].rect.h);

    ASSERT_EQ_INT((int)GUI_WT_LABEL, (int)d.children[1].type);
    ASSERT_EQ_STR("AfterBar", d.children[1].name);
    ASSERT_EQ_INT(8,   d.children[1].rect.x);
    ASSERT_EQ_INT(4,   d.children[1].rect.y);
    ASSERT_EQ_INT(102, d.children[1].rect.w);
    ASSERT_EQ_INT(20,  d.children[1].rect.h);
    GUIDialog_Free(&d);
}

/* A SingleEdit (type 21) carries a version, two sizes, its initial text,
 * five values, a style, four flags and two colours before the record every
 * widget shares (legacy:321014-321059). Stopping there dropped every widget
 * battlemenumulti.gui authors after its chat input. */
TEST(single_edit_keeps_the_lexer_in_sync) {
    GUIDialog d;
    int rc = GUIDialog_LoadFromBuffer(&d, ROOT_EDIT_LABEL, strlen(ROOT_EDIT_LABEL));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(2, d.num_children);

    ASSERT_EQ_INT(21, (int)d.children[0].type);
    ASSERT_EQ_STR("Chat", d.children[0].name);
    ASSERT_EQ_INT(66,  d.children[0].rect.x);
    ASSERT_EQ_INT(369, d.children[0].rect.y);
    ASSERT_EQ_INT(505, d.children[0].rect.w);
    ASSERT_EQ_INT(20,  d.children[0].rect.h);

    ASSERT_EQ_INT((int)GUI_WT_LABEL, (int)d.children[1].type);
    ASSERT_EQ_STR("AfterBox", d.children[1].name);
    ASSERT_EQ_INT(8,   d.children[1].rect.x);
    ASSERT_EQ_INT(102, d.children[1].rect.w);
    GUIDialog_Free(&d);
}

TEST(load_null_buffer_fails) {
    GUIDialog d;
    ASSERT_EQ_INT(-1, GUIDialog_LoadFromBuffer(&d, NULL, 0));
}

TEST(find_by_name_on_empty_dialog_returns_null) {
    GUIDialog d;
    memset(&d, 0, sizeof(d));
    ASSERT_NULL(GUIDialog_FindByName(&d, "anything"));
}

TEST(parse_real_mainmenu_gui) {
    if (VFS_IsInitialized()) VFS_Shutdown();   /* left open by a failed test */
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        printf("SKIP (no data dir) ");
        return;
    }
    GUIDialog d;
    int rc = GUIDialog_Load(&d, "data/guis/mainmenu.gui");
    ASSERT_EQ_INT(0, rc);

    GUIWidget *skirm = GUIDialog_FindByName(&d, "PlayComputer");
    ASSERT_NOT_NULL(skirm);
    ASSERT_EQ_INT((int)GUI_WT_BUTTON, (int)skirm->type);
    ASSERT_EQ_INT(40,  skirm->rect.x);
    ASSERT_EQ_INT(192, skirm->rect.y);
    ASSERT_EQ_STR("Play the Machine", skirm->tooltip);

    GUIWidget *exit_btn = GUIDialog_FindByName(&d, "Exit");
    ASSERT_NOT_NULL(exit_btn);
    ASSERT_EQ_INT(68,  exit_btn->rect.x);
    ASSERT_EQ_INT(407, exit_btn->rect.y);

    GUIWidget *options = GUIDialog_FindByName(&d, "Options");
    ASSERT_NOT_NULL(options);
    ASSERT_EQ_INT(524, options->rect.x);
    ASSERT_EQ_INT(406, options->rect.y);

    GUIWidget *credits = GUIDialog_FindByName(&d, "Credits");
    ASSERT_NOT_NULL(credits);
    ASSERT_EQ_INT(67, credits->rect.x);
    ASSERT_EQ_INT(20, credits->rect.y);

    GUIDialog_Free(&d);
    VFS_Shutdown();
}

TEST(parse_real_battle_setup_gui) {
    if (VFS_IsInitialized()) VFS_Shutdown();   /* left open by a failed test */
    if (VFS_Init(TAK_GAME_DIR, TAK_DATA_DIR) != 0) {
        printf("SKIP (no data dir) ");
        return;
    }
    GUIDialog d;
    int rc = GUIDialog_Load(&d, "data/guis/battlemenusingle.gui");
    ASSERT_EQ_INT(0, rc);

    /* The battlemenusingle.gui has Play / Previous buttons, MapList,
     * a Line-of-Sight checkbox, etc. Verify a few key widgets land. */
    GUIWidget *play = GUIDialog_FindByName(&d, "Play");
    ASSERT_NOT_NULL(play);
    ASSERT_EQ_INT((int)GUI_WT_BUTTON, (int)play->type);
    ASSERT_EQ_INT(544, play->rect.x);

    GUIWidget *prev = GUIDialog_FindByName(&d, "Previous");
    ASSERT_NOT_NULL(prev);
    ASSERT_EQ_INT(59, prev->rect.x);

    GUIWidget *los = GUIDialog_FindByName(&d, "LineOfSight");
    ASSERT_NOT_NULL(los);
    ASSERT_EQ_INT((int)GUI_WT_CHECKBOX, (int)los->type);

    GUIDialog_Free(&d);
    VFS_Shutdown();
}

/* ── Entry ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* --no-data runs only the in-memory cases, for CI. */
    int no_data = argc > 1 && strcmp(argv[1], "--no-data") == 0;
    TEST_SUITE("gui_loader");
    RUN(load_root_only);
    RUN(load_root_plus_button);
    RUN(find_by_name_matches_case_insensitive);
    RUN(label_captures_font_and_tooltip);
    RUN(progress_bar_keeps_the_lexer_in_sync);
    RUN(single_edit_keeps_the_lexer_in_sync);
    RUN(load_null_buffer_fails);
    RUN(find_by_name_on_empty_dialog_returns_null);
    if (!no_data) {
        RUN(parse_real_mainmenu_gui);
        RUN(parse_real_battle_setup_gui);
    }
    TEST_REPORT();
}
