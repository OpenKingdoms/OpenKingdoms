#include "tak_settings.h"
#include "tak_paths.h"
#include "tak_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_MAX_KEYS 64
#define SETTINGS_KEY_MAX  48

typedef struct SettingsEntry {
    char key[SETTINGS_KEY_MAX];
    int  value;
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

int Settings_GetInt(const char *key, int default_value) {
    const SettingsEntry *e = key ? find_entry(key) : NULL;
    return e ? e->value : default_value;
}

void Settings_SetInt(const char *key, int value) {
    if (!key || !key[0]) return;
    SettingsEntry *e = find_entry(key);
    if (!e) {
        if (s_count >= SETTINGS_MAX_KEYS) return;
        e = &s_entries[s_count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->value = value;
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
        Settings_SetInt(key, (int)strtol(eq + 1, NULL, 10));
    }
    fclose(fp);
    return 0;
}

int Settings_Save(void) {
    FILE *fp = fopen(Settings_FilePath(), "w");
    if (!fp) return -1;
    for (int i = 0; i < s_count; i++) {
        fprintf(fp, "%s=%d\n", s_entries[i].key, s_entries[i].value);
    }
    fclose(fp);
    /* In the browser the file so far lives only in a filesystem that
     * dies with the tab, so ask the page to copy it out. */
    Paths_NotifyPrefWritten();
    return 0;
}
