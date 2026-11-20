// ETC1 / ETC2 / EAC decoder, written from the khronos spec. rgb(a) formats
// come out as RGBA8, R11/RG11 EAC as 8 bits per channel (signed ones as s8).
// only depends on gl_defs.h so it can be unit tested on the host.
#include <stdint.h>
#include <string.h>
#include "gles/gl_defs.h"

void etc_decode(GLenum fmt, const uint8_t *src, unsigned w, unsigned h,
                uint8_t *dst, unsigned dst_stride);
int etc_block_bytes(GLenum fmt);
int etc_has_alpha(GLenum fmt);
int etc_out_bpp(GLenum fmt);

static const int etc1_mod[8][2] = {
  { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 },
};

static const int etc2_dist[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };

static const int eac_mod[16][8] = {
  { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 },
  { -2, -5, -8, -13, 1, 4, 7, 12 }, { -2, -4, -6, -13, 1, 3, 5, 12 },
  { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 },
  { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },
  { -2, -6, -8, -10, 1, 5, 7, 9 }, { -2, -5, -8, -10, 1, 4, 7, 9 },
  { -2, -4, -8, -10, 1, 3, 7, 9 }, { -2, -5, -7, -10, 1, 4, 6, 9 },
  { -3, -4, -7, -10, 2, 3, 6, 9 }, { -1, -2, -3, -10, 0, 1, 2, 9 },
  { -4, -6, -8, -9, 3, 5, 7, 8 }, { -3, -5, -7, -9, 2, 4, 6, 8 },
};

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int ext4(int v) { return (v << 4) | v; }
static inline int ext5(int v) { return (v << 3) | (v >> 2); }
static inline int ext6(int v) { return (v << 2) | (v >> 4); }
static inline int ext7(int v) { return (v << 1) | (v >> 6); }

static uint64_t be64(const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

#define BITS(v, hi, lo) ((int)(((v) >> (lo)) & ((1ull << ((hi) - (lo) + 1)) - 1)))

// out[y][x][rgba] for one 4x4 block. etc2 = allow T/H/planar,
// punch = punchthrough alpha variant (diff bit is the opaque bit)
static void decode_rgb_block(const uint8_t *b, uint8_t out[4][4][4], int etc2, int punch)
{
  uint64_t v = be64(b);
  int diff = BITS(v, 33, 33);
  int flip = BITS(v, 32, 32);
  int opaque = punch ? diff : 1;
  uint32_t idx = (uint32_t)v;
  int base[2][3];
  int cw[2] = { BITS(v, 39, 37), BITS(v, 36, 34) };

  if (punch) diff = 1;   // punchthrough has no individual mode

  if (!diff) {
    base[0][0] = ext4(BITS(v, 63, 60)); base[1][0] = ext4(BITS(v, 59, 56));
    base[0][1] = ext4(BITS(v, 55, 52)); base[1][1] = ext4(BITS(v, 51, 48));
    base[0][2] = ext4(BITS(v, 47, 44)); base[1][2] = ext4(BITS(v, 43, 40));
  } else {
    int r = BITS(v, 63, 59), g = BITS(v, 55, 51), bb = BITS(v, 47, 43);
    int dr = BITS(v, 58, 56), dg = BITS(v, 50, 48), db = BITS(v, 42, 40);
    if (dr >= 4) dr -= 8;
    if (dg >= 4) dg -= 8;
    if (db >= 4) db -= 8;
    int r2 = r + dr, g2 = g + dg, b2 = bb + db;

    if (etc2 && (r2 < 0 || r2 > 31)) {
      // T mode
      int c[2][3];
      c[0][0] = ext4((BITS(v, 60, 59) << 2) | BITS(v, 57, 56));
      c[0][1] = ext4(BITS(v, 55, 52));
      c[0][2] = ext4(BITS(v, 51, 48));
      c[1][0] = ext4(BITS(v, 47, 44));
      c[1][1] = ext4(BITS(v, 43, 40));
      c[1][2] = ext4(BITS(v, 39, 36));
      int d = etc2_dist[(BITS(v, 35, 34) << 1) | BITS(v, 32, 32)];
      int pc[4][3];
      for (int k = 0; k < 3; k++) {
        pc[0][k] = c[0][k];
        pc[1][k] = clamp255(c[1][k] + d);
        pc[2][k] = c[1][k];
        pc[3][k] = clamp255(c[1][k] - d);
      }
      for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
          int i = x * 4 + y;
          int pi = (((idx >> (i + 16)) & 1) << 1) | ((idx >> i) & 1);
          uint8_t *o = out[y][x];
          if (!opaque && pi == 2) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
          o[0] = pc[pi][0]; o[1] = pc[pi][1]; o[2] = pc[pi][2]; o[3] = 255;
        }
      return;
    }
    if (etc2 && (g2 < 0 || g2 > 31)) {
      // H mode
      int c[2][3];
      c[0][0] = ext4(BITS(v, 62, 59));
      c[0][1] = ext4((BITS(v, 58, 56) << 1) | BITS(v, 52, 52));
      c[0][2] = ext4((BITS(v, 51, 51) << 3) | BITS(v, 49, 47));
      c[1][0] = ext4(BITS(v, 46, 43));
      c[1][1] = ext4(BITS(v, 42, 39));
      c[1][2] = ext4(BITS(v, 38, 35));
      int v0 = (c[0][0] << 16) | (c[0][1] << 8) | c[0][2];
      int v1 = (c[1][0] << 16) | (c[1][1] << 8) | c[1][2];
      int d = etc2_dist[(BITS(v, 34, 34) << 2) | (BITS(v, 32, 32) << 1) | (v0 >= v1)];
      int pc[4][3];
      for (int k = 0; k < 3; k++) {
        pc[0][k] = clamp255(c[0][k] + d);
        pc[1][k] = clamp255(c[0][k] - d);
        pc[2][k] = clamp255(c[1][k] + d);
        pc[3][k] = clamp255(c[1][k] - d);
      }
      for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
          int i = x * 4 + y;
          int pi = (((idx >> (i + 16)) & 1) << 1) | ((idx >> i) & 1);
          uint8_t *o = out[y][x];
          if (!opaque && pi == 2) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
          o[0] = pc[pi][0]; o[1] = pc[pi][1]; o[2] = pc[pi][2]; o[3] = 255;
        }
      return;
    }
    if (etc2 && (b2 < 0 || b2 > 31)) {
      // planar mode, always opaque
      int ro = ext6(BITS(v, 62, 57));
      int go = ext7((BITS(v, 56, 56) << 6) | BITS(v, 54, 49));
      int bo = ext6((BITS(v, 48, 48) << 5) | (BITS(v, 44, 43) << 3) | BITS(v, 41, 39));
      int rh = ext6((BITS(v, 38, 34) << 1) | BITS(v, 32, 32));
      int gh = ext7(BITS(v, 31, 25));
      int bh = ext6(BITS(v, 24, 19));
      int rv = ext6(BITS(v, 18, 13));
      int gv = ext7(BITS(v, 12, 6));
      int bv = ext6(BITS(v, 5, 0));
      for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
          uint8_t *o = out[y][x];
          o[0] = clamp255((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2);
          o[1] = clamp255((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2);
          o[2] = clamp255((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
          o[3] = 255;
        }
      return;
    }
    base[0][0] = ext5(r);  base[1][0] = ext5(r2);
    base[0][1] = ext5(g);  base[1][1] = ext5(g2);
    base[0][2] = ext5(bb); base[1][2] = ext5(b2);
  }

  // etc1 style individual / differential
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++) {
      int sub = flip ? (y >= 2) : (x >= 2);
      int i = x * 4 + y;
      int msb = (idx >> (i + 16)) & 1, lsb = (idx >> i) & 1;
      int m;
      uint8_t *o = out[y][x];
      if (!opaque) {
        // punchthrough: 0 -> base, 1 -> +large, 2 -> transparent, 3 -> -large
        if (msb && !lsb) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        m = lsb ? etc1_mod[cw[sub]][1] : 0;
        if (msb) m = -m;
      } else {
        m = etc1_mod[cw[sub]][lsb];
        if (msb) m = -m;
      }
      o[0] = clamp255(base[sub][0] + m);
      o[1] = clamp255(base[sub][1] + m);
      o[2] = clamp255(base[sub][2] + m);
      o[3] = 255;
    }
}

static void decode_eac_alpha(const uint8_t *b, uint8_t out[4][4][4])
{
  uint64_t v = be64(b);
  int base = BITS(v, 63, 56);
  int mul = BITS(v, 55, 52);
  const int *tab = eac_mod[BITS(v, 51, 48)];
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++) {
      int i = x * 4 + y;
      int ai = (int)((v >> (45 - 3 * i)) & 7);
      out[y][x][3] = clamp255(base + tab[ai] * mul);
    }
}

// R11/RG11 EAC channel c, squeezed to 8 bits (signed -> s8)
static void decode_r11(const uint8_t *b, uint8_t out[4][4][2], int c, int sgn)
{
  uint64_t v = be64(b);
  int base = BITS(v, 63, 56);
  int mul = BITS(v, 55, 52);
  const int *tab = eac_mod[BITS(v, 51, 48)];
  if (sgn) {
    base = (int8_t)base;
    if (base == -128) base = -127;
  }
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++) {
      int i = x * 4 + y;
      int m = tab[(v >> (45 - 3 * i)) & 7];
      int d = mul ? m * mul * 8 : m;   // multiplier 0 counts as 1/8
      if (sgn) {
        int r = base * 8 + d;
        r = r < -1023 ? -1023 : r > 1023 ? 1023 : r;
        int q = (r * 127 + (r < 0 ? -511 : 511)) / 1023;
        out[y][x][c] = (uint8_t)(int8_t)q;
      } else {
        int r = base * 8 + 4 + d;
        r = r < 0 ? 0 : r > 2047 ? 2047 : r;
        out[y][x][c] = (uint8_t)((r * 255 + 1023) / 2047);
      }
    }
}

int etc_block_bytes(GLenum fmt)
{
  switch (fmt) {
  case GL_COMPRESSED_R11_EAC:
  case GL_COMPRESSED_SIGNED_R11_EAC:
    return 8;
  case GL_COMPRESSED_RG11_EAC:
  case GL_COMPRESSED_SIGNED_RG11_EAC:
    return 16;
  case GL_ETC1_RGB8_OES:
  case GL_COMPRESSED_RGB8_ETC2:
  case GL_COMPRESSED_SRGB8_ETC2:
  case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
  case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
    return 8;
  case GL_COMPRESSED_RGBA8_ETC2_EAC:
  case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
    return 16;
  default:
    return 0;
  }
}

int etc_has_alpha(GLenum fmt)
{
  return fmt == GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 ||
         fmt == GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 ||
         fmt == GL_COMPRESSED_RGBA8_ETC2_EAC || fmt == GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC;
}

int etc_out_bpp(GLenum fmt)
{
  switch (fmt) {
  case GL_COMPRESSED_R11_EAC:
  case GL_COMPRESSED_SIGNED_R11_EAC:
    return 1;
  case GL_COMPRESSED_RG11_EAC:
  case GL_COMPRESSED_SIGNED_RG11_EAC:
    return 2;
  default:
    return etc_block_bytes(fmt) ? 4 : 0;
  }
}

void etc_decode(GLenum fmt, const uint8_t *src, unsigned w, unsigned h,
                uint8_t *dst, unsigned dst_stride)
{
  int bs = etc_block_bytes(fmt);
  if (!bs) return;
  int ch = etc_out_bpp(fmt);
  if (ch < 4) {
    int sgn = fmt == GL_COMPRESSED_SIGNED_R11_EAC || fmt == GL_COMPRESSED_SIGNED_RG11_EAC;
    unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[4][4][2];
    for (unsigned by = 0; by < bh; by++)
      for (unsigned bx = 0; bx < bw; bx++) {
        const uint8_t *b = src + (by * bw + bx) * bs;
        decode_r11(b, px, 0, sgn);
        if (ch == 2) decode_r11(b + 8, px, 1, sgn);
        unsigned n = w - bx * 4 < 4 ? w - bx * 4 : 4;
        for (unsigned y = 0; y < 4 && by * 4 + y < h; y++) {
          uint8_t *row = dst + (by * 4 + y) * dst_stride + bx * 4 * ch;
          for (unsigned x = 0; x < n; x++)
            for (int c = 0; c < ch; c++) row[x * ch + c] = px[y][x][c];
        }
      }
    return;
  }
  int etc2 = fmt != GL_ETC1_RGB8_OES;
  int punch = fmt == GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 ||
              fmt == GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2;
  int eac = bs == 16;
  unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
  uint8_t px[4][4][4];
  for (unsigned by = 0; by < bh; by++) {
    for (unsigned bx = 0; bx < bw; bx++) {
      const uint8_t *b = src + (by * bw + bx) * bs;
      decode_rgb_block(eac ? b + 8 : b, px, etc2, punch);
      if (eac) decode_eac_alpha(b, px);
      for (unsigned y = 0; y < 4 && by * 4 + y < h; y++) {
        uint8_t *row = dst + (by * 4 + y) * dst_stride + bx * 16;
        unsigned n = w - bx * 4 < 4 ? w - bx * 4 : 4;
        memcpy(row, px[y], n * 4);
      }
    }
  }
}
