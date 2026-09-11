/*
 * test_docs.c -- the shipped documents say what we actually built.
 *
 * Data free: it reads the repository's own markdown. Two jobs. It pins the
 * corrections made to docs/MULTIPLAYER.md, which described a design nobody
 * approved, and it keeps deployment detail out of a public repository.
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TAK_SOURCE_DIR
#define TAK_SOURCE_DIR "."
#endif

#define DOC_MAX (512u * 1024u)

static char doc_buf[DOC_MAX];

/* Read a repository file into doc_buf. Returns NULL when it is missing or
 * larger than the buffer, so a caller's ASSERT_NOT_NULL reports both. */
static const char *doc_read(const char *rel) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", TAK_SOURCE_DIR, rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("\n    cannot open %s\n", path);
        return NULL;
    }
    size_t n = fread(doc_buf, 1, DOC_MAX - 1, f);
    int too_big = !feof(f);
    fclose(f);
    if (too_big) return NULL;
    doc_buf[n] = '\0';
    return doc_buf;
}

static int has(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

/* Line numbers of prose lines carrying a banned mark, skipping fenced code
 * blocks. Returns the first offender's line number, or 0 when clean. */
static int prose_offender(const char *text, const char *mark, size_t mark_len) {
    int line = 1;
    int fenced = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= 3 && strncmp(p, "```", 3) == 0) {
            fenced = !fenced;
        } else if (!fenced) {
            for (size_t i = 0; i + mark_len <= len; i++) {
                if (memcmp(p + i, mark, mark_len) == 0) return line;
            }
        }
        if (!eol) break;
        p = eol + 1;
        line++;
    }
    return 0;
}

static void check_prose(const char *rel) {
    const char *text = doc_read(rel);
    if (!text) { printf("\n    missing %s\n", rel); return; }
    int semi = prose_offender(text, ";", 1);
    if (semi) printf("\n    %s:%d has a semicolon in prose\n", rel, semi);
    /* U+2014 EM DASH in UTF-8. */
    int dash = prose_offender(text, "\xe2\x80\x94", 3);
    if (dash) printf("\n    %s:%d has an em dash in prose\n", rel, dash);
    ASSERT(semi == 0);
    ASSERT(dash == 0);
}

/* ── The record the multiplayer document had wrong ───────────────────── */

TEST(multiplayer_doc_drops_the_fixed_point_claim) {
    const char *doc = doc_read("docs/MULTIPLAYER.md");
    ASSERT_NOT_NULL(doc);
    /* The old text stated 16.16 positions as a fact about today's tree. */
    ASSERT(!has(doc, "Positions are 16.16 fixed point"));
    ASSERT(has(doc, "They do not yet"));
}

TEST(multiplayer_doc_drops_the_command_layer_claim) {
    const char *doc = doc_read("docs/MULTIPLAYER.md");
    ASSERT_NOT_NULL(doc);
    /* The old text said every player action is already a command. */
    ASSERT(!has(doc, "Commands are already defined"));
    ASSERT(has(doc, "no caller anywhere"));
}

TEST(multiplayer_doc_names_the_websocket_transport) {
    const char *doc = doc_read("docs/MULTIPLAYER.md");
    ASSERT_NOT_NULL(doc);
    ASSERT(!has(doc, "Native clients use UDP"));
    ASSERT(has(doc, "one WebSocket for every client"));
}

TEST(multiplayer_doc_keeps_the_relay_free_of_game_data) {
    const char *doc = doc_read("docs/MULTIPLAYER.md");
    ASSERT_NOT_NULL(doc);
    ASSERT(has(doc, "never reads game data"));
}

TEST(multiplayer_doc_follows_the_prose_rules) {
    check_prose("docs/MULTIPLAYER.md");
}

int main(void) {
    TEST_SUITE("Shipped documents");
    RUN(multiplayer_doc_drops_the_fixed_point_claim);
    RUN(multiplayer_doc_drops_the_command_layer_claim);
    RUN(multiplayer_doc_names_the_websocket_transport);
    RUN(multiplayer_doc_keeps_the_relay_free_of_game_data);
    RUN(multiplayer_doc_follows_the_prose_rules);
    TEST_REPORT();
}
