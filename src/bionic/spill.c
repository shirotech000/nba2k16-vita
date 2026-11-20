// small-allocation spill. once newlib's heap is full the game still asks for
// little blocks all the time, and it crashes on every NULL (hw match load
// died on an 8K malloc with 7MB of main ram free). so anything under 64K that
// malloc() refuses gets carved out of 1MB bigmem blocks here: first fit,
// free neighbours get merged while scanning. only used after malloc said no.
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/spill.h"
#include "bionic/bigmem.h"
#include "util/log.h"

void mem_rainy_day_release(void);  // libc.c

#define SP_CHUNK   (1024 * 1024)
#define SP_CHUNKS  64
#define SP_HDR     16
#define SP_USED    0x5A11ED01u
#define SP_FREE    0x5A11F4EEu

typedef struct {
  uint32_t size;   // whole block incl. header, multiple of 16
  uint32_t tag;    // SP_USED / SP_FREE
  uint32_t req;    // what the caller asked for
  uint32_t pad;
} sp_hdr;

static uint8_t *chunks[SP_CHUNKS];
static int n_chunks;
static uintptr_t lo = UINTPTR_MAX, hi;
static SceKernelLwMutexWork lock;
static int ready;
static size_t live;

static void sp_init(void) {
  if (ready) return;
  sceKernelCreateLwMutex(&lock, "spill", 0, 0, NULL);
  ready = 1;
}

static int chunk_of(const void *p) {
  uintptr_t a = (uintptr_t)p;
  if (a < lo || a >= hi) return -1;
  for (int i = 0; i < n_chunks; i++)
    if (a > (uintptr_t)chunks[i] && a < (uintptr_t)chunks[i] + SP_CHUNK) return i;
  return -1;
}

// first fit in one chunk, merging runs of free blocks as it goes
static void *fit(uint8_t *c, uint32_t need, size_t n) {
  uint8_t *end = c + SP_CHUNK;
  for (uint8_t *b = c; b < end;) {
    sp_hdr *h = (sp_hdr *)b;
    if (h->tag == SP_FREE) {
      for (uint8_t *nx = b + h->size; nx < end && ((sp_hdr *)nx)->tag == SP_FREE; nx = b + h->size)
        h->size += ((sp_hdr *)nx)->size;
      if (h->size >= need) {
        if (h->size - need >= 64) {
          sp_hdr *rest = (sp_hdr *)(b + need);
          rest->size = h->size - need;
          rest->tag = SP_FREE;
          h->size = need;
        }
        h->tag = SP_USED;
        h->req = (uint32_t)n;
        live += h->size;
        return b + SP_HDR;
      }
    }
    b += h->size;
  }
  return NULL;
}

void *spill_alloc(size_t n) {
  if (!n) n = 1;
  if (n >= SP_CHUNK / 2) return NULL;
  sp_init();
  uint32_t need = (uint32_t)((n + SP_HDR + 15) & ~(size_t)15);
  sceKernelLockLwMutex(&lock, 1, NULL);
  void *p = NULL;
  for (int i = 0; i < n_chunks && !p; i++) p = fit(chunks[i], need, n);
  if (!p && n_chunks < SP_CHUNKS) {
    if (!n_chunks) mem_rainy_day_release();
    uint8_t *c = bigmem_alloc_ex(SP_CHUNK, 1);
    if (c) {
      sp_hdr *h = (sp_hdr *)c;
      h->size = SP_CHUNK;
      h->tag = SP_FREE;
      chunks[n_chunks++] = c;
      if ((uintptr_t)c < lo) lo = (uintptr_t)c;
      if ((uintptr_t)c + SP_CHUNK > hi) hi = (uintptr_t)c + SP_CHUNK;
      L("spill: heap's full, small allocs go to spill chunk #%d now (%uK in use)", n_chunks,
        (unsigned)(live / 1024));
      p = fit(c, need, n);
    }
  }
  sceKernelUnlockLwMutex(&lock, 1);
  return p;
}

int spill_owns(const void *p) {
  if (!ready || !n_chunks) return 0;
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = chunk_of(p);
  sceKernelUnlockLwMutex(&lock, 1);
  return i >= 0;
}

size_t spill_usable(const void *p) {
  const sp_hdr *h = (const sp_hdr *)((const uint8_t *)p - SP_HDR);
  return h->tag == SP_USED ? h->size - SP_HDR : 0;
}

void spill_free(void *p) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = chunk_of(p);
  sp_hdr *h = (sp_hdr *)((uint8_t *)p - SP_HDR);
  if (i < 0 || h->tag != SP_USED) {
    sceKernelUnlockLwMutex(&lock, 1);
    L_ONCE("spill: free of %p that isn't a live spill block", p);
    return;
  }
  h->tag = SP_FREE;
  live -= h->size;
  // whole chunk empty again? hand it back, but keep two around (the heap
  // stays full once it's full, and a chunk per alloc/free pair is churn)
  uint8_t *c = chunks[i];
  sp_hdr *first = (sp_hdr *)c;
  for (uint8_t *nx = c + first->size; first->tag == SP_FREE && nx < c + SP_CHUNK && ((sp_hdr *)nx)->tag == SP_FREE;
       nx = c + first->size)
    first->size += ((sp_hdr *)nx)->size;
  if (first->tag == SP_FREE && first->size == SP_CHUNK && n_chunks > 2) {
    chunks[i] = chunks[--n_chunks];
    sceKernelUnlockLwMutex(&lock, 1);
    bigmem_free(c);
    return;
  }
  sceKernelUnlockLwMutex(&lock, 1);
}
