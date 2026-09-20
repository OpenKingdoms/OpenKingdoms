#include "tak_settings.h"
#include "tak_paths.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A key names a page option, or a player and one of their books, which
 * is the longest: "HighWater.<31 characters>.<the book's file name>". */
#define SETTINGS_MAX_KEYS 128
#define SETTINGS_KEY_MAX  80
#define SETTINGS_TEXT_MAX 64

/* A setting is a number or a piece of text. The file is key=value
 * lines either way, and a reader that wants the other kind gets its
 * default rather than a reinterpretation of the bytes. */
typedef struct SettingsEntry {
    char key[SETTINGS_KEY_MAX];
    int  is_text;
    int  value;
    char text[SETTINGS_TEXT_MAX];
} SettingsEntry;

static SettingsEntry s_entries[SETTINGS_MAX_KEYS];
static int  s_count;
static char s_path[1100];

/* One resolver for everything the game persists, so an override moves
 * the options file and the saved games together. */
static const char *settings_dir(void) {
    return Paths_PrefDir();
}

void Settings_SetDirectory(const char *dir) {
    Paths_SetOverride(dir);
    s_path[0] = '\0';
}

const char *Settings_FilePath(void) {
    if (!s_path[0]) snprintf(s_path, sizeof(s_path), "%soptions.cfg", settings_dir());
    return s_path;
}

static SettingsEntry *find_entry(const char *key) {
    for (int i = 0; i < s_count; i++) {
        if (tak_stricmp(s_entries[i].key, key) == 0) return &s_entries[i];
    }
    return NULL;
}

/* The entry for `key`, made if there is room. NULL when the table is
 * full, which is the one case a setter drops. */
static SettingsEntry *entry_for(const char *key) {
    SettingsEntry *e = find_entry(key);
    if (e) return e;
    if (s_count >= SETTINGS_MAX_KEYS) return NULL;
    e = &s_entries[s_count++];
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof(e->key), "%s", key);
    return e;
}

int Settings_GetInt(const char *key, int default_value) {
    const SettingsEntry *e = key ? find_entry(key) : NULL;
    if (!e || e->is_text) return default_value;
    return e->value;
}

void Settings_SetInt(const char *key, int value) {
    if (!key || !key[0]) return;
    SettingsEntry *e = entry_for(key);
    if (!e) return;
    e->is_text = 0;
    e->value = value;
    e->text[0] = '\0';
}

const char *Settings_GetStr(const char *key, const char *default_value) {
    const SettingsEntry *e = key ? find_entry(key) : NULL;
    if (!e || !e->is_text) return default_value;
    return e->text;
}

void Settings_SetStr(const char *key, const char *value) {
    if (!key || !key[0] || !value) return;
    /* One line per setting. A value carrying a newline would read back
     * as a second key, so it is refused rather than written. */
    if (strchr(value, '\n') || strchr(value, '\r')) return;
    SettingsEntry *e = entry_for(key);
    if (!e) return;
    e->is_text = 1;
    e->value = 0;
    snprintf(e->text, sizeof(e->text), "%s", value);
}

int Settings_Load(void) {
    FILE *fp = fopen(Settings_FilePath(), "r");
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        char *end = eq;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!key[0]) continue;
        char *val = eq + 1;
        size_t vlen = strlen(val);
        while (vlen && (val[vlen - 1] == '\n' ||
                        val[vlen - 1] == '\r')) {
            val[--vlen] = '\0';
        }
        /* A value that is not a whole number is text. strtol stopping
         * short of the end is what says so, and it is what lets a name
         * like "42nd Regiment" survive a round trip. */
        char *stop = val;
        long n = strtol(val, &stop, 10);
        if (stop != val && *stop == '\0') {
            Settings_SetInt(key, (int)n);
        } else {
            Settings_SetStr(key, val);
        }
    }
    fclose(fp);
    return 0;
}

int Settings_Save(void) {
    FILE *fp = fopen(Settings_FilePath(), "w");
    if (!fp) return -1;
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_text) {
            fprintf(fp, "%s=%s\n", s_entries[i].key, s_entries[i].text);
        } else {
            fprintf(fp, "%s=%d\n", s_entries[i].key, s_entries[i].value);
        }
    }
    fclose(fp);
    /* In the browser the file so far lives only in a filesystem that
     * dies with the tab, so ask the page to copy it out. */
    Paths_NotifyPrefWritten();
    return 0;
}
