#ifndef TAK_TDF_H
#define TAK_TDF_H


typedef struct TDFFile TDFFile;

/* Lifecycle */
TDFFile *TDF_Open(const char *path);       /* Read file into memory, return unparsed handle */
int      TDF_Load(TDFFile *tdf);           /* Parse raw data into tree */
void     TDF_Close(TDFFile *tdf);          /* Free tree + raw data + handle */

/* Section navigation (stack-based cursor into the parse tree)
 *
 * PushSection: find a child section by name, push old position, enter it
 * EnterSubsection: same as PushSection (alias)
 * OpenSection: enter the Nth child section by index (for enumeration loops)
 * PopSection: return to the previous position (undo last Push/Enter) */
int      TDF_PushSection(TDFFile *tdf, const char *section);
int      TDF_OpenSection(TDFFile *tdf, int index);
void     TDF_PopSection(TDFFile *tdf);

/* Debug / test accessors */
void    *TDF_GetRoot(TDFFile *tdf);

/* Value readers — search the current section's key-value pairs */
int         TDF_ReadInt(TDFFile *tdf, const char *key, int default_val);
float       TDF_ReadFloat(TDFFile *tdf, const char *key, float default_val);
const char *TDF_ReadString(TDFFile *tdf, const char *key, const char *default_val);
/* Read a comma-separated list value. Splits on ',', trims whitespace.
 * Returns 0 on success (key found, any number of items including zero),
 * -1 on error (missing key, bad args). Caller must free via
 * TDF_FreeStringList. *out_items is NULL and *out_count is 0 if no
 * items / key missing. */
int  TDF_ReadStringList(TDFFile *tdf, const char *key,
                        char ***out_items, int *out_count);
void TDF_FreeStringList(char **items, int count);

/* Iteration — enumerate sections/keys in the current section.
 * Returns name string (owned by TDF) or NULL when exhausted. A fresh
 * call to TDF_GetFirstSection or TDF_GetFirstKey resets the cursor. */
const char *TDF_GetFirstSection(TDFFile *tdf);
const char *TDF_GetNextSection(TDFFile *tdf);
const char *TDF_GetFirstKey(TDFFile *tdf);
const char *TDF_GetNextKey(TDFFile *tdf);

#endif