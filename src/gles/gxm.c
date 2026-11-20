// sceGxm device: memory, rings, scenes, display. written straight against
// sony's gxm api, see gxm.h for the overview.
#include "gles/gxm.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "util/abi.h"
#include "util/log.h"

SceGxmContext *gxm_ctx;
SceGxmShaderPatcher *gxm_patcher;

static int g_ready;

// ============================================================ memblocks

enum { MAP_GPU, MAP_VUSSE, MAP_FUSSE };

typedef struct {
  SceUID uid;
  uint8_t *base;
  uint32_t size;
  int cdram;
  int map;
} memblk;

static struct {
  uint64_t blk[2], blk_peak[2];     // [0]=cdram [1]=lpddr, raw memblock bytes
  uint64_t used[2], used_peak[2];   // pooled allocations actually handed out
  uint32_t n_blocks;
  int logged_fallback;
} st;

static void st_blk(int cdram, int64_t delta)
{
  int i = cdram ? 0 : 1;
  st.blk[i] += delta;
  if (st.blk[i] > st.blk_peak[i]) st.blk_peak[i] = st.blk[i];
  st.n_blocks += delta > 0 ? 1 : -1;
}

// the 26MB phycont pool is separate from the main user budget, and the
// game eats the main one alive. so non-cdram gpu stuff goes there first.
static int phycont_full;

static int blk_alloc(memblk *m, uint32_t size, int cdram, int map, unsigned *usse_off)
{
  SceUID uid = -1;
  if (!cdram && !phycont_full) {
    uint32_t psize = ALIGN_UP(size, 1024 * 1024);
    uid = sceKernelAllocMemBlock("nba_phycont", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, psize, NULL);
    if (uid >= 0) size = psize;
    else {
      phycont_full = 1;
      L("gxm: phycont pool full (0x%08X), lpddr from the main budget from now on", uid);
    }
  }
  if (uid < 0) {
    uint32_t gran = cdram ? 256 * 1024 : 4096;
    size = ALIGN_UP(size, gran);
    uid = sceKernelAllocMemBlock(cdram ? "nba_cdram" : "nba_lpddr",
        cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        size, NULL);
  }
  if (uid < 0) return uid;

  void *base = NULL;
  sceKernelGetMemBlockBase(uid, &base);
  int r = 0;
  if (map == MAP_GPU)
    r = sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW);
  else if (map == MAP_VUSSE)
    r = sceGxmMapVertexUsseMemory(base, size, usse_off);
  else
    r = sceGxmMapFragmentUsseMemory(base, size, usse_off);
  if (r < 0) {
    L("gxm: map of %u bytes (%s) failed: 0x%08X", size, cdram ? "cdram" : "lpddr", r);
    sceKernelFreeMemBlock(uid);
    return r;
  }
  m->uid = uid;
  m->base = base;
  m->size = size;
  m->cdram = cdram;
  m->map = map;
  st_blk(cdram, size);
  return 0;
}

static void blk_free(memblk *m)
{
  if (m->map == MAP_GPU) sceGxmUnmapMemory(m->base);
  else if (m->map == MAP_VUSSE) sceGxmUnmapVertexUsseMemory(m->base);
  else sceGxmUnmapFragmentUsseMemory(m->base);
  sceKernelFreeMemBlock(m->uid);
  st_blk(m->cdram, -(int64_t)m->size);
}

// ============================================================ free-list heap
// sorted list of free ranges, first fit, coalesces on free.

typedef struct fl_node {
  uint32_t off, size;
  struct fl_node *next;
} fl_node;

typedef struct {
  uint32_t size, used;
  fl_node *free;
} range_heap;

static void rh_init(range_heap *h, uint32_t size)
{
  h->size = size;
  h->used = 0;
  h->free = malloc(sizeof(fl_node));
  h->free->off = 0;
  h->free->size = size;
  h->free->next = NULL;
}

static void rh_destroy(range_heap *h)
{
  while (h->free) { fl_node *n = h->free->next; free(h->free); h->free = n; }
}

// returns 0 + *out on success
static int rh_alloc(range_heap *h, uint32_t size, uint32_t align, uint32_t *out)
{
  fl_node **pp = &h->free;
  for (fl_node *n = h->free; n; pp = &n->next, n = n->next) {
    uint32_t start = ALIGN_UP(n->off, align);
    uint32_t pad = start - n->off;
    if (n->size < pad || n->size - pad < size) continue;
    uint32_t tail = n->size - pad - size;
    if (pad && tail) {
      // split in two, keep the front pad as this node
      fl_node *t = malloc(sizeof(fl_node));
      t->off = start + size;
      t->size = tail;
      t->next = n->next;
      n->size = pad;
      n->next = t;
    } else if (pad) {
      n->size = pad;
    } else if (tail) {
      n->off = start + size;
      n->size = tail;
    } else {
      *pp = n->next;
      free(n);
    }
    h->used += size;
    *out = start;
    return 0;
  }
  return -1;
}

static void rh_free(range_heap *h, uint32_t off, uint32_t size)
{
  fl_node *prev = NULL, *n = h->free;
  while (n && n->off < off) { prev = n; n = n->next; }
  h->used -= size;
  // merge with prev?
  if (prev && prev->off + prev->size == off) {
    prev->size += size;
    if (n && prev->off + prev->size == n->off) {
      prev->size += n->size;
      prev->next = n->next;
      free(n);
    }
    return;
  }
  if (n && off + size == n->off) {
    n->off = off;
    n->size += size;
    return;
  }
  fl_node *x = malloc(sizeof(fl_node));
  x->off = off;
  x->size = size;
  x->next = n;
  if (prev) prev->next = x; else h->free = x;
}

// ============================================================ gpu pool

#define CHUNK_SIZE      (8 * 1024 * 1024)
#define DEDICATED_MIN   (2 * 1024 * 1024)

struct gpu_chunk {
  memblk mb;
  range_heap heap;
  int dedicated;
  gpu_chunk *next, *prev;
};

static gpu_chunk *g_chunks[2];   // [0]=cdram [1]=lpddr

static void chunk_link(gpu_chunk *c, int ti)
{
  c->prev = NULL;
  c->next = g_chunks[ti];
  if (g_chunks[ti]) g_chunks[ti]->prev = c;
  g_chunks[ti] = c;
}

static void chunk_unlink(gpu_chunk *c, int ti)
{
  if (c->prev) c->prev->next = c->next; else g_chunks[ti] = c->next;
  if (c->next) c->next->prev = c->prev;
}

static gpu_chunk *chunk_new(uint32_t size, int cdram, int dedicated)
{
  gpu_chunk *c = calloc(1, sizeof(*c));
  if (blk_alloc(&c->mb, size, cdram, MAP_GPU, NULL) < 0) { free(c); return NULL; }
  c->dedicated = dedicated;
  rh_init(&c->heap, c->mb.size);
  chunk_link(c, cdram ? 0 : 1);
  return c;
}

static int pool_try(gpu_block *b, uint32_t size, uint32_t align, int cdram)
{
  int ti = cdram ? 0 : 1;
  uint32_t off;

  if (size >= DEDICATED_MIN) {
    gpu_chunk *c = chunk_new(size, cdram, 1);
    if (!c) return -1;
    rh_alloc(&c->heap, size, align, &off);
    b->chunk = c; b->off = off;
    return 0;
  }

  for (gpu_chunk *c = g_chunks[ti]; c; c = c->next) {
    if (c->dedicated || c->heap.size - c->heap.used < size) continue;
    if (rh_alloc(&c->heap, size, align, &off) == 0) {
      b->chunk = c; b->off = off;
      return 0;
    }
  }
  // new chunk, shrink the ask if memory is tight
  uint32_t gran = cdram ? 256 * 1024 : 4096;
  for (uint32_t cs = CHUNK_SIZE; cs >= gran && cs >= size; cs >>= 1) {
    gpu_chunk *c = chunk_new(cs, cdram, 0);
    if (!c) continue;
    if (rh_alloc(&c->heap, size, align, &off) == 0) {
      b->chunk = c; b->off = off;
      return 0;
    }
  }
  return -1;
}

gpu_block *gpu_alloc(uint32_t size, uint32_t align, int pref)
{
  if (!size) size = 16;
  if (align < 16) align = 16;
  size = ALIGN_UP(size, 16);
  gpu_block *b = calloc(1, sizeof(*b));
  int ok = -1;
  if (pref == GPU_PREF_CDRAM) {
    ok = pool_try(b, size, align, 1);
    if (ok < 0 && !st.logged_fallback) {
      st.logged_fallback = 1;
      L("gxm: cdram full (wanted %u), falling back to lpddr from now on where needed", size);
      gpu_mem_log("cdram full");
    }
  }
  if (ok < 0) ok = pool_try(b, size, align, 0);
  if (ok < 0) {
    L("gxm: OUT OF GPU MEMORY, %u bytes", size);
    gpu_mem_log("oom");
    free(b);
    return NULL;
  }
  b->size = size;
  b->ptr = b->chunk->mb.base + b->off;
  int ti = b->chunk->mb.cdram ? 0 : 1;
  st.used[ti] += size;
  if (st.used[ti] > st.used_peak[ti]) {
    // log every time the peak grows by 4 MiB so we can watch the curve
    if ((st.used[ti] >> 22) != (st.used_peak[ti] >> 22))
      L("gxm: new %s peak %u KiB", ti ? "lpddr" : "cdram", (unsigned)(st.used[ti] >> 10));
    st.used_peak[ti] = st.used[ti];
  }
  return b;
}

void gpu_free(gpu_block *b)
{
  if (!b) return;
  gpu_chunk *c = b->chunk;
  int ti = c->mb.cdram ? 0 : 1;
  st.used[ti] -= b->size;
  rh_free(&c->heap, b->off, b->size);
  if (c->dedicated) {
    chunk_unlink(c, ti);
    rh_destroy(&c->heap);
    blk_free(&c->mb);
    free(c);
  }
  free(b);
}

static void gpu_free_cb(void *p) { gpu_free(p); }

void gpu_free_deferred(gpu_block *b)
{
  if (b) gxm_defer(gpu_free_cb, b);
}

int gpu_is_cdram(const gpu_block *b) { return b && b->chunk->mb.cdram; }

// ============================================================ ptr -> info map
// the patcher frees by pointer, so remember what we handed out

typedef struct pmap_node {
  void *key;
  void *val;
  uint32_t size;
  struct pmap_node *next;
} pmap_node;

#define PMAP_BUCKETS 1024
typedef struct { pmap_node *b[PMAP_BUCKETS]; } pmap;

static unsigned pmap_hash(void *k) { return ((uintptr_t)k >> 4) * 2654435761u >> 22; }

static void pmap_put(pmap *m, void *k, void *v, uint32_t size)
{
  pmap_node *n = malloc(sizeof(*n));
  unsigned h = pmap_hash(k) & (PMAP_BUCKETS - 1);
  n->key = k; n->val = v; n->size = size;
  n->next = m->b[h];
  m->b[h] = n;
}

static pmap_node *pmap_take(pmap *m, void *k)
{
  unsigned h = pmap_hash(k) & (PMAP_BUCKETS - 1);
  for (pmap_node **pp = &m->b[h]; *pp; pp = &(*pp)->next) {
    if ((*pp)->key == k) {
      pmap_node *n = *pp;
      *pp = n->next;
      return n;
    }
  }
  return NULL;
}

// ============================================================ usse heaps

typedef struct usse_heap {
  memblk mb;
  range_heap heap;
  unsigned base_off;
  struct usse_heap *next;
} usse_heap;

static usse_heap *g_usse[2];   // [0]=vertex [1]=fragment
static pmap g_usse_map, g_buf_map;

#define USSE_HEAP_SIZE (1024 * 1024)

static void *usse_alloc(int frag, uint32_t size, unsigned *usse_off)
{
  uint32_t off;
  size = ALIGN_UP(size, 16);
  for (usse_heap *h = g_usse[frag]; ; h = h->next) {
    if (!h) {
      h = calloc(1, sizeof(*h));
      uint32_t hs = size > USSE_HEAP_SIZE ? ALIGN_UP(size, 4096) : USSE_HEAP_SIZE;
      if (blk_alloc(&h->mb, hs, 0, frag ? MAP_FUSSE : MAP_VUSSE, &h->base_off) < 0) {
        L("gxm: can't get %s usse memory (%u)", frag ? "fragment" : "vertex", size);
        free(h);
        return NULL;
      }
      rh_init(&h->heap, h->mb.size);
      h->next = g_usse[frag];
      g_usse[frag] = h;
      L("gxm: new %s usse heap, %u KiB", frag ? "fragment" : "vertex", h->mb.size >> 10);
    }
    if (rh_alloc(&h->heap, size, 64, &off) == 0) {
      void *p = h->mb.base + off;
      *usse_off = h->base_off + off;
      pmap_put(&g_usse_map, p, h, size);
      return p;
    }
  }
}

static void usse_free(void *p)
{
  pmap_node *n = pmap_take(&g_usse_map, p);
  if (!n) { L("gxm: usse_free of unknown %p", p); return; }
  usse_heap *h = n->val;
  rh_free(&h->heap, (uint32_t)((uint8_t *)p - h->mb.base), n->size);
  free(n);
}

// patcher callbacks
static void *pat_host_alloc(void *ud, SceSize size) { (void)ud; return malloc(size); }
static void pat_host_free(void *ud, void *mem) { (void)ud; free(mem); }

static void *pat_buf_alloc(void *ud, SceSize size)
{
  (void)ud;
  gpu_block *b = gpu_alloc(size, 256, GPU_PREF_LPDDR);
  if (!b) return NULL;
  pmap_put(&g_buf_map, b->ptr, b, 0);
  return b->ptr;
}

static void pat_buf_free(void *ud, void *mem)
{
  (void)ud;
  pmap_node *n = pmap_take(&g_buf_map, mem);
  if (!n) { L("gxm: patcher freed unknown buffer %p", mem); return; }
  gpu_free(n->val);
  free(n);
}

static void *pat_vusse_alloc(void *ud, SceSize size, unsigned int *off) { (void)ud; return usse_alloc(0, size, off); }
static void *pat_fusse_alloc(void *ud, SceSize size, unsigned int *off) { (void)ud; return usse_alloc(1, size, off); }
static void pat_usse_free(void *ud, void *mem) { (void)ud; usse_free(mem); }

// ============================================================ fences + deferred work

static volatile unsigned int *g_notif;
static uint32_t g_scene_ctr;      // last begun scene id
static int g_in_scene;

uint32_t gxm_scene_counter(void) { return g_scene_ctr; }

int gxm_scene_done(uint32_t id)
{
  if (!g_notif) return 1;
  return (int32_t)(*g_notif - id) >= 0;
}

void gxm_wait_scene(uint32_t id)
{
  if (!g_ready || gxm_scene_done(id)) return;
  if (g_in_scene && id >= g_scene_ctr) gxm_end_scene();
  int spins = 0;
  while (!gxm_scene_done(id)) {
    sceKernelDelayThread(100);
    if (++spins == 20000) L("gxm: waited 2s for scene %u (gpu at %u), gpu hang?", id, *g_notif);
  }
}

typedef struct {
  void (*fn)(void *);
  void *arg;
  uint32_t scene;
} deferred_t;

static deferred_t *g_def;
static uint32_t g_def_head, g_def_count, g_def_cap;

static void defer_at(void (*fn)(void *), void *arg, uint32_t scene)
{
  if (g_def_count == g_def_cap) {
    uint32_t ncap = g_def_cap ? g_def_cap * 2 : 256;
    deferred_t *n = malloc(ncap * sizeof(*n));
    for (uint32_t i = 0; i < g_def_count; i++) n[i] = g_def[(g_def_head + i) % g_def_cap];
    free(g_def);
    g_def = n;
    g_def_cap = ncap;
    g_def_head = 0;
  }
  deferred_t *d = &g_def[(g_def_head + g_def_count) % g_def_cap];
  d->fn = fn;
  d->arg = arg;
  d->scene = scene;
  g_def_count++;
}

void gxm_defer(void (*fn)(void *), void *arg)
{
  // gpu already caught up -> nothing can still be reading it
  if (!g_in_scene && gxm_scene_done(g_scene_ctr)) { fn(arg); return; }
  defer_at(fn, arg, g_scene_ctr);
}

static void process_deferred(void)
{
  while (g_def_count) {
    deferred_t *d = &g_def[g_def_head];
    if (!gxm_scene_done(d->scene)) break;
    void (*fn)(void *) = d->fn;
    void *arg = d->arg;
    g_def_head = (g_def_head + 1) % g_def_cap;
    g_def_count--;
    fn(arg);
  }
}

// ============================================================ transient ring

static memblk g_ring_mb;
static uint8_t *g_ring;
static uint32_t g_ring_head, g_ring_used, g_ring_pending;

typedef struct { uint32_t scene, bytes; } ring_mark_t;
#define RING_MARKS 1024
static ring_mark_t g_marks[RING_MARKS];
static uint32_t g_mark_tail, g_mark_count;

static void ring_retire(void)
{
  while (g_mark_count && gxm_scene_done(g_marks[g_mark_tail].scene)) {
    g_ring_used -= g_marks[g_mark_tail].bytes;
    g_mark_tail = (g_mark_tail + 1) % RING_MARKS;
    g_mark_count--;
  }
  if (!g_ring_used && !g_ring_pending) g_ring_head = 0;
}

static void ring_mark(uint32_t scene)
{
  if (!g_ring_pending) return;
  if (g_mark_count == RING_MARKS) {
    gxm_wait_scene(g_marks[g_mark_tail].scene);
    ring_retire();
  }
  ring_mark_t *m = &g_marks[(g_mark_tail + g_mark_count) % RING_MARKS];
  m->scene = scene;
  m->bytes = g_ring_pending;
  g_mark_count++;
  g_ring_pending = 0;
}

static void big_free_cb(void *p) { gpu_free(p); }

void *ring_alloc(uint32_t size, uint32_t align)
{
  if (align < 4) align = 4;
  size = ALIGN_UP(size, 4);

  if (size > GXM_TRANSIENT_RING_SIZE / 4) {
    // huge one-off, give it its own block. the draw that uses it lands in
    // the open scene or the next one, free after that.
    L_ONCE("gxm: transient alloc of %u bytes bypasses the ring", size);
    gpu_block *b = gpu_alloc(size, align, GPU_PREF_LPDDR);
    if (!b) return NULL;
    defer_at(big_free_cb, b, g_scene_ctr + (g_in_scene ? 0 : 1));
    return b->ptr;
  }

  for (;;) {
    uint32_t start = ALIGN_UP(g_ring_head, align);
    uint32_t need;
    if (start + size > GXM_TRANSIENT_RING_SIZE) {
      start = 0;
      need = (GXM_TRANSIENT_RING_SIZE - g_ring_head) + size;
    } else {
      need = (start - g_ring_head) + size;
    }
    ring_retire();
    if (g_ring_used + need <= GXM_TRANSIENT_RING_SIZE) {
      g_ring_used += need;
      g_ring_pending += need;
      g_ring_head = start + size;
      return g_ring + start;
    }
    if (g_mark_count) {
      gxm_wait_scene(g_marks[g_mark_tail].scene);
      continue;
    }
    if (g_in_scene) {
      L_ONCE("gxm: transient ring full inside one scene, splitting scenes (raise GXM_TRANSIENT_RING_SIZE?)");
      gxm_end_scene();
      continue;
    }
    // no scene open and nothing submitted: whatever is in there belongs to
    // draws that never made it into a scene, safe to throw away
    L_ONCE("gxm: transient ring reset (used %u pending %u need %u)", g_ring_used, g_ring_pending, need);
    g_ring_used = g_ring_pending = g_ring_head = 0;
  }
}

// ============================================================ render targets

typedef struct {
  unsigned w, h;
  SceGxmRenderTarget *rt;
  uint32_t last_frame;
  uint32_t scenes_this_frame, peak_scenes;
  unsigned drv_mem;
} rt_entry;

#define MAX_RTS 24
static rt_entry g_rts[MAX_RTS];
static int g_n_rts;
static uint32_t g_frame;
static uint32_t g_scenes_frame, g_scenes_peak;

static void rt_destroy_cb(void *p) { sceGxmDestroyRenderTarget(p); }

static rt_entry *rt_get(unsigned w, unsigned h)
{
  for (int i = 0; i < g_n_rts; i++)
    if (g_rts[i].w == w && g_rts[i].h == h) return &g_rts[i];

  rt_entry *e;
  if (g_n_rts < MAX_RTS) {
    e = &g_rts[g_n_rts++];
  } else {
    // evict the least recently used one, gpu might still have it so defer
    e = &g_rts[0];
    for (int i = 1; i < MAX_RTS; i++)
      if (g_rts[i].last_frame < e->last_frame) e = &g_rts[i];
    L("gxm: evicting render target %ux%u", e->w, e->h);
    if (e->rt) gxm_defer(rt_destroy_cb, e->rt);
  }
  memset(e, 0, sizeof(*e));

  SceGxmRenderTargetParams p;
  memset(&p, 0, sizeof(p));
  p.flags = 0;
  p.width = w;
  p.height = h;
  p.scenesPerFrame = GXM_SCENES_PER_FRAME;
  p.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
  p.multisampleLocations = 0;
  p.driverMemBlock = -1;
  sceGxmGetRenderTargetMemSize(&p, &e->drv_mem);
  int r = sceGxmCreateRenderTarget(&p, &e->rt);
  if (r < 0) {
    L("gxm: sceGxmCreateRenderTarget %ux%u failed 0x%08X", w, h, r);
    e->rt = NULL;
  } else {
    L("gxm: render target %ux%u, driver mem %u KiB", w, h, e->drv_mem >> 10);
  }
  e->w = w;
  e->h = h;
  return e;
}

// ============================================================ scenes

static SceGxmColorSurface g_sc_color;
static SceGxmDepthStencilSurface g_sc_depth;
static int g_begin_fail_logs;

int gxm_in_scene(void) { return g_in_scene; }
int gxm_ready(void) { return g_ready; }
uint32_t gxm_frame_count(void) { return g_frame; }

void gxm_begin_scene(const gxm_target *t, int load_depth)
{
  if (!g_ready) return;
  if (g_in_scene) gxm_end_scene();

  rt_entry *e = rt_get(t->w, t->h);
  if (!e->rt) return;
  e->last_frame = g_frame;
  if (++e->scenes_this_frame > e->peak_scenes) {
    e->peak_scenes = e->scenes_this_frame;
    if (e->peak_scenes > GXM_SCENES_PER_FRAME)
      L("gxm: rt %ux%u hit %u scenes in one frame (scenesPerFrame=%d)", e->w, e->h,
        e->peak_scenes, GXM_SCENES_PER_FRAME);
  }

  if (t->color) g_sc_color = *t->color;
  else sceGxmColorSurfaceInitDisabled(&g_sc_color);

  if (t->depth) {
    g_sc_depth = *t->depth;
    // depth only survives a scene boundary if we ask for it
    sceGxmDepthStencilSurfaceSetForceLoadMode(&g_sc_depth,
        load_depth ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
    sceGxmDepthStencilSurfaceSetForceStoreMode(&g_sc_depth, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
  } else {
    sceGxmDepthStencilSurfaceInitDisabled(&g_sc_depth);
  }

  g_scene_ctr++;
  int r = sceGxmBeginScene(gxm_ctx, 0, e->rt, NULL, NULL, t->sync, &g_sc_color, &g_sc_depth);
  if (r < 0) {
    g_scene_ctr--;
    if (g_begin_fail_logs++ < 16)
      L("gxm: sceGxmBeginScene %ux%u failed 0x%08X", t->w, t->h, r);
    return;
  }
  g_in_scene = 1;
  g_scenes_frame++;
}

void gxm_end_scene(void)
{
  if (!g_in_scene) return;
  SceGxmNotification n;
  n.address = g_notif;
  n.value = g_scene_ctr;
  int r = sceGxmEndScene(gxm_ctx, NULL, &n);
  if (r < 0) L("gxm: sceGxmEndScene failed 0x%08X", r);
  g_in_scene = 0;
  ring_mark(g_scene_ctr);
  process_deferred();
}

void gxm_finish(void)
{
  if (!g_ready) return;
  gxm_end_scene();
  sceGxmFinish(gxm_ctx);
  gxm_wait_scene(g_scene_ctr);
  ring_retire();
  process_deferred();
}

// ============================================================ display

typedef struct { void *addr; } flip_data;

static memblk g_disp_mb[DISPLAY_BUFFERS];
static SceGxmColorSurface g_disp_surf[DISPLAY_BUFFERS];
static SceGxmSyncObject *g_disp_sync[DISPLAY_BUFFERS];
static gpu_block *g_depth_mem;
static SceGxmDepthStencilSurface g_disp_depth;
static unsigned g_front, g_back;
static volatile int g_vsync = 1;

static void display_cb(const void *data)
{
  const flip_data *d = data;
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  fb.base = d->addr;
  fb.pitch = DISPLAY_STRIDE;
  fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  fb.width = SCREEN_W;
  fb.height = SCREEN_H;
  sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
  int n = g_vsync;
  if (n == 1) sceDisplayWaitVblankStart();
  else if (n > 1) sceDisplayWaitVblankStartMulti(n);
}

void gxm_set_vsync(int interval) { g_vsync = interval < 0 ? 0 : interval; }

void gxm_display_target(gxm_target *out)
{
  out->color = &g_disp_surf[g_back];
  out->depth = &g_disp_depth;
  out->w = SCREEN_W;
  out->h = SCREEN_H;
  out->sync = g_disp_sync[g_back];
}

void *gxm_back_buffer(void) { return g_disp_mb[g_back].base; }
void *gxm_front_buffer(void) { return g_disp_mb[g_front].base; }

void gxm_swap(void)
{
  if (!g_ready) return;
  gxm_end_scene();

  sceGxmPadHeartbeat(&g_disp_surf[g_back], g_disp_sync[g_back]);
  flip_data d = { g_disp_mb[g_back].base };
  int r = sceGxmDisplayQueueAddEntry(g_disp_sync[g_front], g_disp_sync[g_back], &d);
  if (r < 0) L_ONCE("gxm: sceGxmDisplayQueueAddEntry failed 0x%08X", r);
  g_front = g_back;
  g_back = (g_back + 1) % DISPLAY_BUFFERS;

  g_frame++;
  if (g_scenes_frame > g_scenes_peak) {
    g_scenes_peak = g_scenes_frame;
    L("gxm: new peak of %u scenes in a frame (frame %u)", g_scenes_peak, g_frame);
  }
  g_scenes_frame = 0;
  for (int i = 0; i < g_n_rts; i++) g_rts[i].scenes_this_frame = 0;

  ring_retire();
  process_deferred();
  if (g_frame % 1800 == 0) gpu_mem_log("periodic");
}

// ============================================================ depth helpers

uint32_t gxm_depth_surface_size(unsigned w, unsigned h)
{
  return ALIGN_UP(w, SCE_GXM_TILE_SIZEX) * ALIGN_UP(h, SCE_GXM_TILE_SIZEY) * 4;
}

int gxm_depth_surface_init(SceGxmDepthStencilSurface *ds, unsigned w, unsigned h, void *mem)
{
  // packed d24s8, tiled. stencil pointer is NULL for the packed format
  // (unverified assumption, see report). check the return.
  int r = sceGxmDepthStencilSurfaceInit(ds, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, ALIGN_UP(w, SCE_GXM_TILE_SIZEX), mem, NULL);
  if (r < 0) L("gxm: depth surface init %ux%u failed 0x%08X", w, h, r);
  return r;
}

// ============================================================ stats

void gpu_mem_log(const char *why)
{
  SceKernelFreeMemorySizeInfo fi;
  memset(&fi, 0, sizeof(fi));
  fi.size = sizeof(fi);
  sceKernelGetFreeMemorySize(&fi);
  unsigned rt_mem = 0;
  for (int i = 0; i < g_n_rts; i++) rt_mem += g_rts[i].drv_mem;
  L("gxm mem [%s]: cdram blocks %u KiB (peak %u) used %u KiB (peak %u) | "
    "lpddr blocks %u KiB (peak %u) used %u KiB (peak %u) | %u memblocks, %d rts (%u KiB drv) | "
    "free: user %d KiB cdram %d KiB phycont %d KiB",
    why,
    (unsigned)(st.blk[0] >> 10), (unsigned)(st.blk_peak[0] >> 10),
    (unsigned)(st.used[0] >> 10), (unsigned)(st.used_peak[0] >> 10),
    (unsigned)(st.blk[1] >> 10), (unsigned)(st.blk_peak[1] >> 10),
    (unsigned)(st.used[1] >> 10), (unsigned)(st.used_peak[1] >> 10),
    st.n_blocks, g_n_rts, rt_mem >> 10,
    fi.size_user >> 10, fi.size_cdram >> 10, fi.size_phycont >> 10);
  if (gxm_patcher)
    L("gxm patcher: host %u KiB, buffer %u KiB, vusse %u KiB, fusse %u KiB",
      sceGxmShaderPatcherGetHostMemAllocated(gxm_patcher) >> 10,
      sceGxmShaderPatcherGetBufferMemAllocated(gxm_patcher) >> 10,
      sceGxmShaderPatcherGetVertexUsseMemAllocated(gxm_patcher) >> 10,
      sceGxmShaderPatcherGetFragmentUsseMemAllocated(gxm_patcher) >> 10);
}

// ============================================================ init

static void *ctx_host_mem;
static memblk g_vdm_mb, g_vtx_mb, g_frag_mb, g_fusse_mb;

int gxm_init(void)
{
  if (g_ready) return 0;
  int r;
  gpu_mem_log("before gxm init");

  SceGxmInitializeParams ip;
  memset(&ip, 0, sizeof(ip));
  ip.flags = 0;
  ip.displayQueueMaxPendingCount = DISPLAY_BUFFERS - 1;
  ip.displayQueueCallback = display_cb;
  ip.displayQueueCallbackDataSize = sizeof(flip_data);
  ip.parameterBufferSize = GXM_PARAM_BUF_SIZE;
  r = sceGxmInitialize(&ip);
  if (r < 0) fatal("sceGxmInitialize failed 0x%08X", r);
  gpu_mem_log("after sceGxmInitialize (param buffer)");

  g_notif = sceGxmGetNotificationRegion();
  *g_notif = 0;

  // context rings, gpu only reads them
  if (blk_alloc(&g_vdm_mb, GXM_VDM_RING_SIZE, 0, MAP_GPU, NULL) < 0 ||
      blk_alloc(&g_vtx_mb, GXM_VERTEX_RING_SIZE, 0, MAP_GPU, NULL) < 0 ||
      blk_alloc(&g_frag_mb, GXM_FRAGMENT_RING_SIZE, 0, MAP_GPU, NULL) < 0)
    fatal("gxm: can't allocate context ring buffers");
  unsigned fusse_off = 0;
  if (blk_alloc(&g_fusse_mb, GXM_FRAG_USSE_RING_SIZE, 0, MAP_FUSSE, &fusse_off) < 0)
    fatal("gxm: can't allocate fragment usse ring");

  ctx_host_mem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
  SceGxmContextParams cp;
  memset(&cp, 0, sizeof(cp));
  cp.hostMem = ctx_host_mem;
  cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
  cp.vdmRingBufferMem = g_vdm_mb.base;
  cp.vdmRingBufferMemSize = GXM_VDM_RING_SIZE;
  cp.vertexRingBufferMem = g_vtx_mb.base;
  cp.vertexRingBufferMemSize = GXM_VERTEX_RING_SIZE;
  cp.fragmentRingBufferMem = g_frag_mb.base;
  cp.fragmentRingBufferMemSize = GXM_FRAGMENT_RING_SIZE;
  cp.fragmentUsseRingBufferMem = g_fusse_mb.base;
  cp.fragmentUsseRingBufferMemSize = GXM_FRAG_USSE_RING_SIZE;
  cp.fragmentUsseRingBufferOffset = fusse_off;
  r = sceGxmCreateContext(&cp, &gxm_ctx);
  if (r < 0) fatal("sceGxmCreateContext failed 0x%08X", r);

  // patcher, everything through our callbacks so it can grow
  SceGxmShaderPatcherParams pp;
  memset(&pp, 0, sizeof(pp));
  pp.userData = NULL;
  pp.hostAllocCallback = pat_host_alloc;
  pp.hostFreeCallback = pat_host_free;
  pp.bufferAllocCallback = pat_buf_alloc;
  pp.bufferFreeCallback = pat_buf_free;
  pp.vertexUsseAllocCallback = pat_vusse_alloc;
  pp.vertexUsseFreeCallback = pat_usse_free;
  pp.fragmentUsseAllocCallback = pat_fusse_alloc;
  pp.fragmentUsseFreeCallback = pat_usse_free;
  r = sceGxmShaderPatcherCreate(&pp, &gxm_patcher);
  if (r < 0) fatal("sceGxmShaderPatcherCreate failed 0x%08X", r);

  // display buffers, own cdram blocks (scanout wants them whole)
  for (int i = 0; i < DISPLAY_BUFFERS; i++) {
    uint32_t sz = DISPLAY_STRIDE * SCREEN_H * 4;
    if (blk_alloc(&g_disp_mb[i], sz, 1, MAP_GPU, NULL) < 0 &&
        blk_alloc(&g_disp_mb[i], sz, 0, MAP_GPU, NULL) < 0)
      fatal("gxm: can't allocate display buffer %d", i);
    memset(g_disp_mb[i].base, 0, sz);
    r = sceGxmColorSurfaceInit(&g_disp_surf[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
        SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
        SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCREEN_W, SCREEN_H, DISPLAY_STRIDE,
        g_disp_mb[i].base);
    if (r < 0) fatal("display color surface init failed 0x%08X", r);
    r = sceGxmSyncObjectCreate(&g_disp_sync[i]);
    if (r < 0) fatal("sceGxmSyncObjectCreate failed 0x%08X", r);
  }

  g_depth_mem = gpu_alloc(gxm_depth_surface_size(SCREEN_W, SCREEN_H),
                          SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, GPU_PREF_CDRAM);
  if (!g_depth_mem) fatal("gxm: no memory for the depth buffer");
  if (gxm_depth_surface_init(&g_disp_depth, SCREEN_W, SCREEN_H, g_depth_mem->ptr) < 0)
    fatal("gxm: display depth surface init failed");

  if (blk_alloc(&g_ring_mb, GXM_TRANSIENT_RING_SIZE, 0, MAP_GPU, NULL) < 0)
    fatal("gxm: can't allocate the transient ring");
  g_ring = g_ring_mb.base;

  g_front = 0;
  g_back = 1;
  // show something (black) right away
  flip_data d = { g_disp_mb[0].base };
  int vs = g_vsync;
  g_vsync = 0;
  display_cb(&d);
  g_vsync = vs;

  rt_get(SCREEN_W, SCREEN_H);
  g_ready = 1;
  gpu_mem_log("after gxm init");
  return 0;
}
