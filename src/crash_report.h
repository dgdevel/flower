/* crash_report.h - fatal-signal handler producing a one-line crash
   report on stderr (docs/crash-report.md). */
#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

/* install SIGSEGV/SIGBUS/SIGFPE/SIGABRT handlers; call once from main */
void crash_report_init(const char *argv0);

#endif
