// shader cache pack: every loose .gxp/.g2c file in one pack.bin, opened once
// at boot and read with sceIoPread by offset. warm boots and menu screen
// changes used to do hundreds of card opens (a few ms each, hw measured
// ~30ms per program link) -> seconds of lag per screen. the pack is built
// on the vita whenever it's missing or a new cache file showed up
// (pack.dirty), during boot before the game starts. new entries still land
// as loose files, the pack never gets written while the game runs.
//
// format (all little endian):
//   pack_hdr { u32 magic "NPK1", u32 version, u32 count, u32 index_size }
//   count entries: { u32 name_len (incl NUL), char name[name_len],
//                   u64 offset, u32 size }
//   names strictly strcmp-ascending (bisect lookups), offsets absolute.
//   blobs: the raw bytes of the loose cache file, headers included.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "util/log.h"
#include "util/paths.h"

#define PACK_PATH    SHADER_CACHE "/pack.bin"
#define PACK_TMP     SHADER_CACHE "/pack.tmp"
#define PACK_DIRTY   SHADER_CACHE "/pack.dirty"
#define PACK_MAGIC   0x314B504Eu  // "NPK1"
#define PACK_VERSION 1
#define PACK_NAME_MAX 80
#define PACK_MAX_BLOB (4 * 1024 * 1024)

typedef struct { uint32_t magic, version, count, index_size; } pack_hdr;

static SceUID pack_fd = -1;
static uint8_t *pack_idx;
static uint32_t pack_count;

typedef struct { const char *name; uint64_t off; uint32_t size; } pack_ent;
static pack_ent *pack_ents;

static void pack_reset(void)
{
  if (pack_fd >= 0) sceIoClose(pack_fd);
  pack_fd = -1;
  free(pack_idx);
  pack_idx = NULL;
  free(pack_ents);
  pack_ents = NULL;
  pack_count = 0;
}

// load + validate pack.bin. any problem -> pack stays disabled, the loose
// files work exactly like before
void pack_init(void)
{
  pack_reset();
  SceUID fd = sceIoOpen(PACK_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  pack_hdr h;
  if (sceIoRead(fd, &h, sizeof(h)) != sizeof(h) || h.magic != PACK_MAGIC ||
      h.version != PACK_VERSION || !h.count || h.count > 64 * 1024 ||
      !h.index_size || h.index_size > 8 * 1024 * 1024) goto bad;
  pack_idx = malloc(h.index_size);
  if (!pack_idx || sceIoRead(fd, pack_idx, h.index_size) != (SceSSize)h.index_size) goto bad;
  SceIoStat st;
  if (sceIoGetstat(PACK_PATH, &st) < 0) goto bad;
  pack_ents = calloc(h.count, sizeof(*pack_ents));
  if (!pack_ents) goto bad;
  {
    uint8_t *p = pack_idx, *end = pack_idx + h.index_size;
    const char *prev = NULL;
    for (uint32_t i = 0; i < h.count; i++) {
      if (p + 18 > end) goto bad;   // smallest possible entry
      uint32_t nl;
      memcpy(&nl, p, 4);
      p += 4;
      if (nl < 2 || nl > PACK_NAME_MAX || p + nl + 12 > end) goto bad;
      pack_ents[i].name = (const char *)p;
      if (pack_ents[i].name[nl - 1]) goto bad;   // must be NUL terminated
      p += nl;
      memcpy(&pack_ents[i].off, p, 8);
      p += 8;
      memcpy(&pack_ents[i].size, p, 4);
      p += 4;
      if (!pack_ents[i].size || pack_ents[i].size > PACK_MAX_BLOB ||
          pack_ents[i].off + pack_ents[i].size > (uint64_t)st.st_size) goto bad;
      if (prev && strcmp(prev, pack_ents[i].name) >= 0) goto bad;   // strictly ascending
      prev = pack_ents[i].name;
    }
    if (p != end) goto bad;   // index must end exactly
  }
  pack_count = h.count;
  pack_fd = fd;
  L("shader: pack.bin has %u entries", pack_count);
  return;
bad:
  L("shader: pack.bin invalid, loose files only");
  pack_reset();
  sceIoClose(fd);
}

// one blob by cache path (the loose file's basename). NULL = not in there
void *pack_read(const char *cache_path, uint32_t *size_out)
{
  if (pack_fd < 0 || !pack_count) return NULL;
  const char *name = strrchr(cache_path, '/');
  name = name ? name + 1 : cache_path;
  uint32_t lo = 0, hi = pack_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    int c = strcmp(pack_ents[mid].name, name);
    if (c == 0) {
      uint32_t sz = pack_ents[mid].size;
      void *buf = malloc(sz);
      if (!buf) return NULL;
      if (sceIoPread(pack_fd, buf, sz, (SceOff)pack_ents[mid].off) != (SceSSize)sz) {
        free(buf);
        return NULL;
      }
      *size_out = sz;
      return buf;
    }
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return NULL;
}

// the loose writers call this whenever a new cache file lands: next boot
// folds it into the pack
void pack_mark_dirty(void)
{
  SceUID fd = sceIoOpen(PACK_DIRTY, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) sceIoClose(fd);
}

// ---- rebuilding the pack from the loose files (boot phase only) ----

typedef struct { char name[PACK_NAME_MAX]; uint32_t size; } loose_ent;

static int loose_ok(const char *n)
{
  size_t l = strlen(n);
  if (l + 1 > sizeof(((loose_ent *)0)->name)) return 0;
  if (l < 6) return 0;
  if (!strcmp(n + l - 4, ".bad") || !strcmp(n + l - 4, ".tmp")) return 0;
  if (!strcmp(n + l - 4, ".cg")) return 0;   // .compiling markers are .cg files
  if (!strncmp(n, "pack", 4) || !strncmp(n, "manifest", 8) || !strncmp(n, "g2cstamp", 8)) return 0;
  return !strcmp(n + l - 4, ".gxp") || !strcmp(n + l - 4, ".g2c");
}

static int loose_cmp(const void *a, const void *b)
{
  return strcmp(((const loose_ent *)a)->name, ((const loose_ent *)b)->name);
}

// rebuild if the pack is missing or a new cache file appeared. runs before
// the game starts, single threaded, memory still cheap
void pack_maybe_build(void)
{
  SceIoStat st;
  int need = sceIoGetstat(PACK_PATH, &st) < 0;
  if (!need) need = sceIoGetstat(PACK_DIRTY, &st) >= 0;
  if (!need) return;

  SceUID dh = sceIoDopen(SHADER_CACHE);
  if (dh < 0) return;
  loose_ent *files = NULL;
  uint32_t n = 0, cap = 0;
  SceIoDirent de;
  while (1) {
    memset(&de, 0, sizeof(de));
    SceSSize r = sceIoDread(dh, &de);
    if (r <= 0) break;
    if (de.d_stat.st_size <= 0 || de.d_stat.st_size > PACK_MAX_BLOB) continue;
    if (!loose_ok(de.d_name)) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 512;
      loose_ent *nf = realloc(files, cap * sizeof(*files));
      if (!nf) break;
      files = nf;
    }
    strcpy(files[n].name, de.d_name);
    files[n].size = (uint32_t)de.d_stat.st_size;
    n++;
  }
  sceIoClose(dh);
  if (!n) { sceIoRemove(PACK_DIRTY); free(files); return; }
  qsort(files, n, sizeof(*files), loose_cmp);

  // index first (offsets known after sorting), then the blobs
  uint64_t off = sizeof(pack_hdr) + 4 * n;   // hdr + name_len fields
  for (uint32_t i = 0; i < n; i++) off += strlen(files[i].name) + 1 + 12;
  SceUID out = sceIoOpen(PACK_TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (out < 0) { free(files); return; }
  pack_hdr h = { PACK_MAGIC, PACK_VERSION, n, 0 };
  h.index_size = (uint32_t)(off - sizeof(pack_hdr));
  sceIoWrite(out, &h, sizeof(h));
  uint64_t blob_off = off;
  int bad = 0;
  for (uint32_t i = 0; i < n && !bad; i++) {
    uint32_t nl = (uint32_t)strlen(files[i].name) + 1;
    sceIoWrite(out, &nl, 4);
    sceIoWrite(out, files[i].name, nl);
    sceIoWrite(out, &blob_off, 8);
    sceIoWrite(out, &files[i].size, 4);
    blob_off += files[i].size;
  }
  for (uint32_t i = 0; i < n && !bad; i++) {
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", SHADER_CACHE, files[i].name);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { bad = 1; break; }
    void *buf = malloc(files[i].size);
    if (buf && sceIoRead(fd, buf, files[i].size) == (SceSSize)files[i].size &&
        sceIoWrite(out, buf, files[i].size) == (SceSSize)files[i].size) {
      // fine
    } else bad = 1;
    free(buf);
    sceIoClose(fd);
  }
  sceIoClose(out);
  free(files);
  if (bad) {
    L("shader: pack build failed, loose files only");
    sceIoRemove(PACK_TMP);
    return;
  }
  sceIoRemove(PACK_PATH);
  sceIoRename(PACK_TMP, PACK_PATH);
  sceIoRemove(PACK_DIRTY);
  L("shader: packed %u cache files", n);
  pack_init();
}
