// CapUnlocker detection. there's no api to ask, so just try it: asking for
// the system core gets refused on a stock vita and works with the plugin.
// same trick as the user's flycast / mbaa ports.
#include <psp2/kernel/threadmgr.h>

#include "util/cpu.h"
#include "util/log.h"

int config_int(const char *key, int def);

static int has_core3 = -1;

void cpu_init(void) {
  if (has_core3 >= 0) return;
  has_core3 = 0;
  if (!config_int("capunlocker", 1)) {
    L("cpu: capunlocker=0 in config, sticking to 3 cores");
    return;
  }
  SceUID me = sceKernelGetThreadId();
  int old = sceKernelGetThreadCpuAffinityMask(me);
  int r = sceKernelChangeThreadCpuAffinityMask(me, SCE_KERNEL_CPU_MASK_USER_ALL | SCE_KERNEL_CPU_MASK_SYSTEM);
  if (r >= 0) has_core3 = 1;
  // put the main thread back where it was, only game threads get moved
  sceKernelChangeThreadCpuAffinityMask(me, old > 0 ? old : SCE_KERNEL_CPU_MASK_USER_ALL);
  L("cpu: %s", has_core3 ? "CapUnlocker active, game threads get core 3 too"
                         : "stock core caps (no CapUnlocker), 3 cores");
}

int cpu_capunlocker(void) {
  if (has_core3 < 0) cpu_init();
  return has_core3;
}

int cpu_count(void) { return cpu_capunlocker() ? 4 : 3; }

#ifndef SCE_KERNEL_CPU_MASK_USER_0
#define SCE_KERNEL_CPU_MASK_USER_0 0x10000
#endif

// the render thread is the bottleneck in a match (cpu bound, gpu idle), so
// once we know which thread it is it gets core 0 alone and every other game
// thread moves to the rest (1-2, or 1-3 with CapUnlocker)
static volatile int render_pinned;
static SceUID game_tids[128];
static int n_game_tids;

int cpu_game_mask(void) {
  int m = SCE_KERNEL_CPU_MASK_USER_ALL | (cpu_capunlocker() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0);
  if (render_pinned) m &= ~SCE_KERNEL_CPU_MASK_USER_0;
  return m;
}

void cpu_place_game_thread(void) {
  SceUID me = sceKernelGetThreadId();
  int i = __sync_fetch_and_add(&n_game_tids, 1);
  if (i < 128) game_tids[i] = me;
  if (!cpu_capunlocker() && !render_pinned) return;  // default mask is already all user cores
  sceKernelChangeThreadCpuAffinityMask(me, cpu_game_mask());
}

void cpu_pin_render_thread(void) {
  if (render_pinned) return;
  extern int config_int(const char *key, int def);
  if (!config_int("pin_render", 1)) { render_pinned = -1; return; }
  SceUID me = sceKernelGetThreadId();
  int r = sceKernelChangeThreadCpuAffinityMask(me, SCE_KERNEL_CPU_MASK_USER_0);
  if (r < 0) { L("cpu: couldn't pin the render thread (0x%08X)", r); render_pinned = -1; return; }
  render_pinned = 1;
  int n = n_game_tids < 128 ? n_game_tids : 128, moved = 0;
  for (int i = 0; i < n; i++)
    if (game_tids[i] != me && sceKernelChangeThreadCpuAffinityMask(game_tids[i], cpu_game_mask()) >= 0) moved++;
  L("cpu: render thread on core 0 alone, %d other game threads moved to mask 0x%X", moved, cpu_game_mask());
}
