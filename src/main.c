// NBA 2K16 vita loader: load the android libs, fake enough android
// around them, then let the game run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "android/android.h"
#include "android/jni.h"
#include "game/patches.h"
#include "loader/imports.h"
#include "loader/so_loader.h"
#include "util/cpu.h"
#include "util/log.h"
#include "util/paths.h"

// newlib heap. the .so images, GPU memory and thread stacks live outside
// of this, so it can't be the whole budget.
#ifndef NBA_HEAP_MB
#define NBA_HEAP_MB 288
#endif
int _newlib_heap_size_user = NBA_HEAP_MB * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

void libc_init(void);
void io_init(void);
void mem_report(void);
void crash_init(void);
// graphics layer hook, the GL side fills this in
void gfx_init(void) __attribute__((weak));

static so_module stl_mod, game_mod;

static int file_exists(const char *p) {
  SceIoStat st;
  return sceIoGetstat(p, &st) >= 0;
}

static void need_file(const char *p, const char *what) {
  if (!file_exists(p)) fatal("missing %s: %s (see README)", what, p);
}

static void log_free_mem(const char *when) {
  SceKernelFreeMemorySizeInfo fi;
  memset(&fi, 0, sizeof(fi));
  fi.size = sizeof(fi);
  int r = sceKernelGetFreeMemorySize(&fi);
  if (r < 0) L("free mem (%s): query failed 0x%08X", when, r);
  else L("free mem (%s): user %d KiB, cdram %d KiB, phycont %d KiB", when,
         fi.size_user >> 10, fi.size_cdram >> 10, fi.size_phycont >> 10);
}

// the game reads /proc/cpuinfo (maps to DATA_PATH/root/proc/cpuinfo), give it a
// plausible one: our usable cortex-a9 cores (3, or 4 with CapUnlocker), neon
static void write_fake_cpuinfo(void) {
  char info[1024];
  int n = snprintf(info, sizeof(info), "Processor\t: ARMv7 Processor rev 0 (v7l)\n");
  for (int i = 0; i < cpu_count(); i++)
    n += snprintf(info + n, sizeof(info) - n, "processor\t: %d\nBogoMIPS\t: 888.00\n\n", i);
  n += snprintf(info + n, sizeof(info) - n,
    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls\n"
    "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x4\n"
    "CPU part\t: 0xc09\nCPU revision\t: 0\n\n"
    "Hardware\t: PS Vita\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
  sceIoMkdir(DATA_PATH "/root", 0777);
  sceIoMkdir(DATA_PATH "/root/proc", 0777);
  SceUID fd = sceIoOpen(DATA_PATH "/root/proc/cpuinfo", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) {
    sceIoWrite(fd, info, n);
    sceIoClose(fd);
  }
}

int main(void) {
  log_init();
  // the newlib heap block gets grabbed before main with no fallback. if it
  // didn't fit, every malloc is NULL from here on, so say so right away
  // (volatile so gcc can't fold the malloc/free pair away)
  void *volatile heap_probe = malloc(64);
  if (!heap_probe)
    fatal("newlib heap is dead: malloc(64) failed. the %d MB heap (_newlib_heap_size_user) "
          "probably didn't fit, try a smaller NBA_HEAP_MB", NBA_HEAP_MB);
  free(heap_probe);
  log_free_mem("boot");
  L("nba2k16 vita loader, built " __DATE__ " " __TIME__);
  // the eboot isn't at its link address in extended memory mode. this +
  // nm gives the slide for symbolizing a crash pc
  L("eboot: main @ %p", (void *)main);
  if (emu_is_vita3k()) L("vita3k.flag found, running in vita3k");

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  sceIoMkdir(INTERNAL_DIR, 0777);
  sceIoMkdir(EXTERNAL_DIR, 0777);
  sceIoMkdir(DATA_PATH "/root", 0777);
  sceIoMkdir(FILES_DIR, 0777);
  sceIoMkdir(CACHE_DIR, 0777);
  sceIoMkdir(OBB_DIR, 0777);
  sceIoMkdir(SHADER_CACHE, 0777);
  cpu_init();          // CapUnlocker probe, before anything asks for core counts
  write_fake_cpuinfo();

  // no_crash_handler=1 lets vita3k (or the vita's coredump) report faults itself
  if (!config_int("no_crash_handler", 0)) crash_init();
  libc_init();
  io_init();
  imports_init();
  jni_init();

  need_file(STL_SO_PATH, "libgnustl_shared.so");
  need_file(GAME_SO_PATH, "libnba2k16_clean_opt.so");
  // not fatal, the game itself bails cleanly without it, but say why
  if (!file_exists(obb_resolve()))
    L("WARNING: no game data at %s, the game will exit right after boot", obb_resolve());

  if (so_load(&stl_mod, STL_SO_PATH) < 0) fatal("couldn't load gnustl");
  if (so_load(&game_mod, GAME_SO_PATH) < 0) fatal("couldn't load the game lib");
  if (so_relocate(&stl_mod) < 0 || so_relocate(&game_mod) < 0) fatal("relocation failed");
  patches_apply(&game_mod);  // text is still RW here
  imports_finalize();
  if (so_finalize(&stl_mod) < 0 || so_finalize(&game_mod) < 0) fatal("couldn't make code executable");

  so_run_init(&stl_mod);
  so_run_init(&game_mod);
  mem_report();
  game_monitor_init(&game_mod);

  log_free_mem("before gfx_init");
  if (gfx_init) gfx_init();
  {
    // after the display's up: the diet's first-boot pass draws its own
    // progress instead of six minutes of black
    extern void obb_diet_run(void);
    obb_diet_run();
  }
  extern void bootscreen_run(void) __attribute__((weak));
  if (bootscreen_run) bootscreen_run();   // "preparing for first launch" if shaders are missing
  {
    extern void pack_maybe_build(void);
    pack_maybe_build();   // fold any new loose shader files into pack.bin
  }

  int (*jni_onload)(void *vm, void *reserved) = (void *)so_symbol(&game_mod, "JNI_OnLoad");
  void *on_create = (void *)so_symbol(&game_mod, "ANativeActivity_onCreate");
  if (!on_create) fatal("ANativeActivity_onCreate not found");

  if (jni_onload) {
    int v = jni_onload(jni_vm(), NULL);
    L("JNI_OnLoad -> 0x%X", v);
  }

  activity_boot(on_create);
  mem_report();
  activity_loop();
  return 0;
}
