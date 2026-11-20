// bionic stdio. the game has its own idea of stdin/out/err (__sF[3],
// 84 bytes each), anything else is a real newlib FILE* we handed out.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bionic/bionic.h"
#include "loader/exports.h"
#include "util/log.h"
#include "util/paths.h"

#define B_FILE_SIZE 84
static uint8_t fake_sF[3 * B_FILE_SIZE];

static int std_index(FILE *f) {
  uint8_t *p = (uint8_t *)f;
  if (p >= fake_sF && p < fake_sF + sizeof(fake_sF)) return (p - fake_sF) / B_FILE_SIZE;
  return -1;
}

FILE *b_file(FILE *f) {
  switch (std_index(f)) {
    case 0: return stdin;
    case 1: return stdout;
    case 2: return stderr;
  }
  return f;
}

// stdout/stderr -> our log, line at a time
static void std_write(int idx, const char *s, size_t n) {
  static char line[2][512];
  static int len[2];
  int k = idx == 2;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\n' || len[k] >= (int)sizeof(line[k]) - 1) {
      line[k][len[k]] = 0;
      L("[%s] %s", k ? "stderr" : "stdout", line[k]);
      len[k] = 0;
      if (s[i] == '\n') continue;
    }
    line[k][len[k]++] = s[i];
  }
}

static FILE *b_fopen(const char *path, const char *mode) {
  char fixed[512];
  path_fix(path, fixed, sizeof(fixed));
  FILE *f = fopen(fixed, mode);
  L("fopen(%s, %s) -> %s", fixed, mode, f ? "ok" : "FAIL");
  return f;
}

static FILE *b_fdopen(int fd, const char *mode) { return fdopen(fd, mode); }
static int b_fclose(FILE *f) { return std_index(f) >= 0 ? 0 : fclose(f); }

static size_t b_fread(void *p, size_t sz, size_t n, FILE *f) { return fread(p, sz, n, b_file(f)); }

static size_t b_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
  int i = std_index(f);
  if (i > 0) { std_write(i, p, sz * n); return n; }
  return fwrite(p, sz, n, b_file(f));
}

static int b_fseek(FILE *f, long off, int whence) { return fseek(b_file(f), off, whence); }
static long b_ftell(FILE *f) { return ftell(b_file(f)); }
static int b_fflush(FILE *f) { return f && std_index(f) < 0 ? fflush(f) : 0; }
static int b_setvbuf(FILE *f, char *b, int mode, size_t sz) {
  return std_index(f) >= 0 ? 0 : setvbuf(f, b, mode, sz);
}

static int b_fputc(int c, FILE *f) {
  int i = std_index(f);
  if (i > 0) { char ch = c; std_write(i, &ch, 1); return c; }
  return fputc(c, b_file(f));
}

static int b_fputs(const char *s, FILE *f) {
  int i = std_index(f);
  if (i > 0) { std_write(i, s, strlen(s)); return 0; }
  return fputs(s, b_file(f));
}

static int b_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r, i = std_index(f);
  if (i > 0) {
    char buf[1024];
    r = vsnprintf(buf, sizeof(buf), fmt, ap);
    std_write(i, buf, r < (int)sizeof(buf) ? r : (int)sizeof(buf) - 1);
  } else {
    r = vfprintf(b_file(f), fmt, ap);
  }
  va_end(ap);
  return r;
}

static int b_getc(FILE *f) { return getc(b_file(f)); }
static int b_putc(int c, FILE *f) { return b_fputc(c, f); }
static int b_ungetc(int c, FILE *f) { return ungetc(c, b_file(f)); }

EXPORT_TABLE(stdio) = {
  EXP_AS("__sF", fake_sF),
  EXP_AS("fopen", b_fopen),
  EXP_AS("fdopen", b_fdopen),
  EXP_AS("fclose", b_fclose),
  EXP_AS("fread", b_fread),
  EXP_AS("fwrite", b_fwrite),
  EXP_AS("fseek", b_fseek),
  EXP_AS("ftell", b_ftell),
  EXP_AS("fflush", b_fflush),
  EXP_AS("setvbuf", b_setvbuf),
  EXP_AS("fputc", b_fputc),
  EXP_AS("fputs", b_fputs),
  EXP_AS("fprintf", b_fprintf),
  EXP_AS("getc", b_getc),
  EXP_AS("putc", b_putc),
  EXP_AS("ungetc", b_ungetc),
};
EXPORT_TABLE_END(stdio);
