/* The test harness, tested against itself.
 *
 * Every suite in the tree reports through include/test_framework.h, so the
 * line it prints per case is data the whole team reads. These cases pin the
 * shape of that line: a case that takes real time has to say so, and a case
 * that takes none has to say a smaller number. Without that second half a
 * harness printing a constant would look fine.
 *
 * The trick is that the harness writes to stdout, so to read what it wrote we
 * point file descriptor 1 at a temp file for the length of one inner case and
 * put it back afterwards. dup/dup2 rather than freopen, because ctest hands us
 * a pipe and there is no terminal to reopen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_framework.h"

#ifdef _WIN32
#include <io.h>
#define tf_dup   _dup
#define tf_dup2  _dup2
#define tf_close _close
#else
#include <unistd.h>
#define tf_dup   dup
#define tf_dup2  dup2
#define tf_close close
#endif

/* An inner case that costs a measurable amount of time. It spins on clock()
 * rather than sleeping so it burns the same kind of time a real case burns,
 * and the iteration cap means it always ends even where clock() sits still. */
TEST(inner_case_that_takes_a_while) {
    clock_t start = clock();
    long spins = 0;
    while (spins < 200000000L) {
        if ((double)(clock() - start) * (1000.0 / (double)CLOCKS_PER_SEC) >= 30.0) {
            break;
        }
        spins++;
    }
    ASSERT(spins >= 0);
}

/* An inner case that costs as close to nothing as a case can. */
TEST(inner_case_that_returns_at_once) {
    ASSERT(1);
}

/* An inner case that cannot meet its precondition. It checks nothing, so
 * the harness must not call it a pass. */
TEST(inner_case_that_skips) {
    SKIP("nothing to test here");
}

/* A helper that decides the case cannot run and hands that back rather
 * than returning out of the case itself, which is how setup_platform and
 * the other shared fixtures are written. */
static int a_helper_that_cannot_do_its_job(void) {
    SKIP_MARK("the fixture would not start");
    return -1;
}

TEST(inner_case_that_skips_inside_a_helper) {
    if (a_helper_that_cannot_do_its_job() != 0) return;
    ASSERT(0 && "the helper was supposed to fail");
}

/* Run one inner case with stdout captured, and hand back the line it printed.
 * The harness counters move while the inner case runs, so they are put back:
 * an inner case is scaffolding, not a result. Returns 0 on success. */
/* What the captured case did to the harness counters, since capture_case
 * puts them back before it returns. */
static int captured_pass_delta = 0;
static int captured_fail_delta = 0;
static int captured_skip_delta = 0;

static int capture_case(void (*run_inner)(void), char *out, size_t out_size) {
    int saved_pass = _tf_pass_count;
    int saved_fail = _tf_fail_count;
    int saved_skip = _tf_skip_count;
    int saved_total = _tf_total_count;
    int saved_named = _tf_skipped_named;
    int saved_cur_failed = _tf_current_failed;
    int saved_cur_skipped = _tf_current_skipped;
    int saved_fd = -1;
    FILE *tmp = NULL;
    const char *path = "test_framework_self.capture";
    size_t got;
    int ok = -1;

    tmp = fopen(path, "w+");
    if (!tmp) return -1;

    fflush(stdout);
    saved_fd = tf_dup(1);
    if (saved_fd < 0) goto done;
    if (tf_dup2(fileno(tmp), 1) < 0) goto done;

    run_inner();

    /* Push what the harness printed out of the stdout buffer and into the
     * file before reading any of it back. */
    fflush(stdout);
    rewind(tmp);
    got = fread(out, 1, out_size - 1, tmp);
    out[got] = '\0';
    ok = 0;

done:
    /* Put stdout back before anything else, including before reporting a
     * failure above: a test that cannot print is no use to anyone. */
    fflush(stdout);
    if (saved_fd >= 0) {
        tf_dup2(saved_fd, 1);
        tf_close(saved_fd);
    }
    if (tmp) fclose(tmp);
    remove(path);
    captured_pass_delta = _tf_pass_count - saved_pass;
    captured_fail_delta = _tf_fail_count - saved_fail;
    captured_skip_delta = _tf_skip_count - saved_skip;
    _tf_pass_count = saved_pass;
    _tf_fail_count = saved_fail;
    _tf_skip_count = saved_skip;
    _tf_total_count = saved_total;
    _tf_skipped_named = saved_named;
    _tf_current_failed = saved_cur_failed;
    _tf_current_skipped = saved_cur_skipped;
    return ok;
}

/* Pull the milliseconds out of a captured line, or -1 if it carries none. */
static double elapsed_in(const char *line) {
    const char *ms = strstr(line, " ms");
    const char *p;
    if (!ms) return -1.0;
    p = ms;
    while (p > line && (p[-1] == ' ' || p[-1] == '.' ||
                        (p[-1] >= '0' && p[-1] <= '9'))) {
        p--;
    }
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1.0;
    return atof(p);
}

TEST(a_passing_case_reports_how_long_it_took) {
    char line[512];
    double ms;
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_takes_a_while,
                                  line, sizeof line));
    ASSERT_NOT_NULL(strstr(line, "inner_case_that_takes_a_while"));
    ASSERT_NOT_NULL(strstr(line, "PASS"));
    ms = elapsed_in(line);
    ASSERT(ms >= 10.0);
}

TEST(a_case_that_does_nothing_reports_a_smaller_time) {
    char slow[512], quick[512];
    double slow_ms, quick_ms;
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_takes_a_while,
                                  slow, sizeof slow));
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_returns_at_once,
                                  quick, sizeof quick));
    slow_ms = elapsed_in(slow);
    quick_ms = elapsed_in(quick);
    ASSERT(slow_ms >= 10.0);
    ASSERT(quick_ms >= 0.0);
    ASSERT(quick_ms < slow_ms);
}

TEST(the_last_elapsed_time_is_there_for_a_caller_to_read) {
    char line[512];
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_takes_a_while,
                                  line, sizeof line));
    ASSERT(_tf_last_ms >= 10.0);
}

/* The one that matters most. Before this, a case that skipped printed its
 * reason and the harness then printed PASS over the top of it, so a run
 * with no data reported every case green. */
TEST(a_skipped_case_does_not_report_a_pass) {
    char line[512];
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_skips,
                                  line, sizeof line));
    ASSERT_NOT_NULL(strstr(line, "inner_case_that_skips"));
    ASSERT_NOT_NULL(strstr(line, "SKIP"));
    ASSERT_NOT_NULL(strstr(line, "nothing to test here"));
    ASSERT_NULL(strstr(line, "PASS"));
    ASSERT_EQ_INT(0, captured_pass_delta);
    ASSERT_EQ_INT(0, captured_fail_delta);
    ASSERT_EQ_INT(1, captured_skip_delta);
}

/* The shared fixtures mark the case from inside a helper, so that path
 * needs its own case. */
TEST(a_helper_can_mark_the_case_skipped) {
    char line[512];
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_skips_inside_a_helper,
                                  line, sizeof line));
    ASSERT_NOT_NULL(strstr(line, "SKIP"));
    ASSERT_NOT_NULL(strstr(line, "the fixture would not start"));
    ASSERT_NULL(strstr(line, "PASS"));
    ASSERT_EQ_INT(0, captured_pass_delta);
    ASSERT_EQ_INT(1, captured_skip_delta);
}

/* A skip has to be findable in the summary, not only in the line that
 * scrolled past a thousand lines ago. */
TEST(a_skipped_case_is_named_for_the_report) {
    char line[512];
    int before = _tf_skipped_named;
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_skips,
                                  line, sizeof line));
    /* capture_case puts the count back, so read the slot it wrote. */
    ASSERT_NOT_NULL(_tf_skipped_names[before]);
    ASSERT_EQ_STR("inner_case_that_skips", _tf_skipped_names[before]);
}

/* A pass still has to look like a pass. */
TEST(a_passing_case_is_not_counted_as_a_skip) {
    char line[512];
    ASSERT_EQ_INT(0, capture_case(_run_inner_case_that_returns_at_once,
                                  line, sizeof line));
    ASSERT_NOT_NULL(strstr(line, "PASS"));
    ASSERT_NULL(strstr(line, "SKIP"));
    ASSERT_EQ_INT(1, captured_pass_delta);
    ASSERT_EQ_INT(0, captured_skip_delta);
}

int main(void) {
    TEST_SUITE("Test harness");
    RUN(a_passing_case_reports_how_long_it_took);
    RUN(a_case_that_does_nothing_reports_a_smaller_time);
    RUN(the_last_elapsed_time_is_there_for_a_caller_to_read);
    RUN(a_skipped_case_does_not_report_a_pass);
    RUN(a_helper_can_mark_the_case_skipped);
    RUN(a_skipped_case_is_named_for_the_report);
    RUN(a_passing_case_is_not_counted_as_a_skip);
    TEST_REPORT();
}
