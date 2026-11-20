#pragma once
#include <stdarg.h>

// logs go to ux0:data/nba2k16/port.log (+ the debug console)
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);
void log_flush(void);

#define L(...) log_printf(__VA_ARGS__)

// print once per call site, handy for stubs that get hammered every frame
#define L_ONCE(...) do { static int _once; if (!_once) { _once = 1; L(__VA_ARGS__); } } while (0)

void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
