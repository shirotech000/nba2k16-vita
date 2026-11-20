#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <psp2/io/stat.h>
#include "loader/so_loader.h"
#include "util/log.h"
#include "util/paths.h"

// android path prefix -> where we keep it on the vita
static const struct { const char *from; const char *to; } path_map[] = {
  { "/sdcard/Android/obb/" PACKAGE_NAME,           OBB_DIR },
  { "/storage/emulated/0/Android/obb/" PACKAGE_NAME, OBB_DIR },
  { "/mnt/sdcard/Android/obb/" PACKAGE_NAME,       OBB_DIR },
  { "/sdcard/Android/data/" PACKAGE_NAME,          EXTERNAL_DIR },
  { "/storage/emulated/0/Android/data/" PACKAGE_NAME, EXTERNAL_DIR },
  { "/data/data/" PACKAGE_NAME "/files",           FILES_DIR },
  { "/data/data/" PACKAGE_NAME "/cache",           CACHE_DIR },
  { "/data/data/" PACKAGE_NAME,                    INTERNAL_DIR },
  { "/data/user/0/" PACKAGE_NAME,                  INTERNAL_DIR },
  { "/sdcard",                                     DATA_PATH "/sdcard" },
  { "/storage/emulated/0",                         DATA_PATH "/sdcard" },
  { "/mnt/sdcard",                                 DATA_PATH "/sdcard" },
};

char *path_fix(const char *in, char *out, int outlen) {
  if (!in) { out[0] = 0; return out; }
  // already a vita path (ux0:, ur0:, etc) or relative -> leave it
  if (in[0] != '/') {
    snprintf(out, outlen, "%s", in);
    return out;
  }
  for (unsigned i = 0; i < sizeof(path_map) / sizeof(path_map[0]); i++) {
    size_t n = strlen(path_map[i].from);
    if (!strncmp(in, path_map[i].from, n) && (in[n] == '/' || in[n] == 0)) {
      snprintf(out, outlen, "%s%s", path_map[i].to, in + n);
      return out;
    }
  }
  // anything else absolute goes under root/, never next to our own files
  snprintf(out, outlen, "%s/root%s", DATA_PATH, in);
  return out;
}

int path_is_bad(const char *p) {
  // a vita path has one ':' and it ends the device name ("ux0:"). a second
  // one, or one after a '/', is junk no vita fs can hold, and vita3k doesn't
  // survive some of those (mkdir of one exits the whole emulator)
  const char *c = strchr(p, ':');
  if (!c) return 0;
  const char *slash = strchr(p, '/');
  if (slash && slash < c) return 1;
  return strchr(c + 1, ':') != NULL;
}

int path_fix_checked(const char *what, const char *in, char *out, int outlen) {
  path_fix(in, out, outlen);
  if (!path_is_bad(out)) return 0;
  static int logged;
  if (logged < 16) {
    L("%s(%s) refused, bad path (game passed %s)", what, out, in);
    if (!logged) so_backtrace("bad path");
    logged++;
  }
  errno = ENOENT;
  return -1;
}

int emu_is_vita3k(void) {
  static int v = -1;
  if (v < 0) {
    SceIoStat st;
    v = sceIoGetstat(DATA_PATH "/vita3k.flag", &st) >= 0;
  }
  return v;
}
