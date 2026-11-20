// ALooper, just enough for android_native_app_glue: it adds its msg
// pipe + the input queue, then sits in ALooper_pollAll.
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "android/android.h"
#include "bionic/bionic.h"
#include "loader/exports.h"
#include "util/log.h"

#define ALOOPER_POLL_CALLBACK -2
#define ALOOPER_POLL_TIMEOUT  -3
#define ALOOPER_EVENT_INPUT    1
#define MAX_SOURCES 16

typedef int (*looper_cb)(int fd, int events, void *data);

typedef struct {
  int used;
  int fd;          // pipe fd, or -1 for the input queue
  void *queue;     // set if this is the input queue
  int ident;
  looper_cb cb;
  void *data;
} looper_src;

typedef struct {
  int magic;
  looper_src src[MAX_SOURCES];
} looper;

static looper the_looper = { 0x100BE5 };
static SceUID poke_sema = -1;

static void ensure_sema(void) {
  if (poke_sema < 0) poke_sema = sceKernelCreateSema("looper", 0, 0, 0x7FFFFFFF, NULL);
}

void looper_poke(void) {
  ensure_sema();
  sceKernelSignalSema(poke_sema, 1);
}

static looper *b_ALooper_prepare(int opts) {
  (void)opts;
  ensure_sema();
  return &the_looper;
}

static int add_src(looper *lp, int fd, void *queue, int ident, looper_cb cb, void *data) {
  for (int i = 0; i < MAX_SOURCES; i++) {
    looper_src *s = &lp->src[i];
    if (s->used && ((queue && s->queue == queue) || (!queue && s->fd == fd))) {
      s->ident = ident; s->cb = cb; s->data = data;
      return 1;
    }
  }
  for (int i = 0; i < MAX_SOURCES; i++) {
    looper_src *s = &lp->src[i];
    if (!s->used) {
      *s = (looper_src){ 1, fd, queue, ident, cb, data };
      return 1;
    }
  }
  L("looper: out of sources");
  return -1;
}

static int b_ALooper_addFd(looper *lp, int fd, int ident, int events, looper_cb cb, void *data) {
  L("ALooper_addFd(fd %d, ident %d, events %d, cb %p)", fd, ident, events, cb);
  return add_src(lp, fd, NULL, ident, cb, data);
}

// input.c calls these through AInputQueue_attach/detachLooper
int looper_attach_queue(void *lp, void *queue, int ident, void *cb, void *data) {
  return add_src(lp ? lp : &the_looper, -1, queue, ident, (looper_cb)cb, data);
}

void looper_detach_queue(void *queue) {
  for (int i = 0; i < MAX_SOURCES; i++)
    if (the_looper.src[i].used && the_looper.src[i].queue == queue) the_looper.src[i].used = 0;
}

static int src_ready(looper_src *s) {
  return s->queue ? input_pending(s->queue) : vpipe_available(s->fd) > 0;
}

static int b_ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data) {
  ensure_sema();
  SceUInt64 deadline = timeout_ms < 0 ? 0 : sceKernelGetProcessTimeWide() + (SceUInt64)timeout_ms * 1000;
  int ran_cb = 0;

  for (;;) {
    for (int i = 0; i < MAX_SOURCES; i++) {
      looper_src *s = &the_looper.src[i];
      if (!s->used || !src_ready(s)) continue;
      if (s->cb) {
        if (!s->cb(s->fd, ALOOPER_EVENT_INPUT, s->data)) s->used = 0;
        ran_cb = 1;
        continue;
      }
      if (out_fd) *out_fd = s->fd;
      if (out_events) *out_events = ALOOPER_EVENT_INPUT;
      if (out_data) *out_data = s->data;
      return s->ident;
    }
    if (ran_cb) return ALOOPER_POLL_CALLBACK;
    if (timeout_ms == 0) return ALOOPER_POLL_TIMEOUT;

    if (timeout_ms < 0) {
      sceKernelWaitSema(poke_sema, 1, NULL);
    } else {
      SceUInt64 now = sceKernelGetProcessTimeWide();
      if (now >= deadline) return ALOOPER_POLL_TIMEOUT;
      SceUInt32 wait = (SceUInt32)(deadline - now);
      if (sceKernelWaitSema(poke_sema, 1, &wait) < 0) return ALOOPER_POLL_TIMEOUT;
    }
  }
}

EXPORT_TABLE(looper) = {
  EXP_AS("ALooper_prepare", b_ALooper_prepare),
  EXP_AS("ALooper_addFd", b_ALooper_addFd),
  EXP_AS("ALooper_pollAll", b_ALooper_pollAll),
};
EXPORT_TABLE_END(looper);
