#ifndef TAK_TRANSLATE_H
#define TAK_TRANSLATE_H

#include "tak_gui.h"
#include <stddef.h>

/*
 * The english/translate string tables. The original shows every string
 * through a key to text table built from english/translate/*.tdf, and a
 * lookup that misses hands back the key unchanged (legacy:267931).
 */

typedef struct TranslateEntry TranslateEntry;

typedef struct TranslateTable {
    TranslateEntry *entries;
    int             count;
    int             cap;
} TranslateTable;

/* Add the English text of every section in a translate .tdf. */
void Translate_Load(TranslateTable *t, const char *path);
void Translate_Free(TranslateTable *t);

/* The text for a key (case-insensitive), NULL when the table has none. */
const char *Translate_Find(const TranslateTable *t, const char *key);

/* The same, except a miss hands back the key itself. */
const char *Translate_Lookup(const TranslateTable *t, const char *key);

/* Run every child's display text and tooltip through the table. */
void Translate_Dialog(const TranslateTable *t, GUIDialog *dialog);

/* A map's shown name: the table's entry for its .ota base name, else that
 * name with each word capitalised (legacy:167724-167726). */
void Translate_MapName(const TranslateTable *t, const char *key,
                       char *out, size_t cap);

#endif /* TAK_TRANSLATE_H */
