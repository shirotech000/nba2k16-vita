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

// VCHEAP2::Init stores the minimum growth chunk into [heap+0x6c] with one
// "mov r2, #0x400000" at +0x132AA08. VCHEAP2::GrowMemory (+0x1329718) sizes
// every new chunk as max(request + overhead, [heap+0x6c]), so every grow also
// pulls at least 4 MB (4194315 = 4 MB + the 11-byte VCHEAPINTERFACE header,
// that's the malloc(4194315) in the log) even when the requester only needs a
// few hundred KB. A partly-used chunk then carries up to ~4 MB of dead tail.
// We shrink the floor to 1 MB: big requests (>1 MB) still grow by their exact
// size, small ones waste at most ~1 MB instead of ~4.
// arm_mod_imm encodes an ARM "mov" modify-immediate (imm8 rotate-right by
// 2*rot). The sizes we hand it here are always powers of two MB values.
static int arm_mod_imm(uint32_t val, uint32_t *out) {
  if (val <= 0xFF) {
    *out = 0xE3A00000 | val;
    return 0;
  }
  for (uint32_t rot = 1; rot < 16; rot++) {
    uint32_t v = (val << (rot * 2)) | (val >> (32 - rot * 2));
    if (v <= 0xFF) {
      *out = 0xE3A00000 | (rot << 8) | v;
      return 0;
    }
  }
  return -1;
}

static void patch_grow_step(so_module *game) {
  int mb = config_int("heap_grow_mb", 1);
  if (mb < 1) mb = 1;
  if (mb > 64) mb = 64;
  uint32_t imm = 0xE3A02601;  /* 1MB if the encoder ever fails */
  if (arm_mod_imm((uint32_t)mb * 1024 * 1024, &imm) != 0) {
    L("patch: heap grow step %dMB not encodable, falling back to 1MB", mb);
    arm_mod_imm(1024 * 1024, &imm);
  }
  // keep the destination register: the original is mov r2, so re-encoding with
  // the plain 0xE3A00000 base would clobber r0 (the heap base, in use)
  uint32_t word = (imm & ~0xF00u) | (0xE3A02501 & 0xF00u);
  if (!patch_word(game, 0x132AA08, 0xE3A02501, word, "VCHEAP2 grow step")) return;
  L("patch: VCHEAP2 grow step 4MB -> %dMB", mb);
}

void patches_apply(so_module *game) {
  patch_global_heap(game);
  patch_delete_retries(game);
  patch_grow_step(game);
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
