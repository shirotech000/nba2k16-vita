// textures. 2d ones are linear gxm textures: level 0 sits at mem->ptr with the
// stride the fbo code renders into, mips are packed right after it. cube maps
// are gxm CUBE textures (swizzled, pow2). pixels get converted on the cpu when
// the GL format has no direct gxm match. etc rgb / rgba gets transcoded to
// bc1 / bc3 (bcenc.c) instead of decoded to rgba8: 1/8 resp. 1/4 of the size.
//
// layout, checked against what vita3k's texture decoder reads:
// - linear level n: rows padded to 8 pixels, levels back to back. only pow2
//   textures get a chain (vita3k lays npot chains out on pow2-rounded sizes,
//   too shaky to bet on), and the chain stops where the short side hits 1.
// - cube: face f starts at f * face_size. a face is its whole chain down to
//   1x1 (reserved even if fewer levels are used), each level morton swizzled
//   (texutil.c), padded to 2 KiB once it's big enough. always init cubes with
//   mipCount >= 1: 0 means a packed layout without the chain.
// - bc1/bc3 (2d only): gxm SWIZZLED, 4x4 blocks in morton order over the
//   level's block grid (tu_bc_dim: pow2 size / 4, min 1 block), levels back
//   to back. swizzled because that's the layout every retail dxt .gxt uses,
//   linear dxt is something we only know vita3k to accept. npot ones are
//   SWIZZLED_ARBITRARY, same memory on the pow2-rounded grid. vita3k reads
//   both like this (it unswizzles whole blocks with the same morton order).
//   no mips on npot here either.
// - cube faces: gl and gxm use the same order (+x -x +y -y +z -z, gxm face i
//   = GL_TEXTURE_CUBE_MAP_POSITIVE_X + i) and the same per-face orientation
//   (row 0 of the data = row 0 in memory = t 0 of the face). the direction
//   reaches texCUBE untouched (glsl2cg), so no flips or swaps anywhere.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include "gles/gl_internal.h"
#include "gles/gl_api.h"
#include "gles/texutil.h"
#include "gles/bcenc.h"

#ifndef GL_UNSIGNED_INT_10F_11F_11F_REV
#define GL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#endif
#ifndef GL_UNSIGNED_INT_5_9_9_9_REV
#define GL_UNSIGNED_INT_5_9_9_9_REV 0x8C3E
#endif
#ifndef GL_SRGB_EXT
#define GL_SRGB_EXT 0x8C40
#endif
#ifndef GL_SRGB_ALPHA_EXT
#define GL_SRGB_ALPHA_EXT 0x8C42
#endif

static name_table g_texs;

gl_texture *tex_get(GLuint name) { return nt_get(&g_texs, name); }

// ============================================================ formats

enum { CV_COPY, CV_RGB8_RGBA8, CV_F32_F16, CV_RGB16F, CV_RGB32F, CV_ZERO };

typedef struct {
  SceGxmTextureFormat fmt;
  unsigned src_bpp, bpp;
  int conv;
  int renderable;
  SceGxmColorFormat color_fmt;
  SceGxmOutputRegisterFormat out_fmt;
  SceGxmOutputRegisterSize out_size;
  int is_depth;
  unsigned bc;   // block compressed: bytes per 4x4 block (bpp holds the same), else 0
} fmt_info;

// 8 for bc1, 16 for bc3, 0 for everything else
static unsigned fmt_bc(SceGxmTextureFormat f)
{
  switch ((uint32_t)f & 0x9F000000u) {
  case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1: return 8;
  case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3: return 16;
  default: return 0;
  }
}

static void log_format_once(GLint ifmt, GLenum fmt, GLenum type, const char *what)
{
  static uint32_t seen[64];
  static int n;
  uint32_t key = (uint32_t)ifmt * 31u + fmt * 7u + type;
  for (int i = 0; i < n; i++) if (seen[i] == key) return;
  if (n < 64) seen[n++] = key;
  L("tex: new format ifmt=0x%X fmt=0x%X type=0x%X -> %s", ifmt, fmt, type, what);
}

static int is_srgb_ifmt(GLenum ifmt)
{
  switch (ifmt) {
  case GL_SRGB8: case GL_SRGB8_ALPHA8: case GL_SRGB_EXT: case GL_SRGB_ALPHA_EXT:
  case GL_COMPRESSED_SRGB8_ETC2: case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
  case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
    return 1;
  default:
    return 0;
  }
}

// gxm only decodes gamma on 8 bit channels, we only ever need it on rgba8
static int gamma_ok(SceGxmTextureFormat f)
{
  return (f & 0x9F000000u) == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8;
}

static void log_with_srgb(GLint ifmt, GLenum fmt, GLenum type, SceGxmTextureFormat gf, const char *what)
{
  if (!is_srgb_ifmt(ifmt)) { log_format_once(ifmt, fmt, type, what); return; }
  char buf[160];
  snprintf(buf, sizeof(buf), "%s, sRGB %s", what,
           gamma_ok(gf) ? "via gxm gamma (BGR)" : "SAMPLED LINEAR (no gxm gamma for this format)");
  log_format_once(ifmt, fmt, type, buf);
}

// GXM names packed formats from the msb down, so U8U8U8U8_ABGR is GL's RGBA
// byte order on a little endian word, U5U6U5_RGB is GL 565 (r on top),
// U4U4U4U4_RGBA is GL 4444 and U5U5U5U1_RGBA is GL 5551. single channel
// swizzles: _R reads (r,0,0,1) like GL_RED, 1RRR (l,l,l,1), R000 (0,0,0,a).
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
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8_R; fi->src_bpp = fi->bpp = 1;
    what = "U8_R (R8)";
  } else if (fmt == GL_RG && type == GL_UNSIGNED_BYTE) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8_GR; fi->src_bpp = fi->bpp = 2;
    what = "U8U8_GR (RG8)";
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
  } else if (fmt == GL_RGB && (type == GL_HALF_FLOAT || type == GL_FLOAT)) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_X16F16F16F16_1BGR;
    fi->src_bpp = type == GL_FLOAT ? 12 : 6; fi->bpp = 8;
    fi->conv = type == GL_FLOAT ? CV_RGB32F : CV_RGB16F;
    fi->renderable = 1; fi->color_fmt = SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR;
    fi->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4;
    fi->out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT;
    what = "X16F16F16F16_1BGR (rgb float expanded to rgba16f)";
  } else if (fmt == GL_RED && type == GL_HALF_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F16_R; fi->src_bpp = fi->bpp = 2;
    what = "F16_R";
  } else if (fmt == GL_RG && type == GL_HALF_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F16F16_GR; fi->src_bpp = fi->bpp = 4;
    what = "F16F16_GR";
  } else if (fmt == GL_RED && type == GL_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F32_R; fi->src_bpp = fi->bpp = 4;
    what = "F32_R";
  } else if (fmt == GL_RG && type == GL_FLOAT) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F32F32_GR; fi->src_bpp = fi->bpp = 8;
    what = "F32F32_GR";
  } else if (fmt == GL_RGBA && type == GL_UNSIGNED_INT_2_10_10_10_REV) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U2U10U10U10_ABGR; fi->src_bpp = fi->bpp = 4;
    what = "U2U10U10U10_ABGR";
  } else if (fmt == GL_RGB && type == GL_UNSIGNED_INT_10F_11F_11F_REV) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_F10F11F11_BGR; fi->src_bpp = fi->bpp = 4;
    what = "F10F11F11_BGR";
  } else if (fmt == GL_RGB && type == GL_UNSIGNED_INT_5_9_9_9_REV) {
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_SE5M9M9M9_BGR; fi->src_bpp = fi->bpp = 4;
    what = "SE5M9M9M9_BGR";
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
  log_with_srgb(ifmt, fmt, type, fi->fmt, what);
  return 1;
}

// tex_bc=0 in config.txt: decode etc to rgba8 like before (4-8x the memory)
static int bc_enabled(void)
{
  static int on = -1;
  if (on < 0) {
    extern int config_int(const char *key, int def);
    on = config_int("tex_bc", 1) != 0;
    L("tex: etc textures get %s", on ? "transcoded to bc1/bc3 (tex_bc=0 turns it off)"
                                     : "decoded to rgba8 (tex_bc=0)");
  }
  return on;
}

// etc/eac targets. bc = transcode to bc1/bc3 if the format has one, else
// (and for r11/rg11) cpu decode to plain pixels
static int etc_format(GLenum ifmt, fmt_info *fi, int bc)
{
  memset(fi, 0, sizeof(*fi));
  fi->conv = CV_COPY;
  fi->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
  fi->out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
  const char *what;
  switch (ifmt) {
  case GL_COMPRESSED_R11_EAC:
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8_R; what = "EAC R11, cpu decoded to U8_R"; break;
  case GL_COMPRESSED_SIGNED_R11_EAC:
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_S8_R; what = "EAC signed R11, cpu decoded to S8_R"; break;
  case GL_COMPRESSED_RG11_EAC:
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_U8U8_GR; what = "EAC RG11, cpu decoded to U8U8_GR"; break;
  case GL_COMPRESSED_SIGNED_RG11_EAC:
    fi->fmt = SCE_GXM_TEXTURE_FORMAT_S8S8_GR; what = "EAC signed RG11, cpu decoded to S8S8_GR"; break;
  default:
    if (!etc_block_bytes(ifmt)) {
      log_format_once(ifmt, 0, 0, "compressed UNSUPPORTED");
      return 0;
    }
    if (bc && bc_bytes_for_etc(ifmt)) {
      // bc1 blocks without transparent texels read the same with and without
      // dxt1 alpha, so plain UBC1 does for rgb too
      fi->bc = bc_bytes_for_etc(ifmt);
      fi->fmt = fi->bc == 16 ? SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR : SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR;
      fi->bpp = fi->src_bpp = fi->bc;
      log_format_once(ifmt, 0, 1, fi->bc == 16 ? "compressed ETC2 rgba, transcoded to UBC3 (bc3)"
                                  : etc_has_alpha(ifmt) ? "compressed ETC2 punch-through, transcoded to UBC1 (bc1, 1 bit alpha)"
                                                        : "compressed ETC rgb, transcoded to UBC1 (bc1)");
      return 1;
    }
    fi->fmt = etc_has_alpha(ifmt) ? SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR : SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR;
    fi->renderable = 1;
    fi->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
    what = etc_has_alpha(ifmt) ? "compressed ETC2 w/ alpha, cpu decoded to rgba8"
                               : "compressed ETC, cpu decoded to rgbx8";
    break;
  }
  fi->bpp = fi->src_bpp = etc_out_bpp(ifmt);
  log_with_srgb(ifmt, 0, 0, fi->fmt, what);
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

// box filter kind for glGenerateMipmap, 0 = can't
static int box_kind(SceGxmTextureFormat f)
{
  switch ((uint32_t)f) {
  case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR:
  case SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR:
  case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB:
    return TU_BOX_8888;
  case SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB: return TU_BOX_565;
  case SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA: return TU_BOX_4444;
  case SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA: return TU_BOX_5551;
  case SCE_GXM_TEXTURE_FORMAT_U8_1RRR:
  case SCE_GXM_TEXTURE_FORMAT_U8_R000:
  case SCE_GXM_TEXTURE_FORMAT_U8_R:
    return TU_BOX_8;
  case SCE_GXM_TEXTURE_FORMAT_U8U8_GRRR:
  case SCE_GXM_TEXTURE_FORMAT_U8U8_GR:
    return TU_BOX_88;
  default:
    return TU_BOX_NONE;
  }
}

// ============================================================ layout

typedef struct {
  unsigned w, h, bpp, levels;
  int cube;
  uint32_t face_size;
  int bc;   // swizzled bc blocks, bpp = bytes per block
} shape;

static unsigned lvl_dim(unsigned d, unsigned l) { d >>= l; return d ? d : 1; }

static uint32_t lvl_size(const shape *s, unsigned l)
{
  unsigned w = lvl_dim(s->w, l), h = lvl_dim(s->h, l);
  if (s->bc) return tu_bc_level_size(s->w, s->h, l, s->bpp);
  return s->cube ? w * h * s->bpp : tu_linear_level_size(w, h, s->bpp);
}

static uint32_t lvl_off(const shape *s, unsigned face, unsigned l)
{
  uint32_t o = s->cube ? face * s->face_size : 0;
  for (unsigned i = 0; i < l; i++) o += lvl_size(s, i);
  return o;
}

static unsigned lvl_stride(const shape *s, unsigned l)
{
  unsigned w = lvl_dim(s->w, l);
  if (s->bc) return tu_bc_dim(s->w, l) * s->bpp;   // a row of blocks, nothing uses it as a stride
  return s->cube ? w * s->bpp : ALIGN_UP(w, 8) * s->bpp;
}

static uint32_t shape_bytes(const shape *s)
{
  return s->cube ? 6 * s->face_size : lvl_off(s, 0, s->levels);
}

// longest chain we keep: until the short side hits 1 (that's where gxm stops)
static unsigned chain_len(unsigned w, unsigned h)
{
  unsigned n = tu_log2(w < h ? w : h) + 1;
  return n > 13 ? 13 : n;
}

static shape make_shape(unsigned w, unsigned h, unsigned bpp, int cube, unsigned levels, int bc)
{
  shape s = { w, h, bpp, levels, cube, 0, bc };
  if (cube) {
    s.levels = chain_len(w, h);
    s.face_size = tu_cube_face_size(w, bpp);
  }
  return s;
}

static shape shape_of(const gl_texture *t)
{
  shape s = { t->w, t->h, t->bpp, t->levels, t->is_cube, t->face_size, fmt_bc(t->fmt) != 0 };
  return s;
}

static int is_mip_filter(GLenum f)
{
  return f == GL_NEAREST_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_NEAREST ||
         f == GL_NEAREST_MIPMAP_LINEAR || f == GL_LINEAR_MIPMAP_LINEAR;
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
  // mips on/off is the descriptor's level count. the mip filter only picks
  // nearest level vs blending two
  int lin_mip = t->mips_used > 1 &&
                (t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR);
  sceGxmTextureSetMipFilter(g, lin_mip ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
  if (t->is_cube) {
    // es3 cube maps always clamp
    sceGxmTextureSetUAddrMode(g, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(g, SCE_GXM_TEXTURE_ADDR_CLAMP);
  } else {
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
  if (t->srgb) {
    int r = sceGxmTextureSetGammaMode(g, SCE_GXM_TEXTURE_GAMMA_BGR);
    if (r < 0) L_ONCE("tex: sceGxmTextureSetGammaMode failed 0x%08X (fmt 0x%08X), sRGB sampled linear", r, t->fmt);
  }
}

// (re)build the gxm descriptor from storage + GL params. the descriptor gets
// the run of specified levels from the base level (on every face for cubes),
// cut by GL_TEXTURE_MAX_LEVEL, or just 1 level with a non-mip min filter.
// missing mips sample the levels that exist instead of GL's black.
static void tex_desc(gl_texture *t)
{
  t->valid = 0;
  if (!t->mem) return;
  shape s = shape_of(t);
  unsigned faces = t->is_cube ? 6 : 1;
  unsigned have = 0xFFFF;
  for (unsigned f = 0; f < faces; f++) have &= t->defined[f];
  unsigned base = 0;
  if (t->base_level > 0) {
    if (t->is_cube) {
      L_ONCE("tex: GL_TEXTURE_BASE_LEVEL on a cube map ignored");
    } else {
      base = (unsigned)t->base_level < t->levels ? (unsigned)t->base_level : t->levels - 1;
      if (!(have >> base & 1) || (lvl_off(&s, 0, base) & 15)) base = 0;
    }
  }
  if (!(have >> base & 1)) return;   // base level missing (on some face)
  unsigned top = t->levels - 1;
  if (t->max_level >= 0 && (unsigned)t->max_level < top) top = (unsigned)t->max_level;
  unsigned n = 1;
  while (base + n <= top && (have >> (base + n) & 1)) n++;
  unsigned cnt = is_mip_filter(t->min_filter) ? n : 1;
  void *p = (uint8_t *)t->mem->ptr + lvl_off(&s, 0, base);
  unsigned w = lvl_dim(t->w, base), h = lvl_dim(t->h, base);
  int r;
  const char *kind = t->is_cube ? "cube" : "linear";
  if (s.bc) {
    // always a real mip count here: 0 is the linear-only "no chain" marker
    if (tu_is_pow2(w) && tu_is_pow2(h)) {
      kind = "swizzled bc";
      r = sceGxmTextureInitSwizzled(&t->gxm, p, t->fmt, w, h, cnt);
    } else {
      kind = "swizzled arbitrary bc";
      L_ONCE("tex: npot bc texture %u (%ux%u) uses SWIZZLED_ARBITRARY, the least tested layout here", t->name, w, h);
      r = sceGxmTextureInitSwizzledArbitrary(&t->gxm, p, t->fmt, w, h, cnt);
    }
  } else if (t->is_cube) {
    r = sceGxmTextureInitCube(&t->gxm, p, t->fmt, w, h, cnt);
  } else {
    r = sceGxmTextureInitLinear(&t->gxm, p, t->fmt, w, h, t->levels > 1 ? cnt : 0);
  }
  if (r < 0) {
    L("tex: gxm %s init %ux%u fmt 0x%08X levels %u failed 0x%08X", kind, w, h, t->fmt, cnt, r);
    return;
  }
  t->mips_used = cnt;
  apply_sampler(t);
  t->valid = 1;
}

// ============================================================ dummies

static gl_texture g_white2d;
static SceGxmTexture g_black_cube;
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
  // 1x1 cube, (0,0,0,1) like an incomplete GL cube. padded way past any
  // face layout so every texel gxm could fetch is opaque black
  gpu_block *c = gpu_alloc(4096, 4096, GPU_PREF_LPDDR);
  if (c) {
    uint32_t px[1024];
    for (int i = 0; i < 1024; i++) px[i] = 0xFF000000u;
    memcpy(c->ptr, px, sizeof(px));
    int r = sceGxmTextureInitCube(&g_black_cube, c->ptr, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 1, 1, 1);
    if (r < 0) L("tex: dummy cube init failed 0x%08X, cube samplers get the 2d dummy", r);
    else g_cube_ok = 1;
  }
}

const SceGxmTexture *tex_for_unit(int unit, int cube, uint32_t scene)
{
  make_dummies();
  if (cube) {
    // cube_dummy=1 in config.txt: every cube samples black, for bisecting
    // whether a rendering problem comes from our cube maps
    static int dbg_cube = -1;
    if (dbg_cube < 0) {
      extern int config_int(const char *key, int def);
      dbg_cube = config_int("cube_dummy", 0);
      if (dbg_cube) L("tex: cube_dummy set, all cube maps sample black");
    }
    gl_texture *t = dbg_cube ? NULL : tex_get(G.texcube[unit]);
    if (t && t->valid && t->is_cube) {
      t->last_use = scene;
      return &t->gxm;
    }
    if (t) L_ONCE("tex: cube map %u sampled before all 6 faces were specified, black fallback", t->name);
    // never hand gxm an uninitialised descriptor
    return g_cube_ok ? &g_black_cube : &g_white2d.gxm;
  }
  gl_texture *t = tex_get(G.tex2d[unit]);
  if (!t || !t->valid || t->is_cube) return &g_white2d.gxm;
  t->last_use = scene;
  return &t->gxm;
}

// ============================================================ objects

static uint64_t g_tex_bytes;

// memory the bc textures alive right now save against rgba8 storage
static int64_t g_bc_saved;

// what t saves by being bc: the same levels as linear rgba8 minus what it has
static int64_t bc_saving(const gl_texture *t)
{
  if (!t->mem || !fmt_bc(t->fmt)) return 0;
  shape r = make_shape(t->w, t->h, 4, 0, t->levels, 0);
  return (int64_t)shape_bytes(&r) - (int64_t)t->mem->size;
}

static void bc_saved_log(void)
{
  static int64_t last;
  int64_t d = g_bc_saved - last;
  if (d < (8 << 20) && d > -(8 << 20)) return;
  last = g_bc_saved;
  L("tex: bc saves %d KiB right now (all textures %u KiB)", (int)(g_bc_saved >> 10), (unsigned)(g_tex_bytes >> 10));
}

static gl_texture *tex_make(GLuint name)
{
  gl_texture *t = calloc(1, sizeof(*t));
  t->name = name;
  t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
  t->mag_filter = GL_LINEAR;
  t->wrap_s = t->wrap_t = GL_REPEAT;
  t->max_level = 1000;
  nt_set(&g_texs, name, t);
  return t;
}

static void tex_release(gl_texture *t)
{
  if (t->mem) {
    g_bc_saved -= bc_saving(t);
    g_tex_bytes -= t->mem->size;
    gpu_free_deferred(t->mem);
    t->mem = NULL;
    bc_saved_log();
  }
  memset(t->defined, 0, sizeof(t->defined));
  t->valid = 0;
  t->chain_guess = 0;
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
  if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) {
    L_ONCE("tex: glBindTexture target 0x%X not supported", target);
    GL_ERR(GL_INVALID_ENUM);
    return;
  }
  gl_texture *t = name ? tex_get(name) : NULL;
  if (name && !t) t = tex_make(name);
  if (t) {
    if (!t->target) {
      t->target = target;
    } else if (t->target != target) {
      L_ONCE("tex: texture %u bound as 0x%X, but it's a 0x%X", name, target, t->target);
      GL_ERR(GL_INVALID_OPERATION);
      return;
    }
  }
  if (target == GL_TEXTURE_2D) G.tex2d[G.active_tex] = name;
  else G.texcube[G.active_tex] = name;
}

static GLuint g_hint_tex;   // see hint_check

void glDeleteTextures(GLsizei n, const GLuint *names)
{
  for (int i = 0; i < n; i++) {
    gl_texture *t = tex_get(names[i]);
    if (!t) continue;
    for (int u = 0; u < MAX_TEX_UNITS; u++) {
      if (G.tex2d[u] == names[i]) G.tex2d[u] = 0;
      if (G.texcube[u] == names[i]) G.texcube[u] = 0;
    }
    if (g_hint_tex == names[i]) g_hint_tex = 0;
    tex_release(t);
    nt_set(&g_texs, names[i], NULL);
    free(t);
  }
}

static gl_texture *bound_tex(GLenum target)
{
  if (target == GL_TEXTURE_2D) return tex_get(G.tex2d[G.active_tex]);
  if (target == GL_TEXTURE_CUBE_MAP) return tex_get(G.texcube[G.active_tex]);
  return NULL;
}

// texture for an image target. face = cube face 0..5, -1 for 2d
static gl_texture *target_tex(GLenum target, int *face)
{
  *face = -1;
  if (target == GL_TEXTURE_2D) {
    gl_texture *t = tex_get(G.tex2d[G.active_tex]);
    return t && t->target == GL_TEXTURE_2D ? t : NULL;
  }
  if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
    *face = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    gl_texture *t = tex_get(G.texcube[G.active_tex]);
    return t && t->target == GL_TEXTURE_CUBE_MAP ? t : NULL;
  }
  L_ONCE("tex: image target 0x%X not supported", target);
  return NULL;
}

// ============================================================ storage

// (re)allocate t as shape ns (format from fi, or unchanged if fi is NULL).
// keep = carry over every specified level whose size stays the same, except
// skip_face/skip_level which the caller is about to rewrite. the old block is
// freed once the gpu is done with it. on failure t is left untouched.
static int tex_realloc(gl_texture *t, const shape *ns, const fmt_info *fi, int keep,
                       int skip_face, int skip_level)
{
  SceGxmTextureFormat nfmt = fi ? fi->fmt : t->fmt;
  uint32_t size = shape_bytes(ns);
  uint32_t align = ns->cube ? 4096 : 256;
  gpu_block *nb;
  if (!keep && t->mem && !gxm_busy(t->last_use) && t->mem->size >= size && t->mem->size <= size * 2 &&
      ((uintptr_t)t->mem->ptr & (align - 1)) == 0) {
    nb = t->mem;   // fits and the gpu is done with it
  } else {
    nb = gpu_alloc(size, align, GPU_PREF_CDRAM);
    if (!nb) {
      L("tex: OOM for %ux%u%s texture %u (%u bytes, %u levels, gxm fmt 0x%08X)", ns->w, ns->h,
        ns->cube ? " cube" : "", t->name, size, ns->levels, (unsigned)nfmt);
      return -1;
    }
  }
  uint16_t nd[6] = { 0 };
  if (keep && t->mem && nb != t->mem && t->fmt == nfmt && t->bpp == ns->bpp && t->is_cube == ns->cube) {
    shape os = shape_of(t);
    unsigned faces = ns->cube ? 6 : 1;
    unsigned nl = os.levels < ns->levels ? os.levels : ns->levels;
    for (unsigned f = 0; f < faces; f++)
      for (unsigned l = 0; l < nl; l++) {
        if (!(t->defined[f] >> l & 1) || ((int)f == skip_face && (int)l == skip_level)) continue;
        if (lvl_dim(os.w, l) != lvl_dim(ns->w, l) || lvl_dim(os.h, l) != lvl_dim(ns->h, l)) continue;
        memcpy((uint8_t *)nb->ptr + lvl_off(ns, f, l), (const uint8_t *)t->mem->ptr + lvl_off(&os, f, l),
               lvl_size(ns, l));
        nd[f] |= (uint16_t)(1u << l);
      }
  }
  g_bc_saved -= bc_saving(t);
  if (nb != t->mem) {
    if (t->mem) {
      g_tex_bytes -= t->mem->size;
      gpu_free_deferred(t->mem);
    }
    t->mem = nb;
    g_tex_bytes += nb->size;
    t->last_use = 0;
  }
  t->w = ns->w;
  t->h = ns->h;
  t->bpp = ns->bpp;
  t->levels = ns->levels;
  t->is_cube = ns->cube;
  t->face_size = ns->face_size;
  t->stride = lvl_stride(ns, 0);
  if (fi) {
    t->fmt = fi->fmt;
    t->renderable = fi->renderable;
    t->color_fmt = fi->color_fmt;
    t->out_fmt = fi->out_fmt;
    t->out_size = fi->out_size;
    t->is_depth = fi->is_depth;
  }
  // cube memory is swizzled, the fbo code would render linear rows into it
  if (ns->cube) t->renderable = 0;
  memcpy(t->defined, nd, sizeof(nd));
  t->chain_guess = 0;
  t->valid = 0;   // caller rebuilds the descriptor
  g_bc_saved += bc_saving(t);
  bc_saved_log();
  return 0;
}

// about to write into t's memory with the cpu. if the gpu may still read it,
// move to a fresh copy first. full = face/level gets rewritten completely, so
// it doesn't need copying
static int make_writable(gl_texture *t, int face, int level, int full)
{
  if (!t->mem || !gxm_busy(t->last_use)) return 0;
  shape s = shape_of(t);
  uint16_t def = t->defined[face < 0 ? 0 : face];
  if (tex_realloc(t, &s, NULL, 1, full ? face : -1, full ? level : -1) < 0) return -1;
  // the level being written stays specified
  if (full && face >= 0 && level >= 0 && (def >> level & 1)) t->defined[face] |= (uint16_t)(1u << level);
  return 0;
}

// mip chain guessing. growing a texture's chain when level 1 shows up costs a
// copy of level 0 out of (slow to read) cdram, so once the game has shown it
// uploads mips we allocate whole chains right away. a guess that gets no level
// 1 by the time the next texture starts is shrunk back to level 0 only.
static int g_mip_hint;

static void hint_check(gl_texture *nt)
{
  gl_texture *o = g_hint_tex ? tex_get(g_hint_tex) : NULL;
  g_hint_tex = nt->name;
  if (!o || o == nt || !o->chain_guess) return;
  o->chain_guess = 0;
  g_mip_hint = 0;
  if (o->mem && !o->is_cube && !(o->defined[0] & ~1u)) {
    shape ns = make_shape(o->w, o->h, o->bpp, 0, 1, fmt_bc(o->fmt) != 0);
    if (tex_realloc(o, &ns, NULL, 1, -1, -1) == 0) tex_desc(o);
  }
}

// budget mode: when the pool runs low, big new chains go straight to a
// reduced size instead of eating the last mb and forcing every texture
// after them into the white dummy (hw: the career load lost 43 textures
// and the heads + crowd with them). returns the skip level, 0 = store full.
// tex_half=1 (default): every mipped chain big enough to matter starts at
// half resolution - stage, character, crowd textures - freeing the pool so
// the crowd sections fit (owner: "compromise on all stage/character
// textures"). ui art has no mips and stays full. tex_half=0 puts it back
static unsigned budget_skip(unsigned w, unsigned h, const shape *full, uint32_t *kb_out)
{
  static int on = -1;
  if (on < 0) {
    extern int config_int(const char *key, int def);
    on = config_int("tex_budget", 1);
  }
  if (!on) return 0;
  if (shape_bytes(full) < 128 * 1024) return 0;   // small ones aren't the problem
  static int half = -1;
  if (half < 0) {
    extern int config_int(const char *key, int def);
    half = config_int("tex_half", 1);
  }
  extern uint32_t gxm_pool_free_kb(void);
  uint32_t kb = gxm_pool_free_kb();
  if (kb_out) *kb_out = kb;
  unsigned sk = half ? 1 : 0;
  if (kb < 2 * 1024) { if (sk < 3) sk = 3; }
  else if (kb < 4 * 1024) { if (sk < 2) sk = 2; }
  else if (kb < 9 * 1024) { if (sk < 1) sk = 1; }
  if (!sk) return 0;
  unsigned hw = w >> sk, hh = h >> sk;
  if (hw < 8 || hh < 8) return 0;
  return sk;
}

// get 2d storage ready for (level, w, h). 0 ok, -1 upload dropped, -2 oom
static int prep_2d(gl_texture *t, unsigned level, unsigned w, unsigned h, const fmt_info *fi, int has_data)
{
  int same_fmt = t->mem && !t->is_cube && t->fmt == fi->fmt && t->bpp == fi->bpp;
  if (level == 0) {
    if (same_fmt && t->w == w && t->h == h) return make_writable(t, 0, 0, 1) < 0 ? -2 : 0;
    int pow2 = tu_is_pow2(w) && tu_is_pow2(h);
    // mips that came before level 0 are kept if they fit
    int keep = same_fmt && !(t->defined[0] & 1) && t->defined[0];
    if (has_data) hint_check(t);   // may turn the hint off, so before using it
    unsigned levels = 1;
    if (pow2 && (keep || (has_data && g_mip_hint && is_mip_filter(t->min_filter))))
      levels = chain_len(w, h);
    shape ns = make_shape(w, h, fi->bpp, 0, levels, fi->bc != 0);
    // low pool: store it reduced right away instead of trying full size and
    // risking whatever loads after it. only with mips coming (levels > 1),
    // skip mode drops level 0 and waits for the smaller ones
    if (pow2 && has_data && levels > 1) {
      uint32_t kb = 0;
      unsigned sk = budget_skip(w, h, &ns, &kb);
      if (sk) {
        unsigned hw = w >> sk, hh = h >> sk;
        shape hs = make_shape(hw, hh, fi->bpp, 0, chain_len(hw, hh), fi->bc != 0);
        if (tex_realloc(t, &hs, fi, 0, 0, 0) == 0) {
          t->skip = sk;
          t->chain_guess = 0;
          static unsigned n_bud;
          if (n_bud++ < 16 || (n_bud & 63) == 0)
            L("tex: %ux%u texture %u stored at %ux%u (budget, %uK left)", w, h, t->name, hw, hh, kb);
          return -3;
        }
      }
    }
    if (tex_realloc(t, &ns, fi, keep, 0, 0) < 0) {
      // near the limit: level 0 alone beats the white dummy
      ns = make_shape(w, h, fi->bpp, 0, 1, fi->bc != 0);
      levels = 1;
      if (tex_realloc(t, &ns, fi, 0, 0, 0) < 0) {
        // still no room: keep it at half size if the game sends mips (it does
        // for nearly all of these). its level 0 gets dropped and level 1
        // becomes ours, a quarter of the memory and a bit softer, instead of
        // the white dummy (hw: hair, crowd, hoops)
        // half, then quarter, then eighth: blurry beats white
        // no is_mip_filter gate: the game sets the filter after uploading (tex 227,
        // the hoop, never fell back). no mips coming = same white dummy as before
        for (unsigned sk = 1; sk <= 3 && pow2 && (w >> sk) >= 8 && (h >> sk) >= 8; sk++) {
          unsigned hw = w >> sk, hh = h >> sk;
          shape hs = make_shape(hw, hh, fi->bpp, 0, chain_len(hw, hh), fi->bc != 0);
          if (tex_realloc(t, &hs, fi, 0, 0, 0) == 0) {
            t->skip = sk;
            t->chain_guess = 0;
            static unsigned n_half;
            if (n_half++ < 16 || (n_half & 63) == 0)
              L("tex: %ux%u texture %u stored at %ux%u to fit (#%u)", w, h, t->name, hw, hh, n_half);
            return -3;
          }
        }
        tex_release(t);
        return -2;
      }
    }
    t->chain_guess = levels > 1 && !keep;
    return 0;
  }
  if (same_fmt && level < t->levels && lvl_dim(t->w, level) == w && lvl_dim(t->h, level) == h)
    return make_writable(t, 0, (int)level, 1) < 0 ? -2 : 0;
  if (t->mem && (t->defined[0] & 1)) {
    // level 0 is real: grow into its full chain if this level belongs to it
    if (!same_fmt) {
      L_ONCE("tex: mip level with a different format than level 0 dropped (tex %u)", t->name);
      return -1;
    }
    if (!tu_is_pow2(t->w) || !tu_is_pow2(t->h)) {
      L_ONCE("tex: mips of npot textures are dropped (tex %u %ux%u)", t->name, t->w, t->h);
      return -1;
    }
    unsigned n = chain_len(t->w, t->h);
    if (level >= n) return -1;   // past where gxm stops (short side already 1)
    if (lvl_dim(t->w, level) != w || lvl_dim(t->h, level) != h) {
      L_ONCE("tex: level %u is %ux%u, doesn't fit level 0 %ux%u, dropped", level, w, h, t->w, t->h);
      return -1;
    }
    // budget mode: don't grow the chain while the pool is low. level 0
    // stays sharp and the mips just don't come, better than the pressure
    // this growth would put on everything still to load
    {
      shape fs = make_shape(t->w, t->h, t->bpp, 0, n, fmt_bc(t->fmt) != 0);
      if (budget_skip(t->w, t->h, &fs, NULL)) {
        static unsigned n_grow;
        if (n_grow++ < 16 || (n_grow & 63) == 0)
          L("tex: mip growth of texture %u skipped (budget)", t->name);
        return -1;
      }
    }
    shape ns = make_shape(t->w, t->h, t->bpp, 0, n, fmt_bc(t->fmt) != 0);
    return tex_realloc(t, &ns, NULL, 1, -1, -1) < 0 ? -2 : 0;
  }
  // no level 0 yet (mips first): guess level 0 from this one. a 1 pixel wide
  // level can't tell the shape, so it doesn't get to replace an earlier guess
  if (same_fmt && t->defined[0] && (w == 1 || h == 1)) return -1;
  unsigned bw = w << level, bh = h << level;
  if (bw > 4096 || bh > 4096 || !tu_is_pow2(bw) || !tu_is_pow2(bh)) {
    L_ONCE("tex: level %u (%ux%u) uploaded before level 0 and can't be placed, dropped", level, w, h);
    return -1;
  }
  shape ns = make_shape(bw, bh, fi->bpp, 0, chain_len(bw, bh), fi->bc != 0);
  if (level >= ns.levels) return -1;
  return tex_realloc(t, &ns, fi, same_fmt, -1, -1) < 0 ? -2 : 0;
}

// get cube storage ready for (face, level, w, h). npot faces get resampled to
// pow2 on upload. 0 ok, -1 dropped, -2 oom
static int prep_cube(gl_texture *t, unsigned face, unsigned level, unsigned w, unsigned h, const fmt_info *fi)
{
  int same_fmt = t->mem && t->is_cube && t->fmt == fi->fmt && t->bpp == fi->bpp;
  unsigned s = tu_pow2_ceil(w > h ? w : h);
  if (w != h || s != w) L_ONCE("tex: cube face %ux%u isn't pow2 square, resampled to %u (nearest)", w, h, s);
  if (level == 0) {
    if (same_fmt && t->w == s) return make_writable(t, (int)face, 0, 1) < 0 ? -2 : 0;
    shape ns = make_shape(s, s, fi->bpp, 1, 0, 0);
    if (tex_realloc(t, &ns, fi, same_fmt, (int)face, 0) < 0) {
      tex_release(t);
      return -2;
    }
    return 0;
  }
  if (same_fmt && level < t->levels) return make_writable(t, (int)face, (int)level, 1) < 0 ? -2 : 0;
  if (same_fmt) return -1;   // past the 1x1 level
  int any0 = 0;
  for (int f = 0; f < 6; f++) any0 |= t->defined[f] & 1;
  if (t->mem && t->is_cube && any0) {
    L_ONCE("tex: cube mip with a different format than level 0 dropped (tex %u)", t->name);
    return -1;
  }
  // no level 0 on any face yet: guess the size from this level
  unsigned bs = s << level;
  if (bs > 4096) return -1;
  shape ns = make_shape(bs, bs, fi->bpp, 1, 0, 0);
  return tex_realloc(t, &ns, fi, 0, -1, -1) < 0 ? -2 : 0;
}

// ============================================================ pixel transfer

typedef struct {
  const fmt_info *fi;
  const uint8_t *data;   // NULL = zeros
  unsigned w, h;
  unsigned stride;       // client row stride (raw data)
  GLenum etc;            // etc/eac format of compressed data, 0 = raw pixels
} src_t;

// ---- raw uploads -> bc ----
// the textures still not fitting in a match are mostly uncompressed (565,
// 4444, rgba8). mipped pow2 ones 64px and up (3d stuff, not ui) get bc
// encoded at upload: 565/rgb8 -> bc1 (1/4, 1/8), 4444/5551/rgba8 -> bc3
// (1/2, 1/4), rgba8 that's fully opaque -> bc1. tex_bc_raw=0 turns it off.
#define SRC_RGBA_BC 1   // src_t.etc marker: data is linear rgba8 to bc encode

static void bc_from_rgba(const uint8_t *rgba, unsigned w, unsigned h, unsigned bs,
                         uint8_t *level, unsigned gw, unsigned gh, unsigned bx0, unsigned by0)
{
  uint8_t blk[64];
  uint32_t out[4];
  for (unsigned by = 0; by < (h + 3) / 4; by++) {
    for (unsigned bx = 0; bx < (w + 3) / 4; bx++) {
      for (unsigned y = 0; y < 4; y++) {
        unsigned sy = by * 4 + y < h ? by * 4 + y : h - 1;
        for (unsigned x = 0; x < 4; x++) {
          unsigned sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
          memcpy(blk + (y * 4 + x) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
        }
      }
      if (bs == 8) bc1_encode(blk, 16, (uint8_t *)out, 0);
      else bc3_encode(blk, 16, (uint8_t *)out);
      // whole words only, it's uncached gpu memory
      uint32_t *d = (uint32_t *)(level + (size_t)tu_morton(bx0 + bx, by0 + by, gw, gh) * bs);
      for (unsigned i = 0; i < bs / 4; i++) d[i] = out[i];
    }
  }
}

// raw pixels -> linear rgba8. 0 = format we don't convert
static int raw_to_rgba(GLenum fmt, GLenum type, const uint8_t *src, unsigned w, unsigned h,
                       unsigned stride, uint8_t *dst, int *opaque)
{
  int op = 1;
  for (unsigned y = 0; y < h; y++) {
    const uint8_t *r = src + (size_t)y * stride;
    uint8_t *d = dst + (size_t)y * w * 4;
    for (unsigned x = 0; x < w; x++, d += 4) {
      if (fmt == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        unsigned v = r[x * 2] | r[x * 2 + 1] << 8;
        d[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
        d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
        d[2] = (uint8_t)((v & 31) * 255 / 31);
        d[3] = 255;
      } else if (fmt == GL_RGBA && type == GL_UNSIGNED_SHORT_4_4_4_4) {
        unsigned v = r[x * 2] | r[x * 2 + 1] << 8;
        d[0] = (uint8_t)((v >> 12) * 17);
        d[1] = (uint8_t)(((v >> 8) & 15) * 17);
        d[2] = (uint8_t)(((v >> 4) & 15) * 17);
        d[3] = (uint8_t)((v & 15) * 17);
      } else if (fmt == GL_RGBA && type == GL_UNSIGNED_SHORT_5_5_5_1) {
        unsigned v = r[x * 2] | r[x * 2 + 1] << 8;
        d[0] = (uint8_t)((v >> 11) * 255 / 31);
        d[1] = (uint8_t)(((v >> 6) & 31) * 255 / 31);
        d[2] = (uint8_t)(((v >> 1) & 31) * 255 / 31);
        d[3] = (v & 1) ? 255 : 0;
      } else if (fmt == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        memcpy(d, r + x * 4, 4);
      } else if (fmt == GL_RGB && type == GL_UNSIGNED_BYTE) {
        d[0] = r[x * 3]; d[1] = r[x * 3 + 1]; d[2] = r[x * 3 + 2]; d[3] = 255;
      } else {
        return 0;
      }
      if (d[3] != 255) op = 0;
    }
  }
  if (opaque) *opaque = op;
  return 1;
}

static unsigned raw_src_bpp(GLenum fmt, GLenum type)
{
  if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) return 2;
  if (type == GL_UNSIGNED_BYTE) return fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : 0;
  return 0;
}

static unsigned client_stride(unsigned w, unsigned src_bpp)
{
  unsigned rowlen = G.unpack_row_len ? (unsigned)G.unpack_row_len : w;
  unsigned align = G.unpack_align > 0 ? (unsigned)G.unpack_align : 4;
  return ALIGN_UP(rowlen * src_bpp, align);
}

// rows [y0, y0+n) of the source in storage format, into cached memory
static void src_rows(const src_t *s, unsigned y0, unsigned n, uint8_t *dst, unsigned dstride)
{
  unsigned bpp = s->fi->bpp, w = s->w;
  if (!s->data || s->fi->conv == CV_ZERO) {
    for (unsigned r = 0; r < n; r++) memset(dst + r * dstride, 0, w * bpp);
    return;
  }
  if (s->etc) {
    // y0 is a multiple of 4 here
    unsigned bw = (w + 3) / 4;
    etc_decode(s->etc, s->data + (y0 / 4) * bw * etc_block_bytes(s->etc), w, n, dst, dstride);
    return;
  }
  for (unsigned r = 0; r < n; r++) {
    const uint8_t *in = s->data + (y0 + r) * s->stride;
    uint8_t *o = dst + r * dstride;
    switch (s->fi->conv) {
    case CV_COPY:
      memcpy(o, in, w * bpp);
      break;
    case CV_RGB8_RGBA8:
      for (unsigned x = 0; x < w; x++, in += 3, o += 4) {
        o[0] = in[0]; o[1] = in[1]; o[2] = in[2]; o[3] = 0xFF;
      }
      break;
    case CV_F32_F16:
    case CV_RGB32F: {
      int c3 = s->fi->conv == CV_RGB32F;
      for (unsigned x = 0; x < w; x++, o += 8) {
        float f[4] = { 0, 0, 0, 1 };
        memcpy(f, in, c3 ? 12 : 16);
        in += c3 ? 12 : 16;
        uint16_t hv[4];
        for (int c = 0; c < 4; c++) hv[c] = f32_to_f16(f[c]);
        memcpy(o, hv, 8);
      }
      break;
    }
    case CV_RGB16F:
      for (unsigned x = 0; x < w; x++, in += 6, o += 8) {
        uint16_t hv[4] = { 0, 0, 0, 0x3C00 };   // alpha 1.0
        memcpy(hv, in, 6);
        memcpy(o, hv, 8);
      }
      break;
    }
  }
}

// write rows [y, y+n), columns [x0, x0+w) of face/level from linear pixels
// already in storage format
static void put_rows(gl_texture *t, unsigned face, unsigned l, unsigned x0, unsigned y, unsigned w,
                     unsigned n, const uint8_t *src, unsigned sstride)
{
  shape s = shape_of(t);
  uint8_t *base = (uint8_t *)t->mem->ptr + lvl_off(&s, face, l);
  if (t->is_cube) {
    tu_swizzle_rect(base, lvl_dim(t->w, l), lvl_dim(t->h, l), t->bpp, x0, y, w, n, src, sstride);
    return;
  }
  unsigned st = lvl_stride(&s, l);
  for (unsigned r = 0; r < n; r++)
    memcpy(base + (y + r) * st + x0 * t->bpp, src + r * sstride, w * t->bpp);
}

// staging strip in cached ram (gpu memory is uncached, no byte writes there)
static uint8_t g_strip[64 * 1024] __attribute__((aligned(16)));

static void write_src(gl_texture *t, unsigned face, unsigned l, unsigned x0, unsigned y0, const src_t *s)
{
  unsigned lw = lvl_dim(t->w, l), lh = lvl_dim(t->h, l), bpp = t->bpp;
  if (fmt_bc(t->fmt)) {
    // etc blocks -> bc blocks, straight into the swizzled level. x0/y0 are
    // multiples of 4. a 4096 wide row of decoded blocks just fits the strip.
    if (!s->etc || !s->data) return;   // callers never get here with anything else
    shape sh = shape_of(t);
    uint8_t *base = (uint8_t *)t->mem->ptr + lvl_off(&sh, 0, l);
    // npot: the grid is bigger than the texture, don't leave junk in the rest
    if (!x0 && !y0 && s->w == lw && s->h == lh && (!tu_is_pow2(t->w) || !tu_is_pow2(t->h)))
      memset(base, 0, lvl_size(&sh, l));
    if (s->etc == SRC_RGBA_BC) {
      bc_from_rgba(s->data, s->w, s->h, fmt_bc(t->fmt), base, tu_bc_dim(t->w, l), tu_bc_dim(t->h, l), x0 / 4, y0 / 4);
      return;
    }
    bc_from_etc(s->etc, s->data, s->w, s->h, base, tu_bc_dim(t->w, l), tu_bc_dim(t->h, l), x0 / 4, y0 / 4, g_strip);
    return;
  }
  if (t->is_cube && (s->w != lw || s->h != lh)) {
    // npot cube face: whole face, nearest resampled to the level size
    uint8_t *a = malloc(s->w * s->h * bpp), *b = malloc(lw * lh * bpp);
    if (a && b) {
      src_rows(s, 0, s->h, a, s->w * bpp);
      tu_resample_nearest(a, s->w, s->h, s->w * bpp, b, lw, lh, lw * bpp, bpp);
      put_rows(t, face, l, 0, 0, lw, lh, b, lw * bpp);
    } else {
      L("tex: out of memory resampling a %ux%u cube face", s->w, s->h);
    }
    free(a);
    free(b);
    return;
  }
  if (!t->is_cube && s->data && !s->etc && s->fi->conv == CV_COPY) {
    put_rows(t, face, l, x0, y0, s->w, s->h, s->data, s->stride);
    return;
  }
  unsigned rb = s->w * bpp;
  unsigned rows = (unsigned)(sizeof(g_strip) / rb) & ~3u;   // etc wants whole block rows
  uint8_t *buf = g_strip, *heap = NULL;
  if (rows < 4) {
    rows = 4;
    heap = malloc(rows * rb);
    if (!heap) { L("tex: out of memory staging a %u wide upload", s->w); return; }
    buf = heap;
  }
  for (unsigned y = 0; y < s->h; y += rows) {
    unsigned n = s->h - y < rows ? s->h - y : rows;
    src_rows(s, y, n, buf, rb);
    put_rows(t, face, l, x0, y0 + y, s->w, n, buf, rb);
  }
  free(heap);
}

// turn a bc texture back into the rgba8 one tex_bc=0 would have made. for the
// things bc storage can't take: pixel sub images and mip generation. reads
// the blocks back from gpu memory, slow, but nothing the game does hits it.
static int tex_unbc(gl_texture *t, const char *why)
{
  unsigned bs = fmt_bc(t->fmt);
  if (!bs || !t->mem) return 0;
  L("tex: %s on bc texture %u (%ux%u, %u levels), falling back to rgba8 for it", why, t->name, t->w, t->h, t->levels);
  shape os = shape_of(t);
  shape ns = make_shape(t->w, t->h, 4, 0, t->levels, 0);
  gpu_block *nb = gpu_alloc(shape_bytes(&ns), 256, GPU_PREF_CDRAM);
  uint8_t *tmp = malloc(t->w * t->h * 4);
  if (!nb || !tmp) {
    L("tex: OOM turning bc texture %u into rgba8 (%u bytes)", t->name, shape_bytes(&ns));
    if (nb) gpu_free(nb);
    free(tmp);
    return -1;
  }
  for (unsigned l = 0; l < t->levels; l++) {
    if (!(t->defined[0] >> l & 1)) continue;
    unsigned lw = lvl_dim(t->w, l), lh = lvl_dim(t->h, l), st = lvl_stride(&ns, l);
    bc_level_to_rgba(bs, (const uint8_t *)t->mem->ptr + lvl_off(&os, 0, l), tu_bc_dim(t->w, l), tu_bc_dim(t->h, l),
                     lw, lh, tmp, lw * 4);
    uint8_t *d = (uint8_t *)nb->ptr + lvl_off(&ns, 0, l);
    for (unsigned y = 0; y < lh; y++) memcpy(d + y * st, tmp + y * lw * 4, lw * 4);
  }
  free(tmp);
  g_bc_saved -= bc_saving(t);
  g_tex_bytes -= t->mem->size;
  gpu_free_deferred(t->mem);   // queued draws keep sampling the bc copy
  t->mem = nb;
  g_tex_bytes += nb->size;
  t->last_use = 0;
  t->bpp = 4;
  t->stride = lvl_stride(&ns, 0);
  t->fmt = bs == 16 || etc_has_alpha(t->ifmt) ? SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR
                                              : SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR;
  t->renderable = 1;
  t->color_fmt = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
  t->out_fmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
  t->out_size = SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
  t->srgb = is_srgb_ifmt(t->ifmt);
  bc_saved_log();
  tex_desc(t);
  return 0;
}

// shared by glTexImage2D / glCompressedTexImage2D: specify a whole level
static void tex_image(gl_texture *t, int face, unsigned level, GLenum ifmt, const fmt_info *fi,
                      const src_t *s, int has_data)
{
  unsigned f = face < 0 ? 0 : (unsigned)face;
  if (face < 0 && t->skip) {
    if (level == 0) t->skip = 0;              // re-specified, try full size again
    else if (level < t->skip) return;         // a level we dropped
    else level -= t->skip;
  }
  int r = face < 0 ? prep_2d(t, level, s->w, s->h, fi, has_data)
                   : prep_cube(t, f, level, s->w, s->h, fi);
  if (r == -3) return;                        // stored smaller, level 0 skipped on purpose
  if (r == -2) { GL_ERR(GL_OUT_OF_MEMORY); tex_desc(t); return; }
  if (r < 0) { t->levels_dropped++; return; }
  if (level == 0) {
    t->ifmt = ifmt;
    int want = is_srgb_ifmt(ifmt);
    // fbo writes don't gamma encode, so an srgb render target must not decode
    // bc only gets here with tex_bc_srgb=1, then gxm gets to say if it can
    t->srgb = want && has_data && (gamma_ok(fi->fmt) || fi->bc);
    if (want && !has_data)
      L_ONCE("tex: sRGB texture %u specified without data (render target?), sampled without gamma", t->name);
  }
  write_src(t, f, level, 0, 0, s);
  t->defined[f] |= (uint16_t)(1u << level);
  if (level > 0) {
    t->chain_guess = 0;
    g_mip_hint = 1;
  }
  if (face >= 0 && !has_data && level == 0)
    L_ONCE("tex: cube face specified without data (render to cube?), rendering into cube faces isn't supported");
  tex_desc(t);
}

static void glTexImage2D_impl(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum fmt, GLenum type, const void *data);
void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum fmt, GLenum type, const void *data)
{
  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  glTexImage2D_impl(target, level, ifmt, w, h, border, fmt, type, data);
  extern void gxm_prof_tex(uint64_t us);
  gxm_prof_tex(sceKernelGetProcessTimeWide() - t0);
}

static void glTexImage2D_impl(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum fmt, GLenum type, const void *data)
{
  (void)border;
  int face;
  gl_texture *t = target_tex(target, &face);
  if (!t) { GL_ERR(GL_INVALID_OPERATION); return; }
  if (level < 0 || level > 12 || w <= 0 || h <= 0 || w > 4096 || h > 4096) { GL_ERR(GL_INVALID_VALUE); return; }
  fmt_info fi;
  pick_format(ifmt, fmt, type, &fi);
  static int raw_bc = -1;
  if (raw_bc < 0) {
    extern int config_int(const char *key, int def);
    raw_bc = config_int("tex_bc_raw", 0) != 0;  // 200-400ms per big menu texture on hw
  }
  unsigned rbpp = raw_src_bpp(fmt, type);
  if (raw_bc && bc_enabled() && face < 0 && data && rbpp) {
    // level 0: mipped pow2 3d-ish textures. mips: follow what level 0 became
    int start = level == 0 && w >= 64 && h >= 64 && tu_is_pow2((unsigned)w) && tu_is_pow2((unsigned)h) &&
                is_mip_filter(t->min_filter);
    int follow = level > 0 && t->mem && !t->is_cube && fmt_bc(t->fmt) && t->raw_bc;
    if (start || follow) {
      uint8_t *rgba = malloc((size_t)w * h * 4);
      int opaque = 0;
      if (rgba && raw_to_rgba(fmt, type, data, (unsigned)w, (unsigned)h, client_stride(w, rbpp), rgba, &opaque)) {
        int bc3 = start ? !opaque : fmt_bc(t->fmt) == 16;
        fmt_info bfi;
        etc_format(bc3 ? 0x9278 /* etc2 rgba -> bc3 */ : 0x9274 /* etc2 rgb -> bc1 */, &bfi, 1);
        src_t bs = { &bfi, rgba, (unsigned)w, (unsigned)h, 0, SRC_RGBA_BC };
        tex_image(t, face, (unsigned)level, (GLenum)ifmt, &bfi, &bs, 1);
        if (start && fmt_bc(t->fmt)) {
          t->raw_bc = 1;
          static unsigned n_raw;
          if (n_raw++ < 8 || (n_raw & 63) == 0)
            L("tex: raw %ux%u (fmt 0x%X type 0x%X) stored as %s (#%u)", w, h, fmt, type, bc3 ? "bc3" : "bc1", n_raw);
        }
        free(rgba);
        return;
      }
      free(rgba);
    }
  }
  src_t s = { &fi, data, (unsigned)w, (unsigned)h, client_stride(w, fi.src_bpp), 0 };
  tex_image(t, face, (unsigned)level, (GLenum)ifmt, &fi, &s, data != NULL);
  TRACE("glTexImage2D tex %u face %d level %d %dx%d fmt 0x%X type 0x%X", t->name, face, level, w, h, fmt, type);
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                     GLenum fmt, GLenum type, const void *data)
{
  int face;
  gl_texture *t = target_tex(target, &face);
  unsigned f = face < 0 ? 0 : (unsigned)face;
  if (t && face < 0 && t->skip && level >= 0) {
    if ((unsigned)level < t->skip) return;   // that level isn't stored
    level -= (GLint)t->skip;
  }
  if (!t || !t->mem || level < 0 || (unsigned)level >= t->levels || !(t->defined[f] >> level & 1)) {
    GL_ERR(GL_INVALID_OPERATION);
    return;
  }
  unsigned lw = lvl_dim(t->w, level), lh = lvl_dim(t->h, level);
  if (x < 0 || y < 0 || w < 0 || h < 0 || (unsigned)(x + w) > lw || (unsigned)(y + h) > lh) {
    GL_ERR(GL_INVALID_VALUE);
    return;
  }
  if (!w || !h || !data) return;
  // pixels can't go into bc blocks (GL doesn't allow this on a compressed
  // texture anyway)
  if (fmt_bc(t->fmt) && tex_unbc(t, "glTexSubImage2D") < 0) { GL_ERR(GL_OUT_OF_MEMORY); return; }
  fmt_info fi;
  pick_format(t->ifmt, fmt, type, &fi);
  if (fi.bpp != t->bpp || fi.fmt != t->fmt) {
    L_ONCE("tex: glTexSubImage2D format mismatch (0x%X/0x%X), ignored", fmt, type);
    return;
  }
  // copy-on-write (whole chain) so queued draws still see the old texels
  if (make_writable(t, face, level, 0) < 0) { GL_ERR(GL_OUT_OF_MEMORY); tex_desc(t); return; }
  src_t s = { &fi, data, (unsigned)w, (unsigned)h, client_stride(w, fi.src_bpp), 0 };
  write_src(t, f, (unsigned)level, (unsigned)x, (unsigned)y, &s);
  tex_desc(t);
}

static void glCompressedTexImage2D_impl(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                            GLint border, GLsizei size, const void *data);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                            GLint border, GLsizei size, const void *data)
{
  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  glCompressedTexImage2D_impl(target, level, ifmt, w, h, border, size, data);
  extern void gxm_prof_tex(uint64_t us);
  gxm_prof_tex(sceKernelGetProcessTimeWide() - t0);
}

static void glCompressedTexImage2D_impl(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                            GLint border, GLsizei size, const void *data)
{
  (void)border;
  static uint64_t etc_in, etc_out;
  int face;
  gl_texture *t = target_tex(target, &face);
  if (!t) { GL_ERR(GL_INVALID_OPERATION); return; }
  if (level < 0 || level > 12 || w <= 0 || h <= 0 || w > 4096 || h > 4096) { GL_ERR(GL_INVALID_VALUE); return; }
  // bc for 2d textures. cubes stay rgba8 (few, and bc cube faces have their
  // own padding rules). srgb stays rgba8 too, that's the one format gxm is
  // known to gamma decode; tex_bc_srgb=1 tries bc + gamma anyway.
  // tex_bc_npot=0 keeps npot ones on rgba8 (they need SWIZZLED_ARBITRARY, the
  // layout we have the least proof for), to tell that apart from the rest.
  static int bc_srgb = -1, bc_npot;
  if (bc_srgb < 0) {
    extern int config_int(const char *key, int def);
    bc_srgb = config_int("tex_bc_srgb", 0) != 0;
    bc_npot = config_int("tex_bc_npot", 1) != 0;
  }
  int bc = bc_enabled() && face < 0 && (bc_srgb || !is_srgb_ifmt(ifmt));
  if (bc && !bc_npot && (!tu_is_pow2((unsigned)w) || !tu_is_pow2((unsigned)h))) bc = 0;
  // mips go where the texture already is, a format change would drop them
  if (level > 0 && face < 0 && t->mem && !t->is_cube && t->defined[0]) bc = fmt_bc(t->fmt) != 0;
  fmt_info fi;
  if (!etc_format(ifmt, &fi, bc)) {
    L_ONCE("tex: glCompressedTexImage2D ifmt 0x%X not handled", ifmt);
    return;
  }
  if (!data) { L_ONCE("tex: glCompressedTexImage2D without data, ignored"); return; }
  int need = ((w + 3) / 4) * ((h + 3) / 4) * etc_block_bytes(ifmt);
  if (size < need) { L("tex: compressed size %d < expected %d", size, need); return; }
  src_t s = { &fi, data, (unsigned)w, (unsigned)h, 0, ifmt };
  tex_image(t, face, (unsigned)level, ifmt, &fi, &s, 1);
  etc_in += size;
  etc_out += fi.bc ? (uint64_t)need : (uint64_t)w * h * fi.bpp;
  static unsigned n;
  if ((n++ & 255) == 0)
    L("tex: etc uploads so far: %u KiB compressed -> %u KiB stored (all textures now %u KiB, bc saves %d KiB)",
      (unsigned)(etc_in >> 10), (unsigned)(etc_out >> 10), (unsigned)(g_tex_bytes >> 10), (int)(g_bc_saved >> 10));
}

// the game doesn't import this one, so it isn't in gl_api.h / the export table
// yet. whole blocks only, like GL wants. works on bc and on rgba8 storage.
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                               GLenum fmt, GLsizei size, const void *data);
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                               GLenum fmt, GLsizei size, const void *data)
{
  int face;
  gl_texture *t = target_tex(target, &face);
  unsigned f = face < 0 ? 0 : (unsigned)face;
  if (!t || !t->mem || level < 0 || (unsigned)level >= t->levels || !(t->defined[f] >> level & 1)) {
    GL_ERR(GL_INVALID_OPERATION);
    return;
  }
  unsigned lw = lvl_dim(t->w, level), lh = lvl_dim(t->h, level);
  if (x < 0 || y < 0 || w < 0 || h < 0 || (unsigned)(x + w) > lw || (unsigned)(y + h) > lh) {
    GL_ERR(GL_INVALID_VALUE);
    return;
  }
  if ((x & 3) || (y & 3) || ((w & 3) && (unsigned)(x + w) != lw) || ((h & 3) && (unsigned)(y + h) != lh)) {
    GL_ERR(GL_INVALID_OPERATION);
    return;
  }
  if (!w || !h || !data) return;
  fmt_info fi;
  if (!etc_format(fmt, &fi, fmt_bc(t->fmt) != 0) || fi.fmt != t->fmt || fi.bpp != t->bpp) {
    L_ONCE("tex: glCompressedTexSubImage2D format 0x%X doesn't match the texture, ignored", fmt);
    GL_ERR(GL_INVALID_OPERATION);
    return;
  }
  int need = ((w + 3) / 4) * ((h + 3) / 4) * etc_block_bytes(fmt);
  if (size < need) { L("tex: compressed sub image size %d < expected %d", size, need); return; }
  if (make_writable(t, face, level, 0) < 0) { GL_ERR(GL_OUT_OF_MEMORY); tex_desc(t); return; }
  src_t s = { &fi, data, (unsigned)w, (unsigned)h, 0, fmt };
  write_src(t, f, (unsigned)level, (unsigned)x, (unsigned)y, &s);
  tex_desc(t);
}

// ============================================================ params

void glTexParameteri(GLenum target, GLenum pname, GLint v)
{
  gl_texture *t = bound_tex(target);
  if (!t) return;
  switch (pname) {
  case GL_TEXTURE_MIN_FILTER: t->min_filter = v; break;
  case GL_TEXTURE_MAG_FILTER: t->mag_filter = v; break;
  case GL_TEXTURE_WRAP_S: t->wrap_s = v; break;
  case GL_TEXTURE_WRAP_T: t->wrap_t = v; break;
  case GL_TEXTURE_BASE_LEVEL:
    if (v) L_ONCE("tex: GL_TEXTURE_BASE_LEVEL %d used", v);
    t->base_level = v < 0 ? 0 : v;
    break;
  case GL_TEXTURE_MAX_LEVEL:
    if (v < 12) L_ONCE("tex: GL_TEXTURE_MAX_LEVEL %d used", v);
    t->max_level = v < 0 ? 0 : v;
    break;
  case GL_TEXTURE_MIN_LOD:
  case GL_TEXTURE_MAX_LOD:
    // gxm only has a lod bias (and a min lod we don't have a prototype for)
    if (pname == GL_TEXTURE_MIN_LOD ? v > 0 : v < 12)
      L_ONCE("tex: GL_TEXTURE_MIN/MAX_LOD (0x%X = %d) ignored", pname, v);
    return;
  case GL_TEXTURE_WRAP_R:
  case GL_TEXTURE_MAX_ANISOTROPY_EXT:
    return;
  case GL_TEXTURE_COMPARE_MODE:
  case GL_TEXTURE_COMPARE_FUNC:
    if (v) L_ONCE("tex: shadow compare mode not supported");
    return;
  default:
    if (pname >= GL_TEXTURE_SWIZZLE_R && pname <= GL_TEXTURE_SWIZZLE_A) {
      L_ONCE("tex: GL_TEXTURE_SWIZZLE_* ignored (0x%X = 0x%X)", pname, v);
      return;
    }
    L_ONCE("tex: glTexParameteri 0x%X = 0x%X ignored", pname, v);
    return;
  }
  if (t->mem) tex_desc(t);
}

SOFTFP void glTexParameterf(GLenum target, GLenum pname, GLfloat v)
{
  glTexParameteri(target, pname, (GLint)v);
}

// ============================================================ mipmap generation

// cpu box filter, level 0 of each face down to the end of the chain. reads
// level 0 back from gpu memory once per face, temps live only for the call.
void glGenerateMipmap(GLenum target)
{
  gl_texture *t = bound_tex(target);
  if (!t || !t->mem) { GL_ERR(GL_INVALID_OPERATION); return; }
  // no box filter on bc blocks: needs the pixels back
  if (fmt_bc(t->fmt) && tex_unbc(t, "glGenerateMipmap") < 0) { GL_ERR(GL_OUT_OF_MEMORY); return; }
  int kind = box_kind(t->fmt);
  if (!kind) {
    L_ONCE("tex: glGenerateMipmap on gxm format 0x%08X not supported", t->fmt);
    return;
  }
  if (!t->is_cube && (!tu_is_pow2(t->w) || !tu_is_pow2(t->h))) {
    L_ONCE("tex: glGenerateMipmap on npot %ux%u skipped", t->w, t->h);
    return;
  }
  // usually right after rendering into level 0: let that finish (ends the
  // scene if it's the open one) instead of copying half drawn pixels
  if (gxm_busy(t->last_use)) gxm_wait_scene(t->last_use);
  if (!t->is_cube) {
    unsigned n = chain_len(t->w, t->h);
    if (t->levels < n) {
      shape ns = make_shape(t->w, t->h, t->bpp, 0, n, 0);
      if (tex_realloc(t, &ns, NULL, 1, -1, -1) < 0) { GL_ERR(GL_OUT_OF_MEMORY); tex_desc(t); return; }
    }
  }
  if (t->levels < 2) { tex_desc(t); return; }
  unsigned bpp = t->bpp;
  uint8_t *a = malloc(t->w * t->h * bpp);
  uint8_t *b = malloc(lvl_dim(t->w, 1) * lvl_dim(t->h, 1) * bpp);
  if (!a || !b) {
    L("tex: out of memory generating mips for %ux%u", t->w, t->h);
    free(a);
    free(b);
    tex_desc(t);
    return;
  }
  shape s = shape_of(t);
  unsigned faces = t->is_cube ? 6 : 1;
  for (unsigned f = 0; f < faces; f++) {
    if (!(t->defined[f] & 1)) continue;
    const uint8_t *l0 = (const uint8_t *)t->mem->ptr + lvl_off(&s, f, 0);
    if (t->is_cube) {
      tu_unswizzle(l0, t->w, t->h, bpp, a, t->w * bpp);
    } else {
      for (unsigned y = 0; y < t->h; y++) memcpy(a + y * t->w * bpp, l0 + y * t->stride, t->w * bpp);
    }
    uint8_t *cur = a, *nxt = b;
    unsigned cw = t->w, ch = t->h;
    for (unsigned l = 1; l < t->levels; l++) {
      unsigned nw = lvl_dim(t->w, l), nh = lvl_dim(t->h, l);
      tu_box_down(kind, cur, cw, ch, cw * bpp, nxt, nw, nh, nw * bpp);
      put_rows(t, f, l, 0, 0, nw, nh, nxt, nw * bpp);
      uint8_t *tmp = cur; cur = nxt; nxt = tmp;
      cw = nw; ch = nh;
    }
    t->defined[f] |= (uint16_t)((1u << t->levels) - 1);
  }
  free(a);
  free(b);
  t->chain_guess = 0;
  tex_desc(t);
}
