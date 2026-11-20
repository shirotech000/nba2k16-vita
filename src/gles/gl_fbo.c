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

static int rb_has_stencil(GLenum f)
{
  return f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8 || f == GL_STENCIL_INDEX8 ||
         f == GL_DEPTH_STENCIL;
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
  // note: nba2k16 never gets here with depth/stencil, it only attaches
  // COLOR_ATTACHMENT0 textures and uses renderbuffers for depth
  if (tex && att != GL_COLOR_ATTACHMENT0)
    L_ONCE("fbo: depth/stencil texture attachment, depth renders to a private surface and can't be sampled (TODO)");
  switch (att) {
  case GL_COLOR_ATTACHMENT0:
    f->color_tex = tex;
    f->color_rb = 0;
    break;
  case GL_DEPTH_ATTACHMENT:
    f->depth_tex = tex;
    f->depth_rb = 0;
    break;
  case GL_DEPTH_STENCIL_ATTACHMENT:
    f->depth_tex = f->stencil_tex = tex;
    f->depth_rb = f->stencil_rb = 0;
    break;
  case GL_STENCIL_ATTACHMENT:
    f->stencil_tex = tex;
    f->stencil_rb = 0;
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
  case GL_STENCIL_ATTACHMENT: f->stencil_rb = rb; f->stencil_tex = 0; break;
  case GL_DEPTH_STENCIL_ATTACHMENT:
    f->depth_rb = f->stencil_rb = rb;
    f->depth_tex = f->stencil_tex = 0;
    break;
  default: L_ONCE("fbo: renderbuffer attachment 0x%X not supported", att); break;
  }
}

GLenum glCheckFramebufferStatus(GLenum target)
{
  gl_framebuffer *f = bound_fbo(target);
  if (!f) return GL_FRAMEBUFFER_COMPLETE;
  if (!f->color_tex && !f->color_rb && !f->depth_rb && !f->depth_tex && !f->stencil_rb && !f->stencil_tex)
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
  gl_renderbuffer *crb = NULL, *zrb = NULL;

  if (!f) {
    gxm_display_target(&t);
    T.w = SCREEN_W;
    T.h = SCREEN_H;
    T.flip = 1;
    T.has_color = 1;
    T.has_depth = 1;
    T.has_stencil = 1;   // egl reports 24/8
    T.out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
    T.fbo = 0;
  } else {
    unsigned w = 0, h = 0;
    T.flip = 0;
    T.has_color = 0;
    T.has_depth = 0;
    T.has_stencil = 0;
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
          // once per format/size: which textures the game renders into
          static uint32_t seen[16];
          static int nseen;
          uint32_t key = (uint32_t)ctex->color_fmt ^ (ctex->w << 20) ^ (ctex->h << 8) ^ ctex->bpp;
          int known = 0;
          for (int i = 0; i < nseen; i++) known |= seen[i] == key;
          if (!known && nseen < 16) {
            seen[nseen++] = key;
            L("fbo: rendering into texture %u %ux%u color fmt 0x%08X bpp %u stride %u levels %u%s",
              ctex->name, ctex->w, ctex->h, (unsigned)ctex->color_fmt, ctex->bpp, ctex->stride,
              ctex->levels, ctex->is_cube ? " (cube!)" : "");
          }
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
    // depth + stencil. every gxm surface here is packed d24s8, but GL only has
    // a stencil buffer if something is attached at the stencil point. the game
    // puts its D24S8 rb on both points and its D16 / D24 rbs on depth only.
    gl_renderbuffer *drb = f->depth_rb ? rb_get(f->depth_rb) : NULL;
    gl_renderbuffer *srb = f->stencil_rb ? rb_get(f->stencil_rb) : NULL;
    if (drb && !(drb->mem && drb->is_depth && drb->ifmt != GL_STENCIL_INDEX8)) drb = NULL;
    if (srb && !(srb->mem && srb->is_depth && rb_has_stencil(srb->ifmt))) srb = NULL;
    zrb = drb ? drb : srb;
    if (zrb) {
      if (!w) { w = zrb->w; h = zrb->h; }
      if (zrb->w >= w && zrb->h >= h) {
        t.depth = &zrb->ds;
        T.has_depth = drb != NULL;
        T.has_stencil = srb != NULL;
        // gxm can't bind two surfaces, stencil ends up in the depth rb's plane
        if (drb && srb && drb != srb) L_ONCE("fbo: separate depth + stencil renderbuffers, sharing one surface");
      } else {
        L_ONCE("fbo: depth rb %ux%u smaller than color %ux%u, depth off", zrb->w, zrb->h, w, h);
        zrb = NULL;
      }
    } else if (f->depth_tex || f->stencil_tex) {
      gl_texture *dt = tex_get(f->depth_tex ? f->depth_tex : f->stencil_tex);
      if (!w && dt && dt->valid) { w = dt->w; h = dt->h; }
      if (w) {
        t.depth = scratch_depth_for(w, h);
        T.has_depth = t.depth && f->depth_tex;
        T.has_stencil = t.depth && f->stencil_tex;
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
  if (zrb) zrb->last_use = sc;
  G.dirty = 1;
  return 0;
}

// ============================================================ readback

// the game reads back with whatever format/type its texture table says
// (portrait + career photo capture), so 565/4444/5551 show up next to
// RGBA/UB. everything goes through rgba8 in between.

static unsigned rp_src_bpp(SceGxmColorFormat cf)
{
  switch (cf) {
  case SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR:
  case SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB:
    return 4;
  case SCE_GXM_COLOR_FORMAT_U5U6U5_RGB:
  case SCE_GXM_COLOR_FORMAT_U4U4U4U4_RGBA:
  case SCE_GXM_COLOR_FORMAT_U5U5U5U1_RGBA:
    return 2;
  default:
    return 0;
  }
}

static unsigned rp_dst_bpp(GLenum fmt, GLenum type)
{
  switch (type) {
  case GL_UNSIGNED_BYTE: return fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : 0;
  case GL_UNSIGNED_SHORT_5_6_5: return fmt == GL_RGB ? 2 : 0;
  case GL_UNSIGNED_SHORT_4_4_4_4:
  case GL_UNSIGNED_SHORT_5_5_5_1: return fmt == GL_RGBA ? 2 : 0;
  default: return 0;
  }
}

// gxm packed names list channels msb first, same bit order as GL's packed types
static void rp_decode(SceGxmColorFormat cf, const uint8_t *s, uint8_t *o)
{
  unsigned v = s[0] | (s[1] << 8);
  switch (cf) {
  case SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB:
    o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3];
    break;
  case SCE_GXM_COLOR_FORMAT_U5U6U5_RGB:
    o[0] = (v >> 11) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255;
    break;
  case SCE_GXM_COLOR_FORMAT_U4U4U4U4_RGBA:
    o[0] = (v >> 12) * 17; o[1] = ((v >> 8) & 15) * 17; o[2] = ((v >> 4) & 15) * 17; o[3] = (v & 15) * 17;
    break;
  case SCE_GXM_COLOR_FORMAT_U5U5U5U1_RGBA:
    o[0] = (v >> 11) * 255 / 31; o[1] = ((v >> 6) & 31) * 255 / 31; o[2] = ((v >> 1) & 31) * 255 / 31;
    o[3] = (v & 1) ? 255 : 0;
    break;
  default:
    o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3];
    break;
  }
}

static void rp_encode(GLenum type, unsigned dbpp, const uint8_t *c, uint8_t *d)
{
  unsigned v;
  switch (type) {
  case GL_UNSIGNED_SHORT_5_6_5: v = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3); break;
  case GL_UNSIGNED_SHORT_4_4_4_4: v = ((c[0] >> 4) << 12) | ((c[1] >> 4) << 8) | ((c[2] >> 4) << 4) | (c[3] >> 4); break;
  case GL_UNSIGNED_SHORT_5_5_5_1: v = ((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (c[3] >> 7); break;
  default: memcpy(d, c, dbpp); return;
  }
  d[0] = v & 0xFF;
  d[1] = v >> 8;
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out)
{
  L_ONCE("gl: glReadPixels used (stalls the gpu)");
  if (w <= 0 || h <= 0 || !out) return;
  unsigned dbpp = rp_dst_bpp(fmt, type);
  if (!dbpp) {
    L_ONCE("gl: glReadPixels fmt 0x%X type 0x%X unsupported", fmt, type);
    GL_ERR(GL_INVALID_OPERATION);
    return;
  }
  gxm_finish();

  const uint8_t *base;
  unsigned sw, sh, sstride;
  int flip, force_a = 0;
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
    if (t && t->valid && t->renderable && t->bpp == rp_src_bpp(t->color_fmt)) {
      base = t->mem->ptr; sw = t->w; sh = t->h; sstride = t->stride; cf = t->color_fmt;
      force_a = t->fmt == SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR;   // GL_RGB texture
    } else if (r && r->mem && !r->is_depth && rp_src_bpp(r->color_fmt)) {
      base = r->mem->ptr; sw = r->w; sh = r->h; cf = r->color_fmt;
      sstride = r->stride_px * rp_src_bpp(cf);
    } else {
      L_ONCE("gl: glReadPixels on fbo without readable color (f16 or unrenderable?)");
      return;
    }
    flip = 0;
  }
  unsigned sbpp = rp_src_bpp(cf);
  // source rows go through cached ram first, uncached byte reads crawl
  uint8_t *row = malloc(sw * sbpp);
  if (!row) return;
  unsigned dstride = ALIGN_UP((unsigned)w * dbpp, (unsigned)(G.pack_align > 0 ? G.pack_align : 4));
  uint8_t *dst = out;
  for (int yy = 0; yy < h; yy++, dst += dstride) {
    int gy = y + yy;
    if (gy < 0 || gy >= (int)sh) { memset(dst, 0, w * dbpp); continue; }
    int srow = flip ? (int)sh - 1 - gy : gy;
    memcpy(row, base + srow * sstride, sw * sbpp);
    for (int xx = 0; xx < w; xx++) {
      int gx = x + xx;
      uint8_t *d = dst + xx * dbpp;
      if (gx < 0 || gx >= (int)sw) { memset(d, 0, dbpp); continue; }
      uint8_t c[4];
      rp_decode(cf, row + gx * sbpp, c);
      if (force_a) c[3] = 255;
      rp_encode(type, dbpp, c, d);
    }
  }
  free(row);
}
