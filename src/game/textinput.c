// text entry (career name etc). the game's text fields read a native
// "hardware keyboard" (VCKEYBOARD), on android that only worked with a real
// keyboard plugged in. we replace VCKeyboard_IsConnected and
// VCKeyboard_GetNextCharAndCode: "connected" only while a text field has
// focus, and then our on-screen keyboard (gles/osk.c) pops up. on OK its text comes
// back as backspaces (clear the old name), the characters, then enter.
// contract from the vckeyboard analysis: returns 1 if a char is there,
// wchar_t (32 bit) char, int code, u8 modifier, last arg 1 = peek.
// enter = 0xFE, backspace = 0xFD.
#include <stdint.h>
#include <string.h>

#include "loader/so_loader.h"
#include "util/log.h"

#define KEY_ENTER 0xFE
#define KEY_BACKSPACE 0xFD
#define QN 256
#define MAXTEXT 24


static int (*g_textedit_active)(void);   // TextEdit_IsActive @ +0x98a098
static uint32_t q[QN];
static int qh, qt;
static int ime_state;                    // 0 idle, 1 dialog up, 2 done for this focus
static char ime_buf[MAXTEXT + 1];
static int jni_mode;          // the game is reading this ime session through jni
static char jni_text[MAXTEXT + 1];

static void q_push(uint32_t c)
{
  int n = (qt + 1) % QN;
  if (n == qh) return;   // full, drop
  q[qt] = c;
  qt = n;
}

// our own on-screen keyboard (gles/osk.c). the system ime dialog started
// fine but never showed up over our frames on hw
void osk_open(void);
int osk_poll(int *ok, char *out, int max);

static void ime_open(void)
{
  memset(ime_buf, 0, sizeof(ime_buf));
  osk_open();
  ime_state = 1;
  L("textinput: text field focused, keyboard up");
}

static void ime_poll(void)
{
  int ok = 0;
  char out[MAXTEXT + 1];
  if (!osk_poll(&ok, out, sizeof(out))) return;
  if (ok) {
    int n = (int)strlen(out);
    memcpy(jni_text, out, n + 1);   // ascii, already utf-8
    if (!jni_mode) {
      // native text field: feed it as key presses
      for (int i = 0; i < 40; i++) q_push(KEY_BACKSPACE);   // clear what the field had
      for (int i = 0; i < n; i++) q_push((unsigned char)out[i]);
      q_push(KEY_ENTER);
    }
    L("textinput: keyboard returned %d chars (%s)", n, jni_mode ? "jni" : "keys");
  } else {
    L("textinput: keyboard cancelled");
  }
  ime_state = 2;
}

static unsigned n_polls;

static void pump(void)
{
  n_polls++;
  if (ime_state == 1) { ime_poll(); return; }
  int active = g_textedit_active && g_textedit_active();
  static int last_active = -1;
  if (active != last_active) {
    L("textinput: text field %s (%u keyboard polls so far)", active ? "focused" : "unfocused", n_polls);
    last_active = active;
  }
  if (!active) { if (ime_state == 2 && qh == qt) ime_state = 0; return; }
  if (ime_state == 0 && qh == qt) { jni_mode = 0; jni_text[0] = 0; ime_open(); }
}

// always "connected": the game asks once at init too, and a "no" there
// seems to switch keyboard text entry off for good (career save-name box
// never asked for chars). the ime still only opens while a field has focus
static int my_is_connected(void)
{
  static int logged;
  if (!logged++) L("textinput: game asked if a keyboard is connected (yes)");
  return 1;
}

static int my_get_next(uint32_t *ch, int *code, uint8_t *mod, int peek)
{
  pump();
  if (qh == qt) return 0;
  uint32_t c = q[qh];
  if (ch) *ch = c;
  if (code) *code = (int)c;
  if (mod) *mod = 0;
  if (!peek) qh = (qh + 1) % QN;
  return 1;
}

// ---- jni side: gameactivity.showInput / isInputShown / getInputText ----
void textinput_jni_show(void)
{
  jni_mode = 1;
  jni_text[0] = 0;
  if (ime_state != 1) ime_open();
}

int textinput_jni_shown(void)
{
  jni_mode = 1;
  if (ime_state == 1) ime_poll();
  return ime_state == 1;
}

const char *textinput_jni_text(void)
{
  return jni_text;
}

// entry-to-entry replacement: ldr pc, [pc, #-4] ; .word target
static int hook(so_module *m, uint32_t off, uint32_t w0, uint32_t w1, void *fn, const char *what)
{
  uint32_t *p = (uint32_t *)(m->base + off);
  if (p[0] != w0 || p[1] != w1) {
    L("textinput: %s hook skipped, +0x%X is %08X %08X", what, off, p[0], p[1]);
    return -1;
  }
  p[0] = 0xE51FF004;
  p[1] = (uint32_t)(uintptr_t)fn;
  return 0;
}

// called from patches_apply while the game's text is still writable
void textinput_install(so_module *game)
{
  g_textedit_active = (int (*)(void))(game->base + 0x98a098);
  int ok = 0;
  ok += !hook(game, 0x12cebbc, 0xE92D4008, 0xEBFFFE7C, (void *)my_is_connected, "VCKeyboard_IsConnected");
  ok += !hook(game, 0x12ceb3c, 0xE92D40F0, 0xE24DD00C, (void *)my_get_next, "VCKeyboard_GetNextCharAndCode");
  if (ok == 2) L("textinput: text fields use the vita ime");
}
