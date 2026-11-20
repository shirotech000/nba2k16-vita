#include <stdio.h>
#include <string.h>
#include "util/paths.h"

// android path prefix -> where we keep it on the vita
static const struct { const char *from; const char *to; } path_map[] = {
  { "/sdcard/Android/obb/" PACKAGE_NAME,           OBB_DIR },
  { "/storage/emulated/0/Android/obb/" PACKAGE_NAME, OBB_DIR },
  { "/mnt/sdcard/Android/obb/" PACKAGE_NAME,       OBB_DIR },
  { "/sdcard/Android/data/" PACKAGE_NAME,          DATA_PATH },
  { "/storage/emulated/0/Android/data/" PACKAGE_NAME, DATA_PATH },
  { "/data/data/" PACKAGE_NAME "/files",           FILES_DIR },
  { "/data/data/" PACKAGE_NAME "/cache",           CACHE_DIR },
  { "/data/data/" PACKAGE_NAME,                    DATA_PATH },
  { "/data/user/0/" PACKAGE_NAME,                  DATA_PATH },
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
  // anything else absolute just gets dumped under our data dir
  snprintf(out, outlen, "%s%s", DATA_PATH, in);
  return out;
}
