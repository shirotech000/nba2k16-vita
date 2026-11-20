// cpu texture helpers, see texutil.h. no gxm/gl deps (host tested).
// layout rules below come from reading vita3k's texture decoder, so they
// match what the emulator expects. the code is our own.
#include <string.h>
#include "gles/texutil.h"

// ---------------------------------------------------------------- swizzle

static inline uint32_t spread(uint32_t v)
{
  v &= 0xFFFF;
  v = (v | (v << 8)) & 0x00FF00FFu;
  v = (v | (v << 4)) & 0x0F0F0F0Fu;
  v = (v | (v << 2)) & 0x33333333u;
  v = (v | (v << 1)) & 0x55555555u;
  return v;
}

unsigned tu_log2(unsigned v)
{
  unsigned r = 0;
  while (v > 1) { v >>= 1; r++; }
  return r;
}

unsigned tu_pow2_ceil(unsigned v)
{
  unsigned p = 1;
  while (p < v) p <<= 1;
  return p;
}

uint32_t tu_morton(unsigned x, unsigned y, unsigned w, unsigned h)
{
  unsigned m = w < h ? w : h;
  unsigned k = tu_log2(m);
  uint32_t lo = (spread(x & (m - 1)) << 1) | spread(y & (m - 1));
  return lo | ((uint32_t)((x >> k) | (y >> k)) << (2 * k));
}

void tu_swizzle_rect(uint8_t *level, unsigned lw, unsigned lh, unsigned bpp,
                     unsigned x0, unsigned y0, unsigned w, unsigned h,
                     const uint8_t *src, unsigned src_stride)
{
  for (unsigned y = 0; y < h; y++) {
    const uint8_t *s = src + y * src_stride;
    for (unsigned x = 0; x < w; x++, s += bpp) {
      uint8_t *d = level + tu_morton(x0 + x, y0 + y, lw, lh) * bpp;
      switch (bpp) {
      case 4: { uint32_t v; memcpy(&v, s, 4); memcpy(d, &v, 4); break; }
      case 2: { uint16_t v; memcpy(&v, s, 2); memcpy(d, &v, 2); break; }
      case 1: *d = *s; break;
      default: memcpy(d, s, bpp); break;
      }
    }
  }
}

void tu_unswizzle(const uint8_t *level, unsigned lw, unsigned lh, unsigned bpp,
                  uint8_t *dst, unsigned dst_stride)
{
  for (unsigned y = 0; y < lh; y++)
    for (unsigned x = 0; x < lw; x++)
      memcpy(dst + y * dst_stride + x * bpp, level + tu_morton(x, y, lw, lh) * bpp, bpp);
}

// ---------------------------------------------------------------- layout

uint32_t tu_linear_level_size(unsigned w, unsigned h, unsigned bpp)
{
  return ((w + 7u) & ~7u) * h * bpp;
}

uint32_t tu_cube_face_size(unsigned size, unsigned bpp)
{
  uint32_t bytes = 0;
  for (unsigned s = size; s; s >>= 1) bytes += s * s * bpp;
  // faces with a mip chain start 2 KiB aligned once they're big enough
  unsigned bits = bpp * 8;
  int big = (bits <= 8 && size >= 32) || ((bits == 16 || bits == 32) && size >= 16) ||
            (bits == 64 && size >= 8);
  uint32_t a = big ? 2048 : 4;
  return (bytes + a - 1) & ~(a - 1);
}

// ---------------------------------------------------------------- box filter

typedef struct { uint8_t shift, bits; } field;

static const field f565[3] = { { 11, 5 }, { 5, 6 }, { 0, 5 } };
static const field f4444[4] = { { 12, 4 }, { 8, 4 }, { 4, 4 }, { 0, 4 } };
static const field f5551[4] = { { 11, 5 }, { 6, 5 }, { 1, 5 }, { 0, 1 } };

static uint16_t avg16(uint16_t a, uint16_t b, uint16_t c, uint16_t d, const field *f, int n)
{
  uint16_t o = 0;
  for (int i = 0; i < n; i++) {
    unsigned m = (1u << f[i].bits) - 1, s = f[i].shift;
    unsigned v = ((a >> s) & m) + ((b >> s) & m) + ((c >> s) & m) + ((d >> s) & m);
    o |= (uint16_t)(((v + 2) >> 2) << s);
  }
  return o;
}

void tu_box_down(int kind, const uint8_t *src, unsigned sw, unsigned sh, unsigned sstride,
                 uint8_t *dst, unsigned dw, unsigned dh, unsigned dstride)
{
  unsigned bpp = kind == TU_BOX_8 ? 1 : kind == TU_BOX_8888 ? 4 : 2;
  for (unsigned y = 0; y < dh; y++) {
    unsigned y0 = y * 2 < sh ? y * 2 : sh - 1;
    unsigned y1 = y0 + 1 < sh ? y0 + 1 : y0;
    const uint8_t *r0 = src + y0 * sstride, *r1 = src + y1 * sstride;
    uint8_t *o = dst + y * dstride;
    for (unsigned x = 0; x < dw; x++, o += bpp) {
      unsigned x0 = x * 2 < sw ? x * 2 : sw - 1;
      unsigned x1 = x0 + 1 < sw ? x0 + 1 : x0;
      const uint8_t *p[4] = { r0 + x0 * bpp, r0 + x1 * bpp, r1 + x0 * bpp, r1 + x1 * bpp };
      if (kind == TU_BOX_8 || kind == TU_BOX_88 || kind == TU_BOX_8888) {
        for (unsigned c = 0; c < bpp; c++)
          o[c] = (uint8_t)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) >> 2);
        continue;
      }
      uint16_t v[4];
      for (int i = 0; i < 4; i++) memcpy(&v[i], p[i], 2);
      uint16_t r = kind == TU_BOX_565 ? avg16(v[0], v[1], v[2], v[3], f565, 3)
                 : kind == TU_BOX_4444 ? avg16(v[0], v[1], v[2], v[3], f4444, 4)
                 : avg16(v[0], v[1], v[2], v[3], f5551, 4);
      memcpy(o, &r, 2);
    }
  }
}

void tu_resample_nearest(const uint8_t *src, unsigned sw, unsigned sh, unsigned sstride,
                         uint8_t *dst, unsigned dw, unsigned dh, unsigned dstride, unsigned bpp)
{
  for (unsigned y = 0; y < dh; y++) {
    const uint8_t *r = src + (y * sh / dh) * sstride;
    for (unsigned x = 0; x < dw; x++)
      memcpy(dst + y * dstride + x * bpp, r + (x * sw / dw) * bpp, bpp);
  }
}
