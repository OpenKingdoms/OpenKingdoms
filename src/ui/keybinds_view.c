/*
 * keybinds_view.c -- what the key binds screen shows, without the art.
 *
 * See tak_keybinds_view.h.
 */

#include "tak_keybinds_view.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

enum { KV_CAMERA = 3, KV_BOOKMARKS = 4 };

static const char *const k_group_label[KEYVIEW_GROUPS] = {
    "Orders", "Select", "Weapons", "Camera", "Bookmarks", "Game", "Building"
};

static int kv_is_bookmark(KeyAction a) {
    return a >= KA_BOOKMARK_1 && a <= KA_BOOKMARK_STORE_4;
}

const char *KeyView_GroupLabel(int group) {
    return (group >= 0 && group < KEYVIEW_GROUPS) ? k_group_label[group] : "";
}

int KeyView_GroupOf(KeyAction a) {
    const KeyActionInfo *info = Keybinds_Info(a);
    if (!info) return -1;
    if (kv_is_bookmark(a)) return KV_BOOKMARKS;
    switch (info->group) {
    case KG_ORDERS:   return 0;
    case KG_SELECT:   return 1;
    case KG_WEAPONS:  return 2;
    case KG_CAMERA:   return KV_CAMERA;
    case KG_GAME:     return 5;
    case KG_BUILDING: return 6;
    default:          return -1;
    }
}

int KeyView_GroupActions(int group, KeyAction *out, int cap) {
    int n = 0;
    for (int a = 0; a < KA_COUNT; a++) {
        if (KeyView_GroupOf((KeyAction)a) != group) continue;
        if (out && n < cap) out[n] = (KeyAction)a;
        n++;
    }
    return (out && n > cap) ? cap : n;
}

int KeyView_ClampScroll(int scroll, int rows, int visible) {
    int max = rows - visible;
    if (max < 0) max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    return scroll;
}

int KeyView_ThumbOffset(int scroll, int rows, int visible, int travel) {
    int max = rows - visible;
    if (max <= 0 || travel <= 0) return 0;
    return travel * KeyView_ClampScroll(scroll, rows, visible) / max;
}

int KeyView_ScrollAt(int offset, int rows, int visible, int travel) {
    int max = rows - visible;
    if (max <= 0 || travel <= 0) return 0;
    if (offset < 0) offset = 0;
    if (offset > travel) offset = travel;
    return (offset * max + travel / 2) / travel;
}

/* Swap a leading or inner word for a shorter one, in place. */
static void kv_swap(char *text, int cap, const char *from, const char *to) {
    char *at = strstr(text, from);
    if (!at) return;
    char rest[64];
    snprintf(rest, sizeof(rest), "%s", at + strlen(from));
    snprintf(at, (size_t)(cap - (int)(at - text)), "%s%s", to, rest);
}

char *KeyView_ChordFit(KeyChord chord, int max_w, KeyViewMeasure measure,
                       void *ctx, char *buf, int cap) {
    if (!buf || cap <= 0) return buf;
    Keybinds_ChordText(chord, 0, buf, cap);
    kv_swap(buf, cap, "Keypad ", "Num ");
    if (!measure || measure(ctx, buf) <= max_w) return buf;
    kv_swap(buf, cap, "Ctrl+", "C+");
    kv_swap(buf, cap, "Shift+", "S+");
    size_t n = strlen(buf);
    while (n > 1 && measure(ctx, buf) > max_w) buf[--n] = '\0';
    return buf;
}

static const char *const k_scheme_note[KEY_SCHEME_COUNT] = {
    [KEY_SCHEME_STANDARD] = "Standard: OpenKingdoms' default",
    [KEY_SCHEME_OTAK]     = "OTAK: Cavedog's default",
    [KEY_SCHEME_GRID]     = "Grid: Modernized grid layout",
};

char *KeyView_Status(char *buf, int cap) {
    if (!buf || cap <= 0) return buf;
    int n = Keybinds_CustomCount();
    KeyScheme s = Keybinds_Scheme();
    if (n <= 0)
        snprintf(buf, (size_t)cap, "%s", k_scheme_note[s]);
    else
        snprintf(buf, (size_t)cap, "%d %s %s%s", n,
                 n == 1 ? "key overrides" : "keys override", Keybinds_SchemeLabel(s),
                 Keybinds_CustomOn() ? "" : ", off");
    return buf;
}

/* Ctrl with these closes, opens or changes a tab, and no page can stop
 * it. F11 is the browser's own full screen. */
static const uint16_t k_browser_keeps_ctrl[] = {
    SDL_SCANCODE_W, SDL_SCANCODE_T, SDL_SCANCODE_N,
    SDL_SCANCODE_TAB, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN,
};

/* Keys a browser has a use for that a page can usually take first:
 * reload, save, find, print, the address bar, zoom and the like. */
static const uint16_t k_browser_uses_ctrl[] = {
    SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_L, SDL_SCANCODE_D,
    SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_U, SDL_SCANCODE_P,
    SDL_SCANCODE_O, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_E,
    SDL_SCANCODE_K, SDL_SCANCODE_B, SDL_SCANCODE_EQUALS, SDL_SCANCODE_MINUS,
    SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_MINUS,
};

static const uint16_t k_browser_uses_plain[] = {
    SDL_SCANCODE_F1, SDL_SCANCODE_F3, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
    SDL_SCANCODE_F7, SDL_SCANCODE_F12,
};

static int kv_in(const uint16_t *list, size_t n, uint16_t key) {
    for (size_t i = 0; i < n; i++)
        if (list[i] == key) return 1;
    return 0;
}

#define KV_IN(list, key) kv_in(list, sizeof(list) / sizeof((list)[0]), key)

int KeyView_BrowserRisk(KeyChord chord) {
    if (!chord.key) return 0;
    /* A key that minds no Ctrl still answers without it. */
    if (chord.mods & KM_CTRL) {
        if (KV_IN(k_browser_keeps_ctrl, chord.key)) return 2;
        if (KV_IN(k_browser_uses_ctrl, chord.key)) return 1;
        return 0;
    }
    if (chord.key == SDL_SCANCODE_F11) return 2;
    return KV_IN(k_browser_uses_plain, chord.key) ? 1 : 0;
}

const char *KeyView_BrowserNote(KeyChord chord) {
    switch (KeyView_BrowserRisk(chord)) {
    case 2:  return "Key reserved for the browser";
    case 1:  return "Key may be used by the browser";
    default: return "";
    }
}

int KeyView_InBrowser(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#else
    return 0;
#endif
}

const char *KeyView_RefusalNote(const uint8_t *keys, const uint8_t *prev) {
    if (!keys || !prev) return "";
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT])
        return "Alt is reserved for debug tools";
    for (int sc = SDL_SCANCODE_A; sc < SDL_NUM_SCANCODES; sc++) {
        if (!keys[sc] || prev[sc]) continue;
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
            return "Enter is reserved for chat";
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0)
            return "The digits are reserved for groups";
        if (sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI) continue;
        break;
    }
    return "That key cannot be bound";
}
