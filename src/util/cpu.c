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

int cpu_game_mask(void) {
  return SCE_KERNEL_CPU_MASK_USER_ALL | (cpu_capunlocker() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0);
}

void cpu_place_game_thread(void) {
  if (!cpu_capunlocker()) return;  // default mask is already all user cores
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), cpu_game_mask());
}
