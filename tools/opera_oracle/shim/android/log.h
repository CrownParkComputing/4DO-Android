#ifndef ANDROID_LOG_SHIM_H
#define ANDROID_LOG_SHIM_H
#include <stdio.h>
#include <stdarg.h>
enum { ANDROID_LOG_VERBOSE=2, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR };
static inline int __android_log_print(int p, const char* tag, const char* fmt, ...) {
    (void)p; va_list a; va_start(a,fmt);
    fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a); return 0;
}
static inline int __android_log_vprint(int p, const char* tag, const char* fmt, va_list a) {
    (void)p; fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, a); fputc('\n', stderr); return 0;
}
#endif
