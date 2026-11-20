#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "util/log.h"
#include "util/paths.h"

static SceUID log_fd = -1;
static SceKernelLwMutexWork log_lock;
static int log_ready;

void log_init(void) {
  sceIoMkdir(DATA_PATH, 0777);
  sceKernelCreateLwMutex(&log_lock, "log", 0, 0, NULL);
  log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  log_ready = 1;
}

void log_vprintf(const char *fmt, va_list ap) {
  char buf[1024];
  // timestamp in ms since boot, makes hangs way easier to spot
  unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000);
  int n = snprintf(buf, sizeof(buf), "[%6u.%03u] ", ms / 1000, ms % 1000);
  n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
  if (n > 0 && buf[n - 1] != '\n') buf[n++] = '\n';
  buf[n] = 0;

  sceClibPrintf("%s", buf);
  if (!log_ready) return;
  sceKernelLockLwMutex(&log_lock, 1, NULL);
  if (log_fd >= 0) sceIoWrite(log_fd, buf, n);
  sceKernelUnlockLwMutex(&log_lock, 1);
}

void log_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
}

void log_flush(void) {
  // sceIoWrite isn't buffered, nothing to do. here in case that changes.
}

void fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_printf("!!! FATAL !!!");
  log_vprintf(fmt, ap);
  va_end(ap);
  if (log_fd >= 0) sceIoClose(log_fd);
  sceKernelDelayThread(500 * 1000);
  sceKernelExitProcess(1);
  for (;;) {}
}
