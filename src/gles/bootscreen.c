// "preparing for first launch": before the game starts, compile every shader
// pair the manifest knows about that the cache doesn't have yet. drawn with
// scissored clears through our own gl layer, text in a tiny 5x7 font made for
// this screen. nothing from the game is used. a normal boot skips all of it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "gles/gl_api.h"
#include "gles/gl_defs.h"
#include "gles/gxm.h"
#include "util/log.h"
#include "util/paths.h"

void shader_manifest_load(void);
uint32_t shader_manifest_count(void);
int shader_precompile(const char *vs, const char *fs);
void shacc_unload(void);
void gl_boot_ensure(void);

#define SCR_W 960
#define SCR_H 544
#define MAN_BIN  DATA_PATH "/shadercache/manifest.bin"
#define MAN_DONE DATA_PATH "/shadercache/manifest.done"
#define MAN_MAGIC 0x31464D4Eu

// ---- font: 5x7, one byte per row, low 5 bits, msb = left ----
typedef struct { char c; uint8_t rows[7]; } glyph;
static const glyph g_font[] = {
  {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
  {'C', {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}}, {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
  {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
  {'G', {0x0F,0x10,0x10,0x13,0x11,0x11,0x0F}}, {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
  {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
  {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
  {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
  {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}, {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
  {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
  {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
  {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}}, {'X', {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11}},
  {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
  {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
  {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
  {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
  {'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}}, {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
  {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
};

static const uint8_t *glyph_rows(char c)
{
  for (unsigned i = 0; i < sizeof(g_font) / sizeof(g_font[0]); i++)
    if (g_font[i].c == c) return g_font[i].rows;
  return NULL;   // space and anything we didn't draw
}

// ---- drawing: solid quads at pixel coords (top-left origin) ----
// not scissored clears: gxm's region clip is 32x32-tile granular, which
// turned every font pixel into a block on hw
void gl_fill_rect(int x, int y, int w, int h, float r, float g, float b);
static void rect(int x, int y, int w, int h, float r, float g, float b)
{
  gl_fill_rect(x, y, w, h, r, g, b);
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }

static void text(const char *s, int x, int y, int scale, float r, float g, float b)
{
  for (; *s; s++, x += 6 * scale) {
    const uint8_t *rows = glyph_rows(*s);
    if (!rows) continue;
    for (int ry = 0; ry < 7; ry++) {
      // merge runs of lit pixels in a row into one clear
      for (int rx = 0; rx < 5;) {
        if (!(rows[ry] >> (4 - rx) & 1)) { rx++; continue; }
        int run = 0;
        while (rx + run < 5 && (rows[ry] >> (4 - rx - run) & 1)) run++;
        rect(x + rx * scale, y + ry * scale, run * scale, scale, r, g, b);
        rx += run;
      }
    }
  }
}

// shared with the on-screen keyboard (osk.c)
void ui_rect(int x, int y, int w, int h, float r, float g, float b) { rect(x, y, w, h, r, g, b); }
int ui_text_w(const char *s, int scale) { return text_w(s, scale); }
void ui_text(const char *s, int x, int y, int scale, float r, float g, float b) { text(s, x, y, scale, r, g, b); }

static void text_c(const char *s, int y, int scale, float r, float g, float b)
{
  text(s, (SCR_W - text_w(s, scale)) / 2, y, scale, r, g, b);
}

// rounded bar, one clear per scanline
static void rrect(int x, int y, int w, int h, float r, float g, float b)
{
  float rad = h * 0.5f;
  for (int i = 0; i < h; i++) {
    float dy = fabsf(i + 0.5f - rad);
    int in = (int)(rad - sqrtf(rad * rad - dy * dy) + 0.5f);
    if (in * 2 >= w) continue;
    rect(x + in, y + i, w - in * 2, 1, r, g, b);
  }
}

static void draw_frame(uint32_t done, uint32_t total)
{
  // background: dark blue to near black
  const int bands = 34;
  for (int i = 0; i < bands; i++) {
    float t = (float)i / (bands - 1);
    rect(0, i * SCR_H / bands, SCR_W, SCR_H / bands + 1,
         0.07f - 0.05f * t, 0.09f - 0.065f * t, 0.15f - 0.11f * t);
  }
  // thin accent line above the title
  rect(SCR_W / 2 - 40, 150, 80, 3, 0.25f, 0.55f, 1.0f);
  text_c("PREPARING FOR FIRST LAUNCH", 180, 4, 0.95f, 0.96f, 0.98f);

  // progress bar
  const int bx = 180, by = 280, bw = SCR_W - 360, bh = 18;
  rrect(bx, by, bw, bh, 0.15f, 0.17f, 0.23f);
  if (total) {
    int fw = (int)((uint64_t)bw * done / total);
    if (fw >= bh) rrect(bx, by, fw, bh, 0.25f, 0.55f, 1.0f);
  }

  char line[64];
  snprintf(line, sizeof(line), "COMPILING SHADERS  %u / %u", (unsigned)done, (unsigned)total);
  text_c(line, 320, 2, 0.70f, 0.73f, 0.80f);
  text_c("THIS ONLY HAPPENS ONCE PER INSTALL OR UPDATE", 470, 2, 0.40f, 0.43f, 0.50f);

  gxm_swap();
}

static uint32_t read_done(void)
{
  uint32_t v = 0;
  SceUID fd = sceIoOpen(MAN_DONE, SCE_O_RDONLY, 0);
  if (fd >= 0) { if (sceIoRead(fd, &v, 4) != 4) v = 0; sceIoClose(fd); }
  return v;
}

static void write_done(uint32_t v)
{
  SceUID fd = sceIoOpen(MAN_DONE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) { sceIoWrite(fd, &v, 4); sceIoClose(fd); }
}

void bootscreen_run(void)
{
  shader_manifest_load();
  uint32_t total = shader_manifest_count();
  uint32_t done = read_done();
  if (done >= total) return;   // nothing new, normal boot

  SceUID fd = sceIoOpen(MAN_BIN, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  L("boot: %u shader pairs to prepare (%u already done)", total - done, done);
  gl_boot_ensure();
  SceUInt64 t0 = sceKernelGetProcessTimeWide(), last_draw = 0;
  uint32_t todo = total - done, i = 0, n_ok = 0, rec = 0;
  draw_frame(0, todo);

  for (;;) {
    uint32_t hdr[3];
    if (sceIoRead(fd, hdr, sizeof(hdr)) != sizeof(hdr) || hdr[0] != MAN_MAGIC ||
        hdr[1] > 1024 * 1024 || hdr[2] > 1024 * 1024) break;
    if (rec++ < done) {   // already handled on an earlier boot
      sceIoLseek(fd, hdr[1] + hdr[2], SCE_SEEK_CUR);
      continue;
    }
    char *vs = malloc(hdr[1] + 1), *fs = malloc(hdr[2] + 1);
    if (!vs || !fs || sceIoRead(fd, vs, hdr[1]) != (SceSSize)hdr[1] || sceIoRead(fd, fs, hdr[2]) != (SceSSize)hdr[2]) {
      free(vs); free(fs);
      break;
    }
    vs[hdr[1]] = 0;
    fs[hdr[2]] = 0;
    if (shader_precompile(vs, fs) == 0) n_ok++;
    free(vs);
    free(fs);
    i++;
    if (!(i & 15)) write_done(done + i);   // a crash or power-off resumes from here
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - last_draw > 100 * 1000 || i == todo) { draw_frame(i, todo); last_draw = now; }
  }
  sceIoClose(fd);
  write_done(done + i);
  draw_frame(todo, todo);
  L("boot: prepared %u of %u shader pairs in %u ms", n_ok, i,
    (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
  glClearColor(0, 0, 0, 0);   // leave gl state the way the game expects it
  shacc_unload();
}
