// bc1 / bc3 encoder, see bcenc.h. made for upload time transcoding of etc
// textures: one pass, no search over endpoint pairs.
// colour block: principal axis of the block's colours -> endpoints at the
// outermost projections, pulled in a bit -> 2 bit index fit -> least squares
// refit of the endpoints for those indices. flat blocks and blocks that are
// nearly flat (skin, sky) first try endpoints picked so the interpolated
// colours land between the 565 steps, that's what keeps gradients from
// banding (and it's the cheap path, the axis fit only runs if that's not
// good enough).
#include <string.h>
#include "gles/bcenc.h"
#include "gles/texutil.h"

void etc_decode(GLenum fmt, const uint8_t *src, unsigned w, unsigned h,
                uint8_t *dst, unsigned dst_stride);
int etc_block_bytes(GLenum fmt);

static inline int ex5(int v) { return (v << 3) | (v >> 2); }
static inline int ex6(int v) { return (v << 2) | (v >> 4); }
static inline int q5(int v) { return (v * 31 + 127) / 255; }
static inline int q6(int v) { return (v * 63 + 127) / 255; }
static inline unsigned pack565(int r, int g, int b) { return (unsigned)(r << 11 | g << 5 | b); }
static inline int clamp8(float f) { return f <= 0 ? 0 : f >= 255 ? 255 : (int)(f + 0.5f); }

// ---------------------------------------------------------------- flat colours

// for every 8 bit value: the 5/6 bit pair (hi >= lo) whose 2/3 point is the
// closest. hi/lo stay near each other so sloppy interpolation in a decoder
// can't move it far.
static uint8_t g_m5[256][2], g_m6[256][2];
static int g_tables;

static void make_table(uint8_t m[256][2], int bits)
{
  int n = 1 << bits;
  for (int v = 0; v < 256; v++) {
    int best = 1 << 30;
    for (int hi = 0; hi < n; hi++)
      for (int lo = 0; lo <= hi; lo++) {
        int a = bits == 5 ? ex5(hi) : ex6(hi), b = bits == 5 ? ex5(lo) : ex6(lo);
        int e = (2 * a + b + 1) / 3 - v;
        int cost = (e < 0 ? -e : e) * 32 + (a - b);
        if (cost < best) { best = cost; m[v][0] = (uint8_t)hi; m[v][1] = (uint8_t)lo; }
      }
  }
}

// ---------------------------------------------------------------- colour block

typedef struct {
  int n;               // texels that count (opaque ones)
  uint8_t px[16][3];
  uint8_t pos[16];     // where in the block each one sits
} cset;

typedef struct {
  unsigned c0, c1;
  unsigned err;
  uint8_t idx[16];
} cand;

static void palette(unsigned c0, unsigned c1, int three, int p[4][3])
{
  p[0][0] = ex5(c0 >> 11); p[0][1] = ex6(c0 >> 5 & 63); p[0][2] = ex5(c0 & 31);
  p[1][0] = ex5(c1 >> 11); p[1][1] = ex6(c1 >> 5 & 63); p[1][2] = ex5(c1 & 31);
  for (int c = 0; c < 3; c++) {
    if (three) {
      p[2][c] = (p[0][c] + p[1][c] + 1) >> 1;
      p[3][c] = 0;
    } else {
      p[2][c] = (2 * p[0][c] + p[1][c] + 1) / 3;
      p[3][c] = (p[0][c] + 2 * p[1][c] + 1) / 3;
    }
  }
}

// index fit for an endpoint pair, kept if it beats what we have. texels get
// the palette entry their projection onto the c1 -> c0 line is nearest to.
static void try_pair(const cset *s, unsigned c0, unsigned c1, int three, cand *best)
{
  int p[4][3];
  palette(c0, c1, three, p);
  int d[3] = { p[0][0] - p[1][0], p[0][1] - p[1][1], p[0][2] - p[1][2] };
  int dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  static const uint8_t map4[4] = { 1, 3, 2, 0 }, map3[3] = { 1, 2, 0 };
  uint8_t idx[16];
  unsigned err = 0, lim = best->err;
  for (int i = 0; i < s->n; i++) {
    int r = s->px[i][0], g = s->px[i][1], b = s->px[i][2];
    int t = (r - p[1][0]) * d[0] + (g - p[1][1]) * d[1] + (b - p[1][2]) * d[2];
    // steps up the line from c1, no branches (noisy blocks mispredict them all)
    int k = three ? map3[(t * 4 >= dd) + (t * 4 >= 3 * dd)]
                  : map4[(t * 6 >= dd) + (t * 2 >= dd) + (t * 6 >= 5 * dd)];
    int dr = r - p[k][0], dg = g - p[k][1], db = b - p[k][2];
    idx[i] = (uint8_t)k;
    err += (unsigned)(dr * dr + dg * dg + db * db);
  }
  if (err >= lim) return;
  best->c0 = c0;
  best->c1 = c1;
  best->err = err;
  memcpy(best->idx, idx, sizeof(idx));
}

// endpoints that fit the current indices best (least squares), then refit
static void refine(const cset *s, int three, cand *best)
{
  static const int w4[4] = { 3, 0, 2, 1 }, w3[4] = { 2, 0, 1, 0 };
  const int *wt = three ? w3 : w4;
  int D = three ? 2 : 3;
  int aa = 0, ab = 0, bb = 0, ap[3] = { 0, 0, 0 }, bp[3] = { 0, 0, 0 };
  for (int i = 0; i < s->n; i++) {
    int a = wt[best->idx[i]], b = D - a;
    aa += a * a; ab += a * b; bb += b * b;
    for (int c = 0; c < 3; c++) { ap[c] += a * s->px[i][c]; bp[c] += b * s->px[i][c]; }
  }
  int det = aa * bb - ab * ab;
  if (!det) return;   // every texel on the same index
  float k = (float)D / (float)det;
  int e0[3], e1[3];
  for (int c = 0; c < 3; c++) {
    e0[c] = clamp8(k * (float)(bb * ap[c] - ab * bp[c]));
    e1[c] = clamp8(k * (float)(aa * bp[c] - ab * ap[c]));
  }
  try_pair(s, pack565(q5(e0[0]), q6(e0[1]), q5(e0[2])), pack565(q5(e1[0]), q6(e1[1]), q5(e1[2])), three, best);
}

static void put_color(uint8_t out[8], unsigned c0, unsigned c1, uint32_t bits)
{
  out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8);
  out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
  out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8);
  out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
}

#define LOW_RANGE    24      // biggest channel range that still counts as nearly flat
#define LOW_GOOD     6       // sum of squared rgb error per texel the flat fit may leave
#define REFINE_ABOVE 3       // same, below this the refit isn't worth its time
#define INSET        (1.0f / 32.0f)

// three = dxt1 3 colour + transparent mode: texels not in the set get index 3
static void color_block(const cset *s, int three, uint8_t out[8])
{
  if (!g_tables) {
    make_table(g_m5, 5);
    make_table(g_m6, 6);
    g_tables = 1;
  }
  if (!s->n) { put_color(out, 0, 0, 0xFFFFFFFFu); return; }

  int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 }, sum[3] = { 0, 0, 0 };
  for (int i = 0; i < s->n; i++)
    for (int c = 0; c < 3; c++) {
      int v = s->px[i][c];
      if (v < mn[c]) mn[c] = v;
      if (v > mx[c]) mx[c] = v;
      sum[c] += v;
    }
  int range = 0, wide = 0;
  for (int c = 0; c < 3; c++)
    if (mx[c] - mn[c] > range) { range = mx[c] - mn[c]; wide = c; }

  cand best;
  best.err = ~0u;
  best.c0 = best.c1 = 0;
  memset(best.idx, 0, sizeof(best.idx));

  if (!range && !three) {
    // one colour: the 2/3 point of a tuned pair
    unsigned c0 = pack565(g_m5[mn[0]][0], g_m6[mn[1]][0], g_m5[mn[2]][0]);
    unsigned c1 = pack565(g_m5[mn[0]][1], g_m6[mn[1]][1], g_m5[mn[2]][1]);
    put_color(out, c0, c1, c0 == c1 ? 0 : 0xAAAAAAAAu);
    return;
  }

  if (range <= LOW_RANGE && !three) {
    // nearly flat (most of a skin or sky texture): the 565 grid is coarser
    // than the block's whole range, so fitting a line through the colours and
    // rounding its ends bands. instead take the tuned pair for the average
    // colour, and the 565 grid cell around the block, whose thirds sit
    // between the grid steps.
    int av[3], lo[3], hi[3];
    for (int c = 0; c < 3; c++) av[c] = (sum[c] + s->n / 2) / s->n;
    try_pair(s, pack565(g_m5[av[0]][0], g_m6[av[1]][0], g_m5[av[2]][0]),
             pack565(g_m5[av[0]][1], g_m6[av[1]][1], g_m5[av[2]][1]), 0, &best);
    lo[0] = mn[0] * 31 / 255; lo[1] = mn[1] * 63 / 255; lo[2] = mn[2] * 31 / 255;
    hi[0] = (mx[0] * 31 + 254) / 255; hi[1] = (mx[1] * 63 + 254) / 255; hi[2] = (mx[2] * 31 + 254) / 255;
    try_pair(s, pack565(hi[0], hi[1], hi[2]), pack565(lo[0], lo[1], lo[2]), 0, &best);
  }

  // good enough already (about 1.5 levels rms)? else fit a line. the cell
  // above only works when the channels rise together.
  if (range && best.err > (unsigned)s->n * LOW_GOOD) {
    // principal axis: start from the covariance row of the widest channel,
    // a few power iterations
    float inv = 1.0f / (float)s->n;
    float mean[3] = { (float)sum[0] * inv, (float)sum[1] * inv, (float)sum[2] * inv };
    float d[16][3], cv[3][3] = { { 0 } };
    for (int i = 0; i < s->n; i++) {
      d[i][0] = (float)s->px[i][0] - mean[0];
      d[i][1] = (float)s->px[i][1] - mean[1];
      d[i][2] = (float)s->px[i][2] - mean[2];
      cv[0][0] += d[i][0] * d[i][0]; cv[0][1] += d[i][0] * d[i][1]; cv[0][2] += d[i][0] * d[i][2];
      cv[1][1] += d[i][1] * d[i][1]; cv[1][2] += d[i][1] * d[i][2]; cv[2][2] += d[i][2] * d[i][2];
    }
    cv[1][0] = cv[0][1]; cv[2][0] = cv[0][2]; cv[2][1] = cv[1][2];
    float v[3] = { cv[wide][0], cv[wide][1], cv[wide][2] };
    for (int it = 0; it < 3; it++) {
      float m = 0;
      for (int c = 0; c < 3; c++) { float a = v[c] < 0 ? -v[c] : v[c]; if (a > m) m = a; }
      if (m <= 0) break;
      m = 1.0f / m;
      float n[3] = { v[0] * m, v[1] * m, v[2] * m };
      for (int c = 0; c < 3; c++) v[c] = cv[c][0] * n[0] + cv[c][1] * n[1] + cv[c][2] * n[2];
    }
    float vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (vv > 0) {
      float tmin = 1e30f, tmax = -1e30f;
      for (int i = 0; i < s->n; i++) {
        float t = d[i][0] * v[0] + d[i][1] * v[1] + d[i][2] * v[2];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
      }
      // inset: the outermost texels end up a bit outside the endpoints,
      // which spreads the error over all the palette entries
      float in = (tmax - tmin) * INSET;
      vv = 1.0f / vv;
      tmax = (tmax - in) * vv;
      tmin = (tmin + in) * vv;
      int e0[3], e1[3];
      for (int c = 0; c < 3; c++) {
        e0[c] = clamp8(mean[c] + v[c] * tmax);
        e1[c] = clamp8(mean[c] + v[c] * tmin);
      }
      try_pair(s, pack565(q5(e0[0]), q6(e0[1]), q5(e0[2])), pack565(q5(e1[0]), q6(e1[1]), q5(e1[2])), three, &best);
    }
  }
  if (best.err == ~0u) {
    // flat block in 3 colour mode, or the axis went nowhere: bounding box corners
    try_pair(s, pack565(q5(mx[0]), q6(mx[1]), q5(mx[2])), pack565(q5(mn[0]), q6(mn[1]), q5(mn[2])), three, &best);
  }
  if (best.err > (unsigned)s->n * REFINE_ABOVE) refine(s, three, &best);

  // order the endpoints for the mode and build the index word
  unsigned c0 = best.c0, c1 = best.c1;
  uint32_t bits = three ? 0xFFFFFFFFu : 0;
  int swap = three ? c0 > c1 : c0 < c1;
  if (swap) { unsigned t = c0; c0 = c1; c1 = t; }
  for (int i = 0; i < s->n; i++) {
    unsigned k = best.idx[i];
    if (swap && k < 2) k ^= 1;                    // the two endpoints trade places
    else if (swap && !three) k ^= 1;              // and so do the thirds
    if (c0 == c1) k = 0;
    bits = (bits & ~(3u << (s->pos[i] * 2))) | (k << (s->pos[i] * 2));
  }
  put_color(out, c0, c1, bits);
}

void bc1_encode(const uint8_t *rgba, unsigned stride, uint8_t out[8], int punch)
{
  cset s;
  s.n = 0;
  for (int i = 0; i < 16; i++) {
    const uint8_t *p = rgba + (i >> 2) * stride + (i & 3) * 4;
    if (punch && p[3] < 128) continue;
    s.px[s.n][0] = p[0]; s.px[s.n][1] = p[1]; s.px[s.n][2] = p[2];
    s.pos[s.n++] = (uint8_t)i;
  }
  color_block(&s, s.n < 16, out);
}

// ---------------------------------------------------------------- alpha block

static void alpha_block(const uint8_t a[16], uint8_t out[8])
{
  int mn = 255, mx = 0, mn2 = 255, mx2 = 0, ends = 0, mid = 0;
  for (int i = 0; i < 16; i++) {
    int v = a[i];
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    if (v == 0 || v == 255) { ends = 1; continue; }
    mid = 1;
    if (v < mn2) mn2 = v;
    if (v > mx2) mx2 = v;
  }
  memset(out, 0, 8);
  out[0] = (uint8_t)mx;
  out[1] = (uint8_t)mn;
  if (mn == mx) return;

  // 8 value mode over the full range, and when the block touches 0 / 255
  // (cutout edges) also the 6 value mode that has those two for free
  int pal[2][8], use[2] = { 1, ends && mid };
  pal[0][0] = mx; pal[0][1] = mn;
  for (int i = 2; i < 8; i++) pal[0][i] = ((8 - i) * mx + (i - 1) * mn + 3) / 7;
  pal[1][0] = mn2; pal[1][1] = mx2;
  for (int i = 2; i < 6; i++) pal[1][i] = ((6 - i) * mn2 + (i - 1) * mx2 + 2) / 5;
  pal[1][6] = 0; pal[1][7] = 255;

  // nearest palette entry = rounded step along the ramp. one division per
  // mode (the vita has no divide instruction), then 16.16 fixed point
  static const uint8_t map8[8] = { 1, 7, 6, 5, 4, 3, 2, 0 }, map6[6] = { 0, 2, 3, 4, 5, 1 };
  unsigned best = ~0u;
  for (int m = 0; m < 2; m++) {
    if (!use[m]) continue;
    int lo = m ? mn2 : mn, hi = m ? mx2 : mx, steps = m ? 5 : 7;
    int f = hi > lo ? (steps << 16) / (hi - lo) : 0;
    uint64_t bits = 0;
    unsigned err = 0;
    for (int i = 0; i < 16; i++) {
      int v = a[i], k;
      if (m && (v == 0 || v == 255)) {
        k = v ? 7 : 6;
      } else {
        int st = ((v - lo) * f + 0x8000) >> 16;
        k = m ? map6[st] : map8[st];
      }
      int e = v - pal[m][k];
      err += (unsigned)(e * e);
      bits |= (uint64_t)k << (3 * i);
    }
    if (err >= best) continue;
    best = err;
    out[0] = (uint8_t)pal[m][0];
    out[1] = (uint8_t)pal[m][1];
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
  }
}

void bc3_encode(const uint8_t *rgba, unsigned stride, uint8_t out[16])
{
  cset s;
  uint8_t a[16];
  s.n = 16;
  for (int i = 0; i < 16; i++) {
    const uint8_t *p = rgba + (i >> 2) * stride + (i & 3) * 4;
    s.px[i][0] = p[0]; s.px[i][1] = p[1]; s.px[i][2] = p[2];
    s.pos[i] = (uint8_t)i;
    a[i] = p[3];
  }
  alpha_block(a, out);
  color_block(&s, 0, out + 8);
}

// ---------------------------------------------------------------- decode

void bc_decode_block(unsigned bs, const uint8_t *blk, uint8_t *rgba, unsigned stride)
{
  const uint8_t *cb = bs == 16 ? blk + 8 : blk;
  unsigned c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
  uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
  int three = bs != 16 && c0 <= c1;
  int p[4][3];
  palette(c0, c1, three, p);
  int al[8] = { 0 };
  uint64_t abits = 0;
  if (bs == 16) {
    al[0] = blk[0]; al[1] = blk[1];
    if (al[0] > al[1]) {
      for (int i = 2; i < 8; i++) al[i] = ((8 - i) * al[0] + (i - 1) * al[1] + 3) / 7;
    } else {
      for (int i = 2; i < 6; i++) al[i] = ((6 - i) * al[0] + (i - 1) * al[1] + 2) / 5;
      al[6] = 0; al[7] = 255;
    }
    for (int i = 0; i < 6; i++) abits |= (uint64_t)blk[2 + i] << (8 * i);
  }
  for (int i = 0; i < 16; i++) {
    uint8_t *o = rgba + (i >> 2) * stride + (i & 3) * 4;
    unsigned k = bits >> (2 * i) & 3;
    o[0] = (uint8_t)p[k][0]; o[1] = (uint8_t)p[k][1]; o[2] = (uint8_t)p[k][2];
    if (bs == 16) o[3] = (uint8_t)al[abits >> (3 * i) & 7];
    else o[3] = three && k == 3 ? 0 : 255;
  }
}

// ---------------------------------------------------------------- levels

static int is_punch(GLenum etc)
{
  return etc == GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 ||
         etc == GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2;
}

unsigned bc_bytes_for_etc(GLenum etc)
{
  switch (etc) {
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

void bc_from_etc(GLenum etc, const uint8_t *src, unsigned w, unsigned h,
                 uint8_t *level, unsigned gw, unsigned gh, unsigned bx0, unsigned by0,
                 uint8_t *tmp)
{
  unsigned bs = bc_bytes_for_etc(etc);
  if (!bs) return;
  unsigned ebs = (unsigned)etc_block_bytes(etc);
  int punch = is_punch(etc);
  unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
  uint32_t *out = (uint32_t *)(void *)level;
  for (unsigned by = 0; by < bh; by++) {
    // whole blocks, so edge blocks keep the padding texels the etc data has
    etc_decode(etc, src + by * bw * ebs, bw * 4, 4, tmp, bw * 16);
    for (unsigned bx = 0; bx < bw; bx++) {
      uint32_t blk[4];
      if (bs == 16) bc3_encode(tmp + bx * 16, bw * 16, (uint8_t *)blk);
      else bc1_encode(tmp + bx * 16, bw * 16, (uint8_t *)blk, punch);
      uint32_t *d = out + tu_morton(bx0 + bx, by0 + by, gw, gh) * (bs / 4);
      d[0] = blk[0];
      d[1] = blk[1];
      if (bs == 16) { d[2] = blk[2]; d[3] = blk[3]; }
    }
  }
}

void bc_level_to_rgba(unsigned bs, const uint8_t *level, unsigned gw, unsigned gh,
                      unsigned w, unsigned h, uint8_t *dst, unsigned dstride)
{
  unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
  for (unsigned by = 0; by < bh; by++)
    for (unsigned bx = 0; bx < bw; bx++) {
      uint32_t blk[4];
      uint8_t px[64];
      memcpy(blk, level + tu_morton(bx, by, gw, gh) * bs, bs);
      bc_decode_block(bs, (const uint8_t *)blk, px, 16);
      unsigned n = w - bx * 4 < 4 ? w - bx * 4 : 4;
      for (unsigned y = 0; y < 4 && by * 4 + y < h; y++)
        memcpy(dst + (by * 4 + y) * dstride + bx * 16, px + y * 16, n * 4);
    }
}
