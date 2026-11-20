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

// VCHEAP2 grows in steps of at least 4MB (mov r2,#0x400000 in VCHEAP2::Init,
// read by GrowMemory as max(request + overhead, floor)). late in a load no
// 4MB hole is left anywhere and the game crashes on the NULL (career mode,
// match loads). 1MB steps fit the smaller gaps that are still around.
// heap_grow_mb=4 puts the original back.
static void patch_heap_grow(so_module *game) {
  int mb = config_int("heap_grow_mb", 1);
  uint32_t enc;
  switch (mb) {
    case 1: enc = 0xE3A02601; break;   // mov r2, #0x100000
    case 2: enc = 0xE3A02602; break;   // mov r2, #0x200000
    default: return;                   // 4 = leave it alone
  }
  if (!patch_word(game, 0x132AA08, 0xE3A02501, enc, "VCHEAP2 grow step"))
    L("patch: VCHEAP2 grow step 4MB -> %dMB", mb);
}

// the game's video settings (crowd / crowd2d / stadium / player / shadow /
// sideline / frontend / reflection / presentation lighting LOD) live in one
// global struct and default low on this build. the crowd renderer only draws
// its full 3d sections at crowd lod 3 (it literally checks for 3). forcing
// ALL of them to max blew the memory budget ~35MB over and the match load
// died at 76% (hw29), so the default only forces the arena set; quality_max=0
// disables, quality_max_all=1 forces every getter for testing
static int quality_lod_max(void) { return 3; }

static void patch_video_lods(so_module *game) {
  int all = config_int("quality_max_all", 0);
  if (!config_int("quality_max", 1)) return;
  static const struct { uint32_t off, w0, w1; const char *name; int arena; } lods[] = {
    { 0x81EF80, 0xE59F3004, 0xE79F0003, "crowd", 1 },
    { 0x81EFA4, 0xE59F3008, 0xE08F3003, "player", 0 },
    { 0x81EFCC, 0xE59F3008, 0xE08F3003, "reflection", 0 },
    { 0x81EFF4, 0xE59F3008, 0xE08F3003, "sideline", 0 },
    { 0x81F01C, 0xE59F3008, 0xE08F3003, "shadow", 0 },
    { 0x81F044, 0xE59F3008, 0xE08F3003, "stadium", 1 },
    { 0x81F06C, 0xE59F3008, 0xE08F3003, "frontend", 0 },
    { 0x81F094, 0xE59F3008, 0xE08F3003, "crowd2d", 1 },
    { 0x81F0BC, 0xE59F3008, 0xE08F3003, "presentation lighting", 0 },
  };
  int n = 0;
  for (unsigned i = 0; i < sizeof(lods) / sizeof(lods[0]); i++) {
    if (!lods[i].arena && !all) continue;
    uint32_t *p = (uint32_t *)(game->base + lods[i].off);
    if (p[0] != lods[i].w0 || p[1] != lods[i].w1) {
      L("patch: %s lod getter skipped, +0x%X is %08X %08X (different game version?)",
        lods[i].name, lods[i].off, p[0], p[1]);
      continue;
    }
    p[0] = 0xE51FF004;   // ldr pc, [pc, #-4]
    p[1] = (uint32_t)(uintptr_t)quality_lod_max;
    n++;
  }
  if (n) L("patch: %d video LOD getters -> max (3)%s", n, all ? " (all)" : "");
}

void patches_apply(so_module *game) {
  patch_global_heap(game);
  patch_delete_retries(game);
  patch_heap_grow(game);
  patch_video_lods(game);
  extern void textinput_install(so_module *game);
  textinput_install(game);
}

// --- heap monitor: is the game's own heap actually full? ---
// get_global_heap() returns a VCHEAP2_THREADSAFE, its VCHEAP2 part sits at
// +0x44 (that's where get_global_heap constructs it). the getters below are
// plain field reads, no floats, so calling them from our side is fine.
#define VCHEAP2_OFFSET 0x44
static uintptr_t (*g_get_heap)(void);
static unsigned (*g_heap_free)(uintptr_t), (*g_heap_total)(uintptr_t), (*g_heap_peak)(uintptr_t);

// the game frees a texture/stream/index block's cpu copy one frame after its
// gl upload, through a 1024-entry free queue drained in VCSCREEN_RENDERTHREAD::
// Render. if more than 1024 uploads land in one frame the rest are just
// dropped (leaked). this watches the queue's fill so we know if that happens.
static volatile int *g_freeq_count;
static int g_freeq_peak;

void game_freeq_sample(void) {
  if (!g_freeq_count) return;
  int n = *g_freeq_count;
  if (n > g_freeq_peak) g_freeq_peak = n;
}

static void freeq_init(so_module *game) {
  // ldr r2,[pc,#1128] @ +0x1303974 loads this literal, add r2,pc,r2 @ +0x130397c
  const uint32_t *lit = (const uint32_t *)(game->base + 0x1303de4);
  if (*lit != 0x0074bb7c) { L("freeq: literal mismatch (%08X), not watching the free queue", *lit); return; }
  g_freeq_count = (volatile int *)(game->base + 0x1303984 + 0x74bb7c + 0x7454);
}

void game_monitor_init(so_module *game) {
  freeq_init(game);
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
  if (g_freeq_count) {
    L("game free queue: peak %d / 1024 per frame%s", g_freeq_peak, g_freeq_peak >= 1024 ? " (FULL, blocks leaked)" : "");
    g_freeq_peak = 0;
  }
  if (!g_get_heap) return;
  uintptr_t h = g_get_heap();
  if (!h) return;
  uintptr_t vh = h + VCHEAP2_OFFSET;
  L("game heap: total %uK free %uK peak used %uK",
    g_heap_total(vh) / 1024, g_heap_free(vh) / 1024, g_heap_peak(vh) / 1024);
}
