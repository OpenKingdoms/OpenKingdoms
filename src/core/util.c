#include "tak_util.h"
#include "tak_memory.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
/* On non-Windows there is no GetCommandLineA. The cmdline parsing functions
   below short-circuit on a NULL command line, so providing a NULL stub keeps
   them safe no-ops on POSIX (sufficient for tests that don't drive cmdline
   parsing, and main.c is Win32-only anyway). */
   // TODO: implement POSIX command line parsing for better test coverage on that platform.
static char *GetCommandLineA(void) { return NULL; }
#endif

/* One matcher, every platform. The Windows build used to call
 * PathMatchSpecA and everyone else fnmatch, and the two disagree: the
 * Windows one lets a star cross a directory separator and folds case,
 * the other does neither. A pattern that found files on a developer's
 * machine could quietly find none in the browser.
 *
 * The rules here are the ones the engine's own patterns need:
 *   *   any run of characters, but never a separator
 *   ?   exactly one character, but never a separator
 *   everything else is a literal
 * Matching folds case and treats a backslash as a separator, because
 * the game's own paths are written both ways and in every case. There
 * are no character classes: no shipped pattern uses one, and a stray
 * bracket in a filename should match itself.
 *
 * Iterative with one backtrack point, so a long path against a pattern
 * full of stars cannot blow the stack or go exponential. */
static int glob_norm(char c) {
    if (c == '\\') return '/';
    if (c >= 'A' && c <= 'Z') return c + 32;
    return (unsigned char)c;
}

static int glob_char_eq(char pc, char sc) {
    if (pc == '?') return glob_norm(sc) != '/';
    return glob_norm(pc) == glob_norm(sc);
}

int glob_path_match(const char *pattern, const char *str) {
    if (!pattern || !str) return 0;
    const char *p = pattern, *s = str;
    const char *star_p = NULL, *star_s = NULL;
    while (*s) {
        if (*p == '*') {
            star_p = ++p;      /* remember where to resume */
            star_s = s;
            continue;
        }
        if (*p && glob_char_eq(*p, *s)) { p++; s++; continue; }
        /* Give the last star one more character, unless that character
         * is a separator: a star stays inside its path segment. */
        if (star_p && glob_norm(*star_s) != '/') {
            p = star_p;
            s = ++star_s;
            continue;
        }
        return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

// Normalize a path: backslashes -> forward slashes, letters lowercased.
// Returns a tak_strdup'd copy that the caller must release with tak_free.
char *normalize_path(const char *path) {
    if (!path) return NULL;
    char *out = tak_strdup(path);
    if (!out) return NULL;
    for (char *p = out; *p; p++) {
        if (*p == '\\') *p = '/';
        *p = (char)tolower((unsigned char)*p);
    }
    return out;
}

// Stores the trimmed input string into the given output buffer, which must be
// large enough to store the result.  If it is too small, the output is
// truncated.
size_t trimwhitespace(char *out, size_t len, const char *str)
{
  if(len == 0)
    return 0;

  const char *end;
  size_t out_size;

  // Trim leading space
  while(isspace((unsigned char)*str)) str++;

  if(*str == 0)  // All spaces?
  {
    *out = 0;
    return 1;
  }

  // Trim trailing space
  end = str + strlen(str) - 1;
  while(end > str && isspace((unsigned char)*end)) end--;
  end++;

  // Set output size to minimum of trimmed string length and buffer size minus 1
  out_size = (end - str) < len-1 ? (end - str) : len-1;

  // Copy trimmed string and add null terminator
  memcpy(out, str, out_size);
  out[out_size] = 0;

  return out_size;
}

/* Search the process command line for a flag/argument string (case-insensitive).
 * Returns pointer to the character AFTER the match (i.e. where the value starts),
 * or NULL if not found.
 *
 * A match is only accepted at a word boundary: the character immediately after
 * the flag must be end-of-string, whitespace, or '='.
 *
 * Example: find_cmdline_arg("-memset") on "game.exe -memset 0xDEADBEEF"
 *          returns pointer to " 0xDEADBEEF" (the space after "-memset"). */
char* find_cmdline_arg(const char *flag_to_find) {
  char current_char;
  char* cmdline_cursor;
  int compare_result;
  unsigned int strlen_countdown;
  int flag_length;
  char* temp_ptr;

  if (flag_to_find == NULL) {
    return NULL;
  }

  cmdline_cursor = GetCommandLineA();
  if (cmdline_cursor == NULL) {
    return NULL;
  }

  /* compute strlen(flag_to_find) via inline loop */
  strlen_countdown = 0xffffffff;
  temp_ptr = flag_to_find;
  do {
    if (strlen_countdown == 0) break;
    strlen_countdown = strlen_countdown - 1;
    current_char = *temp_ptr;
    temp_ptr = temp_ptr + 1;
  } while (current_char != '\0');
  flag_length = ~strlen_countdown - 1;

  if ((flag_length == 0) || (*cmdline_cursor == '\0')) {
    return NULL;
  }

  /* slide through the command line one character at a time */
  do {
    compare_result = tak_strnicmp(cmdline_cursor, flag_to_find, flag_length);
    if (compare_result == 0) {
      /* flag text matched -- check for word boundary after it */
      current_char = cmdline_cursor[flag_length];
      if (current_char == '\0') {
at_word_boundary:
        return cmdline_cursor + flag_length;   /* return pointer past the flag */
      }
      compare_result = isspace((int)current_char);
      if ((compare_result != 0) || (current_char == '=')) goto at_word_boundary;
      /* not a word boundary -- e.g. "-memfussy" should not match "-mem" */
    }
    cmdline_cursor = cmdline_cursor + 1;
    if (*cmdline_cursor == '\0') {
      return NULL;
    }
  } while( true );
}

/* Check the command line for an enable/disable flag pair.
 *
 * Each "feature" can have up to two enable flags and two disable flags
 * (the second of each is optional -- pass NULL to skip).
 *
 * Priority:  disable flags win over enable flags.
 *   - If a disable flag is found:  returns 0  (forced off)
 *   - If an enable flag is found:  returns 1  (forced on)
 *   - If neither is found:         returns default_value
 *
 * The first two params (unused_name, unused_ver) are vestigial debug
 * identifiers that were stripped in the release build.
 *
 * Example:
 *   check_cmdline_flag_pair("memfussy", 1, 0,
 *       "-memfussy", "-memnofussy", "-memfrontalign", NULL)
 *
 *   Command line "-memfussy"      -> returns 1
 *   Command line "-memnofussy"    -> returns 0
 *   Command line (neither)        -> returns 0 (the default_value)
 * 
    -memfussy          Enable VirtualAlloc debug allocator
    -memnofussy        Disable it (override)
    -memfrontalign     Put guard pages in front (vs back) of allocations
    -memset            Fill allocations with debug pattern (default 0xDEADBEEF)
    -memset 0xFF00FF00 Fill with custom hex value
    -memnoset          Disable debug fill
    -gonzo             Unknown debug mode (default on, no disable flag)
    -fpufussy          Enable FPU exception checking
    -fpunofussy        Disable FPU exceptions
    -enableimagehlp    Enable IMAGEHLP.DLL for stack traces in crash dumps
    -disableimagehlp   Disable it
    -enableimagehlplines   Include source line info in stack traces
    -disableimagehlplines  Disable line info 
 * 
 * 
 * 
 * 
 * 
 * */
uint8_t check_cmdline_flag_pair(
    const char *unused_name,
    uint32_t    unused_ver,
    uint8_t     default_value,
    const char *enable_flag_1,
    const char *disable_flag_1,
    const char *enable_flag_2,
    const char *disable_flag_2
) {
  char *enable_found;
  char *disable_found;
  uint8_t result;

  result = default_value;

  /* Step 1: check if either enable flag is on the command line */
  enable_found = find_cmdline_arg(enable_flag_1);
  if (enable_found == NULL) {
    enable_found = find_cmdline_arg(enable_flag_2);
  }
  if (enable_found != NULL) {
    result = 1;                  /* an enable flag was found */
  }

  /* Step 2: check if either disable flag is on the command line */
  disable_found = find_cmdline_arg(disable_flag_1);
  if (disable_found == NULL) {
    disable_found = find_cmdline_arg(disable_flag_2);
  }
  if (disable_found != NULL) {
    return 0;                    /* disable overrides everything */
  }

  return result;
}



/* Read an integer value from a command-line flag.
 *
 * Searches the command line for `flag_to_search`, then parses the text
 * immediately after it as an integer. Supports both decimal and hex (0x prefix).
 * Whitespace and '=' between the flag and value are skipped.
 *
 * Returns `default_value` if the flag is not present on the command line.
 *
 * The first two params (unused_name, unused_fallback) are vestigial debug
 * identifiers stripped in the release build.
 *
 * Example: command line "game.exe -memset 0xDEADBEEF"
 *   read_cmdline_int_value("setvalue", 0xdeadbeef, 0xdeadbeef, "-memset")
 *   -> skips spaces after "-memset", sees "0x", parses hex, returns 0xDEADBEEF */
uint32_t read_cmdline_int_value(
    const char *unused_name,
    uint32_t    unused_fallback,
    uint32_t    default_value,
    const char *flag_to_search
) {
  char current_char;
  char *value_text;
  uint32_t parsed_result;

  parsed_result = default_value;

  value_text = find_cmdline_arg(flag_to_search);
  if (value_text == NULL) {
    return parsed_result;        /* flag not found, return default */
  }

  /* skip whitespace and '=' between the flag and its value */
  current_char = *value_text;
  while (current_char == ' ' || current_char == '\t' || current_char == '=') {
    value_text = value_text + 1;
    current_char = *value_text;
  }

  /* parse the value as hex or decimal */
  if ((*value_text == '0') && ((value_text[1] == 'x' || (value_text[1] == 'X')))) {
    sscanf(value_text, "%x", &parsed_result);         
  } else {
    sscanf(value_text, "%d", &parsed_result);
  }

  return parsed_result;
}
