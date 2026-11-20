#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

#include "game/patches.h"
#include "util/log.h"
#include "util/paths.h"

// --- tiny config file ---
static char cfg_buf[2048];
static int cfg_loaded;

int config_int(const char *key, int def) {
  if (!cfg_loaded) {
    cfg_loaded = 1;
    FILE *f = fopen(DATA_PATH "/config.txt", "r");
    if (f) {
      size_t n = fread(cfg_buf, 1, sizeof(cfg_buf) - 1, f);
      cfg_buf[n] = 0;
      fclose(f);
    }
  }
  size_t kl = strlen(key);
  for (char *p = cfg_buf; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p)) {
    if (!strncmp(p, key, kl) && p[kl] == '=') return atoi(p + kl + 1);
  }
  return def;
}

// --- patch helpers ---
static int patch_word(so_module *m, uint32_t off, uint32_t orig, uint32_t repl, const char *what) {
  uint32_t *p = (uint32_t *)(m->base + off);
  if (*p != orig) {
    L("patch: %s skipped, +0x%X is %08X not %08X (different game version?)", what, off, *p, orig);
    return -1;
  }
  *p = repl;
  return 0;
}

// get_global_heap() grabs one 400MB (0x19000000) block for the game's main
// VCHEAP2 and never asks for more. that's never fitting on a vita, so we
// rewrite both "mov rX, #0x19000000" to something smaller. ARM immediates
// here are imm8 ror 8, so the size goes in 16MB steps.
static void patch_global_heap(so_module *game) {
  extern int _newlib_heap_size_user;
  // measured in vita3k: the game heap peaks ~133MB at the menus, and the
  // game's plain mallocs + our GL + the Cg compiler want 60MB+ on top.
  // so the game heap gets 176MB and plain malloc keeps everything else.
  int def_mb = _newlib_heap_size_user / (1024 * 1024) - 112;
  int mb = config_int("game_heap_mb", def_mb);
  if (mb < 64) mb = 64;
  if (mb > 400) mb = 400;
  uint32_t imm8 = (uint32_t)mb / 16;  // 16MB units
  if (imm8 > 0xFF) imm8 = 0xFF;

  int ok = 0;
  ok += !patch_word(game, 0x786AA8, 0xE3A01419, 0xE3A01400 | imm8, "global heap size (alloc)");
  ok += !patch_word(game, 0x786B3C, 0xE3A02419, 0xE3A02400 | imm8, "global heap size (add)");
  if (ok == 2) L("patch: game global heap 400MB -> %uMB", (unsigned)imm8 * 16);
}

// VCANDROIDFILEDEVICE::DeleteFile_LowLevel retries unlink() 33 times on any
// failure, errno never checked. mid match the game sweeps its tr/amb/ts cache
// files by number and most of them don't exist, so that's ~1100 failed
// removes a second. nearly free on linux, a real fs call here. one try it is.
static void patch_delete_retries(so_module *game) {
  if (!patch_word(game, 0x12CD2C4, 0xE3A04021, 0xE3A04001, "DeleteFile_LowLevel retries"))
    L("patch: DeleteFile_LowLevel retries 33 -> 1");
}

void patches_apply(so_module *game) {
  patch_global_heap(game);
  patch_delete_retries(game);
}

// --- heap monitor: is the game's own heap actually full? ---
// get_global_heap() returns a VCHEAP2_THREADSAFE, its VCHEAP2 part sits at
// +0x44 (that's where get_global_heap constructs it). the getters below are
// plain field reads, no floats, so calling them from our side is fine.
#define VCHEAP2_OFFSET 0x44
static uintptr_t (*g_get_heap)(void);
static unsigned (*g_heap_free)(uintptr_t), (*g_heap_total)(uintptr_t), (*g_heap_peak)(uintptr_t);

void game_monitor_init(so_module *game) {
  g_get_heap = (void *)so_symbol(game, "_Z15get_global_heapv");
  g_heap_free = (void *)so_symbol(game, "_ZNK7VCHEAP220GetNumberOfFreeBytesEv");
  g_heap_total = (void *)so_symbol(game, "_ZNK7VCHEAP221GetTotalNumberOfBytesEv");
  g_heap_peak = (void *)so_symbol(game, "_ZNK7VCHEAP227GetPeakUsageInNumberOfBytesEv");
  if (!g_get_heap || !g_heap_free || !g_heap_total || !g_heap_peak) {
    L("heapmon: game heap getters not found, monitor off");
    g_get_heap = NULL;
  }
}

void game_monitor_tick(void) {
  static SceUInt64 next;
  SceUInt64 now = sceKernelGetProcessTimeWide();
  if (now < next) return;
  next = now + 5 * 1000 * 1000;

  extern void mem_report(void);
  extern unsigned shacc_mem_live(void);
  extern void gpu_mem_log(const char *why);
  extern uint32_t gxm_frame_count(void);
  // fps over the last 5s window, so "how fast is it" comes from the log
  static uint32_t last_frames;
  uint32_t f = gxm_frame_count();
  L("fps: %u.%u (frame %u)", (f - last_frames) / 5, ((f - last_frames) * 2) % 10, (unsigned)f);
  last_frames = f;
  mem_report();
  L("Cg compiler live %uK", shacc_mem_live() / 1024);
  gpu_mem_log("monitor");  // cdram/lpddr use + free user/cdram/phycont
  extern void gxm_prof_report(void);
  gxm_prof_report();
  if (!g_get_heap) return;
  uintptr_t h = g_get_heap();
  if (!h) return;
  uintptr_t vh = h + VCHEAP2_OFFSET;
  L("game heap: total %uK free %uK peak used %uK",
    g_heap_total(vh) / 1024, g_heap_free(vh) / 1024, g_heap_peak(vh) / 1024);
}
