/*
 * mod_fetch.c -- a room's mod from the registry, asked for from the lobby.
 *
 * See tak_mod_fetch.h. The desktop asks the server it plays on and does
 * the work on a thread, so the lobby keeps drawing. The browser hands the
 * room to the page, which owns the storage the game files live in.
 */

#include "tak_mod_fetch.h"
#include "tak_mod_install.h"
#include "tak_mod_registry.h"
#include "tak_modset.h"
#include "tak_http_get.h"
#include "tak_settings.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum { MF_IDLE = 0, MF_LOOKING, MF_NONE, MF_OFFER, MF_MANUAL, MF_FETCHING, MF_DONE, MF_FAILED };

static struct {
    SDL_mutex      *lock;
    int             state;
    char            address[160];
    char            code[16];
    char            mod[32];
    char            version[16];
    uint64_t        content;
    TAK_ModRegistry reg;
    int             have_reg;
    TAK_ModEntry    entry;
    char            text[400];
    char            told[400];
    volatile size_t progress;
    int             settings_due;
} g;

static void lock(void) {
    if (!g.lock) g.lock = SDL_CreateMutex();
    if (g.lock) SDL_LockMutex(g.lock);
}

static void unlock(void) {
    if (g.lock) SDL_UnlockMutex(g.lock);
}

static void set(int state, const char *text) {
    lock();
    g.state = state;
    snprintf(g.text, sizeof g.text, "%s", text ? text : "");
    unlock();
}

static void label(const TAK_ModEntry *e, char *out, size_t cap) {
    TAK_ModSet_Label(e->name, e->version, out, cap);
}

/* "1.5 MB", or "1 KB" for a small one. */
static const char *size_text(uint64_t n) {
    static char out[32];
    if (n < 1048576u) snprintf(out, sizeof out, "%u KB", (unsigned)(n < 1024u ? 1u : (n + 512u) / 1024u));
    else snprintf(out, sizeof out, "%.1f MB", (double)n / 1048576.0);
    return out;
}

#ifndef __EMSCRIPTEN__

static int look(void *unused) {
    (void)unused;
    char why[256];
    if (!g.have_reg) {
        uint8_t *body = NULL;
        size_t len = 0;
        if (TAK_HttpGet(g.address, "/api/mods", 256u << 10, &body, &len, NULL, why, sizeof why) != 0) {
            char line[320];
            snprintf(line, sizeof line, "The mod registry could not be read. %s", why);
            set(MF_FAILED, line);
            return 0;
        }
        g.have_reg = TAK_ModRegistry_Parse((const char *)body, len, &g.reg) >= 0;
        free(body);
    }
    const TAK_ModEntry *e = g.have_reg ? TAK_ModRegistry_ForRoom(&g.reg, g.mod, g.version, g.content) : NULL;
    if (!e) { set(MF_NONE, ""); return 0; }
    g.entry = *e;
    char name[64], line[400];
    label(e, name, sizeof name);
    if (!TAK_ModEntry_OneClick(e)) {
        snprintf(line, sizeof line, "That game plays %s by %s, which installs by hand. See %.160s",
                 name, e->author, e->page);
        set(MF_MANUAL, line);
    } else if (TAK_ModEntry_RoomCheck(e, g.content, why, sizeof why) != 0) {
        set(MF_FAILED, why);
    } else {
        snprintf(line, sizeof line, "That game plays %s by %s. Press Join again to fetch it "
                 "from the mod registry, %s.", name, e->author, size_text(e->size));
        set(MF_OFFER, line);
    }
    return 0;
}

static int fetch(void *unused) {
    (void)unused;
    char path[128], why[256], line[320], name[64];
    const TAK_ModEntry *e = &g.entry;
    label(e, name, sizeof name);
    snprintf(path, sizeof path, "/api/mods/%s/download", e->id);
    uint8_t *body = NULL;
    size_t len = 0;
    g.progress = 0;
    if (TAK_HttpGet(g.address, path, (size_t)e->size, &body, &len, &g.progress, why, sizeof why) != 0) {
        snprintf(line, sizeof line, "%s could not be fetched. %.200s", name, why);
        set(MF_FAILED, line);
        return 0;
    }
    int rc = TAK_ModEntry_Check(e, body, len, g.content, why, sizeof why);
    if (rc == 0) rc = TAK_ModInstall_Zip(TAK_ModSet_Root(), e, body, len, why, sizeof why);
    free(body);
    if (rc != 0) { set(MF_FAILED, why); return 0; }
    snprintf(line, sizeof line, "%s is installed and checked. Restart OpenKingdoms and it "
             "joins the game with it.", name);
    lock();
    g.settings_due = 1;
    unlock();
    set(MF_DONE, line);
    return 0;
}

static void run(int (*fn)(void *), const char *name) {
    SDL_Thread *t = SDL_CreateThread(fn, name, NULL);
    if (t) SDL_DetachThread(t);
    else set(MF_FAILED, "The fetch could not start.");
}

#endif

int ModFetch_Join(const char *address, const char *code, const char *mod,
                  const char *version, uint64_t content) {
    if (!code || !code[0]) return 0;
#ifdef __EMSCRIPTEN__
    (void)address;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)content);
    int took = EM_ASM_INT({
        if (!Module['okFetchMod']) return 0;
        Module['okFetchMod']({ code: UTF8ToString($0), mod: UTF8ToString($1),
                               mod_version: UTF8ToString($2), fingerprint: UTF8ToString($3) });
        return 1;
    }, code, mod ? mod : "", version ? version : "", hex);
    if (took) set(MF_LOOKING, "Looking in the mod registry for that game's mod.");
    return took;
#else
    lock();
    int state = g.state;
    int same = strcmp(g.code, code) == 0 && g.content == content;
    unlock();
    if (state == MF_LOOKING || state == MF_FETCHING) return 1;
    if (same && state == MF_NONE) return 0;
    if (same && (state == MF_MANUAL || state == MF_DONE)) {
        lock();
        g.told[0] = '\0';
        unlock();
        return 1;
    }
    if (same && state == MF_OFFER) {
        char name[64], line[160];
        label(&g.entry, name, sizeof name);
        snprintf(line, sizeof line, "Fetching %s from the mod registry.", name);
        set(MF_FETCHING, line);
        run(fetch, "okmodfetch");
        return 1;
    }
    if (!address || !address[0]) return 0;
    lock();
    snprintf(g.address, sizeof g.address, "%s", address);
    snprintf(g.code, sizeof g.code, "%s", code);
    snprintf(g.mod, sizeof g.mod, "%s", mod ? mod : "");
    snprintf(g.version, sizeof g.version, "%s", version ? version : "");
    g.content = content;
    unlock();
    set(MF_LOOKING, "Looking in the mod registry for that game's mod.");
    run(look, "okmodlook");
    return 1;
#endif
}

const char *ModFetch_Text(void) {
    static char out[400];
    lock();
    if (g.state == MF_FETCHING && g.entry.size) {
        char name[64];
        label(&g.entry, name, sizeof name);
        snprintf(out, sizeof out, "Fetching %s from the mod registry, %d%%.", name,
                 (int)((double)g.progress * 100.0 / (double)g.entry.size));
    } else {
        snprintf(out, sizeof out, "%s", g.text);
    }
    /* An install is chosen for the next start, and that start joins. */
    int due = g.settings_due;
    g.settings_due = 0;
    unlock();
    if (due) {
        Settings_SetStr("ModSet", g.entry.modset);
        Settings_SetStr(MODFETCH_JOIN_SETTING, g.code);
        Settings_Save();
    }
    return out;
}

int ModFetch_Command(const char *what, const char *id, const char *address,
                     const char *root) {
    char why[256], version[16];
    if (strcmp(what, "remove") == 0) {
        if (TAK_ModInstall_Remove(root, id, why, sizeof why) != 0) {
            fprintf(stderr, "%s\n", why);
            return 1;
        }
        printf("Removed %s from %s/Mods\n", id, root);
        return 0;
    }
    if (!address || !address[0]) {
        fprintf(stderr, "The registry comes from a game server. Name one with --relay ws://host[:port],\n"
                        "or join a game once so its address is remembered.\n");
        return 2;
    }
    uint8_t *body = NULL;
    size_t len = 0;
    static TAK_ModRegistry reg;
    if (TAK_HttpGet(address, "/api/mods", 256u << 10, &body, &len, NULL, why, sizeof why) != 0 ||
        TAK_ModRegistry_Parse((const char *)body, len, &reg) < 0) {
        fprintf(stderr, "The mod registry could not be read from %s. %s\n", address, why);
        free(body);
        return 1;
    }
    free(body);
    if (strcmp(what, "list") == 0) {
        for (int i = 0; i < reg.count; i++) {
            const TAK_ModEntry *e = &reg.mod[i];
            int have = TAK_ModInstall_Installed(root, e->id, version, sizeof version);
            printf("%-20s %s %s by %s%s\n    %s\n", e->id, e->name, e->version, e->author,
                   have ? (strcmp(version, e->version) == 0 ? ", installed" : ", another version installed")
                        : TAK_ModEntry_OneClick(e) ? "" : ", installs by hand", e->page);
        }
        return 0;
    }
    const TAK_ModEntry *e = TAK_ModRegistry_Find(&reg, id);
    if (!e) { fprintf(stderr, "The registry has no mod %s. --registry lists them.\n", id); return 1; }
    if (!TAK_ModEntry_OneClick(e)) {
        fprintf(stderr, "%s installs by hand:\n%s\n%s\n", e->name, e->manual, e->page);
        return 1;
    }
    char path[128];
    snprintf(path, sizeof path, "/api/mods/%s/download", e->id);
    printf("Fetching %s %s by %s, %s\n", e->name, e->version, e->author, size_text(e->size));
    if (TAK_HttpGet(address, path, (size_t)e->size, &body, &len, NULL, why, sizeof why) != 0 ||
        TAK_ModEntry_Check(e, body, len, 0, why, sizeof why) != 0 ||
        TAK_ModInstall_Zip(root, e, body, len, why, sizeof why) != 0) {
        fprintf(stderr, "%s\n", why);
        free(body);
        return 1;
    }
    free(body);
    printf("Installed into %s. Play it with --mods %s\n", root, e->modset);
    return 0;
}

int ModFetch_Changed(void) {
    const char *now = ModFetch_Text();
    if (!now[0] || strcmp(now, g.told) == 0) return 0;
    snprintf(g.told, sizeof g.told, "%s", now);
    return 1;
}
