/* tak_test_trace.h - per-case function tracing for the coverage map.
 * Only built with TAK_TEST_TRACE=ON; see src/core/test_trace.c. */
#ifndef TAK_TEST_TRACE_H
#define TAK_TEST_TRACE_H

#ifdef __GNUC__
#define TAK_NOTRACE __attribute__((no_instrument_function))
#else
#define TAK_NOTRACE
#endif

TAK_NOTRACE void TAK_TestTrace_Begin(void);
TAK_NOTRACE void TAK_TestTrace_End(const char *case_name);

#endif
