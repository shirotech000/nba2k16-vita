// our own on-screen keyboard. the system ime dialog came up but never got
// drawn over our frames, so text entry (career save name) gets this instead:
// a panel drawn over the game's last frame with the boot screen's quads and
// font. d-pad / touch to pick, cross types, circle deletes, triangle space,
// start done, select cancels. while it's up it eats all input.
#include <stdint.h>
#include <string.h>

#include "gles/gl_internal.h"
#include "gles/gl_api.h"
#include "util/log.h"

void ui_rect(int x, int y, int w, int h, float r, float g, float b);
void ui_text(const char *s, int x, int y, int scale, float r, float g, float b);
int ui_text_w(const char *s, int scale);

#define MAXLEN 24
#define KW 62
#define KH 46
#define GAP 6
#define ROW_W (10 * (KW + GAP) - GAP)
#define X0 ((960 - ROW_W) / 2)
#define Y0 226

static const char *g_rows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM." };
static const char *g_special[3] = { "SPACE", "DELETE", "DONE" };

static volatile int g_on, g_done, g_ok;
static char g_buf[MAXLEN + 1];
static int g_len, g_row, g_col;
static unsigned g_blink;

static int row_len(int r) { return r < 4 ? (int)strlen(g_rows[r]) : 3; }

// key rect for (row, col)
static void key_rect(int r, int c, int *x, int *y, int *w, int *h)
{
  *y = Y0 + r * (KH + GAP);
  *h = KH;
  if (r < 4) {
    *x = X0 + c * (KW + GAP);
    *w = KW;
    return;
  }
  // bottom row: space 4 units, delete 3, done 3
  static const int units[3] = { 4, 3, 3 }, start[3] = { 0, 4, 7 };
  int unit = KW + GAP;
  *x = X0 + start[c] * unit;
  *w = units[c] * unit - GAP;
}

static void finish(int ok)
{
  g_ok = ok;
  g_on = 0;
  g_done = 1;
  L("osk: %s (%d chars)", ok ? "done" : "cancelled", g_len);
}

static void press(int r, int c)
{
  if (r < 4) {
    if (g_len < MAXLEN) { g_buf[g_len++] = g_rows[r][c]; g_buf[g_len] = 0; }
  } else if (c == 0) {
    if (g_len < MAXLEN) { g_buf[g_len++] = ' '; g_buf[g_len] = 0; }
  } else if (c == 1) {
    if (g_len > 0) g_buf[--g_len] = 0;
  } else {
    finish(1);
  }
}

void osk_open(void)
{
  g_len = 0;
  g_buf[0] = 0;
  g_row = 1;
  g_col = 0;
  g_done = 0;
  g_on = 1;
  L("osk: up");
}

int osk_active(void) { return g_on; }

// 1 once it's finished: *ok and the text
int osk_poll(int *ok, char *out, int max)
{
  if (!g_done) return 0;
  g_done = 0;
  *ok = g_ok;
  int n = g_len < max - 1 ? g_len : max - 1;
  memcpy(out, g_buf, n);
  out[n] = 0;
  return 1;
}

// from input_poll while the keyboard is up. pressed = newly pressed buttons,
// tap = a fresh touch at (tx, ty) in screen pixels
void osk_input(unsigned pressed, int tap, int tx, int ty)
{
  if (!g_on) return;
  enum { UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80, TRI = 0x1000, CIRC = 0x2000, CROSS = 0x4000,
         SELECT = 0x1, START = 0x8 };   // SCE_CTRL_* bits
  if (pressed & UP) { if (g_row > 0) g_row--; }
  if (pressed & DOWN) { if (g_row < 4) g_row++; }
  if (pressed & LEFT) { if (g_col > 0) g_col--; }
  if (pressed & RIGHT) g_col++;
  if (g_col >= row_len(g_row)) g_col = row_len(g_row) - 1;
  if (pressed & CROSS) press(g_row, g_col);
  if (pressed & CIRC) { if (g_len > 0) g_buf[--g_len] = 0; }
  if (pressed & TRI) press(4, 0);
  if (pressed & START) finish(1);
  if (pressed & SELECT) finish(0);
  if (tap) {
    // touches arrive in the (possibly scaled) window space, the key rects
    // are laid out in 960x544
    extern unsigned g_rw, g_rh;
    tx = tx * 960 / (int)g_rw;
    ty = ty * 544 / (int)g_rh;
    for (int r = 0; r < 5; r++) {
      for (int c = 0; c < row_len(r); c++) {
        int x, y, w, h;
        key_rect(r, c, &x, &y, &w, &h);
        if (tx >= x && tx < x + w && ty >= y && ty < y + h) {
          g_row = r;
          g_col = c;
          press(r, c);
          return;
        }
      }
    }
  }
}

// over the game's last frame, right before the swap
void osk_draw(void)
{
  if (!g_on) return;
  GLuint prev = G.draw_fbo;
  if (prev) glBindFramebuffer(GL_FRAMEBUFFER, 0);

  ui_rect(112, 112, 736, 420, 0.06f, 0.08f, 0.13f);           // panel
  ui_rect(112, 112, 736, 3, 0.25f, 0.55f, 1.0f);              // accent edge
  ui_text("ENTER NAME", X0, 130, 2, 0.70f, 0.73f, 0.80f);
  // text box with a blinking cursor
  ui_rect(X0, 156, ROW_W, 52, 0.12f, 0.14f, 0.20f);
  ui_text(g_buf, X0 + 14, 168, 4, 0.95f, 0.96f, 0.98f);
  if ((g_blink++ / 20) & 1)
    ui_rect(X0 + 14 + (g_len ? ui_text_w(g_buf, 4) + 6 : 0), 166, 4, 30, 0.25f, 0.55f, 1.0f);

  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < row_len(r); c++) {
      int x, y, w, h;
      key_rect(r, c, &x, &y, &w, &h);
      int sel = r == g_row && c == g_col;
      if (sel) ui_rect(x, y, w, h, 0.25f, 0.55f, 1.0f);
      else ui_rect(x, y, w, h, 0.16f, 0.18f, 0.25f);
      char lab[8];
      const char *s = lab;
      if (r < 4) { lab[0] = g_rows[r][c]; lab[1] = 0; }
      else s = g_special[c];
      int sc = r < 4 ? 3 : 2;
      ui_text(s, x + (w - ui_text_w(s, sc)) / 2, y + (h - 7 * sc) / 2, sc,
              sel ? 1.0f : 0.85f, sel ? 1.0f : 0.87f, sel ? 1.0f : 0.92f);
    }
  }
  ui_text("CROSS TYPE  CIRCLE DELETE  START DONE  SELECT CANCEL", X0, 500, 2, 0.40f, 0.43f, 0.50f);

  if (prev) glBindFramebuffer(GL_FRAMEBUFFER, prev);
}
