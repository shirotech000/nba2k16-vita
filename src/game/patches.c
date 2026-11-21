#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

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

// the match load's tail killer: VCHEAPWRAPPER::Allocate returns NULL when
// every pool is at its floor, and the game writes straight through it
// (write at address 0x0, backtrace +0x132CB04). the render thread keeps
// retiring scenes in that state, so the deferred gpu frees come back within
// seconds. hook the wrapper and retry instead of handing out the NULL.
// faithful reimplementation of the original: same header math, same stats
// bookkeeping, same tls-side-effect calls. only the failure ladder (the
// try-other-heaps handlers that returned NULL in every observed death)
// is replaced by the retry loop.
static void *(*g_tls_state)(void *);

static void *heap_wrapper_allocate(void *self, unsigned size, unsigned align_, void *mode, unsigned crc, int extra) {
  (void)crc; (void)extra;
  if (!mode) (void)g_tls_state(self);   // the game touches tls state here
  if (!align_) {
    void *st = g_tls_state(self);
    align_ = *(unsigned *)((char *)st + 4);   // the tls default alignment
  }
  if (align_ < 4) align_ = 4;
  if (size == 0) { size = 1; align_ = 4; }

  unsigned raw_n = size + 7 + align_;
  typedef void *(*alloc_fn)(unsigned);
  alloc_fn fn = *(alloc_fn *)((char *)self + 0x2c);
  void *p = fn(raw_n);
  if (!p) {
    // wait for the deferred gpu frees (the render thread retires scenes on
    // its own) and try again. ~15s cap, then the game's own fallback ladder
    extern uint32_t gxm_pool_release_empty(void);
    extern uint32_t gxm_defer_drain(void);
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    for (int i = 0; i < 40; i++) {
      sceKernelDelayThread(500000);
      gxm_pool_release_empty();
      gxm_defer_drain();
      p = fn(raw_n);
      if (p) {
        L("vcwrap: a %uK ask recovered after retry %d", size >> 10, i + 1);
        break;
      }
      if (sceKernelGetProcessTimeWide() - t0 > 15 * 1000000) break;
    }
  }
  if (!p) {
    // note: the game's own fallback ladder (vtable+0xa8) was tried here and
    // it HANGS when called out of the original frame's context (hw44: the
    // retry cadence ends and the thread never returns). don't call it.
    L("vcwrap: a %uK ask still failing after retries, handing back NULL", size >> 10);
    return NULL;
  }
  uintptr_t a = ((uintptr_t)p + align_ + 7) / align_ * align_;
  ((unsigned *)a)[-2] = size;
  ((unsigned *)a)[-1] = (unsigned)(uintptr_t)p;
  // the wrapper's own stats: count / used / peak at +0x38
  unsigned *w = (unsigned *)((char *)self + 0x38);
  w[1] += size;
  w[0] += 1;
  if (w[2] < w[1]) w[2] = w[1];
  return (void *)a;
}

static void patch_heap_wrapper_retry(so_module *game) {
  uint32_t *p = (uint32_t *)(game->base + 0x132CABC);
  // the original prologue: push {r4-r9,lr}; subs r9, r3, #0
  if (p[0] != 0xE92D43F0 || p[1] != 0xE2539000) {
    L("patch: heap wrapper retry skipped, +0x132CABC is %08X %08X (different game version?)", p[0], p[1]);
    return;
  }
  g_tls_state = (void *(*)(void *))(game->base + 0x132CA08);
  p[0] = 0xE51FF004;   // ldr pc, [pc, #-4]
  p[1] = (uint32_t)(uintptr_t)heap_wrapper_allocate;
  L("patch: heap wrapper retry hooked (+0x132CABC)");
}

// the crowd diet run bought 3% (hw36/37) but the owner wants crowds back:
// "compromise on all stage/character textures and maybe be able to fit
// crowds in". with tex_half freeing the pool, crowd lod 2 (the medium 3d
// sections) is the target. quality_max=0 restores the game's own defaults
static unsigned g_lod_calls[2];   // crowd, crowd2d - liveness probe for the monitor
static int quality_lod_max(void) { return 3; }
static int quality_lod_crowd2(void) {
  g_lod_calls[0]++;
  // crowd_lod=1 in config.txt tries the smaller section if lod 2 won't fit
  return config_int("crowd_lod", 2);
}
static int quality_lod_crowd2d(void) { g_lod_calls[1]++; return 3; }

static void patch_video_lods(so_module *game) {
  int all = config_int("quality_max_all", 0);
  if (!config_int("quality_max", 1)) return;
  static const struct { uint32_t off, w0, w1; const char *name; int arena; int (*fn)(void); } lods[] = {
    { 0x81EF80, 0xE59F3004, 0xE79F0003, "crowd", 1, quality_lod_crowd2 },
    { 0x81EFA4, 0xE59F3008, 0xE08F3003, "player", 0, quality_lod_max },
    { 0x81EFCC, 0xE59F3008, 0xE08F3003, "reflection", 0, quality_lod_max },
    { 0x81EFF4, 0xE59F3008, 0xE08F3003, "sideline", 0, quality_lod_max },
    { 0x81F01C, 0xE59F3008, 0xE08F3003, "shadow", 0, quality_lod_max },
    { 0x81F044, 0xE59F3008, 0xE08F3003, "stadium", 0, quality_lod_max },
    { 0x81F06C, 0xE59F3008, 0xE08F3003, "frontend", 0, quality_lod_max },
    { 0x81F094, 0xE59F3008, 0xE08F3003, "crowd2d", 1, quality_lod_crowd2d },
    { 0x81F0BC, 0xE59F3008, 0xE08F3003, "presentation lighting", 0, quality_lod_max },
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
    p[1] = (uint32_t)(uintptr_t)lods[i].fn;
    n++;
  }
  if (n) L("patch: %d video LOD getters hooked (crowd=2, crowd2d=3)%s", n, all ? " (all)" : "");
}

// crowd_fill=N in config.txt: the crowd's memory scales with the seats
// filled, and the game's own lod-1 path returns a hardcoded 0.3 - so any
// fraction is a supported mode. hooking the attendance function lets a
// half (or third) crowd fit where the full one never could
static int (*g_game_in_progress)(void);
static float (*g_attendance_fraction)(void);

static float crowd_fill_hook(void) {
  int fill = config_int("crowd_fill", 0);
  if (fill > 0 && fill < 100) return (float)fill / 100.0f;
  if (!g_game_in_progress || !g_game_in_progress()) return 0.0f;
  if (quality_lod_crowd2() == 1) return 0.3f;
  return g_attendance_fraction ? g_attendance_fraction() : 0.0f;
}

static void patch_crowd_fill(so_module *game) {
  uint32_t *p = (uint32_t *)(game->base + 0xc57e90);
  // the original prologue: push {r3, lr} (ARM)
  if (p[0] != 0xE92D4008) {
    L("patch: crowd fill skipped, +0xc57e90 is %08X (different game version?)", p[0]);
    return;
  }
  g_game_in_progress = (int (*)(void))(game->base + 0xd99000);
  g_attendance_fraction = (float (*)(void))(game->base + 0xc5a804);
  p[0] = 0xE51FF004;   // ldr pc, [pc, #-4]
  p[1] = (uint32_t)(uintptr_t)crowd_fill_hook;
  L("patch: crowd fill hooked (+0xc57e90)");
}

// the missing MakeAbsolute for the resource index. the game's context
// loader leaves the sorted-name table as a raw offset in idx+28 on this
// port (hw65/66: identical crashes on healed AND pristine data - not the
// diet, a relocation step that never runs here). the fix completes the
// game's own encoding: raw +28 fields rebase with +27 (the tag rule is
// field offset - 1, confirmed on both known rebase sites), stored back
// so every later lookup sees the pointer too. idx_rebase=0 disables.
uint32_t g_idx_rebase_ret;   // set at patch time: base + 0x1342C4C

__asm__(
  "  .arm                        \n"
  "  .align 2                    \n"
  "  .global idx_rebase_stub     \n"
  "idx_rebase_stub:              \n"
  "  ldr  r8, [r9, #40]          \n"   // the displaced 0x1342C44 (the archive image)
  "  cmp  r8, #0                 \n"   // the displaced 0x1342C48
  "  beq  8f                     \n"
  // level 1: the pointer-table field at +0x1C (raw -> image + raw + 0x1B)
  "  ldr  ip, [r8, #28]          \n"
  "  cmp  ip, #0x1000            \n"
  "  bhs  1f                     \n"
  "  add  ip, r8, ip             \n"
  "  add  ip, ip, #27            \n"   // tag = 0x1C - 1
  "  str  ip, [r8, #28]          \n"
  // level 2: the table's entries -> the records, tag = table's home - 1
  // (hw68's decode: entries 13/25/37 + 63 -> the records at +0x4C/+0x58/+0x64)
  "1: push {r1, r2, r3, r4}      \n"
  "  ldr  ip, [r8, #28]          \n"
  "  cmp  ip, #0x1000            \n"
  "  blo  7f                     \n"   // level 1 didn't land: leave it
  "  sub  r4, ip, r8             \n"   // the table's offset in the image
  "  sub  r4, r4, #1             \n"   // the entries' tag
  "  ldr  r3, [r8, #24]          \n"   // the record count at +0x18
  "  cmp  r3, #64                \n"
  "  bhi  7f                     \n"   // sanity cap
  "  mov  r2, #0                 \n"
  "2: ldr  r1, [ip, r2, lsl #2]  \n"   // table[i]
  "  cmp  r1, #0                 \n"
  "  beq  4f                     \n"
  "  cmp  r1, #0x1000            \n"
  "  bhs  4f                     \n"   // already absolute
  "  add  r1, r8, r1             \n"
  "  add  r1, r1, r4             \n"   // + the computed tag
  "  str  r1, [ip, r2, lsl #2]   \n"
  "4: add  r2, r2, #1            \n"
  "  cmp  r2, r3                 \n"
  "  blo  2b                     \n"
  "7: pop  {r1, r2, r3, r4}       \n"
  "8: cmp  r8, #0                \n"   // flags for the game's beq at return
  "  ldr  ip, 9f                 \n"
  "  ldr  ip, [ip]               \n"
  "  bx   ip                     \n"
  "9: .word g_idx_rebase_ret     \n"
);

extern void idx_rebase_stub(void);

static void patch_idx_rebase(so_module *game) {
  if (!config_int("idx_rebase", 1)) return;
  uint32_t *p = (uint32_t *)(game->base + 0x1342C44);
  // the original: ldr r8,[r9,#40]; cmp r8,#0 (GetObject's index load)
  if (p[0] != 0xE5998028 || p[1] != 0xE3580000) {
    L("patch: idx rebase skipped, +0x1342C44 is %08X %08X (different game version?)", p[0], p[1]);
    return;
  }
  g_idx_rebase_ret = (uint32_t)(uintptr_t)(game->base + 0x1342C4C);
  p[0] = 0xE51FF004;   // ldr pc, [pc, #-4]
  p[1] = (uint32_t)(uintptr_t)&idx_rebase_stub;
  L("patch: idx rebase hooked (+0x1342C44)");
}

// the queueing watch: AddCreateRequest links a request into the manager's
// list, then hands it to VCASYNCTHREAD::AddRequest - and a refusal there
// (the request not available, or the async thread's state word at +0x124
// dead) strands it in the list with nothing processing it. the loader then
// polls IsDone forever (the 97% hang, hw69-72). this hook logs both
// refusal fields at every call, rate-limited, so the hang names its own
// reason
uint32_t g_asyncq_ret;   // non-static: the top-level asm references it
static SceUInt64 g_asyncq_last;
static void *g_async_obj;   // set at the first AddRequest, used by the drain watchdog
static void *g_async_mgr;   // the manager, captured alongside

static void __attribute__((used)) asyncq_note(uint32_t async, uint32_t req) {
  // capture the async object's runtime address for the drain watchdog
  if (async && !g_async_obj) {
    g_async_obj = (void *)(uintptr_t)async;
    L("asyncq: captured async obj %p for drain watchdog", g_async_obj);
  }
  // capture the manager pointer for the drain's IsDone force-pass
  if (req && !g_async_mgr) {
    uint32_t asyncreq = req - 0x18;
    if (asyncreq > 0x80000000) {
      uint32_t m = *(uint32_t *)(uintptr_t)(asyncreq + 0x10);
      if (m > 0x80000000 && m < 0x82000000) {
        g_async_mgr = (void *)(uintptr_t)m;
        L("asyncq: captured mgr %p for drain", g_async_mgr);
      }
    }
  }
  SceUInt64 now = sceKernelGetProcessTimeWide();
  if (now - g_asyncq_last < 2000000) return;   // once per 2s max
  g_asyncq_last = now;
  // req = ASYNCTHREADREQUEST + 0x18 (the embedded payload). the manager
  // pointer lives at ASYNCTHREADREQUEST + 0x10 = req - 8
  uint32_t inflight = *(uint32_t *)(uintptr_t)(async + 0x114);
  uint32_t qhead = *(uint32_t *)(uintptr_t)(async + 0x34);
  uint32_t qanchor = *(uint32_t *)(uintptr_t)(async + 0x18);
  uint32_t done_head = *(uint32_t *)(uintptr_t)(async + 0x54);
  uint32_t done_anchor = *(uint32_t *)(uintptr_t)(async + 0x38);
  uint32_t asyncreq = req ? (req - 0x18) : 0;
  uint32_t mgr = (asyncreq > 0x80000000) ? *(uint32_t *)(uintptr_t)(asyncreq + 0x10) : 0;
  uint32_t mgr_field = (mgr > 0x80000000) ? *(uint32_t *)(uintptr_t)(mgr + 0x10) : 0;
  uint32_t req_next = (asyncreq > 0x80000000) ? *(uint32_t *)(uintptr_t)(asyncreq + 0x10) : 0;
  L("asyncq: q %s in-flt %08X done %s | asyncreq %08X mgr %08X field %08X %s",
    qhead != qanchor ? "HAS-WORK" : "empty",
    inflight,
    done_head != done_anchor ? "HAS-DONE" : "empty",
    asyncreq, mgr, mgr_field,
    (mgr > 0x80000000 && mgr_field == mgr) ? "DONE!" :
    (mgr > 0x80000000 && req_next == asyncreq) ? "SELF-LINKED" : "PENDING");
}

__asm__(
  "  .arm                        \n"
  "  .align 2                    \n"
  "  .global asyncq_stub         \n"
  "asyncq_stub:                  \n"
  "  push {r0, r1, r2, r3, lr}   \n"
  "  bl   asyncq_note            \n"   // (r0, r1) still the original args
  "  pop  {r0, r1, r2, r3, lr}   \n"
  "  push {r4, r5, r6, r7, r8, r9, sl, lr} \n"   // the displaced prologue
  "  mov  r8, r0                 \n"   // the displaced second insn
  "  ldr  ip, 1f                 \n"
  "  ldr  ip, [ip]               \n"
  "  bx   ip                     \n"
  "1: .word g_asyncq_ret         \n"
);

extern void asyncq_stub(void);

static void patch_asyncq_log(so_module *game) {
  if (!config_int("asyncq_log", 1)) return;
  uint32_t *p = (uint32_t *)(game->base + 0x131E8EC);
  // the original: push {r4-r9,sl,lr}; mov r8, r0
  if (p[0] != 0xE92D47F0 || p[1] != 0xE1A08000) {
    L("patch: asyncq log skipped, +0x131E8EC is %08X %08X (different game version?)", p[0], p[1]);
    return;
  }
  g_asyncq_ret = (uint32_t)(uintptr_t)(game->base + 0x131E8F4);
  p[0] = 0xE51FF004;   // ldr pc, [pc, #-4]
  p[1] = (uint32_t)(uintptr_t)&asyncq_stub;
  L("patch: asyncq log hooked (+0x131E8EC)");
}

// the done-list watchdog: Berlin's huge contexts complete on the async
// thread (in-flight returns to 0) but their callbacks only fire from
// VCLIBRARY::Update on the main thread - and the loader's retry loop
// never invokes it. force VCASYNCTHREAD::Update from the monitor tick
// so the done list always drains. the async object's address comes from
// the asyncq hook at runtime (the pre-computed .so offset was 1MB off -
// Ghidra's data segment base differs from the text base)
static void (*g_async_update)(void *);
// g_async_obj and g_async_mgr declared above asyncq_note (set there at runtime)

static void patch_async_drain(so_module *game) {
  if (!config_int("async_drain", 1)) return;
  // the object address comes from the asyncq hook at runtime - no .so
  // offset math (the Ghidra-to-runtime offset for the data segment
  // turned out to be 0x110000, not the text's 0x10000)
  g_async_update = (void (*)(void *))((char *)game->base + 0x131E15C);
  L("patch: async drain watchdog armed (update +0x131E15C, obj from asyncq hook)");
}

void patches_apply(so_module *game) {
  patch_global_heap(game);
  patch_delete_retries(game);
  patch_heap_grow(game);
  patch_heap_wrapper_retry(game);
  patch_crowd_fill(game);
  patch_idx_rebase(game);
  patch_asyncq_log(game);
  patch_async_drain(game);
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
  extern void shader_relink_tick(void);
  shader_relink_tick();     // retry links that oom-died at the load's peak
  L("crowd lod getters: crowd %u, crowd2d %u calls this window", g_lod_calls[0], g_lod_calls[1]);
  g_lod_calls[0] = g_lod_calls[1] = 0;
  extern void b_threads_wait_report(void);
  b_threads_wait_report();
  extern void b_sleep_report(void);
  b_sleep_report();
  extern void gxm_prof_report(void);
  gxm_prof_report();
  // the done-list drain: the loader's retry loop doesn't call
  // VCLIBRARY::Update, so completed requests' callbacks never fire and
  // the manager's pending list never empties. force it here - this is
  // what un-sticks the crowd's context at 95-97%
  if (g_async_update && g_async_obj) {
    if (*(volatile uint32_t *)((char *)g_async_obj + 0x124)) {
      g_async_update(g_async_obj);
      // after the callback fires, the loader's retry loop may have
      // re-added the request before IsDone could see the empty state.
      // force the manager's pending list to "done" so IsDone passes
      // and the retry loop exits
      if (g_async_mgr) {
        volatile uint32_t *mgr_head = (volatile uint32_t *)((char *)g_async_mgr + 0x10);
        volatile uint32_t *mgr_tail = (volatile uint32_t *)((char *)g_async_mgr + 0x0C);
        if (*mgr_head != (uint32_t)(uintptr_t)g_async_mgr) {
          L("async drain: forcing mgr pending list to done (head %08X -> %08X)",
            *mgr_head, (uint32_t)(uintptr_t)g_async_mgr);
          *mgr_head = (uint32_t)(uintptr_t)g_async_mgr;
          *mgr_tail = (uint32_t)(uintptr_t)g_async_mgr;
        }
      }
    }
  }
  // proactive pool release: hand empty GPU chunks back to the system
  // every tick, so the crowd's context creation finds contiguous CDRAM
  // instead of hitting the fragmentation wall. the bigmem retry does
  // this reactively when allocations fail; this does it before they can
  {
    extern uint32_t gxm_pool_release_empty(void);
    gxm_pool_release_empty();
  }
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
