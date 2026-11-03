/* crash_report.c - fatal-signal handler: one line to stderr, optional
   backtrace, then _exit. async-signal-safe inside the handler. */
#include "crash_report.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <execinfo.h>
#endif

/* checked once in crash_report_init, not in the handler: getenv is not
   async-signal-safe */
static int g_backtrace;

static void handler(int sig) {
    /* write(2) only: malloc/stdio are off-limits in a signal handler */
    const char msg[] = "crash: signal ";
    const char tail[] = " received\n";
    char digit = (char)('0' + sig % 10);
    (void)write(2, msg, sizeof msg - 1);
    if (sig >= 10) (void)write(2, "1", 1);
    (void)write(2, &digit, 1);
    (void)write(2, tail, sizeof tail - 1);
    /* best-effort single backtrace; in a crashed process this may fail */
#ifdef _WIN32
    /* mingw has no execinfo; the line above is the whole report */
    (void)g_backtrace;
#else
    if (g_backtrace) {
        void *frames[32];
        int n = backtrace(frames, 32);
        if (n > 0) backtrace_symbols_fd(frames, n, 2);
    }
#endif
    _exit(139);
}

void crash_report_init(const char *argv0) {
    struct sigaction sa;
    (void)argv0; /* reserved for argv0 in the report line */
    g_backtrace = getenv("FLOWER_CRASH_BACKTRACE") != NULL;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    /* no SA_RESTART: crash paths must not resume whatever we were doing */
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
