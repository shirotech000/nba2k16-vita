// host tests for the texture cpu code: etc/eac decode, gxm swizzle order,
// layout sizes, mip box filter, bc1/bc3 encoder + the etc -> bc transcode.
// build: gcc -Wall -O2 -Isrc tools/tex_test.c src/gles/etc.c src/gles/texutil.c src/gles/bcenc.c -o tex_test
// the etc vectors are hand built from the khronos spec bit layouts, the
// expected colors worked out by hand (see the comments per block).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gles/gl_defs.h"
#include "gles/texutil.h"
#include "gles/bcenc.h"

void etc_decode(GLenum fmt, const uint8_t *src, unsigned w, unsigned h, uint8_t *dst, unsigned dst_stride);
int etc_out_bpp(GLenum fmt);

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
  printf(__VA_ARGS__); printf("\n"); } } while (0)

static void px_eq(const uint8_t *img, unsigned x, unsigned y, int r, int g, int b, int a, const char *what)
{
  const uint8_t *p = img + (y * 4 + x) * 4;
  CHECK(p[0] == r && p[1] == g && p[2] == b && p[3] == a, "%s (%u,%u) = %d,%d,%d,%d want %d,%d,%d,%d",
        what, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
}

static void test_etc1_individual(void)
{
  // R1=A R2=5 G1=3 G2=C B1=0 B2=F, table 0 (2,8) / 7 (47,183), diff 0 flip 0.
  // msb plane 0x800C, lsb plane 0x810A (pixel i = x*4+y)
  static const uint8_t blk[8] = { 0xA5, 0x3C, 0x0F, 0x1C, 0x80, 0x0C, 0x81, 0x0A };
  uint8_t img[4 * 4 * 4];
  etc_decode(GL_ETC1_RGB8_OES, blk, 4, 4, img, 16);
  px_eq(img, 0, 0, 172, 53, 2, 255, "etc1 ind +a");
  px_eq(img, 0, 1, 178, 59, 8, 255, "etc1 ind +b");
  px_eq(img, 0, 2, 168, 49, 0, 255, "etc1 ind -a");
  px_eq(img, 0, 3, 162, 43, 0, 255, "etc1 ind -b");
  px_eq(img, 1, 0, 172, 53, 2, 255, "etc1 ind sub0");
  px_eq(img, 2, 0, 255, 255, 255, 255, "etc1 ind sub1 +183");
  px_eq(img, 3, 3, 0, 21, 72, 255, "etc1 ind sub1 -183");
  px_eq(img, 2, 1, 132, 251, 255, 255, "etc1 ind sub1 +47");
}

static void test_etc1_diff_flip(void)
{
  // R=10 dR=+3, G=20 dG=-4, B=31 dB=-1 -> bases (82,165,255) / (107,132,247),
  // tables 1 (5,17) / 2 (9,29), diff 1 flip 1 (top/bottom halves)
  static const uint8_t blk[8] = { 0x53, 0xA4, 0xFF, 0x2B, 0x80, 0x00, 0x00, 0x04 };
  uint8_t img[64];
  GLenum fmts[2] = { GL_ETC1_RGB8_OES, GL_COMPRESSED_RGB8_ETC2 };
  for (int i = 0; i < 2; i++) {
    etc_decode(fmts[i], blk, 4, 4, img, 16);
    px_eq(img, 0, 0, 87, 170, 255, 255, "etc diff top +5");
    px_eq(img, 1, 1, 87, 170, 255, 255, "etc diff top +5 (1,1)");
    px_eq(img, 0, 2, 136, 161, 255, 255, "etc diff bottom +29");
    px_eq(img, 3, 3, 98, 123, 238, 255, "etc diff bottom -9");
    px_eq(img, 2, 3, 116, 141, 255, 255, "etc diff bottom +9");
  }
}

static void test_etc2_modes(void)
{
  uint8_t img[64];
  // T mode: R underflows (R=2, dR=-3). C1=(9,3,C) C2=(8,8,8), dist idx 3 -> 16
  static const uint8_t t_blk[8] = { 0x15, 0x3C, 0x88, 0x87, 0x00, 0x0C, 0x00, 0x0A };
  etc_decode(GL_COMPRESSED_RGB8_ETC2, t_blk, 4, 4, img, 16);
  px_eq(img, 0, 0, 153, 51, 204, 255, "etc2 T p0");
  px_eq(img, 0, 1, 152, 152, 152, 255, "etc2 T p1");
  px_eq(img, 0, 2, 136, 136, 136, 255, "etc2 T p2");
  px_eq(img, 0, 3, 120, 120, 120, 255, "etc2 T p3");
  px_eq(img, 3, 3, 153, 51, 204, 255, "etc2 T p0 corner");

  // H mode: G underflows. C1=(0,2,0) C2=(F,0,0), v0 < v1, dist idx 4 -> 23
  static const uint8_t h_blk[8] = { 0x01, 0x04, 0x78, 0x06, 0x00, 0x0C, 0x00, 0x0A };
  etc_decode(GL_COMPRESSED_RGB8_ETC2, h_blk, 4, 4, img, 16);
  px_eq(img, 0, 0, 23, 57, 23, 255, "etc2 H p0");
  px_eq(img, 0, 1, 0, 11, 0, 255, "etc2 H p1");
  px_eq(img, 0, 2, 255, 23, 23, 255, "etc2 H p2");
  px_eq(img, 0, 3, 232, 0, 0, 255, "etc2 H p3");

  // planar: B underflows. RO=RH=63, RV=0, the rest 0 -> red fades down the rows
  static const uint8_t p_blk[8] = { 0x7E, 0x00, 0x04, 0x7F, 0x00, 0x00, 0x00, 0x00 };
  etc_decode(GL_COMPRESSED_RGB8_ETC2, p_blk, 4, 4, img, 16);
  int want[4] = { 255, 191, 128, 64 };
  for (unsigned y = 0; y < 4; y++)
    for (unsigned x = 0; x < 4; x++) px_eq(img, x, y, want[y], 0, 0, 255, "etc2 planar");
}

static void test_eac(void)
{
  uint8_t img[64];
  // alpha: base 128, mul 2, table 0. pixel 0 idx 7 (+14), pixel 1 idx 3 (-15), rest idx 0 (-3)
  static const uint8_t blk[16] = { 0x80, 0x20, 0xEC, 0, 0, 0, 0, 0,
                                   0x53, 0xA4, 0xFF, 0x2B, 0x80, 0x00, 0x00, 0x04 };
  etc_decode(GL_COMPRESSED_RGBA8_ETC2_EAC, blk, 4, 4, img, 16);
  px_eq(img, 0, 0, 87, 170, 255, 156, "eac alpha +28");
  px_eq(img, 0, 1, 87, 170, 255, 98, "eac alpha -30");
  px_eq(img, 1, 0, 87, 170, 255, 122, "eac alpha -6");
  px_eq(img, 3, 3, 98, 123, 238, 122, "eac alpha -6 corner");

  // r11 unsigned: base 100 mul 3 table 0 -> 11 bit 1140 / 444 / 732
  CHECK(etc_out_bpp(GL_COMPRESSED_R11_EAC) == 1 && etc_out_bpp(GL_COMPRESSED_RG11_EAC) == 2, "eac bpp");
  static const uint8_t r11[8] = { 0x64, 0x30, 0xEC, 0, 0, 0, 0, 0 };
  uint8_t r[16];
  etc_decode(GL_COMPRESSED_R11_EAC, r11, 4, 4, r, 4);
  CHECK(r[0] == 142 && r[4] == 55 && r[1] == 91 && r[15] == 91, "r11 got %d %d %d %d", r[0], r[4], r[1], r[15]);
  // mul 0 counts as 1/8: 804 + 14 = 818
  static const uint8_t r11z[8] = { 0x64, 0x00, 0xE0, 0, 0, 0, 0, 0 };
  etc_decode(GL_COMPRESSED_R11_EAC, r11z, 4, 4, r, 4);
  CHECK(r[0] == 102, "r11 mul0 got %d", r[0]);
  // signed: base -50 mul 2 -> -176 / -448 out of 1023
  static const uint8_t s11[8] = { 0xCE, 0x20, 0xE0, 0, 0, 0, 0, 0 };
  etc_decode(GL_COMPRESSED_SIGNED_R11_EAC, s11, 4, 4, r, 4);
  CHECK((int8_t)r[0] == -22 && (int8_t)r[1] == -56, "signed r11 got %d %d", (int8_t)r[0], (int8_t)r[1]);
  // rg11: second block is green
  uint8_t rg[32], rgblk[16];
  memcpy(rgblk, r11, 8);
  memcpy(rgblk + 8, r11z, 8);
  etc_decode(GL_COMPRESSED_RG11_EAC, rgblk, 4, 4, rg, 8);
  CHECK(rg[0] == 142 && rg[1] == 102, "rg11 got %d %d", rg[0], rg[1]);
}

static void test_swizzle(void)
{
  // absolute positions: y is bit 0, x is bit 1
  CHECK(tu_morton(0, 0, 2, 2) == 0, "morton 0,0");
  CHECK(tu_morton(0, 1, 2, 2) == 1, "morton x0 y1 -> 1, got %u", tu_morton(0, 1, 2, 2));
  CHECK(tu_morton(1, 0, 2, 2) == 2, "morton x1 y0 -> 2, got %u", tu_morton(1, 0, 2, 2));
  CHECK(tu_morton(1, 1, 2, 2) == 3, "morton 1,1");
  CHECK(tu_morton(2, 1, 4, 4) == 9, "morton 2,1 in 4x4 -> 9, got %u", tu_morton(2, 1, 4, 4));
  // non-square: the long axis' extra bits go on top
  CHECK(tu_morton(5, 1, 8, 2) == 11, "morton 5,1 in 8x2 -> 11, got %u", tu_morton(5, 1, 8, 2));
  CHECK(tu_morton(1, 5, 2, 8) == 11, "morton 1,5 in 2x8 -> 11, got %u", tu_morton(1, 5, 2, 8));
  CHECK(tu_morton(0, 0, 1, 1) == 0, "morton 1x1");

  // every texel lands somewhere unique, and unswizzle undoes swizzle
  unsigned W = 16, H = 4;
  uint32_t src[16 * 4], sw[16 * 4], back[16 * 4];
  for (unsigned i = 0; i < W * H; i++) src[i] = 0xA0000000u + i;
  memset(sw, 0, sizeof(sw));
  tu_swizzle_rect((uint8_t *)sw, W, H, 4, 0, 0, W, H, (const uint8_t *)src, W * 4);
  int seen[64] = { 0 };
  for (unsigned i = 0; i < W * H; i++) if (sw[i] >= 0xA0000000u && sw[i] - 0xA0000000u < 64) seen[sw[i] - 0xA0000000u]++;
  int ok = 1;
  for (unsigned i = 0; i < W * H; i++) ok &= seen[i] == 1;
  CHECK(ok, "swizzle is a permutation");
  tu_unswizzle((const uint8_t *)sw, W, H, 4, (uint8_t *)back, W * 4);
  CHECK(!memcmp(src, back, sizeof(src)), "unswizzle(swizzle) round trip");
  // sub rect write lands at the right spots
  uint32_t one = 0x12345678u;
  tu_swizzle_rect((uint8_t *)sw, W, H, 4, 3, 2, 1, 1, (const uint8_t *)&one, 4);
  CHECK(sw[tu_morton(3, 2, W, H)] == one, "subrect write");
}

static void test_layout(void)
{
  CHECK(tu_linear_level_size(3, 2, 4) == 64, "linear 3x2 rgba8 = %u", tu_linear_level_size(3, 2, 4));
  CHECK(tu_linear_level_size(16, 16, 4) == 1024, "linear 16x16");
  CHECK(tu_linear_level_size(1, 1, 1) == 8, "linear 1x1 l8");
  CHECK(tu_cube_face_size(1, 4) == 4, "cube 1 rgba8 = %u", tu_cube_face_size(1, 4));
  CHECK(tu_cube_face_size(8, 4) == 340, "cube 8 rgba8 = %u", tu_cube_face_size(8, 4));
  CHECK(tu_cube_face_size(16, 4) == 2048, "cube 16 rgba8 = %u", tu_cube_face_size(16, 4));
  CHECK(tu_cube_face_size(128, 4) == 88064, "cube 128 rgba8 = %u", tu_cube_face_size(128, 4));
  CHECK(tu_cube_face_size(32, 1) == 2048, "cube 32 l8 = %u", tu_cube_face_size(32, 1));
  CHECK(tu_cube_face_size(16, 1) == 344, "cube 16 l8 = %u", tu_cube_face_size(16, 1));
  CHECK(tu_cube_face_size(8, 8) == 2048, "cube 8 rgba16f = %u", tu_cube_face_size(8, 8));
  CHECK(tu_log2(1) == 0 && tu_log2(4096) == 12 && tu_log2(96) == 6, "log2");
  CHECK(tu_pow2_ceil(96) == 128 && tu_pow2_ceil(64) == 64 && tu_pow2_ceil(1) == 1, "pow2 ceil");
}

static void test_box(void)
{
  uint8_t s8888[16] = { 0, 10, 20, 30, 4, 10, 20, 30, 0, 10, 20, 30, 4, 11, 20, 33 };
  uint8_t d[4];
  tu_box_down(TU_BOX_8888, s8888, 2, 2, 8, d, 1, 1, 4);
  CHECK(d[0] == 2 && d[1] == 10 && d[2] == 20 && d[3] == 31, "box 8888 got %d %d %d %d", d[0], d[1], d[2], d[3]);

  uint16_t s565[4] = { 0xFFFF, 0xFFFF, 0, 0 }, o;
  tu_box_down(TU_BOX_565, (uint8_t *)s565, 2, 2, 4, (uint8_t *)&o, 1, 1, 2);
  CHECK(o == 0x8410, "box 565 got 0x%04X", o);

  uint16_t s5551[4] = { 0x0001, 0x0001, 0x0001, 0x0000 };   // alpha 3 of 4
  tu_box_down(TU_BOX_5551, (uint8_t *)s5551, 2, 2, 4, (uint8_t *)&o, 1, 1, 2);
  CHECK(o == 0x0001, "box 5551 alpha majority got 0x%04X", o);
  uint16_t s4444[4] = { 0xF000, 0xF000, 0x0000, 0x0000 };
  tu_box_down(TU_BOX_4444, (uint8_t *)s4444, 2, 2, 4, (uint8_t *)&o, 1, 1, 2);
  CHECK(o == 0x8000, "box 4444 got 0x%04X", o);

  // odd width: 3x1 -> 1x1 averages columns 0,1 only (row repeats)
  uint8_t s8[3] = { 10, 20, 200 }, d8;
  tu_box_down(TU_BOX_8, s8, 3, 1, 3, &d8, 1, 1, 1);
  CHECK(d8 == 15, "box 8 odd got %d", d8);
  // la8: two channels averaged on their own
  uint8_t s88[8] = { 0, 255, 100, 255, 0, 255, 100, 251 }, d88[2];
  tu_box_down(TU_BOX_88, s88, 2, 2, 4, d88, 1, 1, 2);
  CHECK(d88[0] == 50 && d88[1] == 254, "box 88 got %d %d", d88[0], d88[1]);

  // nearest resample 3x1 -> 4x1
  uint8_t r3[3] = { 1, 2, 3 }, r4[4];
  tu_resample_nearest(r3, 3, 1, 3, r4, 4, 1, 4, 1);
  CHECK(r4[0] == 1 && r4[1] == 1 && r4[2] == 2 && r4[3] == 3, "resample got %d %d %d %d", r4[0], r4[1], r4[2], r4[3]);
}

// ================================================================ bc

typedef struct { double psnr; int max_rgb, max_a; } bc_err;

// 10 * log10(x) without libm, so the test links with a bare gcc line
static double to_db(double x)
{
  double db = 0;
  while (x >= 10) { x /= 10; db += 10; }
  while (x < 1) { x *= 10; db -= 10; }
  // ln x = 2 atanh((x - 1) / (x + 1))
  double y = (x - 1) / (x + 1), y2 = y * y, t = y, ln = 0;
  for (int k = 1; k < 200; k += 2) { ln += t / k; t *= y2; }
  return db + 2 * ln * 4.342944819032518;
}

// a vs b, both rgba8 w x h with the same stride
static bc_err img_err(const uint8_t *a, const uint8_t *b, unsigned w, unsigned h, unsigned stride)
{
  bc_err e = { 0, 0, 0 };
  double sse = 0;
  for (unsigned y = 0; y < h; y++)
    for (unsigned x = 0; x < w; x++)
      for (int c = 0; c < 4; c++) {
        int d = a[y * stride + x * 4 + c] - b[y * stride + x * 4 + c];
        if (d < 0) d = -d;
        if (c < 3) { sse += d * d; if (d > e.max_rgb) e.max_rgb = d; }
        else if (d > e.max_a) e.max_a = d;
      }
  e.psnr = sse > 0 ? to_db(255.0 * 255.0 * w * h * 3 / sse) : 99;
  return e;
}

// encode + decode an image block by block (w, h multiples of 4)
static void bc_round_trip(unsigned bs, int punch, const uint8_t *src, unsigned w, unsigned h, uint8_t *dst)
{
  for (unsigned y = 0; y < h; y += 4)
    for (unsigned x = 0; x < w; x += 4) {
      uint8_t blk[16];
      if (bs == 16) bc3_encode(src + (y * w + x) * 4, w * 4, blk);
      else bc1_encode(src + (y * w + x) * 4, w * 4, blk, punch);
      bc_decode_block(bs, blk, dst + (y * w + x) * 4, w * 4);
    }
}

// worst difference between the 4x4 block averages of two images. this is
// what banding looks like in numbers.
static double block_mean_err(const uint8_t *a, const uint8_t *b, unsigned w, unsigned h)
{
  double worst = 0;
  for (unsigned y = 0; y < h; y += 4)
    for (unsigned x = 0; x < w; x += 4)
      for (int c = 0; c < 3; c++) {
        int sa = 0, sb = 0;
        for (unsigned j = 0; j < 4; j++)
          for (unsigned i = 0; i < 4; i++) {
            sa += a[((y + j) * w + x + i) * 4 + c];
            sb += b[((y + j) * w + x + i) * 4 + c];
          }
        double d = (sa > sb ? sa - sb : sb - sa) / 16.0;
        if (d > worst) worst = d;
      }
  return worst;
}

static unsigned rnd_state = 0x1234567u;
static unsigned rnd(void)
{
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return rnd_state >> 8;
}

static void test_bc_decode(void)
{
  // hand built blocks. c0 = pure red (F800), c1 = pure blue (001F), 4 colour
  // mode, indices 0 1 2 3 along the first row
  static const uint8_t b1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00 };
  uint8_t px[64];
  bc_decode_block(8, b1, px, 16);
  CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255, "bc1 idx0");
  CHECK(px[4] == 0 && px[6] == 255, "bc1 idx1");
  CHECK(px[8] == 170 && px[10] == 85, "bc1 idx2 got %d %d", px[8], px[10]);
  CHECK(px[12] == 85 && px[14] == 170 && px[15] == 255, "bc1 idx3");
  // c0 < c1: 3 colour mode, index 3 is transparent black
  static const uint8_t b2[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0x00, 0x00, 0x00 };
  bc_decode_block(8, b2, px, 16);
  CHECK(px[8] == 128 && px[10] == 128 && px[11] == 255, "bc1 3 colour mid got %d %d", px[8], px[10]);
  CHECK(px[12] == 0 && px[14] == 0 && px[15] == 0, "bc1 transparent");
  // bc3: alpha 255 / 0 in 8 value mode, texel 1 = index 2 (6/7), texel 2 = index 7 (1/7).
  // same colour block, but bc3 never uses the 3 colour mode
  static const uint8_t b3[16] = { 0xFF, 0x00, 0xD0, 0x01, 0, 0, 0, 0, 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0 };
  bc_decode_block(16, b3, px, 16);
  CHECK(px[3] == 255 && px[7] == 219 && px[11] == 36, "bc3 alpha got %d %d %d", px[3], px[7], px[11]);
  CHECK(px[12] == 170 && px[14] == 85, "bc3 colour idx3 is a third, got %d %d", px[12], px[14]);
  // 6 value mode: a0 <= a1, index 6 = 0, index 7 = 255
  static const uint8_t b4[16] = { 0x40, 0x80, 0x3E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  bc_decode_block(16, b4, px, 16);
  CHECK(px[3] == 0 && px[7] == 255 && px[11] == 0x40, "bc3 6 value mode got %d %d %d", px[3], px[7], px[11]);
}

static void test_bc_encode(void)
{
  enum { W = 64, H = 64 };
  static uint8_t src[W * H * 4], dst[W * H * 4];
  bc_err e;

  // flat blocks, every grey + a sweep of colours. the worst case decides how
  // visible banding gets on very slow gradients
  int worst = 0;
  for (int v = 0; v < 256 * 3; v++) {
    uint8_t blk[16], in[64], out[64];
    int r = v < 256 ? v : (v * 7) & 255, g = v < 256 ? v : (v * 13 + 5) & 255, b = v < 256 ? v : (v * 29 + 11) & 255;
    for (int i = 0; i < 16; i++) { in[i * 4] = r; in[i * 4 + 1] = g; in[i * 4 + 2] = b; in[i * 4 + 3] = 255; }
    bc1_encode(in, 16, blk, 0);
    bc_decode_block(8, blk, out, 16);
    e = img_err(in, out, 4, 4, 16);
    if (e.max_rgb > worst) worst = e.max_rgb;
    // opaque blocks must read the same as dxt1 with and without alpha
    unsigned c0 = blk[0] | blk[1] << 8, c1 = blk[2] | blk[3] << 8;
    if (c0 < c1 || e.max_a) worst = 999;
  }
  printf("bc1 flat colours: worst error %d\n", worst);
  CHECK(worst <= 2, "bc1 flat colour error %d", worst);

  // skin-ish gradients, from 1 level per 16 texels up to 2 levels per texel
  static const int slope16[5] = { 1, 4, 8, 16, 32 };
  for (int k = 0; k < 5; k++) {
    for (unsigned y = 0; y < H; y++)
      for (unsigned x = 0; x < W; x++) {
        uint8_t *p = src + (y * W + x) * 4;
        int t = (int)(x * slope16[k]) / 16, u = (int)(y * slope16[k]) / 32;
        p[0] = (uint8_t)(120 + t); p[1] = (uint8_t)(90 + (t * 3) / 4 + u); p[2] = (uint8_t)(70 + t / 2 + u / 2);
        p[3] = 255;
      }
    bc_round_trip(8, 0, src, W, H, dst);
    e = img_err(src, dst, W, H, W * 4);
    double bm = block_mean_err(src, dst, W, H);
    printf("bc1 gradient %2d/16 per texel: psnr %.1f dB, max err %d, worst block mean off by %.2f\n",
           slope16[k], e.psnr, e.max_rgb, bm);
    CHECK(e.psnr >= 40.0 && e.max_rgb <= 6 && bm <= 2.0, "bc1 gradient %d: psnr %.1f max %d block mean %.2f",
          slope16[k], e.psnr, e.max_rgb, bm);
    bc_round_trip(16, 0, src, W, H, dst);
    bc_err e3 = img_err(src, dst, W, H, W * 4);
    CHECK(e3.max_a == 0 && e3.max_rgb == e.max_rgb, "bc3 on opaque gradient: alpha %d rgb %d", e3.max_a, e3.max_rgb);
  }

  // noisy gradient (grain on top of a ramp)
  for (unsigned i = 0; i < W * H; i++) {
    unsigned x = i % W, y = i / W;
    int n = (int)(rnd() % 9) - 4;
    src[i * 4] = (uint8_t)(100 + x + n); src[i * 4 + 1] = (uint8_t)(80 + y + n); src[i * 4 + 2] = (uint8_t)(60 + (x + y) / 2 + n);
    src[i * 4 + 3] = 255;
  }
  bc_round_trip(8, 0, src, W, H, dst);
  e = img_err(src, dst, W, H, W * 4);
  printf("bc1 noisy gradient: psnr %.1f dB, max err %d\n", e.psnr, e.max_rgb);
  CHECK(e.psnr >= 38.0 && e.max_rgb <= 10, "bc1 noisy gradient psnr %.1f max %d", e.psnr, e.max_rgb);

  // hard edges: two colours per block, every split position. two colours are
  // always on one line, so only 565 rounding is left
  static const uint8_t pairs[4][2][3] = {
    { { 0, 0, 0 }, { 255, 255, 255 } }, { { 255, 0, 0 }, { 0, 0, 255 } },
    { { 30, 200, 90 }, { 220, 40, 160 } }, { { 250, 240, 10 }, { 10, 20, 30 } },
  };
  int worst_edge = 0;
  for (int pi = 0; pi < 4; pi++)
    for (unsigned m = 1; m < 0xFFFF; m += 0x111) {
      uint8_t blk[8], in[64], out[64];
      for (int i = 0; i < 16; i++) {
        memcpy(in + i * 4, pairs[pi][m >> i & 1], 3);
        in[i * 4 + 3] = 255;
      }
      bc1_encode(in, 16, blk, 0);
      bc_decode_block(8, blk, out, 16);
      e = img_err(in, out, 4, 4, 16);
      if (e.max_rgb > worst_edge) worst_edge = e.max_rgb;
    }
  printf("bc1 hard edges: worst error %d\n", worst_edge);
  CHECK(worst_edge <= 7, "bc1 hard edge error %d", worst_edge);

  // alpha: smooth ramps of different steepness under a colour gradient
  for (int k = 0; k < 3; k++) {
    for (unsigned i = 0; i < W * H; i++) {
      unsigned x = i % W, y = i / W;
      src[i * 4] = (uint8_t)(40 + x * 2); src[i * 4 + 1] = (uint8_t)(200 - y); src[i * 4 + 2] = (uint8_t)(90 + x);
      int a = k == 0 ? (int)(x + y) : k == 1 ? (int)(x * 4) : (int)(x * 16) - 300;
      src[i * 4 + 3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    }
    bc_round_trip(16, 0, src, W, H, dst);
    e = img_err(src, dst, W, H, W * 4);
    printf("bc3 alpha ramp %d: alpha max err %d, rgb psnr %.1f dB\n", k, e.max_a, e.psnr);
    CHECK(e.max_a <= (k == 2 ? 6 : 1), "bc3 alpha ramp %d max err %d", k, e.max_a);
    CHECK(e.psnr >= 38.0, "bc3 colour under alpha psnr %.1f", e.psnr);
    // fully transparent / opaque texels stay exactly that (cutout edges)
    int bad = 0;
    for (unsigned i = 0; i < W * H; i++)
      if ((src[i * 4 + 3] == 0 || src[i * 4 + 3] == 255) && dst[i * 4 + 3] != src[i * 4 + 3]) bad++;
    CHECK(bad == 0, "bc3 alpha ramp %d: %d texels lost their exact 0 / 255", k, bad);
  }
  // alpha hard edge + flat alpha
  for (unsigned i = 0; i < W * H; i++) src[i * 4 + 3] = (i % W + i / W) % 7 < 3 ? 0 : (i / W) < 32 ? 255 : 77;
  bc_round_trip(16, 0, src, W, H, dst);
  e = img_err(src, dst, W, H, W * 4);
  CHECK(e.max_a == 0, "bc3 two level alpha max err %d", e.max_a);

  // punch through: a disc cut out of a gradient. alpha must come back as
  // exactly 0 / 255 and opaque colours stay close
  for (unsigned i = 0; i < W * H; i++) {
    int x = (int)(i % W) - 32, y = (int)(i / W) - 32;
    int in = x * x + y * y < 28 * 28;
    src[i * 4] = in ? (uint8_t)(128 + x * 2) : 0; src[i * 4 + 1] = in ? (uint8_t)(128 + y * 2) : 0;
    src[i * 4 + 2] = in ? 60 : 0; src[i * 4 + 3] = in ? 255 : 0;
  }
  bc_round_trip(8, 1, src, W, H, dst);
  e = img_err(src, dst, W, H, W * 4);
  printf("bc1 punch through: psnr %.1f dB, max err %d\n", e.psnr, e.max_rgb);
  CHECK(e.max_a == 0, "bc1 punch through alpha err %d", e.max_a);
  CHECK(e.psnr >= 38.0 && e.max_rgb <= 8, "bc1 punch through psnr %.1f max %d", e.psnr, e.max_rgb);
}

// ---- etc data builders

// etc1 differential block (delta 0), table t for both halves, mod[i] = 0..3
// (+small +large -small -large) for texel x = i & 3, y = i >> 2
static void etc1_block(uint8_t out[8], int r5, int g5, int b5, int t, const uint8_t mod[16])
{
  unsigned msb = 0, lsb = 0;
  for (int i = 0; i < 16; i++) {
    int bit = (i & 3) * 4 + (i >> 2);
    msb |= (unsigned)(mod[i] >> 1) << bit;
    lsb |= (unsigned)(mod[i] & 1) << bit;
  }
  out[0] = (uint8_t)(r5 << 3); out[1] = (uint8_t)(g5 << 3); out[2] = (uint8_t)(b5 << 3);
  out[3] = (uint8_t)(t << 5 | t << 2 | 2);
  out[4] = (uint8_t)(msb >> 8); out[5] = (uint8_t)msb; out[6] = (uint8_t)(lsb >> 8); out[7] = (uint8_t)lsb;
}

// crude etc1 encode of a smooth image: per block the 5 bit base nearest the
// average, table 0 (+-2, +-8), per texel the closest modifier. gives the kind
// of blocks real etc gradients are made of.
static void etc1_from_image(const uint8_t *rgba, unsigned w, unsigned h, uint8_t *etc)
{
  static const int mv[4] = { 2, 8, -2, -8 };
  for (unsigned by = 0; by < h / 4; by++)
    for (unsigned bx = 0; bx < w / 4; bx++) {
      int sum[3] = { 0, 0, 0 };
      for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) sum[c] += rgba[((by * 4 + (i >> 2)) * w + bx * 4 + (i & 3)) * 4 + c];
      int q[3], base[3];
      for (int c = 0; c < 3; c++) {
        q[c] = (sum[c] / 16 * 31 + 127) / 255;
        base[c] = (q[c] << 3) | (q[c] >> 2);
      }
      uint8_t mod[16];
      for (int i = 0; i < 16; i++) {
        const uint8_t *p = rgba + ((by * 4 + (i >> 2)) * w + bx * 4 + (i & 3)) * 4;
        int best = 1 << 30;
        mod[i] = 0;
        for (int m = 0; m < 4; m++) {
          int e = 0;
          for (int c = 0; c < 3; c++) { int d = p[c] - (base[c] + mv[m]); e += d * d; }
          if (e < best) { best = e; mod[i] = (uint8_t)m; }
        }
      }
      etc1_block(etc + (by * (w / 4) + bx) * 8, q[0], q[1], q[2], 0, mod);
    }
}

// ---- reading a swizzled bc chain back the way vita3k's texture cache does
// it (written from reading its source, on purpose not sharing code with
// tu_morton / bcenc.c): the level's blocks are numbered in z order, y in the
// even bits, x in the odd ones, leftover high bits on the longer axis. mips
// follow each other, each (width * height in pixels, rounded up to blocks) big.

static unsigned v3k_compact(unsigned x)
{
  x &= 0x55555555u;
  x = (x ^ (x >> 1)) & 0x33333333u;
  x = (x ^ (x >> 2)) & 0x0F0F0F0Fu;
  x = (x ^ (x >> 4)) & 0x00FF00FFu;
  x = (x ^ (x >> 8)) & 0x0000FFFFu;
  return x;
}

static unsigned v3k_pow2(unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; }

// level `mip` of a w x h texture (npot = SWIZZLED_ARBITRARY) -> rgba8, out has
// room for the level's whole pixel size
static void v3k_read(const uint8_t *data, unsigned w, unsigned h, unsigned mip, unsigned bs,
                     uint8_t *out, unsigned ostride)
{
  unsigned lw = v3k_pow2(w), lh = v3k_pow2(h), width = w, height = h;
  const uint8_t *pixels = data;
  for (unsigned m = 0; m < mip; m++) {
    pixels += (((lw + 3) & ~3u) * ((lh + 3) & ~3u) >> 4) * bs;
    lw /= 2; lh /= 2; width /= 2; height /= 2;
  }
  if (!width) width = 1;
  if (!height) height = 1;
  unsigned bcx = (v3k_pow2(width) + 3) / 4, bcy = (v3k_pow2(height) + 3) / 4;
  unsigned min = bcx < bcy ? bcx : bcy, k = 0;
  while ((1u << (k + 1)) <= min) k++;
  for (unsigned i = 0; i < bcx * bcy; i++) {
    unsigned x = v3k_compact(i >> 1) & (min - 1), y = v3k_compact(i) & (min - 1);
    unsigned upper = (i >> (2 * k)) << k;
    if (bcx >= bcy) x |= upper; else y |= upper;
    uint8_t px[64];
    bc_decode_block(bs, pixels + i * bs, px, 16);
    for (unsigned r = 0; r < 4 && y * 4 + r < height; r++)
      for (unsigned c = 0; c < 4 && x * 4 + c < width; c++)
        memcpy(out + (y * 4 + r) * ostride + (x * 4 + c) * 4, px + r * 16 + c * 4, 4);
  }
}

// every etc block flat in its own colour: a block in the wrong place shows up
// as a huge error. checks the whole chain of a w x h texture.
static void layout_case(GLenum fmt, unsigned w, unsigned h, unsigned levels)
{
  unsigned ebs = fmt == GL_COMPRESSED_RGBA8_ETC2_EAC ? 16 : 8, bs = bc_bytes_for_etc(fmt);
  uint32_t total = tu_bc_level_off(w, h, levels, bs);
  uint8_t *chain = malloc(total + 64), *tmp = malloc(((w + 3) & ~3u) * 16);
  uint8_t *etc = malloc(((w + 3) / 4) * ((h + 3) / 4) * ebs), *ref = malloc(w * h * 4), *got = malloc(w * h * 4);
  memset(chain, 0xEE, total + 64);
  int worst = 0, worst_a = 0;
  static const uint8_t flat[16] = { 0 };
  for (unsigned l = 0; l < levels; l++) {
    unsigned lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
    unsigned bw = (lw + 3) / 4, bh = (lh + 3) / 4;
    for (unsigned b = 0; b < bw * bh; b++) {
      uint8_t *e = etc + b * ebs;
      if (ebs == 16) {
        // eac alpha: base = block number, multiplier 0 -> flat
        e[0] = (uint8_t)(40 + b * 37 + l * 11); e[1] = 0;
        memset(e + 2, 0, 6);
        e += 8;
      }
      etc1_block(e, (int)((b * 7 + l * 3) % 31), (int)((b * 5 + 3) % 31), (int)((b * 11 + l) % 31), 0, flat);
    }
    etc_decode(fmt, etc, lw, lh, ref, lw * 4);
    bc_from_etc(fmt, etc, lw, lh, chain + tu_bc_level_off(w, h, l, bs), tu_bc_dim(w, l), tu_bc_dim(h, l), 0, 0, tmp);
    memset(got, 0x55, w * h * 4);
    v3k_read(chain, w, h, l, bs, got, lw * 4);
    bc_err e = img_err(ref, got, lw, lh, lw * 4);
    if (e.max_rgb > worst) worst = e.max_rgb;
    if (e.max_a > worst_a) worst_a = e.max_a;
    // and our own reader (the rgba8 fallback uses it) agrees
    memset(got, 0x55, w * h * 4);
    bc_level_to_rgba(bs, chain + tu_bc_level_off(w, h, l, bs), tu_bc_dim(w, l), tu_bc_dim(h, l), lw, lh, got, lw * 4);
    e = img_err(ref, got, lw, lh, lw * 4);
    if (e.max_rgb > worst) worst = e.max_rgb;
  }
  int over = 0;
  for (unsigned i = 0; i < 64; i++) over |= chain[total + i] != 0xEE;
  CHECK(worst <= 2 && worst_a <= 1 && !over, "bc layout %ux%u fmt 0x%X levels %u: rgb err %d alpha err %d%s",
        w, h, fmt, levels, worst, worst_a, over ? ", wrote past the chain" : "");
  free(chain); free(tmp); free(etc); free(ref); free(got);
}

static void test_bc_layout(void)
{
  CHECK(tu_bc_dim(256, 0) == 64 && tu_bc_dim(256, 6) == 1 && tu_bc_dim(256, 8) == 1, "bc dim pow2");
  CHECK(tu_bc_dim(100, 0) == 32 && tu_bc_dim(5, 0) == 2 && tu_bc_dim(3, 0) == 1, "bc dim npot");
  // 16x16 bc1: 16 + 4 + 1 + 1 + 1 blocks
  CHECK(tu_bc_level_off(16, 16, 5, 8) == 23 * 8, "bc chain 16x16 = %u", tu_bc_level_off(16, 16, 5, 8));
  CHECK(tu_bc_level_size(32, 8, 0, 16) == 16 * 16 && tu_bc_level_size(32, 8, 2, 16) == 2 * 16, "bc level size 32x8");
  CHECK(tu_bc_level_size(100, 36, 0, 8) == 32 * 16 * 8, "bc npot level size = %u", tu_bc_level_size(100, 36, 0, 8));

  layout_case(GL_COMPRESSED_RGB8_ETC2, 16, 16, 5);
  layout_case(GL_COMPRESSED_RGB8_ETC2, 64, 64, 7);
  layout_case(GL_COMPRESSED_RGBA8_ETC2_EAC, 32, 8, 4);    // wide, short side stops at 1
  layout_case(GL_COMPRESSED_RGBA8_ETC2_EAC, 8, 32, 4);    // tall
  layout_case(GL_ETC1_RGB8_OES, 8, 2, 2);                 // levels thinner than a block
  layout_case(GL_COMPRESSED_RGB8_ETC2, 128, 16, 5);
  layout_case(GL_COMPRESSED_RGB8_ETC2, 4, 4, 3);
  layout_case(GL_COMPRESSED_RGB8_ETC2, 1, 1, 1);
  layout_case(GL_COMPRESSED_RGB8_ETC2, 20, 12, 1);        // npot, not multiples of 4
  layout_case(GL_COMPRESSED_RGBA8_ETC2_EAC, 100, 36, 1);
  layout_case(GL_COMPRESSED_RGB8_ETC2, 6, 37, 1);
  layout_case(GL_COMPRESSED_RGBA8_ETC2_EAC, 250, 3, 1);

  // sub rect: retranscode 2x1 blocks at block (1,2) of a 16x16 level
  uint8_t lvl[16 * 8], tmp[16 * 16], etc[16], ref[8 * 4 * 4], got[16 * 16 * 4];
  static const uint8_t flat[16] = { 0 };
  memset(lvl, 0, sizeof(lvl));
  etc1_block(etc, 31, 0, 0, 0, flat);
  etc1_block(etc + 8, 0, 31, 0, 0, flat);
  bc_from_etc(GL_ETC1_RGB8_OES, etc, 8, 4, lvl, 4, 4, 1, 2, tmp);
  etc_decode(GL_ETC1_RGB8_OES, etc, 8, 4, ref, 32);
  v3k_read(lvl, 16, 16, 0, 8, got, 64);
  int ok = 1;
  for (unsigned y = 0; y < 4; y++) {
    int d = 0;
    for (unsigned i = 0; i < 32; i++) { int v = got[(8 + y) * 64 + 16 + i] - ref[y * 32 + i]; if (v < -2 || v > 2) d = 1; }
    ok &= !d;
  }
  CHECK(ok && got[3] == 255 && got[0] == 0 && got[(8 * 16 + 3) * 4 + 1] == 0, "bc sub rect lands at block (1,2)");
}

// etc data -> bc -> pixels against etc -> pixels, on gradients built from
// real etc blocks and on random blocks (any 64 bits are a valid etc2 block)
static void test_bc_transcode(void)
{
  enum { W = 128, H = 128 };
  static uint8_t img[W * H * 4], etc[(W / 4) * (H / 4) * 16], ref[W * H * 4], got[W * H * 4];
  static uint8_t lvl[(W / 4) * (H / 4) * 16], tmp[W * 16];
  bc_err e;

  // skin tone gradient through etc1
  for (unsigned i = 0; i < W * H; i++) {
    unsigned x = i % W, y = i / W;
    img[i * 4] = (uint8_t)(150 + x / 2 + y / 8); img[i * 4 + 1] = (uint8_t)(110 + x / 3 + y / 6);
    img[i * 4 + 2] = (uint8_t)(90 + x / 4 + y / 4); img[i * 4 + 3] = 255;
  }
  etc1_from_image(img, W, H, etc);
  etc_decode(GL_ETC1_RGB8_OES, etc, W, H, ref, W * 4);
  bc_from_etc(GL_ETC1_RGB8_OES, etc, W, H, lvl, W / 4, H / 4, 0, 0, tmp);
  v3k_read(lvl, W, H, 0, 8, got, W * 4);
  e = img_err(ref, got, W, H, W * 4);
  double bm = block_mean_err(ref, got, W, H);
  printf("etc1 gradient -> bc1: psnr %.1f dB, max err %d, worst block mean off by %.2f\n", e.psnr, e.max_rgb, bm);
  CHECK(e.psnr >= 40.0 && e.max_rgb <= 6 && bm <= 2.0, "etc1 gradient -> bc1 psnr %.1f max %d mean %.2f", e.psnr, e.max_rgb, bm);

  // the hand built etc2 planar block from above (red fading down the rows)
  static const uint8_t p_blk[8] = { 0x7E, 0x00, 0x04, 0x7F, 0x00, 0x00, 0x00, 0x00 };
  etc_decode(GL_COMPRESSED_RGB8_ETC2, p_blk, 4, 4, ref, 16);
  bc_from_etc(GL_COMPRESSED_RGB8_ETC2, p_blk, 4, 4, lvl, 1, 1, 0, 0, tmp);
  v3k_read(lvl, 4, 4, 0, 8, got, 16);
  e = img_err(ref, got, 4, 4, 16);
  CHECK(e.max_rgb <= 4, "etc2 planar -> bc1 max err %d", e.max_rgb);

  // random etc2 blocks: T/H/planar/individual/differential all show up.
  // noise can't be bounded tightly, this is a sanity + crash check
  static const GLenum fmts[3] = { GL_COMPRESSED_RGB8_ETC2, GL_COMPRESSED_RGBA8_ETC2_EAC,
                                  GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 };
  for (int f = 0; f < 3; f++) {
    unsigned ebs = f == 1 ? 16 : 8, bs = bc_bytes_for_etc(fmts[f]);
    for (unsigned i = 0; i < (W / 4) * (H / 4) * ebs; i++) etc[i] = (uint8_t)rnd();
    etc_decode(fmts[f], etc, W, H, ref, W * 4);
    bc_from_etc(fmts[f], etc, W, H, lvl, W / 4, H / 4, 0, 0, tmp);
    v3k_read(lvl, W, H, 0, bs, got, W * 4);
    e = img_err(ref, got, W, H, W * 4);
    printf("random etc2 blocks fmt 0x%X -> bc%d: psnr %.1f dB, max rgb err %d, max alpha err %d\n",
           fmts[f], bs == 16 ? 3 : 1, e.psnr, e.max_rgb, e.max_a);
    CHECK(e.psnr >= 18.0, "random etc2 0x%X psnr %.1f", fmts[f], e.psnr);
    if (f == 1) CHECK(e.max_a <= 40, "random eac alpha max err %d", e.max_a);
    if (f == 2) CHECK(e.max_a == 0, "punch through alpha must be exact, err %d", e.max_a);
  }

}

int main(void)
{
  test_etc1_individual();
  test_etc1_diff_flip();
  test_etc2_modes();
  test_eac();
  test_swizzle();
  test_layout();
  test_box();
  test_bc_decode();
  test_bc_encode();
  test_bc_layout();
  test_bc_transcode();
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
