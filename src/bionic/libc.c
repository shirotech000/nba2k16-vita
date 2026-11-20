#define _GNU_SOURCE
// misc libc bits the game + gnustl pull in
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "loader/exports.h"
#include "loader/imports.h"
#include "loader/so_loader.h"
#include "util/abi.h"
#include "util/log.h"
#include "bionic/bionic.h"
#include "util/cpu.h"

// --- malloc with stats, memory is THE question for this port ---
static volatile int mem_live, mem_peak, mem_biggest, mem_fails, mem_count;

static void mem_add(void *p, size_t req) {
  if (!p) {
    __sync_add_and_fetch(&mem_fails, 1);
    L("malloc(%u) FAILED, live %uK peak %uK", (unsigned)req, mem_live / 1024, mem_peak / 1024);
    so_backtrace("malloc fail");
    return;
  }
  if (req >= 32 * 1024 * 1024) so_backtrace("huge alloc");
  int n = __sync_add_and_fetch(&mem_live, (int)malloc_usable_size(p));
  if (n > mem_peak) {
    // timeline: one line every 8MB of new peak
    static volatile int last_mark;
    int mark = n >> 23;
    if (mark > last_mark) {
      last_mark = mark;
      L("mem: new peak %uK (%u allocs live)", (unsigned)(n / 1024), mem_count + 1);
    }
    mem_peak = n;
  }
  __sync_add_and_fetch(&mem_count, 1);
  if ((int)req > mem_biggest) mem_biggest = req;
  if (req >= 1024 * 1024) {
    L("big alloc: %uK (live %uK)", (unsigned)(req / 1024), n / 1024);
    if (req < 32 * 1024 * 1024) so_backtrace("big alloc");
  }
}

static void mem_sub(void *p) {
  if (!p) return;
  __sync_sub_and_fetch(&mem_live, (int)malloc_usable_size(p));
  __sync_sub_and_fetch(&mem_count, 1);
}

void mem_report(void) {
  struct mallinfo mi = mallinfo();
  L("mem: live %uK peak %uK biggest %uK fails %d (arena %uK free %uK)",
    mem_live / 1024, mem_peak / 1024, mem_biggest / 1024, mem_fails,
    (unsigned)mi.arena / 1024, (unsigned)mi.fordblks / 1024);
}

static void *b_malloc(size_t n) { void *p = malloc(n); mem_add(p, n); return p; }
static void *b_calloc(size_t a, size_t b) { void *p = calloc(a, b); mem_add(p, a * b); return p; }
static void b_free(void *p) { mem_sub(p); free(p); }
static void *b_realloc(void *p, size_t n) {
  size_t old = p ? malloc_usable_size(p) : 0;
  void *q = realloc(p, n);
  if (q || !n) {
    __sync_sub_and_fetch(&mem_live, (int)old);
    if (q) mem_add(q, n);
  } else {
    mem_add(NULL, n);
  }
  return q;
}
static void *b_memalign(size_t a, size_t n) { void *p = memalign(a, n); mem_add(p, n); return p; }

// --- ctype: bionic reads _ctype_ as a pointer to a 1+256 BSD style table ---
static char ctype_table[257];
static const char *b_ctype_ = ctype_table;

static void ctype_init(void) {
  // same bit values in bionic and newlib (_U 1, _L 2, _N 4, _S 8, _P 0x10, _C 0x20, _X 0x40, _B 0x80)
  for (int c = 0; c < 256; c++) {
    char f = 0;
    if (c < 128) {
      if (isupper(c)) f |= _U;
      if (islower(c)) f |= _L;
      if (isdigit(c)) f |= _N;
      if (isspace(c)) f |= _S;
      if (ispunct(c)) f |= _P;
      if (iscntrl(c)) f |= _C;
      if (isxdigit(c) && !isdigit(c)) f |= _X;
      if (c == ' ') f |= _B;
    }
    ctype_table[1 + c] = f;
  }
}

// --- stack protector ---
static uintptr_t b_stack_chk_guard = 0x2B16D00D;
static void b_stack_chk_fail(void) {
  char w[96];
  fatal("stack smashing detected, caller %s", so_describe((uintptr_t)__builtin_return_address(0), w, sizeof(w)));
}

static void b_assert2(const char *file, int line, const char *func, const char *msg) {
  fatal("assert: %s:%d %s: %s", file, line, func, msg);
}

static void b_abort(void) {
  char w[96];
  fatal("abort() from %s", so_describe((uintptr_t)__builtin_return_address(0), w, sizeof(w)));
}

static int b_raise(int sig) {
  char w[96];
  fatal("raise(%d) from %s", sig, so_describe((uintptr_t)__builtin_return_address(0), w, sizeof(w)));
}

static void b_exit(int code) {
  L("game called exit(%d)", code);
  mem_report();
  sceKernelExitProcess(code);
}

// atexit stuff: we never exit cleanly so just swallow it
static int b_cxa_atexit(void (*f)(void *), void *arg, void *dso) { (void)f; (void)arg; (void)dso; return 0; }
static void b_cxa_finalize(void *dso) { (void)dso; }

// --- properties the game might peek at ---
static int b_system_property_get(const char *name, char *value) {
  static const struct { const char *k, *v; } props[] = {
    { "ro.product.model", "PS Vita" },
    { "ro.product.manufacturer", "Sony" },
    { "ro.product.brand", "Sony" },
    { "ro.product.device", "vita" },
    { "ro.build.version.sdk", "19" },
    { "ro.build.version.release", "4.4.4" },
    { "ro.hardware", "vita" },
    { "ro.product.cpu.abi", "armeabi-v7a" },
  };
  for (unsigned i = 0; i < ARRAY_LEN(props); i++) {
    if (!strcmp(name, props[i].k)) {
      strcpy(value, props[i].v);
      return strlen(value);
    }
  }
  L("__system_property_get(%s) -> empty", name);
  value[0] = 0;
  return 0;
}

static char *b_getenv(const char *n) { L("getenv(%s)", n); return NULL; }
static char *b_setlocale(int cat, const char *l) { (void)cat; (void)l; return "C"; }

static long b_sysconf(int name) {
  switch (name) {
    case 0x0027: return 4096;  // _SC_PAGESIZE
    case 0x0028: return 4096;  // _SC_PAGE_SIZE
    case 0x0060:               // _SC_NPROCESSORS_CONF
    case 0x0061: return cpu_count();  // _SC_NPROCESSORS_ONLN, 3 (4 with CapUnlocker)
    case 0x0006: return 100;   // _SC_CLK_TCK
    case 0x0062: return 64 * 1024 * 1024 / 4096; // _SC_PHYS_PAGES, lie a bit
  }
  L("sysconf(0x%X) unknown", name);
  return -1;
}

static int b_syscall(int n, ...) {
  if (n == 224) return sceKernelGetThreadId(); // gettid
  L("syscall(%d) not supported", n);
  errno = ENOSYS;
  return -1;
}

static int b_prctl(int op, ...) { (void)op; return 0; }

// --- time ---
// bionic struct tm has 2 extra fields at the end, keep our own statics
static b_tm tm_buf;
static b_tm *fill_tm(b_tm *out, const struct tm *in) {
  if (!in) return NULL;
  memcpy(out, in, sizeof(struct tm));
  out->tm_gmtoff = 0;
  out->tm_zone = "UTC";
  return out;
}
static b_tm *b_gmtime(const time_t *t) { return fill_tm(&tm_buf, gmtime(t)); }
static b_tm *b_localtime(const time_t *t) { return fill_tm(&tm_buf, localtime(t)); }
static b_tm *b_localtime_r(const time_t *t, b_tm *out) {
  struct tm tmp;
  return fill_tm(out, localtime_r(t, &tmp));
}
static size_t b_strftime(char *s, size_t max, const char *fmt, const b_tm *tm) {
  return strftime(s, max, fmt, (const struct tm *)tm);
}

static int b_clock_gettime(int clk, struct timespec *ts) {
  if (clk == 0) {  // CLOCK_REALTIME
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
  } else {         // monotonic / boottime / whatever, all the same to us
    SceUInt64 us = sceKernelGetProcessTimeWide();
    ts->tv_sec = us / 1000000;
    ts->tv_nsec = (us % 1000000) * 1000;
  }
  return 0;
}

static int b_nanosleep(const struct timespec *req, struct timespec *rem) {
  SceUInt64 us = (SceUInt64)req->tv_sec * 1000000 + req->tv_nsec / 1000;
  sceKernelDelayThread(us > 0xFFFFFFFF ? 0xFFFFFFFF : (SceUInt32)us);
  if (rem) rem->tv_sec = rem->tv_nsec = 0;
  return 0;
}

static int b_usleep(unsigned us) { sceKernelDelayThread(us); return 0; }
static int b_sched_yield(void) { sceKernelDelayThread(0); return 0; }

// --- strings that touch floats ---
static SOFTFP double b_strtod(const char *s, char **end) { return strtod(s, end); }

static int b_vasprintf(char **out, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  *out = b_malloc(n + 1);
  if (!*out) return -1;
  return vsnprintf(*out, n + 1, fmt, ap);
}

static int b_strcoll(const char *a, const char *b) { return strcmp(a, b); }

// --- dl ---
static void *b_dlsym(void *handle, const char *name) {
  (void)handle;
  uintptr_t a = imports_lookup(name);
  if (!a) a = so_symbol_any(name);
  L("dlsym(%s) -> %p", name, (void *)a);
  return (void *)a;
}

// gnustl's unwinder asks us where the exidx table for a pc lives
static uintptr_t b_gnu_unwind_find_exidx(uintptr_t pc, int *count) {
  return so_find_exidx(pc, count);
}

void libc_init(void) {
  ctype_init();
}

EXPORT_TABLE(libc) = {
  EXP_AS("malloc", b_malloc),
  EXP_AS("calloc", b_calloc),
  EXP_AS("free", b_free),
  EXP_AS("realloc", b_realloc),
  EXP_AS("memalign", b_memalign),
  EXP(memcpy), EXP(memmove), EXP(memset), EXP(memcmp), EXP(memchr), EXP(memmem),
  EXP(strlen), EXP(strcmp), EXP(strncmp), EXP(strncpy), EXP(strchr), EXP(strerror),
  EXP(strxfrm), EXP(strtol), EXP(strtoul),
  EXP_AS("strcoll", b_strcoll),
  EXP_AS("strtod", b_strtod),
  EXP(snprintf), EXP(sprintf), EXP(vsprintf), EXP(sscanf),
  EXP_AS("vasprintf", b_vasprintf),
  EXP(isalnum), EXP(isspace),
  EXP_AS("_ctype_", b_ctype_),
  EXP(__errno),
  EXP_AS("__stack_chk_guard", b_stack_chk_guard),
  EXP_AS("__stack_chk_fail", b_stack_chk_fail),
  EXP_AS("__assert2", b_assert2),
  EXP_AS("abort", b_abort),
  EXP_AS("raise", b_raise),
  EXP_AS("exit", b_exit),
  EXP_AS("_exit", b_exit),
  EXP_AS("__cxa_atexit", b_cxa_atexit),
  EXP_AS("__cxa_finalize", b_cxa_finalize),
  EXP_AS("__system_property_get", b_system_property_get),
  EXP_AS("getenv", b_getenv),
  EXP_AS("setlocale", b_setlocale),
  EXP_AS("sysconf", b_sysconf),
  EXP_AS("syscall", b_syscall),
  EXP_AS("prctl", b_prctl),
  EXP(time),
  EXP(gettimeofday),
  EXP_AS("gmtime", b_gmtime),
  EXP_AS("localtime", b_localtime),
  EXP_AS("localtime_r", b_localtime_r),
  EXP_AS("strftime", b_strftime),
  EXP_AS("clock_gettime", b_clock_gettime),
  EXP_AS("nanosleep", b_nanosleep),
  EXP_AS("usleep", b_usleep),
  EXP_AS("sched_yield", b_sched_yield),
  EXP_AS("dlsym", b_dlsym),
  EXP_AS("__gnu_Unwind_Find_exidx", b_gnu_unwind_find_exidx),
};
EXPORT_TABLE_END(libc);
