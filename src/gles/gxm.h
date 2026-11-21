#pragma once
// sceGxm device layer: init, gpu memory, transient ring, fences, scenes, flips.
// everything here is single-threaded (the GL thread), except the display
// queue callback which gxm runs on its own thread.
#include <stdint.h>
#include <psp2/gxm.h>

#define SCREEN_W        960
#define SCREEN_H        544
#define DISPLAY_STRIDE  960   // pixels, 64 aligned
#define DISPLAY_BUFFERS 3

// render_scale < 100: the game renders into a g_rw x g_rh target and we
// upscale to the native back buffer at flip. == SCREEN_W/H when off
extern unsigned g_rw, g_rh;

// knobs. rings are bigger than the sdk defaults, this game is draw heavy
// param buffer: 4MB (pb_mb= in config.txt). gxm's default 16MB came straight
// out of cdram on hw and a full match load needs every MB; if it ever fills
// the gpu just does a partial render, nothing breaks
#define GXM_PARAM_BUF_MB_DEFAULT 4
// sdk default ring sizes. we had 2x and more, which is 2.6MB of phycont the
// match load ran short of
#define GXM_VDM_RING_SIZE      (128 * 1024)
#define GXM_VERTEX_RING_SIZE   (2 * 1024 * 1024)
#define GXM_FRAGMENT_RING_SIZE (512 * 1024)
#define GXM_FRAG_USSE_RING_SIZE (16 * 1024)
// transient ring for client arrays etc, 1MB (ring_mb= in config.txt). the
// game only draws from vbos (peak 0 on hw); the monitor prints the peak
extern uint32_t g_ring_size;
#define GXM_TRANSIENT_RING_SIZE g_ring_size
// per render target. the game switches fbos a lot, so go to the gxm max
// (SCE_GXM_MAX_SCENES_PER_RENDERTARGET, 8. vitasdk only names it in a comment)
#define GXM_SCENES_PER_FRAME   8

// ---- gpu memory ----
enum {
  GPU_PREF_CDRAM = 0,   // try cdram first, fall back to lpddr
  GPU_PREF_LPDDR = 1,   // lpddr only (uncached)
};

typedef struct gpu_chunk gpu_chunk;
typedef struct gpu_block {
  void *ptr;
  uint32_t size;
  uint32_t off;
  gpu_chunk *chunk;   // NULL = cpu memory mapped for the gpu (gpu_alloc_mapped)
  int mapped;         // that mapping is ours to undo
} gpu_block;

gpu_block *gpu_alloc(uint32_t size, uint32_t align, int pref);
void gpu_free(gpu_block *b);
// pool's full: a bigmem block mapped read-only for the gpu. cpu writes need
// gpu_mapped_flush() after
gpu_block *gpu_alloc_mapped(uint32_t size);
void gpu_mapped_flush(gpu_block *b, uint32_t off, uint32_t size);
// bigmem's last resort: free space inside existing pool chunks, for the cpu
void *gxm_borrow(uint32_t size, void **handle);
void gxm_unborrow(void *handle);
// free once every scene submitted so far is done on the gpu
void gpu_free_deferred(gpu_block *b);
int gpu_is_cdram(const gpu_block *b);
void gpu_mem_log(const char *why);

// ---- transient ring (client arrays, converted indices, clear quads) ----
// may end the open scene if the ring is full of its own data, so grab
// everything a draw needs *before* starting/using the scene.
void *ring_alloc(uint32_t size, uint32_t align);

// ---- fences ----
// scene ids count up from 1. each scene writes its id to a notification
// when its fragment work is done, scenes retire in order.
uint32_t gxm_scene_counter(void);   // id of the open scene, or last one ended
int gxm_scene_done(uint32_t id);
void gxm_wait_scene(uint32_t id);  // ends the open scene if it's the one asked
void gxm_finish(void);             // end scene + wait for the gpu to go idle
// run fn(arg) once all scenes submitted so far are done
void gxm_defer(void (*fn)(void *), void *arg);
// true if something last used in scene 'id' might still be read by the gpu
static inline int gxm_busy(uint32_t id) { return id && !gxm_scene_done(id); }

// ---- scenes ----
typedef struct gxm_target {
  const SceGxmColorSurface *color;        // NULL -> color writes off
  const SceGxmDepthStencilSurface *depth; // NULL -> no depth/stencil
  unsigned w, h;
  SceGxmSyncObject *sync;                 // display sync or NULL
  SceGxmMultisampleMode msaa;             // NONE unless the target asked for msaa
} gxm_target;

extern SceGxmContext *gxm_ctx;
extern SceGxmShaderPatcher *gxm_patcher;

int gxm_init(void);          // idempotent. 0 on success
int gxm_ready(void);
int gxm_in_scene(void);
// load_depth=0 when the scene starts with a full depth+stencil clear
void gxm_begin_scene(const gxm_target *t, int load_depth);
void gxm_end_scene(void);
void gxm_swap(void);         // end scene, queue the flip, rotate buffers
void gxm_set_vsync(int interval);  // vblanks to wait per flip, 0 = none
uint32_t gxm_frame_count(void);
void gxm_prof_report(void);                     // frame timing line, monitor calls it
void gxm_prof_draws(uint32_t n, uint64_t us);   // us = 0 unless prof=1

// the default framebuffer (current back buffer)
void gxm_display_target(gxm_target *out);
void *gxm_back_buffer(void);
void *gxm_front_buffer(void);

// helpers for fbo surfaces
uint32_t gxm_depth_surface_size(unsigned w, unsigned h);
int gxm_depth_surface_init(SceGxmDepthStencilSurface *ds, unsigned w, unsigned h, void *mem);

// msaa (same idea as the user's mbaavita/vita2d setup): the render target and
// every fragment program run at the sample mode, the color surface downscales
// on store so the texture/display stays 1x, depth holds all the samples
// (4x: 2x wide + 2x tall, 2x: 2x tall).
SceGxmMultisampleMode gxm_msaa_setting(void);          // config msaa=0/2/4, default 0 for now
int gxm_msaa_display(void);                            // config msaa_display=1
SceGxmMultisampleMode gxm_scene_msaa(void);            // mode of the open scene
uint32_t gxm_depth_surface_size_ms(unsigned w, unsigned h, SceGxmMultisampleMode ms);
int gxm_depth_surface_init_ms(SceGxmDepthStencilSurface *ds, unsigned w, unsigned h, void *mem,
                              SceGxmMultisampleMode ms);
