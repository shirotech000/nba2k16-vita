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

// knobs. rings are bigger than the sdk defaults, this game is draw heavy
#define GXM_PARAM_BUF_SIZE     (16 * 1024 * 1024)
#define GXM_VDM_RING_SIZE      (256 * 1024)
#define GXM_VERTEX_RING_SIZE   (4 * 1024 * 1024)
#define GXM_FRAGMENT_RING_SIZE (2 * 1024 * 1024)
#define GXM_FRAG_USSE_RING_SIZE (16 * 1024)
#define GXM_TRANSIENT_RING_SIZE (8 * 1024 * 1024)
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
  gpu_chunk *chunk;
} gpu_block;

gpu_block *gpu_alloc(uint32_t size, uint32_t align, int pref);
void gpu_free(gpu_block *b);
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

// the default framebuffer (current back buffer)
void gxm_display_target(gxm_target *out);
void *gxm_back_buffer(void);
void *gxm_front_buffer(void);

// helpers for fbo surfaces
uint32_t gxm_depth_surface_size(unsigned w, unsigned h);
int gxm_depth_surface_init(SceGxmDepthStencilSurface *ds, unsigned w, unsigned h, void *mem);
