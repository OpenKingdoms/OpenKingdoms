#include "tak_tdf.h"
#include "tak_memory.h"
#include "tak_util.h"
#include "tak_hpi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define TDF_SECTION_HEADER_PREFIX '['
#define TDF_SECTION_HEADER_SUFFIX ']'
#define TDF_SECTION_OPEN_BRACE '{'
#define TDF_SECTION_CLOSE_BRACE '}'
#define TDF_KEY_VALUE_SEPARATOR '='
#define TDF_ENTRY_TERMINATOR ';'
#define TDF_COMMENT_PREFIX "//"

/* Forward declaration: defined further down in this file. */
int is_tdf_file_valid(const char *input);

/* ── Internal data structures ─────────────────────────────────────────────
 *
 * TDFEntry is a node in the parse tree. Each node is either:
 *   - A SECTION: key = section name, value = NULL, children = linked list of child entries
 *   - A KEY-VALUE: key = key name, value = value string, children = NULL
 *
 * Children are stored as a singly-linked list via the ->next pointer.
 * Siblings at the same level are chained: entry->next->next->next...
 * A section's children hang off entry->children, which is the FIRST child.
 *
 * Example tree for the FBI file above:
 *
 *   root (name="__root__", value=NULL)
 *    |
 *    children -> UNITINFO (value=NULL) ----next----> WEAPON1 (value=NULL)
 *                 |                                    |
 *                 children -> acceleration="10"        children -> aimtolerance="1024"
 *                              -> bloodcolor1="..."                -> model="Araarrow"
 *                              -> ...                              -> ...
 *                              -> watermultiplier="0.81"           -> DAMAGE (value=NULL)
 *                                                                      |
 *                                                                      children -> default="213"
 */
typedef struct TDFEntry {
    char *key;                    /* Key name (or section name) */
    char *value;                  /* Value string, NULL if this is a section */
    struct TDFEntry *children;    /* First child (sections only) */
    struct TDFEntry *next;        /* Next sibling at the same level */
} TDFEntry;

/* TDFFile holds the parsed tree plus navigation state.
 *
 * The nav_stack lets you push/pop your position in the tree:
 *   TDF_PushSection(tdf, "UNITINFO") → saves current_section, enters UNITINFO
 *   TDF_PopSection(tdf)              → restores previous current_section
 *
 * current_section is where TDF_ReadInt/ReadString/etc. search for keys. */
typedef struct TDFFile {
    char *filepath;               /* Source file path (owned, strdup'd) */
    char *raw_data;               /* Raw file text (owned, tak_malloc'd) */
    size_t raw_size;              /* Size of raw_data in bytes */

    TDFEntry *root;               /* Root node of parse tree */

    TDFEntry *nav_stack[32];      /* Navigation stack (saved positions) */
    int nav_depth;                /* Current stack depth (0 = at root) */
    TDFEntry *current_section;    /* Active section for Read* calls */

    /* Iteration cursors — GetFirst* sets these, GetNext* advances. */
    TDFEntry *iter_section;       /* walker for TDF_GetNextSection   */
    TDFEntry *iter_key;           /* walker for TDF_GetNextKey       */
} TDFFile;


/* ── Forward declarations for internal helpers ────────────────────────── */

static const char *find_key_value(TDFEntry *section, const char *key, const char *default_val);
static TDFEntry *find_child_section_by_name(TDFEntry *parent, const char *name);


/* ── TDF_Open ─────────────────────────────────────────────────────────────
 * Reads a TDF file into memory through the VFS and returns an empty
 * (unparsed) TDFFile. Call TDF_Load() next to parse the raw data into a
 * tree. Path is a VFS-relative path (forward slashes, case-insensitive)
 * — the VFS searches loaded HPI archives and loose data dirs. See
 * memory/feedback_vfs_only.md for the rationale. */
TDFFile *TDF_Open(const char *path) {
    TDFFile *tdf;
    char    *raw_buf;
    char    *path_copy;
    void    *vfs_data = NULL;
    uint32_t vfs_size = 0;

    if (!path) return NULL;
    if (VFS_ReadFile(path, &vfs_data, &vfs_size) != 0 || !vfs_data) {
        return NULL;
    }
    if (vfs_size == 0) {
        tak_free(vfs_data);
        return NULL;
    }

    tdf = (TDFFile *)tak_malloc(sizeof(TDFFile));
    if (!tdf) {
        tak_free(vfs_data);
        return NULL;
    }
    memset(tdf, 0, sizeof(TDFFile));

    /* We need a mutable, null-terminated copy for the parser. VFS_ReadFile
     * returns a tak_malloc'd buffer we could technically extend in-place,
     * but allocating fresh is cleaner (+1 for the terminator) and we free
     * vfs_data immediately after. */
    raw_buf = (char *)tak_malloc((size_t)vfs_size + 1);
    if (!raw_buf) {
        tak_free(tdf);
        tak_free(vfs_data);
        return NULL;
    }
    memcpy(raw_buf, vfs_data, vfs_size);
    raw_buf[vfs_size] = '\0';
    tak_free(vfs_data);

    path_copy = tak_strdup(path);

    tdf->filepath = path_copy;
    tdf->raw_data = raw_buf;
    tdf->raw_size = vfs_size;
    tdf->root = NULL;              /* Set by TDF_Load */
    tdf->nav_depth = 0;
    tdf->current_section = NULL;   /* Set by TDF_Load */

    return tdf;
}

// ── tdf_parse_string ────────────────────────────────────────────────────
// Parses raw TDF text into a tree of TDFEntry nodes.
//
// TDF files are LINE-ORIENTED. Each line is exactly one of:
//   "[SECTIONNAME]"     -> section header
//   "{"                 -> begin section body
//   "  key=value;"      -> key-value pair
//   "}"                 -> end section body
//   "// ..."            -> comment (skip)
//   ""                  -> blank line (skip)
//
// Because each line is one token, you do NOT need a per-character state
// machine. Strip leading whitespace, check the first character (or first
// two for "//"), and you know the line type. The for-loop can be replaced
// with line-level if/else-if branches.
//
// ─── STATE YOU NEED ───────────────────────────────────────────────────
//
//   curr_parent       — section we're currently adding children to (starts as root)
//   last_child        — tail of curr_parent->children list (for O(1) append)
//   pending_section   — section node created by "[...]", waiting for "{" to descend
//   parent_stack[32]  — saves curr_parent when we descend on "{"
//   tail_stack[32]    — saves last_child alongside it (so "}" can restore both)
//   depth             — index into the stacks (0 = at root level)
//
// ─── WHAT EACH LINE TYPE DOES ─────────────────────────────────────────
//
// BLANK / COMMENT:
//   -> skip, continue to next line
//
// "[SECTIONNAME]":
//   1. Allocate new_node, extract name between '[' and ']' into new_node->key
//   2. new_node->value = NULL   (NULL value = section node, not key-value)
//   3. new_node->children = NULL, new_node->next = NULL
//   4. Append new_node as child of curr_parent:
//        if (last_child == NULL)
//            curr_parent->children = new_node;    // first child
//        else
//            last_child->next = new_node;          // chain as sibling
//        last_child = new_node;
//   5. pending_section = new_node    // DON'T descend yet — wait for '{'
//
// "{":
//   1. Save state for later restore on '}':
//        parent_stack[depth] = curr_parent;
//        tail_stack[depth]   = last_child;
//        depth++;
//   2. Descend into the pending section:
//        curr_parent     = pending_section;
//        last_child      = NULL;       // new section has no children yet
//        pending_section = NULL;
//
// "key=value;":
//   1. Find '=' in the line. Everything before it (trimmed) is the key.
//      Everything after '=' up to ';' (trimmed) is the value.
//
//      char *p = line;  (after skipping whitespace)
//      char *eq = strchr(p, '=');
//      key:   p   .. eq  (trim trailing spaces)
//      value: eq+1 .. ';' (trim leading/trailing spaces, strip ';')
//
//   2. Allocate a TDFEntry:
//        kv->key      = tak_strdup(trimmed_key)
//        kv->value    = tak_strdup(trimmed_value)   // NON-NULL = leaf node
//        kv->children = NULL
//        kv->next     = NULL
//   3. Append as child of curr_parent (same if/else as "[SECTION]" step 4)
//
// "}":
//   1. Pop:
//        depth--;
//        curr_parent = parent_stack[depth];
//        last_child  = tail_stack[depth];
//
//      last_child now points to the section we just closed (it was appended
//      before we descended), so the next "[SIBLING]" will chain correctly
//      via last_child->next.
//
// ─── EXAMPLE WALKTHROUGH ──────────────────────────────────────────────
//
//   LINE                       ACTION
//   ────                       ──────
//   [HEADER]                   alloc HEADER, root->children = HEADER
//                              last_child = HEADER, pending = HEADER
//   {                          push(root, HEADER), descend into HEADER
//                              curr_parent = HEADER, last_child = NULL
//       campaignside=Aramon;   alloc kv("campaignside","Aramon")
//                              HEADER->children = kv, last_child = kv
//   }                          pop -> curr_parent = root, last_child = HEADER
//   [MISSION0]                 alloc MISSION0, HEADER->next = MISSION0
//                              last_child = MISSION0, pending = MISSION0
//   {                          push(root, MISSION0), descend into MISSION0
//       missionfile=map.ota;   MISSION0->children = kv1
//       [SUBSECTION]           alloc SUB, kv1->next = SUB, pending = SUB
//       {                      push(MISSION0, SUB), descend into SUB
//           key=val;           SUB->children = kv2
//       }                      pop -> curr_parent = MISSION0, last_child = SUB
//   }                          pop -> curr_parent = root, last_child = MISSION0
//
// ─── CRITICAL: CALLER MUST RECEIVE THE ROOT ───────────────────────────
//
// Right now TDF_Load passes tdf->root (NULL) and never gets the tree back.
// Simplest fix — change the signature to take a double pointer:
//
//   int tdf_parse_string(char *input, TDFEntry **root_out)
//     ...at the end: *root_out = root; return 0;
//
// Then in TDF_Load:
//   int rc = tdf_parse_string(tdf->raw_data, &tdf->root);
//   if (rc == 0) tdf->current_section = tdf->root;
//   return rc;
//
// ─── BUGS IN CURRENT CODE ─────────────────────────────────────────────
//
// 3. The '[' branch creates a node but never links it into the tree.
//    It needs the append-as-child-of-curr_parent logic from above.
//
// 4. Once you handle '[', break out of the char loop — otherwise you'll
//    also hit ']' and double-process. Same for '=' (handle the whole
//    key=value line, then break).
//
int tdf_parse_string(char *input, TDFEntry **root_out) {
    // TODO: The is_tdf_file_valid func actually gives back a number of different error codes,
    // would be nice to bubble those up at some point. But alas, bigger fish to fry. 
    if (is_tdf_file_valid(input) > 0) {
        return -1;
    }

    TDFEntry *root = root_out ? *root_out : NULL;
    if (!root) {
        root = tak_malloc(sizeof(TDFEntry));
        if (!root) {
            printf("Unable to parse TDF File - not enough memory available");
            return -1;
        }
        root->key = (char*)tak_malloc(9);
        strcpy(root->key, "__root__");
        root->value = NULL;
        root->next = NULL;
        root->children = NULL;
    }
    char *line = strtok(input, "\n");
    TDFEntry *curr_parent = root;
    TDFEntry *last_child = NULL;
    TDFEntry *pending_section = NULL;
    // Via these 2 stacks, we handle arbitrary subsection depths (up to 32.. which is a crazy level of nestedness.. don't do that
    // in your TDF files... lol)
    TDFEntry *parent_stack[32]; // For saving off curr_parent nodes each time we open a section up
    TDFEntry *child_stack[32]; // For saving off last_child nodes each time we open a section up
    int stack_depth = 0;

    while(line) {
        char *p = line;
        while(*p == ' ' || *p == '\t' || *p == '\r') p++;

        if (*p == '\0') {
            // blank line — skip
        } else if (p[0] == '/' && p[1] == '/') {
            // comment — skip
        } else if (*p == TDF_SECTION_HEADER_PREFIX) {
            TDFEntry *new_node = (TDFEntry*)tak_malloc(sizeof(TDFEntry));
            new_node->children = NULL;
            new_node->key = NULL;
            new_node->value = NULL;
            new_node->next = NULL;

            char *suffix = strchr(p, TDF_SECTION_HEADER_SUFFIX);
            if (suffix) {
                size_t str_len = suffix - p - 1;
                new_node->key = (char*)tak_malloc(str_len + 1);
                strncpy(new_node->key, p+1, str_len);
                new_node->key[str_len] = '\0';
                new_node->value = NULL;
            }

            // If we weren't parsing a child node, then assign this new section node to a parent as a child
            if (last_child == NULL) {
                curr_parent->children = new_node;
            } else { // Otherwise, assign this new section node as a sister node
                last_child->next = new_node;
            }
            last_child = new_node;
            pending_section = new_node;
            line = strtok(NULL, "\n"); continue;
        } else if (*p == TDF_SECTION_OPEN_BRACE) {
            /* A file from anywhere reaches this parser, a map pack
             * included, so a brace with no section before it or one
             * nested past the stack is ignored rather than trusted. */
            if (!pending_section ||
                stack_depth >= (int)(sizeof(parent_stack) / sizeof(parent_stack[0]))) {
                pending_section = NULL;
                line = strtok(NULL, "\n"); continue;
            }
            parent_stack[stack_depth] = curr_parent;
            child_stack[stack_depth] = last_child;

            curr_parent = pending_section;
            pending_section = NULL;
            last_child = NULL;

            stack_depth++;
            line = strtok(NULL, "\n"); continue;
        } else if (*p == TDF_SECTION_CLOSE_BRACE) {
            // We finished this section so let's go back up to the last known section state (last parent)
            if (stack_depth == 0) { pending_section = NULL; line = strtok(NULL, "\n"); continue; }
            stack_depth--;
            curr_parent = parent_stack[stack_depth];
            last_child = child_stack[stack_depth];
            line = strtok(NULL, "\n"); continue;
        } else if (strchr(p, TDF_KEY_VALUE_SEPARATOR)) {
            TDFEntry *new_node = (TDFEntry*)tak_malloc(sizeof(TDFEntry));
            new_node->children = NULL;
            new_node->key = NULL;
            new_node->value = NULL;
            new_node->next = NULL;

            // Split "key=value;" on the '=' separator.
            char *eq = strchr(p, TDF_KEY_VALUE_SEPARATOR);
            if (!eq) {
                tak_free(new_node);
                line = strtok(NULL, "\n");
                continue;
            }

            // Key = left side, trimmed.
            size_t key_raw_len = (size_t)(eq - p);
            char *key_scratch = (char *)tak_malloc(key_raw_len + 1);
            memcpy(key_scratch, p, key_raw_len);
            key_scratch[key_raw_len] = '\0';
            char *key = (char *)tak_malloc(key_raw_len + 1);
            trimwhitespace(key, key_raw_len + 1, key_scratch);
            tak_free(key_scratch);
            new_node->key = key;

            // Value = right side up to ';' (or end of line), trimmed.
            const char *v_start = eq + 1;
            const char *v_end = strchr(v_start, TDF_ENTRY_TERMINATOR);
            if (!v_end) v_end = v_start + strlen(v_start);
            size_t val_raw_len = (size_t)(v_end - v_start);

            char *val_scratch = (char *)tak_malloc(val_raw_len + 1);
            memcpy(val_scratch, v_start, val_raw_len);
            val_scratch[val_raw_len] = '\0';
            char *val = (char *)tak_malloc(val_raw_len + 1);
            trimwhitespace(val, val_raw_len + 1, val_scratch);
            tak_free(val_scratch);
            new_node->value = val;
            
            if (last_child == NULL) {
                curr_parent->children = new_node;
            } else {
                last_child->next = new_node;
            }
            last_child = new_node;
        }

        line = strtok(NULL, "\n"); continue;
    }

    if (root_out) *root_out = root;
    return 0;
}


/* Post-order free of a TDFEntry and all its descendants + siblings.
 * Iterative rather than recursive so huge deeply-nested files don't
 * blow the C stack. */
static void free_entry_tree(TDFEntry *root) {
    /* Simple hand-rolled pointer stack; plenty for any real TDF depth. */
    TDFEntry *stack[256];
    int sp = 0;
    if (!root) return;

    /* Seed with the root's first child (root itself is freed by caller). */
    TDFEntry *cursor = root;
    while (cursor || sp > 0) {
        if (cursor) {
            if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[sp++] = cursor;
            }
            cursor = cursor->children;
        } else {
            TDFEntry *e = stack[--sp];
            TDFEntry *sib = e->next;
            if (e->key)   tak_free(e->key);
            if (e->value) tak_free(e->value);
            tak_free(e);
            cursor = sib;
        }
    }
}

/* ── TDF_Close ────────────────────────────────────────────────────────────
 * Frees the TDFFile and all associated memory (parse tree + raw data + path). */
void TDF_Close(TDFFile *tdf) {
    if (!tdf) return;
    if (tdf->root)     free_entry_tree(tdf->root);
    if (tdf->raw_data) tak_free(tdf->raw_data);
    if (tdf->filepath) tak_free(tdf->filepath);
    tak_free(tdf);
}


/* ── TDF_Load ─────────────────────────────────────────────────────────────
 * Parses tdf->raw_data into a tree of TDFEntry nodes rooted at tdf->root.
 * After this call, current_section points to root and you can navigate. */
int TDF_Load(TDFFile *tdf) {
    if (!tdf) return -1;

    int result = tdf_parse_string(tdf->raw_data, &tdf->root);
    if (result == 0) {
        tdf->current_section = tdf->root;
    }
    return result;
}


/* ── TDF_PushSection ──────────────────────────────────────────────────────
 * Find a child section of the CURRENT section by name, save the current
 * position on the stack, and move the cursor into the found child.
 *
 * Example: if current_section is root and you call TDF_PushSection(tdf, "UNITINFO"),
 * it searches root->children for a section named "UNITINFO", saves root on the
 * stack, and sets current_section = UNITINFO. */
int TDF_PushSection(TDFFile *tdf, const char *section) {
    TDFEntry *found;

    if (!tdf || !tdf->current_section || !section) return -1;
    if (tdf->nav_depth >= 32) return -1;  /* Stack overflow protection */

    found = find_child_section_by_name(tdf->current_section, section);
    if (!found) return -1;

    tdf->nav_stack[tdf->nav_depth++] = tdf->current_section;
    tdf->current_section = found;
    return 0;
}


/* ── TDF_OpenSection ──────────────────────────────────────────────────────
 * Navigate to the Nth child SECTION of the current section (by index).
 * Only counts children that are sections (value == NULL), skips key-value pairs.
 *
 * This is used for enumeration:
 *   for (size_t i = 0; TDF_OpenSection(tdf, i); i++) {
 *       // process each child section
 *   }
 *
 * NOTE: This does NOT push the stack. If you need to return to the parent
 * after the loop, you should have used TDF_PushSection before the loop. */
int TDF_OpenSection(TDFFile *tdf, int index) {
    TDFEntry *child;
    int section_idx;

    if (!tdf || !tdf->current_section) return -1;

    child = tdf->current_section->children;
    section_idx = 0;

    while (child) {
        if (child->value == NULL) {  /* This child is a section, not a key-value */
            if (section_idx == index) {
                tdf->current_section = child;
                return 0;
            }
            section_idx++;
        }
        child = child->next;  /* Walk siblings, NOT children */
    }

    return -1;  /* No section at this index */
}


/* ── TDF_PopSection ──────────────────────────────────────────────────── */
void TDF_PopSection(TDFFile *tdf) {
	if (tdf && tdf->nav_depth > 0) {
		tdf->current_section = tdf->nav_stack[--tdf->nav_depth];
	}
}


/* ── Debug / test accessors ────────────────────── */
void *TDF_GetRoot(TDFFile *tdf) {
    return tdf ? tdf->root : NULL;
}

/* ── Value readers ─────────────────────────────── */
int TDF_ReadInt(TDFFile *tdf, const char *key, int default_val) {
	if (!tdf) return default_val;
    char str[12]; // Just need enough to store the string version
    sprintf(str, "%d", default_val);
	const char *val = find_key_value(tdf->current_section, key, str);

	return val ? atoi(val) : default_val;
}

float TDF_ReadFloat(TDFFile *tdf, const char *key, float default_val) {
	if (!tdf) return default_val;
    char str[32]; // Just need enough to store the string version
    snprintf(str, sizeof(str), "%.3f", default_val);
	const char *val = find_key_value(tdf->current_section, key, str);

	return val ? (float)atof(val) : default_val;
}

const char *TDF_ReadString(TDFFile *tdf, const char *key, const char *default_val) {
	if (!tdf) return default_val;
	return find_key_value(tdf->current_section, key, default_val);
}

int TDF_ReadStringList(TDFFile *tdf, const char *key,
                       char ***out_items, int *out_count) {
    if (!tdf || !key || !out_items || !out_count) return -1;
    *out_items = NULL;
    *out_count = 0;

    /* Sentinel default: "__missing__" lets us distinguish "not found"
     * from "empty value". */
    const char *sentinel = "__missing__";
    const char *value = find_key_value(tdf->current_section, key, sentinel);
    if (value == sentinel) return -1;  /* key not found */
    if (value == NULL || *value == '\0') return 0;  /* empty value, no items */

    /* Work on a scratch copy because strtok mutates its input, and we
     * don't want to corrupt the parse tree. */
    char *scratch = tak_strdup(value);
    if (!scratch) return -1;

    /* First pass: count commas so we can allocate exactly right. */
    int max_items = 1;
    for (const char *p = scratch; *p; p++) if (*p == ',') max_items++;

    char **items = (char **)tak_malloc((size_t)max_items * sizeof(char *));
    if (!items) { tak_free(scratch); return -1; }

    int count = 0;
    char *saveptr = NULL;
    /* MSVC has strtok_s; POSIX has strtok_r. strtok is non-reentrant and
     * we don't call ourselves recursively, so it's safe here. */
    (void)saveptr;
    char *token = strtok(scratch, ",");
    while (token != NULL) {
        size_t raw_len = strlen(token);
        /* trimwhitespace writes into out_buf of capacity raw_len+1 and
         * truncates to raw_len-1 chars. We need raw_len+1 for the full
         * trimmed string + null. */
        char *trimmed = (char *)tak_malloc(raw_len + 1);
        if (!trimmed) {
            /* clean up anything we've already claimed */
            for (int i = 0; i < count; i++) tak_free(items[i]);
            tak_free(items);
            tak_free(scratch);
            return -1;
        }
        trimwhitespace(trimmed, raw_len + 1, token);
        if (trimmed[0] == '\0') {
            /* skip empty/all-whitespace items */
            tak_free(trimmed);
        } else {
            items[count++] = trimmed;
        }
        token = strtok(NULL, ",");
    }

    tak_free(scratch);
    *out_items = items;
    *out_count = count;
    return 0;
}

void TDF_FreeStringList(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) tak_free(items[i]);
    tak_free(items);
}

/* ── Iteration ────────────────────────────────────────────────────────── */

const char *TDF_GetFirstSection(TDFFile *tdf) {
    if (!tdf || !tdf->current_section) return NULL;
    tdf->iter_section = tdf->current_section->children;
    while (tdf->iter_section && tdf->iter_section->value != NULL) {
        tdf->iter_section = tdf->iter_section->next;
    }
    return tdf->iter_section ? tdf->iter_section->key : NULL;
}

const char *TDF_GetNextSection(TDFFile *tdf) {
    if (!tdf || !tdf->iter_section) return NULL;
    tdf->iter_section = tdf->iter_section->next;
    while (tdf->iter_section && tdf->iter_section->value != NULL) {
        tdf->iter_section = tdf->iter_section->next;
    }
    return tdf->iter_section ? tdf->iter_section->key : NULL;
}

const char *TDF_GetFirstKey(TDFFile *tdf) {
    if (!tdf || !tdf->current_section) return NULL;
    tdf->iter_key = tdf->current_section->children;
    while (tdf->iter_key && tdf->iter_key->value == NULL) {
        tdf->iter_key = tdf->iter_key->next;
    }
    return tdf->iter_key ? tdf->iter_key->key : NULL;
}

const char *TDF_GetNextKey(TDFFile *tdf) {
    if (!tdf || !tdf->iter_key) return NULL;
    tdf->iter_key = tdf->iter_key->next;
    while (tdf->iter_key && tdf->iter_key->value == NULL) {
        tdf->iter_key = tdf->iter_key->next;
    }
    return tdf->iter_key ? tdf->iter_key->key : NULL;
}


/* ── Internal helpers ─────────────────────────────────────────────────── */

/* Walk the immediate children of 'parent' and return the first child
 * section whose name matches (case-insensitive). Only checks sections
 * (entries where value == NULL). Does NOT recurse into grandchildren.
 *
 *   parent->children -> [key="name", value="Archer"]   (skipped, has value)
 *                    -> [key="WEAPON1", value=NULL]     (checked!)
 *                    -> [key="WEAPON2", value=NULL]     (checked!)
 *                    -> NULL
 */
static TDFEntry *find_child_section_by_name(TDFEntry *parent, const char *name) {
    TDFEntry *child;

    if (!parent || !name) return NULL;

    child = parent->children;
    while (child) {
        if (child->value == NULL && tak_stricmp(child->key, name) == 0)
            return child;
        child = child->next;  /* Walk siblings at this level */
    }
    return NULL;
}

static const char *find_key_value(TDFEntry *section, const char *key, const char *default_val ) {
	if (!section) return default_val;
	TDFEntry *curr = section->children;

	while (curr) {
		if (curr->value != NULL && tak_stricmp(curr->key, key) == 0) {
			return curr->value;
		}

		curr = curr->next;
	}

	return default_val;
}

// 0 == Valid
// 1 == Invalid Section Header ([])
// 2 == Invalid Section Body ({})
// 3 == Missing Key/Value Terminator and/or Key/Value Separator (=;)
int is_tdf_file_valid(const char *input) {
    unsigned int total_bracket_count = 0;
    unsigned int total_brace_count = 0;
    unsigned int separator_lines = 0;
    unsigned int terminator_lines = 0;

    size_t input_length = strlen(input);
    for (size_t i=0; i < input_length; i++) {
        if (input[i] == TDF_SECTION_HEADER_PREFIX || input[i] == TDF_SECTION_HEADER_SUFFIX) total_bracket_count++;
        if (input[i] == TDF_SECTION_OPEN_BRACE || input[i] == TDF_SECTION_CLOSE_BRACE) total_brace_count++;
    }

    /* Separators and terminators are judged per line, comment lines
     * aside: a value may carry a '=' or a ';' of its own, as several
     * entries in english/translate/messages.tdf do. */
    const char *line = input;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *p = line;
        while (p < line + len && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        size_t rest = (size_t)(line + len - p);
        if (!(rest >= 2 && p[0] == '/' && p[1] == '/')) {
            if (memchr(p, TDF_KEY_VALUE_SEPARATOR, rest)) separator_lines++;
            if (memchr(p, TDF_ENTRY_TERMINATOR, rest)) terminator_lines++;
        }
        if (!end) break;
        line = end + 1;
    }

    if (total_bracket_count == 0 || total_bracket_count % 2 != 0) return 1;
    if (total_brace_count == 0 || total_brace_count % 2 != 0) return 2;
    if (separator_lines != terminator_lines) return 3;

    return 0;
}
