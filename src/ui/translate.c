/*
 * translate.c -- the english/translate string tables.
 *
 * Shared by the battle screens so every .gui placeholder and map name
 * resolves the way the original resolves it (legacy:267931).
 */

#include "tak_translate.h"
#include "tak_tdf.h"
#include "tak_memory.h"
#include "tak_util.h"
#include <string.h>

struct TranslateEntry {
    char key[96];
    char text[128];
};

static void copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

static void translate_add(TranslateTable *t, const char *key, const char *text) {
    if (!key || !*key || !text || !*text) return;
    if (t->count >= t->cap) {
        int cap = t->cap ? t->cap * 2 : 128;
        TranslateEntry *grown = (TranslateEntry *)tak_realloc(
            t->entries, (size_t)cap * sizeof(TranslateEntry));
        if (!grown) return;
        t->entries = grown;
        t->cap = cap;
    }
    TranslateEntry *e = &t->entries[t->count++];
    copy_text(e->key, sizeof(e->key), key);
    copy_text(e->text, sizeof(e->text), text);
}

void Translate_Load(TranslateTable *t, const char *path) {
    if (!t || !path) return;
    TDFFile *tdf = TDF_Open(path);
    if (!tdf) return;
    if (TDF_Load(tdf) != 0) { TDF_Close(tdf); return; }
    for (const char *name = TDF_GetFirstSection(tdf); name;
         name = TDF_GetNextSection(tdf)) {
        char section[96];
        copy_text(section, sizeof(section), name);
        if (TDF_PushSection(tdf, section) != 0) continue;
        const char *text = TDF_ReadString(tdf, "English", "");
        if (text && *text) translate_add(t, section, text);
        TDF_PopSection(tdf);
    }
    TDF_Close(tdf);
}

void Translate_Free(TranslateTable *t) {
    if (!t) return;
    if (t->entries) tak_free(t->entries);
    t->entries = NULL;
    t->count = t->cap = 0;
}

/* Legacy lower-cases the key before the search, which a case-insensitive
 * compare matches. */
const char *Translate_Find(const TranslateTable *t, const char *key) {
    if (!t || !key || !*key) return NULL;
    for (int i = 0; i < t->count; i++) {
        if (tak_stricmp(t->entries[i].key, key) == 0) return t->entries[i].text;
    }
    return NULL;
}

const char *Translate_Lookup(const TranslateTable *t, const char *key) {
    const char *hit = Translate_Find(t, key);
    return hit ? hit : (key ? key : "");
}

void Translate_Dialog(const TranslateTable *t, GUIDialog *dialog) {
    if (!t || !dialog) return;
    for (int i = 0; i < dialog->num_children; i++) {
        GUIWidget *w = &dialog->children[i];
        const char *hit = Translate_Find(t, w->display_text);
        if (hit) copy_text(w->display_text, sizeof(w->display_text), hit);
        hit = Translate_Find(t, w->tooltip);
        if (hit) copy_text(w->tooltip, sizeof(w->tooltip), hit);
    }
}

void Translate_MapName(const TranslateTable *t, const char *key,
                       char *out, size_t cap) {
    if (!out || !cap) return;
    const char *label = Translate_Find(t, key);
    copy_text(out, cap, label ? label : key);
    if (label) return;
    /* Upper-case the first letter of every space-separated word and leave
     * the rest alone (legacy:167726). */
    for (char *p = out; *p; p++) {
        if (p != out && p[-1] != ' ') continue;
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }
}
