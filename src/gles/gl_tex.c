// textures: linear gxm textures in gpu memory, converted on the cpu when the
// GL format has no direct gxm match. level 0 only for now (mips dropped).
#include <stdlib.h>
#include <string.h>
#include "gles/gl_internal.h"
#include "gles/gl_api.h"

static name_table g_texs;

gl_texture *tex_get(GLuint name) { return nt_get(&g_texs, name); }

// ============================================================ formats

enum { CV_COPY, CV_RGB8_RGBA8, CV_F32_F16, CV_ZERO };

typedef struct {
  SceGxmTextureFormat fmt;
  unsigned src_bpp, bpp;
  int conv;
  int renderable;
  SceGxmColorFormat color_fmt;
  SceGxmOutputRegisterFormat out_fmt;
  SceGxmOutputRegisterSize out_size;
  int is_depth;
} fmt_info;

static void log_format_once(GLint ifmt, GLenum fmt, GLenum type, const char *what)
{
  static uint32_t seen[64];
  static int n;
  uint32_t key = (uint32_t)ifmt * 31u + fmt * 7u + type;
  for (int i = 0; i < n; i++) if (seen[i] == key) return;
  if (n < 64) seen[n++] = key;
  L("tex: new format ifmt=0x%X fmt=0x%X type=0x%X -> %s", ifmt, fmt, type, what);
}

static int pick_format(GLint ifmt, GLenum fmt, GLenum type, fmt_info *fi)
{
  memset(fi, 0, sizeof(*fi));
  fi->conv = CV_COPY;
  fi->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
  fi->out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
  const char *what;

  if (type == GL_HALF_FLOAT_OES) type = GL_HALF_FLOAT;
  if (fmt == GL_RGBA && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR; fi->src_bpp = fi->bpp = 4;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
    what = "U8U8U8U8_ABGR";
  } else if (fmt == GL_RGB && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR; fi->src_bpp = 3; fi->bpp = 4;
    fi->conv = CV_RGB8_RGBA8;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
    what = "X8U8U8U8_1BGR (rgb8 expanded to rgba8)";
  } else if (fmt == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB; fi->src_bpp = fi->bpp = 4;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB;
    what = "U8U8U8U8_ARGB";
  } else if (fmt == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB; fi->src_bpp = fi->bpp = 2;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_U5U6U5_RGB;
    what = "U5U6U5_RGB";
  } else if (fmt == GL_RGBA && type == GL_UNSIGNED_SHORT_4_4_4_4) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA; fi->src_bpp = fi->bpp = 2;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_U4U4U4U4_RGBA;
    what = "U4U4U4U4_RGBA";
  } else if (fmt == GL_RGBA && type == GL_UNSIGNED_SHORT_5_5_5_1) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA; fi->src_bpp = fi->bpp = 2;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_U5U5U5U1_RGBA;
    what = "U5U5U5U1_RGBA";
  } else if (fmt == GL_LUMINANCE && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8_1RRR; fi->src_bpp = fi->bpp = 1;
    what = "U8_1RRR (L8)";
  } else if (fmt == GL_ALPHA && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8_R000; fi->src_bpp = fi->bpp = 1;
    what = "U8_R000 (A8)";
  } else if (fmt == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8_GRRR; fi->src_bpp = fi->bpp = 2;
    what = "U8U8_GRRR (LA8)";
  } else if (fmt == GL_RED && type == GL_UNSIGNED_BYTE) {
    // GL wants (r,0,0,1), closest gxm swizzle gives alpha 0
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8_000R; fi->src_bpp = fi->bpp = 1;
    what = "U8_000R (R8, alpha reads 0)";
  } else if (fmt == GL_RG && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8_00GR; fi->src_bpp = fi->bpp = 2;
    what = "U8U8_00GR (RG8)";
  } else if (fmt == GL_RGBA && type == GL_HALF_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR; fi->src_bpp = fi->bpp = 8;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR;
    fi->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4;
    fi->out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT;
    what = "F16F16F16F16_ABGR";
  } else if (fmt == GL_RGBA && type == GL_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR; fi->src_bpp = 16; fi->bpp = 8;
    fi->conv = CV_F32_F16;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR;
    fi->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4;
    fi->out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT;
    what = "F16F16F16F16_ABGR (from f32, precision loss)";
  } else if (fmt == GL_RED && type == GL_HALF_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F16_000R; fi->src_bpp = fi->bpp = 2;
    what = "F16_000R";
  } else if (fmt == GL_RG && type == GL_HALF_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F16F16_00GR; fi->src_bpp = fi->bpp = 4;
    what = "F16F16_00GR";
  } else if (fmt == GL_RED && type == GL_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F32_000R; fi->src_bpp = fi->bpp = 4;
    what = "F32_000R";
  } else if (fmt == GL_DEPTH_COMPONENT || fmt == GL_DEPTH_STENCIL) {
    // TODO: real depth textures (render into a linear depth surface and sample it)
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR; fi->bpp = 4; fi->src_bpp = 0;
    fi->conv = CV_ZERO;
    fi->is_depth = 1;
    what = "DEPTH texture -> NOT SUPPORTED, samples as zero";
  } else {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR; fi->bpp = 4; fi->src_bpp = 0;
    fi->conv = CV_ZERO;
    log_format_once(ifmt, fmt, type, "UNSUPPORTED, zero rgba8 placeholder");
    return 0;
  }
  if (ifmt == GL_SRGB8_ALPHA8 || ifmt == GL_SRGB8)
    L_ONCE("tex: sRGB internal format used, sampled as linear (no gamma)");
  log_format_once(ifmt, fmt, type, what);
  return 1;
}

static uint16_t f32_to_f16(float f)
{
  union { float f; uint32_t u; } v = { f };
  uint32_t s = (v.u >> 16) & 0x8000, e = (v.u >> 23) & 0xFF, m = v.u & 0x7FFFFF;
  if (e == 0xFF) return s | 0x7C00 | (m ? 0x200 : 0);
  int ne = (int)e - 127 + 15;
  if (ne >= 31) return s | 0x7C00;
  if (ne <= 0) {
    if (ne < -10) return s;
    m |= 0x800000;
    return s | (uint16_t)(m >> (14 - ne));
  }
  return s | (uint16_t)(ne << 10) | (uint16_t)(m >> 13);
}

// copy/convert a w x h block of client pixels into texture memory
static void upload_rect(gl_texture *t, const fmt_info *fi, unsigned x0, unsigned y0,
                        unsigned w, unsigned h, const void *data)
{
  uint8_t *dst = (uint8_t *)t->mem->ptr + y0 * t->stride + x0 * fi->bpp;
  if (fi->conv == CV_ZERO || !data) {
    for (unsigned y = 0; y < h; y++) memset(dst + y * t->stride, 0, w * fi->bpp);
    return;
  }
  unsigned rowlen = G.unpack_row_len ? (unsigned)G.unpack_row_len : w;
  unsigned align = G.unpack_align > 0 ? (unsigned)G.unpack_align : 4;
  unsigned sstride = ALIGN_UP(rowlen * fi->src_bpp, align);
  const uint8_t *src = data;
  for (unsigned y = 0; y < h; y++, src += sstride, dst += t->stride) {
    switch (fi->conv) {
    case CV_COPY:
      memcpy(dst, src, w * fi->bpp);
      break;
    case CV_RGB8_RGBA8: {
      // build the row on the stack, uncached memory hates byte writes
      uint32_t tmp[256];
      for (unsigned x = 0; x < w; x += 256) {
        unsigned n = w - x < 256 ? w - x : 256;
        const uint8_t *s = src + x * 3;
        for (unsigned i = 0; i < n; i++, s += 3)
          tmp[i] = s[0] | (s[1] << 8) | (s[2] << 16) | 0xFF000000u;
        memcpy(dst + x * 4, tmp, n * 4);
      }
      break;
    }
    case CV_F32_F16: {
      const float *s = (const float *)src;
      uint16_t tmp[256];
      for (unsigned x = 0; x < w * 4; x += 256) {
        unsigned n = w * 4 - x < 256 ? w * 4 - x : 256;
        for (unsigned i = 0; i < n; i++) tmp[i] = f32_to_f16(s[x + i]);
        memcpy(dst + x * 2, tmp, n * 2);
      }
      break;
    }
    }
  }
}

// ============================================================ sampler state

static void apply_sampler(gl_texture *t)
{
  SceGxmTexture *g = &t->gxm;
  sceGxmTextureSetMagFilter(g, t->mag_filter == GL_NEAREST ? SCE_GXM_TEXTURE_FILTER_POINT
                                                           : SCE_GXM_TEXTURE_FILTER_LINEAR);
  int min_point = t->min_filter == GL_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                  t->min_filter == GL_NEAREST_MIPMAP_LINEAR;
  sceGxmTextureSetMinFilter(g, min_point ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
  sceGxmTextureSetMipFilter(g, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
  GLenum wr[2] = { t->wrap_s, t->wrap_t };
  for (int i = 0; i < 2; i++) {
    SceGxmTextureAddrMode m = wr[i] == GL_CLAMP_TO_EDGE ? SCE_GXM_TEXTURE_ADDR_CLAMP
                            : wr[i] == GL_MIRRORED_REPEAT ? SCE_GXM_TEXTURE_ADDR_MIRROR
                            : SCE_GXM_TEXTURE_ADDR_REPEAT;
    int r = i ? sceGxmTextureSetVAddrMode(g, m) : sceGxmTextureSetUAddrMode(g, m);
    if (r < 0) {
      L_ONCE("tex: addr mode %d rejected (0x%08X) on %ux%u, using clamp", m, r, t->w, t->h);
      if (i) sceGxmTextureSetVAddrMode(g, SCE_GXM_TEXTURE_ADDR_CLAMP);
      else sceGxmTextureSetUAddrMode(g, SCE_GXM_TEXTURE_ADDR_CLAMP);
    }
  }
}

// ============================================================ dummies

static gl_texture g_white2d;
static SceGxmTexture g_white_cube;
static int g_dummies, g_cube_ok;

static void make_dummies(void)
{
  if (g_dummies) return;
  g_dummies = 1;
  // 8x8 white linear 2d
  gpu_block *m = gpu_alloc(8 * 8 * 4, 256, GPU_PREF_LPDDR);
  if (!m) fatal("tex: can't allocate the dummy texture");
  {
    memset(m->ptr, 0xFF, 8 * 8 * 4);
    g_white2d.mem = m;
    g_white2d.w = g_white2d.h = 8;
    sceGxmTextureInitLinear(&g_white2d.gxm, m->ptr, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 8, 8, 0);
    g_white2d.min_filter = g_white2d.mag_filter = GL_LINEAR;
    g_white2d.wrap_s = g_white2d.wrap_t = GL_REPEAT;
    apply_sampler(&g_white2d);
  }
  // 1x1 cube, padded generously since face layout is gxm's business
  gpu_block *c = gpu_alloc(4096, 4096, GPU_PREF_LPDDR);
  if (c) {
    memset(c->ptr, 0xFF, 4096);
    int r = sceGxmTextureInitCube(&g_white_cube, c->ptr, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 1, 1, 0);
    if (r < 0) L("tex: dummy cube init failed 0x%08X, cube samplers get the 2d dummy", r);
    else g_cube_ok = 1;
  }
}

const SceGxmTexture *tex_for_unit(int unit, int cube, uint32_t scene)
{
  make_dummies();
  if (cube) {
    if (G.texcube[unit]) L_ONCE("tex: cube map sampled, cube maps are TODO (white)");
    // never hand gxm an uninitialised descriptor
    return g_cube_ok ? &g_white_cube : &g_white2d.gxm;
  }
  gl_texture *t = tex_get(G.tex2d[unit]);
  if (!t || !t->valid) return &g_white2d.gxm;
  t->last_use = scene;
  return &t->gxm;
}

// ============================================================ objects

static gl_texture *tex_make(GLuint name)
{
  gl_texture *t = calloc(1, sizeof(*t));
  t->name = name;
  t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
  t->mag_filter = GL_LINEAR;
  t->wrap_s = t->wrap_t = GL_REPEAT;
  nt_set(&g_texs, name, t);
  return t;
}

void glGenTextures(GLsizei n, GLuint *out)
{
  for (int i = 0; i < n; i++) {
    out[i] = nt_gen(&g_texs);
    tex_make(out[i]);
  }
}

void glActiveTexture(GLenum tex)
{
  int u = (int)tex - GL_TEXTURE0;
  if (u < 0 || u >= MAX_TEX_UNITS) { GL_ERR(GL_INVALID_ENUM); return; }
  G.active_tex = u;
}

void glBindTexture(GLenum target, GLuint name)
{
  gl_texture *t = name ? tex_get(name) : NULL;
  if (name && !t) t = tex_make(name);
  if (t && !t->target) t->target = target;
  if (target == GL_TEXTURE_2D) G.tex2d[G.active_tex] = name;
  else if (target == GL_TEXTURE_CUBE_MAP) G.texcube[G.active_tex] = name;
  else L_ONCE("tex: glBindTexture target 0x%X not supported", target);
}

void glDeleteTextures(GLsizei n, const GLuint *names)
{
  for (int i = 0; i < n; i++) {
    gl_texture *t = tex_get(names[i]);
    if (!t) continue;
    for (int u = 0; u < MAX_TEX_UNITS; u++) {
      if (G.tex2d[u] == names[i]) G.tex2d[u] = 0;
      if (G.texcube[u] == names[i]) G.texcube[u] = 0;
    }
    gpu_free_deferred(t->mem);
    nt_set(&g_texs, names[i], NULL);
    free(t);
  }
}

static gl_texture *bound_tex(GLenum target)
{
  if (target == GL_TEXTURE_2D) return tex_get(G.tex2d[G.active_tex]);
  if (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                                        target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
    return tex_get(G.texcube[G.active_tex]);
  return NULL;
}

static uint64_t g_tex_bytes;

// (re)allocate storage for level 0. keeps old memory if it fits and the gpu
// is done with it, otherwise orphans it.
static int tex_storage(gl_texture *t, unsigned w, unsigned h, const fmt_info *fi)
{
  unsigned stride = ALIGN_UP(w, 8) * fi->bpp;
  uint32_t size = stride * h;
  if (t->mem && (t->mem->size < size || t->mem->size > size * 2 || gxm_busy(t->last_use))) {
    g_tex_bytes -= t->mem->size;
    gpu_free_deferred(t->mem);
    t->mem = NULL;
  }
  if (!t->mem) {
    t->mem = gpu_alloc(size, 256, GPU_PREF_CDRAM);
    if (!t->mem) {
      L("tex: OOM for %ux%u texture %u (%u bytes)", w, h, t->name, size);
      t->valid = 0;
      return -1;
    }
    g_tex_bytes += t->mem->size;
    t->last_use = 0;
  }
  t->w = w;
  t->h = h;
  t->stride = stride;
  t->bpp = fi->bpp;
  t->fmt = fi->fmt;
  t->renderable = fi->renderable;
  t->color_fmt = fi->color_fmt;
  t->out_fmt = fi->out_fmt;
  t->out_size = fi->out_size;
  t->is_depth = fi->is_depth;
  int r = sceGxmTextureInitLinear(&t->gxm, t->mem->ptr, fi->fmt, w, h, 0);
  if (r < 0) {
    L("tex: sceGxmTextureInitLinear %ux%u fmt 0x%08X failed 0x%08X", w, h, fi->fmt, r);
    t->valid = 0;
    return -1;
  }
  apply_sampler(t);
  t->valid = 1;
  return 0;
}

void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum fmt, GLenum type, const void *data)
{
  (void)border;
  if (target != GL_TEXTURE_2D) {
    L_ONCE("tex: glTexImage2D target 0x%X (cube faces are TODO), ignored", target);
    return;
  }
  gl_texture *t = bound_tex(target);
  if (!t) { GL_ERR(GL_INVALID_OPERATION); return; }
  if (level > 0) {
    if (!t->levels_dropped++) L_ONCE("tex: mip levels > 0 are dropped for now (TODO mipmaps)");
    return;
  }
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { GL_ERR(GL_INVALID_VALUE); return; }
  fmt_info fi;
  pick_format(ifmt, fmt, type, &fi);
  t->ifmt = ifmt;
  if (tex_storage(t, w, h, &fi) < 0) { GL_ERR(GL_OUT_OF_MEMORY); return; }
  upload_rect(t, &fi, 0, 0, w, h, data);
  TRACE("glTexImage2D tex %u %dx%d fmt 0x%X type 0x%X", t->name, w, h, fmt, type);
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                     GLenum fmt, GLenum type, const void *data)
{
  gl_texture *t = bound_tex(target);
  if (target != GL_TEXTURE_2D || !t || !t->valid || level > 0) return;
  if (x < 0 || y < 0 || x + w > (GLint)t->w || y + h > (GLint)t->h) { GL_ERR(GL_INVALID_VALUE); return; }
  fmt_info fi;
  pick_format(t->ifmt, fmt, type, &fi);
  if (fi.bpp != t->bpp || fi.fmt != t->fmt) {
    L_ONCE("tex: glTexSubImage2D format mismatch (0x%X/0x%X), ignored", fmt, type);
    return;
  }
  if (gxm_busy(t->last_use)) {
    // copy-on-write so queued draws still see the old texels
    gpu_block *nb = gpu_alloc(t->mem->size, 256, gpu_is_cdram(t->mem) ? GPU_PREF_CDRAM : GPU_PREF_LPDDR);
    if (!nb) { GL_ERR(GL_OUT_OF_MEMORY); return; }
    memcpy(nb->ptr, t->mem->ptr, t->stride * t->h);
    gpu_free_deferred(t->mem);
    t->mem = nb;
    t->last_use = 0;
    sceGxmTextureSetData(&t->gxm, nb->ptr);
  }
  upload_rect(t, &fi, x, y, w, h, data);
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                            GLint border, GLsizei size, const void *data)
{
  (void)border;
  static uint64_t etc_in, etc_out;
  if (target != GL_TEXTURE_2D) {
    L_ONCE("tex: compressed target 0x%X (cube) TODO, ignored", target);
    return;
  }
  gl_texture *t = bound_tex(target);
  if (!t) { GL_ERR(GL_INVALID_OPERATION); return; }
  log_format_once(ifmt, 0, 0, etc_block_bytes(ifmt) ? "compressed ETC, cpu decoded to rgba8"
                                                    : "compressed UNSUPPORTED");
  if (level > 0) {
    if (!t->levels_dropped++) L_ONCE("tex: mip levels > 0 are dropped for now (TODO mipmaps)");
    return;
  }
  int bs = etc_block_bytes(ifmt);
  if (!bs || !data) {
    L_ONCE("tex: glCompressedTexImage2D ifmt 0x%X not handled", ifmt);
    return;
  }
  int need = ((w + 3) / 4) * ((h + 3) / 4) * bs;
  if (size < need) { L("tex: compressed size %d < expected %d", size, need); return; }

  fmt_info fi;
  memset(&fi, 0, sizeof(fi));
  fi.fmt = etc_has_alpha(ifmt) ? SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR : SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR;
  fi.bpp = 4;
  fi.renderable = 1;
  fi.color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
  fi.out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
  fi.out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
  t->ifmt = ifmt;
  if (tex_storage(t, w, h, &fi) < 0) { GL_ERR(GL_OUT_OF_MEMORY); return; }

  // decode into cached ram first, then one big copy into uncached gpu memory
  uint8_t *tmp = malloc(t->stride * h);
  if (tmp) {
    etc_decode(ifmt, data, w, h, tmp, t->stride);
    memcpy(t->mem->ptr, tmp, t->stride * h);
    free(tmp);
  }
  etc_in += size;
  etc_out += t->stride * h;
  static unsigned n;
  if ((n++ & 255) == 0)
    L("tex: etc decoded so far: %u KiB compressed -> %u KiB rgba8 (all textures now %u KiB)",
      (unsigned)(etc_in >> 10), (unsigned)(etc_out >> 10), (unsigned)(g_tex_bytes >> 10));
}

void glTexParameteri(GLenum target, GLenum pname, GLint v)
{
  gl_texture *t = bound_tex(target);
  if (!t) return;
  switch (pname) {
  case GL_TEXTURE_MIN_FILTER: t->min_filter = v; break;
  case GL_TEXTURE_MAG_FILTER: t->mag_filter = v; break;
  case GL_TEXTURE_WRAP_S: t->wrap_s = v; break;
  case GL_TEXTURE_WRAP_T: t->wrap_t = v; break;
  case GL_TEXTURE_WRAP_R:
  case GL_TEXTURE_BASE_LEVEL:
  case GL_TEXTURE_MAX_LEVEL:
  case GL_TEXTURE_MIN_LOD:
  case GL_TEXTURE_MAX_LOD:
  case GL_TEXTURE_MAX_ANISOTROPY_EXT:
    return;
  case GL_TEXTURE_COMPARE_MODE:
  case GL_TEXTURE_COMPARE_FUNC:
    if (v) L_ONCE("tex: shadow compare mode not supported");
    return;
  default:
    L_ONCE("tex: glTexParameteri 0x%X = 0x%X ignored", pname, v);
    return;
  }
  if (t->valid) apply_sampler(t);
}

SOFTFP void glTexParameterf(GLenum target, GLenum pname, GLfloat v)
{
  glTexParameteri(target, pname, (GLint)v);
}

void glGenerateMipmap(GLenum target)
{
  (void)target;
  L_ONCE("tex: glGenerateMipmap ignored (no mips yet)");
}
