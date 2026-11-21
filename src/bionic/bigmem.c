// big allocations spilling out of newlib's heap. the game wants more than
// a vita's main budget (~315MB live at the menus, measured), but when the
// heap ran dry there was still ~36MB of main memory free outside it plus
// ~70MB of idle CDRAM. so: main memblocks first (fast, cached), then
// cached CDRAM. both keep a reserve back (stacks/threads, GL textures).
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bigmem.h"
#include "util/abi.h"
#include "util/log.h"

#define CDRAM_GRAN   (256 * 1024)
#define MAIN_GRAN    4096
// reserves we never hand out. GL's own pool keeps spare room inside its
// chunks (31MB idle of 55MB in the match load test), and by the time we
// overflow the game has made its threads, so these can stay small.
#define GL_RESERVE   (6 * 1024 * 1024)
#define MAIN_RESERVE (8 * 1024 * 1024)  // thread stacks come from here (hw run: AAC threads failed at 4)
#define PC_GRAN      (1024 * 1024)       // phycont is 1 MiB granular
#define PC_RESERVE   (4 * 1024 * 1024)   // gxm's lpddr chunks come from here too, up to 4MB
#define MAX_BLOCKS   2048

typedef struct {
  void *base;
  size_t req, size;
  SceUID uid;
  int cdram;
  void *borrow;   // set when it's gpu pool space, not a memblock of ours
} blk;

void *gxm_borrow(uint32_t size, void **handle);  // gles/gxm.c
void gxm_unborrow(void *handle);

static blk blocks[MAX_BLOCKS];
static int n_blocks;
static SceKernelLwMutexWork lock;
static int ready, cached_type = 1;
// address range of everything we ever handed out, for a lock-free "not ours"
static volatile uintptr_t lo = ~(uintptr_t)0, hi;
static size_t live, peak;

// newlib's heap is one memblock, nothing inside it is ever ours. main
// memblocks can land on either side of it, so lo/hi alone won't filter.
static uintptr_t heap_lo, heap_hi;

// the glass reserve: held from boot, handed over only when a big ask has
// exhausted everything else. the match load's scene block (31.8MB) and the
// career exit's context asks (10-12MB) need one contiguous run that a
// fragmented late-load heap can't give them (hw31-33: dead at 55%, with
// cdram nominally free both times). taken at boot while cdram is still
// empty. reserve_mb=0 turns it off
#define RESERVE_MIN (8 * 1024 * 1024)
static SceUID g_res_uid = -1;
static size_t g_res_sz;

static void reserve_init(void)
{
  extern int config_int(const char *key, int def);
  int mb = config_int("reserve_mb", 36);
  if (mb < 16) return;
  g_res_sz = (size_t)mb << 20;
  g_res_uid = sceKernelAllocMemBlock("nba_res", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, g_res_sz, NULL);
  if (g_res_uid < 0) {
    L("bigmem: no %dMB cdram reserve (0x%08X)", mb, g_res_uid);
    g_res_uid = -1;
    return;
  }
  L("bigmem: %dMB cdram reserve held back for scene loads", mb);
}

void bigmem_init(void) {
  if (ready) return;
  sceKernelCreateLwMutex(&lock, "bigmem", 0, 0, NULL);
  reserve_init();
  void *probe = malloc(16);
  SceKernelMemBlockInfo mi;
  memset(&mi, 0, sizeof(mi));
  mi.size = sizeof(mi);
  if (probe && sceKernelGetMemBlockInfoByAddr(probe, &mi) >= 0) {
    heap_lo = (uintptr_t)mi.mappedBase;
    heap_hi = heap_lo + mi.mappedSize;
  }
  free(probe);
  ready = 1;
}

static int maybe_ours(uintptr_t a) {
  if (a >= heap_lo && a < heap_hi) return 0;
  return a >= lo && a < hi;
}

static int find(const void *p) {
  for (int i = 0; i < n_blocks; i++)
    if (blocks[i].base == p) return i;
  return -1;
}

static size_t main_live, cdram_live;

// urgent = the caller is about to hand the game a NULL (which it crashes
// on). then the reserves shrink to small floors: a texture or thread
// failing later beats a crash right now. urgent_no_floor=1 takes that all
// the way: the load tail died with user 2.06MB free, a 682K ask refused
// only because of this 2MB floor
#define MAIN_FLOOR  (2 * 1024 * 1024)
#define PC_FLOOR    0                    // gxm's own chunks are 2MB, a 1MB floor saved nothing
#define CDRAM_FLOOR (256 * 1024)

static void *bigmem_alloc_inner(size_t n, int urgent, int no_release);

// the whole chain failed for a big ask: break the reserve and try once
// more. if this fails too the game derefs the NULL either way
void *bigmem_alloc_ex(size_t n, int urgent) {
  void *p = bigmem_alloc_inner(n, urgent, 0);
  if (p || n < RESERVE_MIN || g_res_uid < 0) return p;
  sceKernelFreeMemBlock(g_res_uid);
  g_res_uid = -1;
  L("bigmem: broke the %uMB reserve for a %uK ask", (unsigned)(g_res_sz >> 20), (unsigned)(n >> 10));
  return bigmem_alloc_inner(n, urgent, 1);
}

static void *bigmem_alloc_inner(size_t n, int urgent, int no_release) {
  if (!ready) return NULL;
  static int zero_floor = -1;
  if (zero_floor < 0) {
    extern int config_int(const char *key, int def);
    zero_floor = config_int("urgent_no_floor", 0);
    if (zero_floor) L("bigmem: urgent asks ignore the last floors (urgent_no_floor=1)");
  }
  const size_t main_res = urgent ? (zero_floor ? 0 : MAIN_FLOOR) : MAIN_RESERVE;
  const size_t pc_res = urgent ? PC_FLOOR : PC_RESERVE;
  const size_t gl_res = urgent ? (zero_floor ? 0 : CDRAM_FLOOR) : GL_RESERVE;

  SceKernelFreeMemorySizeInfo fi;
  memset(&fi, 0, sizeof(fi));
  fi.size = sizeof(fi);
  sceKernelGetFreeMemorySize(&fi);
  // hw has reported user as -2MB once things got tight, don't let that
  // turn into a huge size_t and wave the reserve checks through
  if ((int)fi.size_user < 0) fi.size_user = 0;
  if ((int)fi.size_cdram < 0) fi.size_cdram = 0;
  if ((int)fi.size_phycont < 0) fi.size_phycont = 0;

  SceUID uid = -1;
  size_t sz = ALIGN_UP(n, MAIN_GRAN);
  int in_cdram = 0;
  // main memory first, it's normal cached ram
  if ((size_t)fi.size_user >= sz + main_res)
    uid = sceKernelAllocMemBlock("nba_big", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz, NULL);

  // phycont next: cached ram in its own 26MB pool, mostly gxm rings. the
  // match load died with ~8MB sitting idle in there. 1MB+ blocks only,
  // anything smaller would waste most of its 1MB granule
  if (uid < 0 && n >= (urgent ? 64 * 1024 : PC_GRAN) && (size_t)fi.size_phycont >= ALIGN_UP(n, PC_GRAN) + pc_res) {
    sz = ALIGN_UP(n, PC_GRAN);
    uid = sceKernelAllocMemBlock("nba_big", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, sz, NULL);
  }

  if (uid < 0) {
    sz = ALIGN_UP(n, CDRAM_GRAN);
    if ((size_t)fi.size_cdram < sz + gl_res) {
      // last stop: idle space inside the gpu pool's chunks
      void *h = NULL;
      void *bp = urgent && n <= 2 * 1024 * 1024 ? gxm_borrow((uint32_t)n, &h) : NULL;
      if (bp) {
        sz = ALIGN_UP(n, 16);
        sceKernelLockLwMutex(&lock, 1, NULL);
        if (n_blocks >= MAX_BLOCKS) {
          sceKernelUnlockLwMutex(&lock, 1);
          gxm_unborrow(h);
          return NULL;
        }
        blocks[n_blocks++] = (blk){ bp, n, sz, -1, 1, h };
        cdram_live += sz;
        live += sz;
        if ((uintptr_t)bp < lo) lo = (uintptr_t)bp;
        if ((uintptr_t)bp + sz > hi) hi = (uintptr_t)bp + sz;
        sceKernelUnlockLwMutex(&lock, 1);
        return bp;
      }
      // an ask with nowhere to go: hand the gl pool's empty chunks back
      // and run the deferred gpu frees (the match scene's already-freed
      // blocks waiting on the gpu), then try the whole chain again once.
      // >=1MB: the load's tail dies on ~2MB asks (hw35: 1.9MB at the floors
      // with every pool fragmented below that size)
      if (n >= 1024 * 1024 && !no_release) {
        extern uint32_t gxm_pool_release_empty(void);
        extern uint32_t gxm_defer_drain(void);
        uint32_t got = gxm_pool_release_empty();
        uint32_t drained = gxm_defer_drain();
        if (got || drained) {
          L("bigmem: %u KiB of empty gl chunks + %u deferred frees for a %uK ask, retrying",
            got >> 10, drained, (unsigned)(n >> 10));
          return bigmem_alloc_inner(n, urgent, 1);
        }
      }
      if (urgent)
        L("bigmem: even the floors are hit (user %uK, cdram %uK, phycont %uK), %uK has nowhere to go",
          fi.size_user / 1024, fi.size_cdram / 1024, fi.size_phycont / 1024, (unsigned)(n / 1024));
      else
        L_ONCE("bigmem: main + cdram both at their reserves (user %uK, cdram %uK)",
               fi.size_user / 1024, fi.size_cdram / 1024);
      return NULL;
    }
    // cpu-cached cdram if we can get it, plain (uncached, slow reads) if not
    if (cached_type) {
      uid = sceKernelAllocMemBlock("nba_big", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW, sz, NULL);
      if (uid < 0) {
        cached_type = 0;
        L("bigmem: cached cdram refused (0x%08X), using plain cdram", uid);
      }
    }
    if (uid < 0) uid = sceKernelAllocMemBlock("nba_big", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL);
    if (uid < 0) {
      // cdram had nominal room but no contiguous run that big: fragmentation,
      // not exhaustion (hw33: a 31MB ask died here with cdram free)
      L("bigmem: %uK ask refused by plain cdram too (0x%08X), %uK cdram free",
        (unsigned)(n >> 10), uid, (unsigned)(fi.size_cdram >> 10));
      return NULL;
    }
    in_cdram = 1;
  }
  void *base = NULL;
  sceKernelGetMemBlockBase(uid, &base);

  sceKernelLockLwMutex(&lock, 1, NULL);
  if (n_blocks >= MAX_BLOCKS) {
    sceKernelUnlockLwMutex(&lock, 1);
    sceKernelFreeMemBlock(uid);
    L_ONCE("bigmem: out of block slots");
    return NULL;
  }
  blocks[n_blocks++] = (blk){ base, n, sz, uid, in_cdram, NULL };
  if (in_cdram) cdram_live += sz; else main_live += sz;
  if ((uintptr_t)base < lo) lo = (uintptr_t)base;
  if ((uintptr_t)base + sz > hi) hi = (uintptr_t)base + sz;
  live += sz;
  int new_peak = live > peak;
  if (new_peak) peak = live;
  sceKernelUnlockLwMutex(&lock, 1);

  if (new_peak && (peak >> 23) != ((peak - sz) >> 23))
    L("bigmem: %uK overflow now (main %uK, cdram %uK %s, %d blocks)", (unsigned)(live / 1024),
      (unsigned)(main_live / 1024), (unsigned)(cdram_live / 1024), cached_type ? "cached" : "uncached", n_blocks);
  return base;
}

void *bigmem_alloc(size_t n) { return bigmem_alloc_ex(n, 0); }

int bigmem_is_borrowed(const void *p) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = find(p);
  int r = i >= 0 && blocks[i].borrow != NULL;
  sceKernelUnlockLwMutex(&lock, 1);
  return r;
}

int bigmem_owns(const void *p) {
  if (!maybe_ours((uintptr_t)p)) return 0;  // fast path, nearly every free()
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = find(p);
  sceKernelUnlockLwMutex(&lock, 1);
  return i >= 0;
}

size_t bigmem_size(const void *p) {
  if (!maybe_ours((uintptr_t)p)) return 0;
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = find(p);
  size_t s = i >= 0 ? blocks[i].req : 0;
  sceKernelUnlockLwMutex(&lock, 1);
  return s;
}

void bigmem_free(void *p) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = find(p);
  if (i < 0) {
    sceKernelUnlockLwMutex(&lock, 1);
    return;
  }
  SceUID uid = blocks[i].uid;
  void *borrow = blocks[i].borrow;
  live -= blocks[i].size;
  if (blocks[i].cdram) cdram_live -= blocks[i].size; else main_live -= blocks[i].size;
  blocks[i] = blocks[--n_blocks];
  sceKernelUnlockLwMutex(&lock, 1);
  if (borrow) gxm_unborrow(borrow);
  else sceKernelFreeMemBlock(uid);
}

void bigmem_report(void) {
  if (peak) L("bigmem: %uK live (main %uK, cdram %uK) in %d blocks, peak %uK", (unsigned)(live / 1024),
              (unsigned)(main_live / 1024), (unsigned)(cdram_live / 1024), n_blocks, (unsigned)(peak / 1024));
}
