/*
 * test_util.c: the shared glob matcher.
 *
 * The engine used to match patterns with PathMatchSpecA on Windows and
 * fnmatch everywhere else. The table below is the contract the engine
 * now relies on, including the cases where those two disagreed.
 */

#include "test_framework.h"
#include "tak_util.h"
#include <stdio.h>

struct glob_case {
    const char *pattern;
    const char *path;
    int         want;
    const char *why;
};

/* Patterns the engine actually issues, taken from the unit build menus,
 * the map list, the feature directories and the archive listings. */
static const struct glob_case shipped[] = {
    { "*.hpi",                          "totala1.hpi",                 1, "archive list" },
    { "*.hpi",                          "Rev31.gp3",                   0, "wrong suffix" },
    { "*.ufo",                          "ccdata.ufo",                  1, "expansion archive" },
    { "data/canbuild/araking/*.tdf",    "data/canbuild/araking/1.tdf", 1, "build menu" },
    { "data/canbuild/araking/*.tdf",    "data/canbuild/verkeep/1.tdf", 0, "other menu" },
    { "data/features/corpses/*.tdf",    "data/features/corpses/arawar_dead.tdf", 1, "corpses" },
    { "maps/*.tnt",                     "maps/two castles.tnt",        1, "map with a space" },
    { "data/units/*.fbi",               "data/units/araking.fbi",      1, "unit list" },
    { "data/sounds/*.wav",              "data/sounds/select1.wav",     1, "sound class" },
    { "*",                              "araking.fbi",                 1, "bare star" },
    { "*",                              "",                            1, "star takes nothing" },
    { "",                               "",                            1, "empty matches empty" },
    { "",                               "a",                           0, "empty matches nothing else" },
};

/* Where PathMatchSpecA and fnmatch disagreed. The first group is the
 * separator rule: a star stays inside one path segment, which is what
 * fnmatch does with FNM_PATHNAME and what every caller here assumes.
 * The second group is case, where the Windows matcher folded and the
 * other did not. The game's own paths are written in every case, so
 * folding is the behaviour to keep. */
static const struct glob_case disagreements[] = {
    { "data/*.tdf",       "data/canbuild/araking/1.tdf", 0, "a star does not cross a separator" },
    { "data/*/*.tdf",     "data/canbuild/1.tdf",         1, "one segment each" },
    { "data/*/*.tdf",     "data/canbuild/araking/1.tdf", 0, "two segments is one too many" },
    { "*.tdf",            "sub/one.tdf",                 0, "leaf star stays in the leaf" },
    { "?ata/x.tdf",       "data/x.tdf",                  1, "question mark inside a segment" },
    { "data?x.tdf",       "data/x.tdf",                  0, "question mark is not a separator" },
    { "DATA/UNITS/*.FBI", "data/units/araking.fbi",      1, "pattern in caps" },
    { "data/units/*.fbi", "DATA/UNITS/ARAKING.FBI",      1, "path in caps" },
    { "data\\units\\*.fbi", "data/units/araking.fbi",    1, "backslash is a separator" },
    { "data/units/*.fbi", "data\\units\\araking.fbi",    1, "backslash in the path" },
};

/* A bracket is a character in a filename, not a class. fnmatch would
 * have read it as a class and matched the wrong thing. */
static const struct glob_case brackets[] = {
    { "map[1].tnt",  "map[1].tnt", 1, "bracket matches itself" },
    { "map[1].tnt",  "map1.tnt",   0, "bracket is not a class" },
};

static void run_table(const struct glob_case *cases, int n, const char *label) {
    for (int i = 0; i < n; i++) {
        int got = glob_path_match(cases[i].pattern, cases[i].path);
        if (!!got != cases[i].want) {
            printf("FAIL\n    %s[%d] %s: \"%s\" vs \"%s\" wanted %d got %d\n",
                   label, i, cases[i].why, cases[i].pattern, cases[i].path,
                   cases[i].want, got);
            _tf_fail_count++;
            _tf_current_failed = 1;
            return;
        }
    }
}

TEST(glob_matches_the_patterns_the_engine_issues) {
    run_table(shipped, (int)(sizeof(shipped) / sizeof(shipped[0])), "shipped");
}

TEST(glob_settles_where_the_two_libraries_disagreed) {
    run_table(disagreements,
              (int)(sizeof(disagreements) / sizeof(disagreements[0])),
              "disagreements");
}

TEST(glob_treats_a_bracket_as_a_character) {
    run_table(brackets, (int)(sizeof(brackets) / sizeof(brackets[0])), "brackets");
}

/* Many stars against a long path must not go exponential or recurse
 * deeply. This finishes at once with one backtrack point. */
TEST(glob_handles_a_pattern_full_of_stars) {
    char pattern[128];
    char path[512];
    int n = 0;
    for (int i = 0; i < 30; i++) { pattern[n++] = '*'; pattern[n++] = 'a'; }
    pattern[n] = '\0';
    for (int i = 0; i < 400; i++) path[i] = 'a';
    path[400] = '\0';
    ASSERT_EQ_INT(1, glob_path_match(pattern, path));
    path[399] = 'b';
    ASSERT_EQ_INT(0, glob_path_match(pattern, path));
}

TEST(glob_rejects_null_arguments) {
    ASSERT_EQ_INT(0, glob_path_match(NULL, "a"));
    ASSERT_EQ_INT(0, glob_path_match("a", NULL));
    ASSERT_EQ_INT(0, glob_path_match(NULL, NULL));
}

int main(void) {
    TEST_SUITE("Glob");
    RUN(glob_matches_the_patterns_the_engine_issues);
    RUN(glob_settles_where_the_two_libraries_disagreed);
    RUN(glob_treats_a_bracket_as_a_character);
    RUN(glob_handles_a_pattern_full_of_stars);
    RUN(glob_rejects_null_arguments);
    TEST_REPORT();
}
