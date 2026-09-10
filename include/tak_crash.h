#ifndef TAK_CRASH_H
#define TAK_CRASH_H

/* Print the exception code and a symbolised backtrace to stderr when the
 * process dies on a fault, then let the default handling finish. Call
 * once at startup. No-op on platforms without a backtrace API. */
void TAK_Crash_Install(void);

#endif
