#include "tak_crash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(_MSC_VER)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <crtdbg.h>
#pragma comment(lib, "dbghelp.lib")

static void print_backtrace(CONTEXT *ctx) {
    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 sf;
    DWORD machine;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);

    memset(&sf, 0, sizeof(sf));
#if defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset    = ctx->Eip;
    sf.AddrFrame.Offset = ctx->Ebp;
    sf.AddrStack.Offset = ctx->Esp;
#elif defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset    = ctx->Rip;
    sf.AddrFrame.Offset = ctx->Rbp;
    sf.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    sf.AddrPC.Offset    = ctx->Pc;
    sf.AddrFrame.Offset = ctx->Fp;
    sf.AddrStack.Offset = ctx->Sp;
#else
    SymCleanup(proc);
    return;
#endif
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 64; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        IMAGEHLP_LINE64 line;
        DWORD64 disp = 0;
        DWORD line_disp = 0;
        const char *name;

        if (!StackWalk64(machine, proc, GetCurrentThread(), &sf, ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (sf.AddrPC.Offset == 0) break;

        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        name = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
        line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &line_disp, &line))
            fprintf(stderr, "  #%02d %s  (%s:%lu)\n", i, name,
                    line.FileName, line.LineNumber);
        else
            fprintf(stderr, "  #%02d %s\n", i, name);
    }
    fflush(stderr);
    SymCleanup(proc);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    CONTEXT ctx = *ep->ContextRecord;
    fflush(stdout);
    fprintf(stderr, "\nCRASH: exception 0x%08lX at %p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    print_backtrace(&ctx);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_abort(int sig) {
    CONTEXT ctx;
    (void)sig;
    fflush(stdout);
    fprintf(stderr, "\nCRASH: abort()\n");
    RtlCaptureContext(&ctx);
    print_backtrace(&ctx);
}

void TAK_Crash_Install(void) {
    /* No dialogs: a hung box on a headless runner looks like a stall.
     * Faults, aborts and CRT asserts all land on stderr instead. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, crash_abort);
    SetUnhandledExceptionFilter(crash_filter);
}

#elif (defined(__GLIBC__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void crash_signal(int sig) {
    void *frames[64];
    int n;

    fflush(stdout);
    fprintf(stderr, "\nCRASH: signal %d (%s)\n", sig, strsignal(sig));
    fflush(stderr);
    n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

    signal(sig, SIG_DFL);
    raise(sig);
}

void TAK_Crash_Install(void) {
    signal(SIGSEGV, crash_signal);
    signal(SIGBUS, crash_signal);
    signal(SIGFPE, crash_signal);
    signal(SIGILL, crash_signal);
    signal(SIGABRT, crash_signal);
}

#else

void TAK_Crash_Install(void) {}

#endif
