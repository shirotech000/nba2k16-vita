// text entry (career name etc). the game's text fields read a native
// "hardware keyboard" (VCKEYBOARD), on android that only worked with a real
// keyboard plugged in. we replace VCKeyboard_IsConnected and
// VCKeyboard_GetNextCharAndCode: "connected" only while a text field has
// focus, and then the vita's own ime dialog pops up. on OK its text comes
// back as backspaces (clear the old name), the characters, then enter.
// contract from the vckeyboard analysis: returns 1 if a char is there,
// wchar_t (32 bit) char, int code, u8 modifier, last arg 1 = peek.
// enter = 0xFE, backspace = 0xFD.
#include <stdint.h>
#include <string.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/apputil.h>

#include "loader/so_loader.h"
#include "util/log.h"

#define KEY_ENTER 0xFE
#define KEY_BACKSPACE 0xFD
#define QN 256

extern volatile int g_cdlg_active;   // gxm.c draws common dialogs while set

static int (*g_textedit_active)(void);   // TextEdit_IsActive @ +0x98a098
static uint32_t q[QN];
static int qh, qt;
static int ime_state;                    // 0 idle, 1 dialog up, 2 done for this focus
static SceWChar16 ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static SceWChar16 ime_title[16];
static SceWChar16 ime_empty[1];

static void q_push(uint32_t c)
{
  int n = (qt + 1) % QN;
  if (n == qh) return;   // full, drop
  q[qt] = c;
  qt = n;
}

static void ime_open(void)
{
  static int apputil;
  if (!apputil) {
    SceAppUtilInitParam ip;
    SceAppUtilBootParam bp;
    memset(&ip, 0, sizeof(ip));
    memset(&bp, 0, sizeof(bp));
    sceAppUtilInit(&ip, &bp);
    apputil = 1;
  }
  const char *t = "Enter text";
  for (int i = 0; t[i] && i < 15; i++) ime_title[i] = (SceWChar16)t[i];
  memset(ime_buf, 0, sizeof(ime_buf));
  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  p.supportedLanguages = 0x0001FFFF;
  p.languagesForced = SCE_FALSE;
  p.type = SCE_IME_TYPE_DEFAULT;
  p.option = 0;
  p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  p.title = ime_title;
  p.maxTextLength = 24;
  p.initialText = ime_empty;
  p.inputTextBuffer = ime_buf;
  int r = sceImeDialogInit(&p);
  if (r < 0) {
    L("textinput: sceImeDialogInit failed 0x%08X", r);
    ime_state = 2;   // don't retry every frame
    return;
  }
  g_cdlg_active = 1;
  ime_state = 1;
  L("textinput: text field focused, ime up");
}

static void ime_poll(void)
{
  if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
  SceImeDialogResult res;
  memset(&res, 0, sizeof(res));
  sceImeDialogGetResult(&res);
  if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
    for (int i = 0; i < 40; i++) q_push(KEY_BACKSPACE);   // clear what the field had
    int n = 0;
    for (; ime_buf[n] && n < SCE_IME_DIALOG_MAX_TEXT_LENGTH; n++) q_push(ime_buf[n]);
    q_push(KEY_ENTER);
    L("textinput: ime returned %d chars", n);
  } else {
    L("textinput: ime cancelled");
  }
  sceImeDialogTerm();
  g_cdlg_active = 0;
  ime_state = 2;
}

static void pump(void)
{
  if (ime_state == 1) { ime_poll(); return; }
  int active = g_textedit_active && g_textedit_active();
  if (!active) { if (ime_state == 2 && qh == qt) ime_state = 0; return; }
  if (ime_state == 0 && qh == qt) ime_open();
}

static int my_is_connected(void)
{
  return (g_textedit_active && g_textedit_active()) || ime_state == 1 || qh != qt;
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
