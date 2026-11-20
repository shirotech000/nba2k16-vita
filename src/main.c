// NBA 2K16 vita loader: load the android libs, fake enough android
// around them, then let the game run.
#include <stdio.h>
#include <string.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "android/android.h"
#include "android/jni.h"
#include "loader/imports.h"
#include "loader/so_loader.h"
#include "util/log.h"
#include "util/paths.h"

// newlib heap. the .so images, GPU memory and thread stacks live outside
// of this, so it can't be the whole budget.
int _newlib_heap_size_user = 224 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

void libc_init(void);
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

int main(void) {
  log_init();
  L("nba2k16 vita loader, built " __DATE__ " " __TIME__);

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  sceIoMkdir(FILES_DIR, 0777);
  sceIoMkdir(CACHE_DIR, 0777);
  sceIoMkdir(OBB_DIR, 0777);
  sceIoMkdir(SHADER_CACHE, 0777);

  crash_init();
  libc_init();
  imports_init();
  jni_init();

  need_file(STL_SO_PATH, "libgnustl_shared.so");
  need_file(GAME_SO_PATH, "libnba2k16_clean_opt.so");

  if (so_load(&stl_mod, STL_SO_PATH) < 0) fatal("couldn't load gnustl");
  if (so_load(&game_mod, GAME_SO_PATH) < 0) fatal("couldn't load the game lib");
  if (so_relocate(&stl_mod) < 0 || so_relocate(&game_mod) < 0) fatal("relocation failed");
  imports_finalize();
  if (so_finalize(&stl_mod) < 0 || so_finalize(&game_mod) < 0) fatal("couldn't make code executable");

  so_run_init(&stl_mod);
  so_run_init(&game_mod);
  mem_report();

  if (gfx_init) gfx_init();

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
