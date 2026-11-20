// host tests for the texture cpu code: etc/eac decode, gxm swizzle order,
// layout sizes, mip box filter.
// build: gcc -Wall -Isrc tools/tex_test.c src/gles/etc.c src/gles/texutil.c -o tex_test
// the etc vectors are hand built from the khronos spec bit layouts, the
// expected colors worked out by hand (see the comments per block).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gles/gl_defs.h"
#include "gles/texutil.h"

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

int main(void)
{
  test_etc1_individual();
  test_etc1_diff_flip();
  test_etc2_modes();
  test_eac();
  test_swizzle();
  test_layout();
  test_box();
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
