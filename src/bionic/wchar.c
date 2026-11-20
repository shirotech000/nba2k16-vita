// wide char stuff for gnustl's locale code. we only ever run the "C"
// locale, and bionic's mbstate_t is smaller than newlib's, so don't hand
// the game's mbstate to newlib at all, just do the trivial thing.
#include <errno.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include "bionic/bionic.h"
#include "loader/exports.h"

static wint_t b_btowc(int c) { return c == EOF ? WEOF : (unsigned char)c; }
static int b_wctob(wint_t w) { return w < 256 ? (int)w : EOF; }

static size_t b_mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps) {
  (void)ps;
  if (!s) return 0;
  if (!n) return (size_t)-2;
  unsigned char c = *s;
  if (pwc) *pwc = c;
  return c ? 1 : 0;
}

static size_t b_wcrtomb(char *s, wchar_t wc, void *ps) {
  (void)ps;
  if (!s) return 1;
  if ((unsigned)wc > 0xFF) {
    errno = EILSEQ;
    return (size_t)-1;
  }
  *s = (char)wc;
  return 1;
}

static wint_t b_getwc(FILE *f) { int c = getc(b_file(f)); return c == EOF ? WEOF : (wint_t)c; }
static wint_t b_putwc(wchar_t w, FILE *f) { return fputc((int)w, b_file(f)) == EOF ? WEOF : (wint_t)w; }
static wint_t b_ungetwc(wint_t w, FILE *f) { return ungetc((int)w, b_file(f)) == EOF ? WEOF : w; }

static size_t b_wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const b_tm *tm) {
  return wcsftime(s, max, fmt, (const struct tm *)tm);
}

EXPORT_TABLE(wchar) = {
  EXP_AS("btowc", b_btowc),
  EXP_AS("wctob", b_wctob),
  EXP_AS("mbrtowc", b_mbrtowc),
  EXP_AS("wcrtomb", b_wcrtomb),
  EXP_AS("getwc", b_getwc),
  EXP_AS("putwc", b_putwc),
  EXP_AS("ungetwc", b_ungetwc),
  EXP_AS("wcsftime", b_wcsftime),
  EXP(wcscoll), EXP(wcsxfrm), EXP(wcslen),
  EXP(wctype), EXP(iswctype), EXP(towlower), EXP(towupper),
  EXP(wmemchr), EXP(wmemcmp), EXP(wmemcpy), EXP(wmemmove), EXP(wmemset),
};
EXPORT_TABLE_END(wchar);
