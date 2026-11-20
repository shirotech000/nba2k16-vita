// framebuffers + renderbuffers, and opening scenes on whatever is bound.
// switching the draw fbo ends the scene, the next draw/clear opens a new one.
#include <stdlib.h>
#include <string.h>
#include "gles/gl_internal.h"
#include "gles/gl_api.h"

static name_table g_fbos, g_rbs;
gl_cur_target T;

static gl_framebuffer *fbo_get(GLuint n) { return nt_get(&g_fbos, n); }
static gl_renderbuffer *rb_get(GLuint n) { return nt_get(&g_rbs, n); }

// ============================================================ renderbuffers

void glGenRenderbuffers(GLsizei n, GLuint *out)
{
  for (int i = 0; i < n; i++) {
    gl_renderbuffer *r = calloc(1, sizeof(*r));
    r->name = out[i] = nt_gen(&g_rbs);
    nt_set(&g_rbs, r->name, r);
  }
}

void glBindRenderbuffer(GLenum target, GLuint rb)
{
  (void)target;
  if (rb && !rb_get(rb)) {
    gl_renderbuffer *r = calloc(1, sizeof(*r));
    r->name = rb;
    nt_set(&g_rbs, rb, r);
  }
  G.renderbuffer = rb;
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *rbs)
{
  for (int i = 0; i < n; i++) {
    gl_renderbuffer *r = rb_get(rbs[i]);
    if (!r) continue;
    if (G.renderbuffer == rbs[i]) G.renderbuffer = 0;
    gpu_free_deferred(r->mem);
    nt_set(&g_rbs, rbs[i], NULL);
    free(r);
  }
}

static int is_depth_fmt(GLenum f)
{
  return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 || f == GL_DEPTH_COMPONENT32F ||
         f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8 || f == GL_STENCIL_INDEX8 ||
         f == GL_DEPTH_COMPONENT || f == GL_DEPTH_STENCIL;
}

void glRenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h)
{
  (void)target;
  gl_renderbuffer *r = rb_get(G.renderbuffer);
  if (!r || w <= 0 || h <= 0) { GL_ERR(GL_INVALID_OPERATION); return; }
  gl_fbo_changed();
  gpu_free_deferred(r->mem);
  r->mem = NULL;
  r->ifmt = ifmt;
  r->w = w;
  r->h = h;
  r->is_depth = is_depth_fmt(ifmt);
  if (r->is_depth) {
    // everything depth-ish becomes packed d24s8
    uint32_t sz = gxm_depth_surface_size(w, h);
    r->mem = gpu_alloc(sz, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, GPU_PREF_CDRAM);
    if (!r->mem) { GL_ERR(GL_OUT_OF_MEMORY); return; }
    gxm_depth_surface_init(&r->ds, w, h, r->mem->ptr);
    L("fbo: depth renderbuffer %u %dx%d (0x%X) %u KiB", r->name, w, h, ifmt, sz >> 10);
  } else {
    r->stride_px = ALIGN_UP(w, 8);
    unsigned bpp = 4;
    r->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
    switch (ifmt) {
    case GL_RGB565: r->color_fmt = SCE_GXM_COLOR_FORMAT_U5U6U5_RGB; bpp = 2; break;
    case GL_RGBA4: r->color_fmt = SCE_GXM_COLOR_FORMAT_U4U4U4U4_RGBA; bpp = 2; break;
    case GL_RGB5_A1: r->color_fmt = SCE_GXM_COLOR_FORMAT_U5U5U5U1_RGBA; bpp = 2; break;
    case GL_RGBA8: case GL_RGB8: case GL_RGBA: case GL_RGB: case GL_SRGB8_ALPHA8:
      r->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR; break;
    default:
      L("fbo: color renderbuffer format 0x%X unknown, using rgba8", ifmt);
      r->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
      break;
    }
    uint32_t sz = r->stride_px * h * bpp;
    r->mem = gpu_alloc(sz, 256, GPU_PREF_CDRAM);
    if (!r->mem) { GL_ERR(GL_OUT_OF_MEMORY); return; }
    memset(r->mem->ptr, 0, sz);
    L("fbo: color renderbuffer %u %dx%d (0x%X) %u KiB", r->name, w, h, ifmt, sz >> 10);
  }
}

void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h)
{
  if (samples > 1) L_ONCE("fbo: %dx msaa renderbuffer requested, rendering without msaa", samples);
  glRenderbufferStorage(target, ifmt, w, h);
}

// ============================================================ framebuffers

void glGenFramebuffers(GLsizei n, GLuint *out)
{
  for (int i = 0; i < n; i++) {
    gl_framebuffer *f = calloc(1, sizeof(*f));
    f->name = out[i] = nt_gen(&g_fbos);
    nt_set(&g_fbos, f->name, f);
  }
}

void gl_fbo_changed(void)
{
  if (gxm_in_scene()) gxm_end_scene();
  G.dirty = 1;
}

void glBindFramebuffer(GLenum target, GLuint fb)
{
  if (fb && !fbo_get(fb)) {
    gl_framebuffer *f = calloc(1, sizeof(*f));
    f->name = fb;
    nt_set(&g_fbos, fb, f);
  }
  if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
    if (G.draw_fbo != fb) gl_fbo_changed();
    G.draw_fbo = fb;
  }
  if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) G.read_fbo = fb;
}

void glDeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
  for (int i = 0; i < n; i++) {
    gl_framebuffer *f = fbo_get(fbs[i]);
    if (!f) continue;
    if (G.draw_fbo == fbs[i]) { gl_fbo_changed(); G.draw_fbo = 0; }
    if (G.read_fbo == fbs[i]) G.read_fbo = 0;
    nt_set(&g_fbos, fbs[i], NULL);
    free(f);
  }
}

static gl_framebuffer *bound_fbo(GLenum target)
{
  return fbo_get(target == GL_READ_FRAMEBUFFER ? G.read_fbo : G.draw_fbo);
}

void glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level)
{
  gl_framebuffer *f = bound_fbo(target);
  if (!f) { GL_ERR(GL_INVALID_OPERATION); return; }
  if (level) L_ONCE("fbo: attaching mip level %d, only level 0 exists", level);
  if (textarget != GL_TEXTURE_2D && tex) L_ONCE("fbo: attaching cube face 0x%X, TODO", textarget);
  if (f->name == G.draw_fbo) gl_fbo_changed();
  switch (att) {
  case GL_COLOR_ATTACHMENT0:
    f->color_tex = tex;
    f->color_rb = 0;
    break;
  case GL_DEPTH_ATTACHMENT:
  case GL_DEPTH_STENCIL_ATTACHMENT:
    if (tex) L_ONCE("fbo: depth texture attachment, depth renders to a private surface and can't be sampled (TODO)");
    f->depth_tex = tex;
    f->depth_rb = 0;
    break;
  case GL_STENCIL_ATTACHMENT:
    break;
  default:
    L_ONCE("fbo: attachment 0x%X not supported", att);
    break;
  }
}

void glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbtarget, GLuint rb)
{
  (void)rbtarget;
  gl_framebuffer *f = bound_fbo(target);
  if (!f) { GL_ERR(GL_INVALID_OPERATION); return; }
  if (f->name == G.draw_fbo) gl_fbo_changed();
  switch (att) {
  case GL_COLOR_ATTACHMENT0: f->color_rb = rb; f->color_tex = 0; break;
  case GL_DEPTH_ATTACHMENT: f->depth_rb = rb; f->depth_tex = 0; break;
  case GL_STENCIL_ATTACHMENT: f->stencil_rb = rb; break;
  case GL_DEPTH_STENCIL_ATTACHMENT: f->depth_rb = f->stencil_rb = rb; f->depth_tex = 0; break;
  default: L_ONCE("fbo: renderbuffer attachment 0x%X not supported", att); break;
  }
}

GLenum glCheckFramebufferStatus(GLenum target)
{
  gl_framebuffer *f = bound_fbo(target);
  if (!f) return GL_FRAMEBUFFER_COMPLETE;
  if (!f->color_tex && !f->color_rb && !f->depth_rb && !f->depth_tex)
    return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
  return GL_FRAMEBUFFER_COMPLETE;
}

void glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *atts)
{
  // gxm decides depth load/store at scene begin, so this is only a hint we
  // can't use yet. TODO: skip the depth store when it's invalidated.
  (void)target; (void)n; (void)atts;
  L_ONCE("fbo: glInvalidateFramebuffer ignored (hint only)");
}

// ============================================================ scene targeting

// private depth surfaces for fbos with a depth *texture* attached
typedef struct { unsigned w, h; gpu_block *mem; SceGxmDepthStencilSurface ds; } scratch_depth;
static scratch_depth g_scratch[8];

static SceGxmDepthStencilSurface *scratch_depth_for(unsigned w, unsigned h)
{
  for (int i = 0; i < 8; i++)
    if (g_scratch[i].mem && g_scratch[i].w == w && g_scratch[i].h == h) return &g_scratch[i].ds;
  for (int i = 0; i < 8; i++) {
    if (g_scratch[i].mem) continue;
    g_scratch[i].mem = gpu_alloc(gxm_depth_surface_size(w, h), SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, GPU_PREF_CDRAM);
    if (!g_scratch[i].mem) return NULL;
    g_scratch[i].w = w;
    g_scratch[i].h = h;
    gxm_depth_surface_init(&g_scratch[i].ds, w, h, g_scratch[i].mem->ptr);
    return &g_scratch[i].ds;
  }
  L_ONCE("fbo: out of scratch depth surfaces");
  return NULL;
}

static SceGxmColorSurface g_fb_cs;

void fbo_target_size(unsigned *w, unsigned *h)
{
  *w = T.w;
  *h = T.h;
}

int gl_ensure_scene(int full_ds_clear)
{
  if (!gxm_ready()) return -1;
  if (gxm_in_scene()) return 0;

  gxm_target t;
  memset(&t, 0, sizeof(t));
  gl_framebuffer *f = G.draw_fbo ? fbo_get(G.draw_fbo) : NULL;
  gl_texture *ctex = NULL;
  gl_renderbuffer *crb = NULL, *drb = NULL;

  if (!f) {
    gxm_display_target(&t);
    T.w = SCREEN_W;
    T.h = SCREEN_H;
    T.flip = 1;
    T.has_color = 1;
    T.has_depth = 1;
    T.out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
    T.fbo = 0;
  } else {
    unsigned w = 0, h = 0;
    T.flip = 0;
    T.has_color = 0;
    T.has_depth = 0;
    T.out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
    T.fbo = f->name;
    if (f->color_tex) {
      ctex = tex_get(f->color_tex);
      if (ctex && ctex->valid && ctex->renderable) {
        int r = sceGxmColorSurfaceInit(&g_fb_cs, ctex->color_fmt, SCE_GXM_COLOR_SURFACE_LINEAR,
                                       SCE_GXM_COLOR_SURFACE_SCALE_NONE, ctex->out_size,
                                       ctex->w, ctex->h, ctex->stride / ctex->bpp, ctex->mem->ptr);
        if (r < 0) {
          L_ONCE("fbo: color surface init on texture %u (%ux%u) failed 0x%08X", ctex->name, ctex->w, ctex->h, r);
        } else {
          w = ctex->w; h = ctex->h;
          T.has_color = 1;
          T.out_fmt = ctex->out_fmt;
        }
      } else if (ctex && ctex->valid) {
        L_ONCE("fbo: texture %u format isn't renderable, color writes dropped", ctex->name);
      }
    } else if (f->color_rb) {
      crb = rb_get(f->color_rb);
      if (crb && crb->mem && !crb->is_depth) {
        int r = sceGxmColorSurfaceInit(&g_fb_cs, crb->color_fmt, SCE_GXM_COLOR_SURFACE_LINEAR,
                                       SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                       crb->w, crb->h, crb->stride_px, crb->mem->ptr);
        if (r < 0) {
          L_ONCE("fbo: color surface init on renderbuffer %u failed 0x%08X", crb->name, r);
        } else {
          w = crb->w; h = crb->h;
          T.has_color = 1;
          T.out_fmt = crb->out_fmt;
        }
      }
    }
    if (f->depth_rb) {
      drb = rb_get(f->depth_rb);
      if (drb && drb->mem && drb->is_depth) {
        if (!w) { w = drb->w; h = drb->h; }
        if (drb->w >= w && drb->h >= h) {
          t.depth = &drb->ds;
          T.has_depth = 1;
        } else {
          L_ONCE("fbo: depth rb %ux%u smaller than color %ux%u, depth off", drb->w, drb->h, w, h);
        }
      }
    } else if (f->depth_tex) {
      gl_texture *dt = tex_get(f->depth_tex);
      if (!w && dt && dt->valid) { w = dt->w; h = dt->h; }
      if (w) {
        t.depth = scratch_depth_for(w, h);
        T.has_depth = t.depth != NULL;
      }
    }
    if (!w || !h) {
      L_ONCE("fbo: framebuffer %u has nothing usable attached, draws skipped", f->name);
      return -1;
    }
    t.color = T.has_color ? &g_fb_cs : NULL;
    t.w = T.w = w;
    t.h = T.h = h;
    t.sync = NULL;
  }

  gxm_begin_scene(&t, !full_ds_clear);
  if (!gxm_in_scene()) return -1;
  uint32_t sc = gxm_scene_counter();
  if (ctex) ctex->last_use = sc;
  if (crb) crb->last_use = sc;
  if (drb) drb->last_use = sc;
  G.dirty = 1;
  return 0;
}

// ============================================================ readback

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out)
{
  L_ONCE("gl: glReadPixels used (stalls the gpu)");
  if (w <= 0 || h <= 0 || !out) return;
  if (fmt != GL_RGBA || type != GL_UNSIGNED_BYTE) {
    L_ONCE("gl: glReadPixels fmt 0x%X type 0x%X unsupported (only RGBA/UNSIGNED_BYTE)", fmt, type);
    return;
  }
  gxm_finish();

  const uint8_t *base;
  unsigned sw, sh, sstride, bpp = 4;
  int flip;
  SceGxmColorFormat cf = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
  gl_framebuffer *f = G.read_fbo ? fbo_get(G.read_fbo) : NULL;
  if (!f) {
    // after finish the back buffer holds what was drawn since the last swap
    base = gxm_back_buffer();
    sw = SCREEN_W; sh = SCREEN_H; sstride = DISPLAY_STRIDE * 4;
    flip = 1;
  } else {
    gl_texture *t = f->color_tex ? tex_get(f->color_tex) : NULL;
    gl_renderbuffer *r = f->color_rb ? rb_get(f->color_rb) : NULL;
    if (t && t->valid) {
      base = t->mem->ptr; sw = t->w; sh = t->h; sstride = t->stride; bpp = t->bpp; cf = t->color_fmt;
    } else if (r && r->mem && !r->is_depth) {
      base = r->mem->ptr; sw = r->w; sh = r->h; cf = r->color_fmt;
      bpp = (cf == SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR || cf == SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB) ? 4 : 2;
      sstride = r->stride_px * bpp;
    } else {
      L_ONCE("gl: glReadPixels on fbo without readable color");
      return;
    }
    flip = 0;
  }
  if (bpp != 4) {
    L_ONCE("gl: glReadPixels from 16-bit surface not supported");
    return;
  }
  int swap_rb = cf == SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB;
  unsigned dstride = ALIGN_UP((unsigned)w * 4, (unsigned)(G.pack_align > 0 ? G.pack_align : 4));
  uint8_t *dst = out;
  for (int yy = 0; yy < h; yy++, dst += dstride) {
    int gy = y + yy;
    if (gy < 0 || gy >= (int)sh) { memset(dst, 0, w * 4); continue; }
    int row = flip ? (int)sh - 1 - gy : gy;
    const uint8_t *s = base + row * sstride;
    for (int xx = 0; xx < w; xx++) {
      int gx = x + xx;
      uint8_t *d = dst + xx * 4;
      if (gx < 0 || gx >= (int)sw) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
      const uint8_t *p = s + gx * 4;
      d[0] = swap_rb ? p[2] : p[0];
      d[1] = p[1];
      d[2] = swap_rb ? p[0] : p[2];
      d[3] = p[3];
    }
  }
}
