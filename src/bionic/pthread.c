// bionic pthread -> vitasdk pthread (pthread-embedded).
// bionic's mutex/cond are a single int, we stash a pointer to a real one
// in there the first time it's touched.

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <psp2/kernel/threadmgr.h>

#include "loader/exports.h"
#include "util/log.h"
#include "bionic/bionic.h"
#include "util/cpu.h"

#define DEFAULT_STACK (1024 * 1024)

// bionic 32-bit layouts
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} b_attr_t;
_Static_assert(sizeof(b_attr_t) == 24, "bionic pthread_attr_t is 24 bytes");

#define B_ATTR_DETACHED 1
#define B_MUTEX_NORMAL 0
#define B_MUTEX_RECURSIVE 1
#define B_MUTEX_ERRORCHECK 2

// static initializer values bionic bakes into the game
#define B_MUTEX_INIT_NORMAL     0
#define B_MUTEX_INIT_RECURSIVE  0x4000
#define B_MUTEX_INIT_ERRORCHECK 0x8000

static int is_static_mutex(int v) {
  return v == B_MUTEX_INIT_NORMAL || v == B_MUTEX_INIT_RECURSIVE || v == B_MUTEX_INIT_ERRORCHECK;
}

static pthread_mutex_t *make_mutex(int type) {
  pthread_mutex_t *m = malloc(sizeof(*m));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, type == B_MUTEX_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE
                              : type == B_MUTEX_ERRORCHECK ? PTHREAD_MUTEX_ERRORCHECK
                                                           : PTHREAD_MUTEX_NORMAL);
  pthread_mutex_init(m, &a);
  pthread_mutexattr_destroy(&a);
  return m;
}

static pthread_mutex_t *get_mutex(volatile int *slot) {
  int v = *slot;
  if (!is_static_mutex(v)) return (pthread_mutex_t *)v;
  int type = v == B_MUTEX_INIT_RECURSIVE ? B_MUTEX_RECURSIVE
           : v == B_MUTEX_INIT_ERRORCHECK ? B_MUTEX_ERRORCHECK : B_MUTEX_NORMAL;
  pthread_mutex_t *m = make_mutex(type);
  if (!__sync_bool_compare_and_swap(slot, v, (int)m)) {
    // someone beat us to it
    pthread_mutex_destroy(m);
    free(m);
  }
  return (pthread_mutex_t *)*slot;
}

static int b_mutex_init(volatile int *slot, const int *attr) {
  *slot = (int)make_mutex(attr ? *attr : B_MUTEX_NORMAL);
  return 0;
}

static int b_mutex_destroy(volatile int *slot) {
  int v = *slot;
  if (!is_static_mutex(v)) {
    pthread_mutex_destroy((pthread_mutex_t *)v);
    free((void *)v);
  }
  *slot = 0;
  return 0;
}

static int b_mutex_lock(volatile int *slot) { return pthread_mutex_lock(get_mutex(slot)); }
static int b_mutex_unlock(volatile int *slot) { return pthread_mutex_unlock(get_mutex(slot)); }
static int b_mutex_trylock(volatile int *slot) {
  return pthread_mutex_trylock(get_mutex(slot)) ? EBUSY : 0;
}

static int b_mutexattr_init(int *a) { *a = B_MUTEX_NORMAL; return 0; }
static int b_mutexattr_destroy(int *a) { (void)a; return 0; }
static int b_mutexattr_settype(int *a, int type) { *a = type; return 0; }
static int b_mutexattr_setpshared(int *a, int p) { (void)a; (void)p; return 0; }

// --- conds, same trick, static init is 0 ---
static pthread_cond_t *make_cond(void) {
  pthread_cond_t *c = malloc(sizeof(*c));
  pthread_cond_init(c, NULL);
  return c;
}

static pthread_cond_t *get_cond(volatile int *slot) {
  int v = *slot;
  if (v) return (pthread_cond_t *)v;
  pthread_cond_t *c = make_cond();
  if (!__sync_bool_compare_and_swap(slot, 0, (int)c)) {
    pthread_cond_destroy(c);
    free(c);
  }
  return (pthread_cond_t *)*slot;
}

static int b_cond_init(volatile int *slot, const int *attr) {
  (void)attr;
  *slot = (int)make_cond();
  return 0;
}

static int b_cond_destroy(volatile int *slot) {
  int v = *slot;
  if (v) {
    pthread_cond_destroy((pthread_cond_t *)v);
    free((void *)v);
  }
  *slot = 0;
  return 0;
}

static int b_cond_wait(volatile int *c, volatile int *m) {
  return pthread_cond_wait(get_cond(c), get_mutex(m));
}

static int b_cond_timedwait(volatile int *c, volatile int *m, const struct timespec *abstime) {
  // vitasdk time_t is 32-bit like bionic's, so the timespec passes straight through
  int r = pthread_cond_timedwait(get_cond(c), get_mutex(m), abstime);
  return r == ETIMEDOUT ? B_ETIMEDOUT : r;
}

static int b_cond_signal(volatile int *c) { return pthread_cond_signal(get_cond(c)); }
static int b_cond_broadcast(volatile int *c) { return pthread_cond_broadcast(get_cond(c)); }

static int b_condattr_init(int *a) { *a = 0; return 0; }
static int b_condattr_destroy(int *a) { (void)a; return 0; }
static int b_condattr_setpshared(int *a, int p) { (void)a; (void)p; return 0; }

// --- attrs ---
static int b_attr_init(b_attr_t *a) {
  memset(a, 0, sizeof(*a));
  a->stack_size = DEFAULT_STACK;
  a->guard_size = 4096;
  return 0;
}
static int b_attr_destroy(b_attr_t *a) { (void)a; return 0; }
static int b_attr_setdetachstate(b_attr_t *a, int s) {
  if (s) a->flags |= B_ATTR_DETACHED; else a->flags &= ~B_ATTR_DETACHED;
  return 0;
}
static int b_attr_setstacksize(b_attr_t *a, size_t sz) { a->stack_size = sz; return 0; }
static int b_attr_setschedparam(b_attr_t *a, const struct sched_param *p) {
  a->sched_priority = p->sched_priority;
  return 0;
}

// --- threads ---
typedef struct {
  void *(*fn)(void *);
  void *arg;
} thread_start;

static void *thread_tramp(void *p) {
  thread_start ts = *(thread_start *)p;
  free(p);
  cpu_place_game_thread();  // core 3 too when CapUnlocker is around
  return ts.fn(ts.arg);
}

static volatile int thread_count;

static int b_pthread_create(pthread_t *t, const b_attr_t *battr, void *(*fn)(void *), void *arg) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  size_t stack = battr && battr->stack_size ? battr->stack_size : DEFAULT_STACK;
  // our shims put a few KB of buffers on game stacks, so don't go tiny
  if (stack < 256 * 1024) stack = 256 * 1024;
  pthread_attr_setstacksize(&a, stack);
  if (battr && (battr->flags & B_ATTR_DETACHED))
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

  thread_start *ts = malloc(sizeof(*ts));
  ts->fn = fn;
  ts->arg = arg;
  int r = pthread_create(t, &a, thread_tramp, ts);
  pthread_attr_destroy(&a);
  int n = __sync_add_and_fetch(&thread_count, 1);
  L("pthread_create: #%d entry %p stack %uK -> %d", n, fn, (unsigned)(stack / 1024), r);
  if (r) free(ts);
  return r;
}

static int b_pthread_once(volatile int *once, void (*fn)(void)) {
  if (*once == 2) {
    __sync_synchronize();
    return 0;
  }
  if (__sync_bool_compare_and_swap(once, 0, 1)) {
    fn();
    __sync_synchronize();
    *once = 2;
  } else {
    while (*once != 2) sceKernelDelayThread(100);
  }
  return 0;
}

static int b_pthread_setname_np(pthread_t t, const char *name) {
  (void)t;
  L("thread name: %s", name);
  return 0;
}

static int b_pthread_key_create(pthread_key_t *k, void (*dtor)(void *)) {
  return pthread_key_create(k, dtor);
}

EXPORT_TABLE(pthread) = {
  EXP_AS("pthread_mutex_init", b_mutex_init),
  EXP_AS("pthread_mutex_destroy", b_mutex_destroy),
  EXP_AS("pthread_mutex_lock", b_mutex_lock),
  EXP_AS("pthread_mutex_unlock", b_mutex_unlock),
  EXP_AS("pthread_mutex_trylock", b_mutex_trylock),
  EXP_AS("pthread_mutexattr_init", b_mutexattr_init),
  EXP_AS("pthread_mutexattr_destroy", b_mutexattr_destroy),
  EXP_AS("pthread_mutexattr_settype", b_mutexattr_settype),
  EXP_AS("pthread_mutexattr_setpshared", b_mutexattr_setpshared),
  EXP_AS("pthread_cond_init", b_cond_init),
  EXP_AS("pthread_cond_destroy", b_cond_destroy),
  EXP_AS("pthread_cond_wait", b_cond_wait),
  EXP_AS("pthread_cond_timedwait", b_cond_timedwait),
  EXP_AS("pthread_cond_signal", b_cond_signal),
  EXP_AS("pthread_cond_broadcast", b_cond_broadcast),
  EXP_AS("pthread_condattr_init", b_condattr_init),
  EXP_AS("pthread_condattr_destroy", b_condattr_destroy),
  EXP_AS("pthread_condattr_setpshared", b_condattr_setpshared),
  EXP_AS("pthread_attr_init", b_attr_init),
  EXP_AS("pthread_attr_destroy", b_attr_destroy),
  EXP_AS("pthread_attr_setdetachstate", b_attr_setdetachstate),
  EXP_AS("pthread_attr_setstacksize", b_attr_setstacksize),
  EXP_AS("pthread_attr_setschedparam", b_attr_setschedparam),
  EXP_AS("pthread_create", b_pthread_create),
  EXP_AS("pthread_once", b_pthread_once),
  EXP_AS("pthread_setname_np", b_pthread_setname_np),
  EXP_AS("pthread_key_create", b_pthread_key_create),
  EXP(pthread_key_delete),
  EXP(pthread_getspecific),
  EXP(pthread_setspecific),
  EXP(pthread_self),
  EXP(pthread_equal),
  EXP(pthread_join),
  EXP(pthread_detach),
  EXP(pthread_exit),
};
EXPORT_TABLE_END(pthread);
