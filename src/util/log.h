#pragma once
#include <stdarg.h>

// logs go to ux0:data/nba2k16/port.log (+ the debug console). the file
// side is buffered and written by a low prio thread every ~100ms, unless
// sync_log=1 (then every line hits the card right away, like before).
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);
// write out whatever's buffered, blocks on card io
void log_flush(void);

// crash handler only: no locks, no newlib, tiny stack
void log_flush_unlocked(void);
void log_write_raw(const char *buf, int len);

#define L(...) log_printf(__VA_ARGS__)

// print once per call site, handy for stubs that get hammered every frame
#define L_ONCE(...) do { static int _once; if (!_once) { _once = 1; L(__VA_ARGS__); } } while (0)

void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
