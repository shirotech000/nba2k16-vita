// obb_diet.c - the on-device texture diet. this is the "converter" the
// project always wanted, running where the data lives: at boot, before the
// game loads, against the user's own OBB. no game data ships - only this
// algorithm. every mipped TXTR >= 128KB in every 94EF3BFF archive gets its
// level-0 mip dropped and its declared dims halved; the game's own mip 1
// becomes the new base. one-time (obb_diet.done marker), obb_diet=0 in
// config.txt turns it off, obb_diet_force=1 re-runs it (a no-op on an
// already-dieted obb: the spans fall under the 128K gate).
//
// archive grammar per the iff project (iffs/tex_diet.py, obb_diet_all.py,
// nba2k16_GLM/HANDOFF.md): header {magic FF3BEF94, data_start @+4 (LE),
// total @+8 (BE), block_count @+0x10, dir_count @+0x18}, block table at
// +0x20 (u32 x 0x20/entry), the pointer table + resource records live in
// the raw file from +0x60 to data_start, the two ZLIB-wrapped blocks at
// data_start. resource record: {file_id, type_id, block_index, meta_off}
// + payload_off if block_index==2. TXTR dims are the u16 pair at
// block0[meta_off+0xA0], the payload is a full mip chain: dims*bpp*4/3.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <zlib.h>

#include "util/log.h"
#include "util/paths.h"
#include "game/patches.h"

#define TXTR_CRC 0x5C369069
#define MAX_ENTRY (8 * 1024 * 1024)
#define MIN_SPAN (128 * 1024)
#define DIET_LEVEL 6

static uint32_t ru32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t ru16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wu32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wu16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wbe32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// real sceIoRead/Write return short transfers - loop them
static int rd_full(SceUID fd, void *buf, SceSize n) {
  uint8_t *p = buf;
  while (n) {
    SceSize got = sceIoRead(fd, p, n);
    if (got <= 0) return -1;
    p += got;
    n -= got;
  }
  return 0;
}

static int wr_full(SceUID fd, const void *buf, SceSize n) {
  const uint8_t *p = buf;
  while (n) {
    SceSize put = sceIoWrite(fd, p, n);
    if (put <= 0) return -1;
    p += put;
    n -= put;
  }
  return 0;
}

// two decompressed blocks + the header facts we keep
typedef struct {
  uint8_t *b0, *b1;
  uint32_t b0sz, b1sz;
  uint32_t data_start, dir_count;
  uint32_t ha[2], hb[2], align[2], codec[2];
} arc_t;

static void arc_free(arc_t *a) { free(a->b0); free(a->b1); a->b0 = a->b1 = NULL; }

static int arc_parse(const uint8_t *d, uint32_t n, arc_t *a) {
  memset(a, 0, sizeof(*a));
  if (n < 0x90 || ru32(d) != 0xFF3BEF94) return -1;
  a->data_start = ru32(d + 0x04);
  uint32_t block_count = ru32(d + 0x10);
  a->dir_count = ru32(d + 0x18);
  if (block_count != 2 || a->data_start >= n) return -1;
  for (int i = 0; i < 2; i++) {
    const uint8_t *e = d + 0x20 + i * 0x20;
    a->ha[i] = ru32(e); a->hb[i] = ru32(e + 4); a->align[i] = ru32(e + 8);
    a->codec[i] = ru32(e + 0x10);
    uint32_t off = ru32(e + 0x14), ssz = ru32(e + 0x18);
    if ((uint64_t)off + ssz > n || memcmp(d + off, "ZLIB", 4) != 0) {
      arc_free(a);   // b0 (h7-block archives land here at i=1) must not leak
      return -1;
    }
    uint32_t dsz = (uint32_t)d[off + 4] << 24 | (uint32_t)d[off + 5] << 16 |
                   (uint32_t)d[off + 6] << 8 | d[off + 7];
    if (!dsz || dsz > 32u * 1024 * 1024) {
      arc_free(a);
      return -1;
    }
    uint8_t *out = malloc(dsz);
    if (!out) {
      arc_free(a);
      return -1;
    }
    uLongf got = dsz;
    // the original files' wrapper csz is not the true length: hand inflate
    // the whole stored span (ssz-16) as capacity, it stops at stream end
    if (uncompress(out, &got, d + off + 16, ssz - 16) != Z_OK || got != dsz) {
      free(out);
      arc_free(a);
      return -1;
    }
    if (i == 0) { a->b0 = out; a->b0sz = dsz; } else { a->b1 = out; a->b1sz = dsz; }
  }
  return 0;
}

// the single TXTR record with a payload, from the raw file's directory
// region. 0 = found (meta_off into b0, payload_off into b1), -1 = no
// the single-TXTR rule was the diet's blind spot: the heavy arenas carry
// 8-99 textures each and none of them qualified. the v2 collects every
// block-2 record (all types - spans need the neighbors as boundaries) and
// diets every qualifying TXTR independently, rebasing the record offsets
// after each cut
typedef struct {
  uint32_t file_pos;   // the record's own location in the raw file
  uint32_t type;
  uint32_t meta;      // into b0
  uint32_t po;        // into b1 (its payload_off field is at file_pos+16)
} b2rec;

#define MAX_B2 512

static int collect_b2(const uint8_t *d, const arc_t *a, b2rec *out, int cap) {
  uint32_t pos = 0x60 + a->dir_count * 4;
  int n = 0;
  for (uint32_t i = 0; i < a->dir_count && pos + 16 <= a->data_start; i++) {
    uint32_t type_id = ru32(d + pos + 4);
    uint32_t blk = ru32(d + pos + 8);
    uint32_t mo = ru32(d + pos + 12);
    pos += 16;
    if (blk == 2) {
      if (pos + 4 > a->data_start) break;
      if (n < cap) {
        out[n].file_pos = pos - 16;
        out[n].type = type_id;
        out[n].meta = mo;
        out[n].po = ru32(d + pos);
        n++;
      }
      pos += 4;
    } else if (blk != 1) {
      break;
    }
  }
  return n;
}

static int fits_span(uint32_t w, uint32_t span) {
  for (uint32_t bpp = 2; bpp <= 4; bpp += 2) {
    // the floored chain sum is ~1 over the exact span: either side within 8
    uint64_t want = (uint64_t)w * w * bpp * 4 / 3;
    uint64_t diff = want > span ? want - span : span - want;
    if (diff < 8) return (int)bpp;
  }
  return 0;
}

// the window version of fits_span: the v1-era originals could carry
// alignment padding that leaves a span a few thousand bytes off the exact
// chain (the crowd record sits 1.9K short of its halved chain). a 4%
// window still can't confuse the halved chain with the full one - they
// sit a factor of 4 apart
static int fits_span_loose(uint32_t w, uint32_t span) {
  for (uint32_t bpp = 2; bpp <= 4; bpp += 2) {
    uint64_t want = (uint64_t)w * w * bpp * 4 / 3;
    uint64_t slack = want / 25;
    if (span >= want - slack && span <= want + slack) return (int)bpp;
  }
  return 0;
}

// the diet's depth: 2 = drop one mip level (halve), 4 = drop two (quarter).
// the deeper the cut, the more game-heap bytes the match load keeps
static int obb_diet_scale(void) {
  static int s = -1;
  if (s < 0) {
    extern int config_int(const char *key, int def);
    s = config_int("obb_diet_scale", 2);
    if (s != 4) s = 2;
    if (s == 4) L("obb diet: scale 4, dropping two mip levels per texture");
  }
  return s;
}

static int diet_one(SceUID fd, uint32_t idx, uint32_t size, uint32_t crc, uint32_t sector,
                    uint32_t *saved_out, int *funnel, int repair) {
  if (size > MAX_ENTRY) return 0;
  uint8_t head[4];
  sceIoLseek(fd, (SceOff)sector * 2048, SCE_SEEK_SET);
  if (rd_full(fd, head, 4) < 0) {
    funnel[0]++;
    if (funnel[6]++ < 3) L("obb diet: entry %u head read failed at 0x%X", idx, sector * 2048);
    return 0;
  }
  if (memcmp(head, "\x94\xEF\x3B\xFF", 4) != 0) return 0;
  funnel[1]++;
  uint8_t *d = malloc(size);
  if (!d) return 0;
  sceIoLseek(fd, (SceOff)sector * 2048, SCE_SEEK_SET);
  if (rd_full(fd, d, size) < 0) {
    funnel[2]++;
    if (funnel[6]++ < 3) L("obb diet: entry %u full read failed (%u bytes at 0x%X)", idx, size, sector * 2048);
    free(d);
    return 0;
  }
  funnel[3]++;

  arc_t a;
  if (arc_parse(d, size, &a) < 0) { free(d); return 0; }
  funnel[4]++;
  b2rec recs[MAX_B2];
  int nrec = collect_b2(d, &a, recs, MAX_B2);
  uint8_t *out = NULL, *z0 = NULL, *z1 = NULL;
  int ret = 0;
  uint32_t freed = 0, ndieted = 0;

  // the surgery: qualifying TXTRs from the end of b1 backward, so each
  // cut's rebase can't disturb the cuts before it
  for (int ri = nrec - 1; ri >= 0; ri--) {
    b2rec *r = &recs[ri];
    if (r->type != TXTR_CRC || r->meta + 0xA4 > a.b0sz || r->po >= a.b1sz) continue;
    funnel[5]++;
    // the span: this record's po to the next bigger po (or b1's end)
    uint32_t span = a.b1sz - r->po;
    for (int j = 0; j < nrec; j++)
      if (recs[j].po > r->po && recs[j].po - r->po < span) span = recs[j].po - r->po;
    uint16_t w = ru16(a.b0 + r->meta + 0xA0), h = ru16(a.b0 + r->meta + 0xA2);
    if (repair) {
      // the v1 diet cut the payload but never wrote the halved dims back.
      // the game's own lod-offset math (the crowd context build) walks the
      // raw record fields and dies on the mismatch. the strict +-8 span
      // test missed the v1's products entirely (hw63: their spans carry
      // the padded originals' slack), so the window version takes over -
      // and every mismatched record gets a census line either way: that's
      // the on-device ground truth for what the v1 actually left behind
      if (w == h && (w == 256 || w == 512 || w == 1024 || w == 2048)) {
        int own = fits_span_loose(w, span);
        int half = fits_span_loose(w / 2, span);
        int quar = fits_span_loose(w / 4, span);
        if (!own) {
          if (funnel[6]++ < 40)
            L("obb repair: entry %u tex %ux%u span %u half %d quar %d", idx, w, h, span, half, quar);
          if (half || quar) {
            uint32_t nw = half ? w / 2 : w / 4;
            wu16(a.b0 + r->meta + 0xA0, (uint16_t)nw);
            wu16(a.b0 + r->meta + 0xA2, (uint16_t)nw);
            ndieted++;
          }
        }
      }
      continue;
    }
    int bpp = fits_span(w, span);
    if (!(w == h && (w == 256 || w == 512 || w == 1024 || w == 2048) && span >= MIN_SPAN && bpp)) {
      if (funnel[6]++ < 3 && span >= MIN_SPAN)
        L("obb diet: entry %u tex near-miss: dims %ux%u span %u bpp %d", idx, w, h, span, bpp);
      continue;
    }
    uint32_t scl = (uint32_t)obb_diet_scale();
    uint32_t nw = w / scl, nh = h / scl;
    uint32_t drop = w * h * (uint32_t)bpp;
    if (scl == 4) drop += (w / 2) * (h / 2) * (uint32_t)bpp;
    if (r->po + drop > a.b1sz) continue;
    // patch the dims in b0, excise the top mip level(s) from b1 in place
    wu16(a.b0 + r->meta + 0xA0, (uint16_t)nw);
    wu16(a.b0 + r->meta + 0xA2, (uint16_t)nh);
    memmove(a.b1 + r->po, a.b1 + r->po + drop, a.b1sz - r->po - drop);
    a.b1sz -= drop;
    freed += drop;
    ndieted++;
    // rebase every record after this cut: the local table and the raw
    // file's payload_off fields (the directory region rides in the gap)
    for (int j = 0; j < nrec; j++)
      if (recs[j].po > r->po) {
        recs[j].po -= drop;
        wu32(d + recs[j].file_pos + 16, recs[j].po);
      }
  }

  if (ndieted) {
    // b0 always recompresses (the repair only ever touches it, the diet
    // patched it too). b1 recompresses only for the diet - in repair mode
    // the original compressed block rides along verbatim: the v1-era
    // originals are packed tighter than our level 6, so recompressing b1
    // blew every rebuilt archive past its span cap and the first repair
    // healed nothing for exactly that reason (hw64: detected, rebuilt,
    // rejected)
    uLongf z0cap = compressBound(a.b0sz);
    z0 = malloc(z0cap + 16);
    if (!z0) goto done;
    uLongf z0n = z0cap;
    if (compress2(z0 + 16, &z0n, a.b0, a.b0sz, repair ? 9 : DIET_LEVEL) != Z_OK) goto done;
    memcpy(z0, "ZLIB", 4);
    wbe32(z0 + 4, a.b0sz); wbe32(z0 + 8, (uint32_t)z0n); wu32(z0 + 12, 0);
    // the untouched b1: its original compressed block, whole
    uint32_t ob1_off = ru32(d + 0x40 + 0x14), ob1_ssz = ru32(d + 0x40 + 0x18);
    uLongf z1n = 0;
    if (!repair) {
      uLongf z1cap = compressBound(a.b1sz);
      z1 = malloc(z1cap + 16);
      if (!z1) goto done;
      z1n = z1cap;
      if (compress2(z1 + 16, &z1n, a.b1, a.b1sz, DIET_LEVEL) != Z_OK) goto done;
      memcpy(z1, "ZLIB", 4);
      wbe32(z1 + 4, a.b1sz); wbe32(z1 + 8, (uint32_t)z1n); wu32(z1 + 12, 0);
    }
    uint32_t gap_len = a.data_start - 0x60;
    uint32_t total = a.data_start + (uint32_t)(16 + z0n) +
                     (repair ? ob1_ssz : (uint32_t)(16 + z1n));
    // must fit the entry's real span: the file rounds each entry up to a
    // 2048 boundary, so the tail of the last sector is slack the repair
    // can grow into when the recompressed b0 shifts a few bytes
    uint32_t cap = (size + 2047) & ~2047u;
    if (total >= cap) goto done;
    out = malloc(total);
    if (!out) goto done;
    memcpy(out, d, 0x20);
    wu32(out + 0x04, a.data_start);
    wbe32(out + 0x08, total);
    uint8_t *te = out + 0x20;
    uint32_t b1off = a.data_start + (uint32_t)(16 + z0n);
    for (int i = 0; i < 2; i++, te += 0x20) {
      uint32_t dsz = i == 0 ? a.b0sz : a.b1sz;
      uint32_t off = i == 0 ? a.data_start : b1off;
      uint32_t ssz = repair && i != 0 ? ob1_ssz : (uint32_t)(i == 0 ? z0n : z1n) + 16;
      wu32(te, a.ha[i]); wu32(te + 4, a.hb[i]); wu32(te + 8, a.align[i]);
      wu32(te + 12, dsz); wu32(te + 16, a.codec[i]); wu32(te + 20, off);
      wu32(te + 24, ssz); wu32(te + 28, 0);
    }
    memcpy(out + 0x60, d + 0x60, gap_len);   // the rebased records ride along
    memcpy(out + a.data_start, z0, 16 + z0n);
    if (repair) memcpy(out + b1off, d + ob1_off, ob1_ssz);
    else memcpy(out + b1off, z1, 16 + z1n);
    // verify: the assembled archive must parse, and every dieted texture
    // must come back with a consistent span for its scaled dims
    {
      arc_t v;
      if (arc_parse(out, total, &v) < 0) {
        L("obb diet: entry %u failed verification, skipping", idx);
        arc_free(&v);
        goto done;
      }
      b2rec vr[MAX_B2];
      int vn = collect_b2(out, &v, vr, MAX_B2);
      uint32_t consistent = 0;
      for (int j = 0; j < vn; j++) {
        if (vr[j].type != TXTR_CRC || vr[j].meta + 0xA4 > v.b0sz || vr[j].po >= v.b1sz) continue;
        uint32_t span = v.b1sz - vr[j].po;
        for (int k = 0; k < vn; k++)
          if (vr[k].po > vr[j].po && vr[k].po - vr[j].po < span) span = vr[k].po - vr[j].po;
        uint16_t vw = ru16(v.b0 + vr[j].meta + 0xA0), vh = ru16(v.b0 + vr[j].meta + 0xA2);
        // loose: the v1 products carry packer padding that the strict +-8
        // rejects, and the healed records inherit it
        if (fits_span_loose(vw, span) && vw == vh) consistent++;
      }
      arc_free(&v);
      if (consistent < ndieted) {
        L("obb diet: entry %u verification: %u consistent < %u dieted, skipping", idx, consistent, ndieted);
        goto done;
      }
    }
    // write the data first, the TOC last
    sceIoLseek(fd, (SceOff)sector * 2048, SCE_SEEK_SET);
    if (wr_full(fd, out, total) < 0) {
      L("obb diet: entry %u write failed", idx);
      goto done;
    }
    uint8_t toc[16];
    wu32(toc, total); wu32(toc + 4, 0); wu32(toc + 8, crc); wu32(toc + 12, sector);
    sceIoLseek(fd, 0x800 + (SceOff)idx * 16, SCE_SEEK_SET);
    if (wr_full(fd, toc, 16) < 0) {
      L("obb diet: entry %u TOC write failed", idx);
      goto done;
    }
    L("obb diet: entry %u (crc %08X) %u textures dieted, %uK freed, %u -> %u bytes",
      idx, crc, ndieted, freed / 1024, size, total);
    *saved_out += size - total;
    ret = 1;
  }
done:
  free(out); free(z0); free(z1);
  arc_free(&a);
  free(d);
  return ret;
}

// undo the crowd archive's diet: its 1397 per-spectator metadata records
// carry baked offsets into the payload, so the excision shifted the data
// under them and no dims heal can fix prebaked tables (hw65: the load's
// tail died at 98% in the stored-table path, a raw table base). the donor
// (the pristine archive extract, pushed from the pc as crowd_pristine.iff)
// lands back in the entry's original sector - the diet shrank the entry in
// place, so the span after it is dead space by construction. only the
// toc's size word changes, the crc and sector stay
static void obb_restore_run(void) {
  SceIoStat st;
  if (sceIoGetstat(DATA_PATH "/obb_restore.done", &st) >= 0) return;
  SceUID df = sceIoOpen(DATA_PATH "/crowd_pristine.iff", SCE_O_RDONLY, 0);
  if (df < 0) {
    L("obb restore: no donor at " DATA_PATH "/crowd_pristine.iff, skipping");
    return;
  }
  SceOff dn = sceIoLseek(df, 0, SCE_SEEK_END);
  sceIoLseek(df, 0, SCE_SEEK_SET);
  if (dn <= 0 || dn > 8 * 1024 * 1024) {
    sceIoClose(df);
    L("obb restore: donor size weird (%lld)", (long long)dn);
    return;
  }
  uint32_t dsz = (uint32_t)dn;
  uint8_t *donor = malloc(dsz);
  if (!donor) { sceIoClose(df); return; }
  if (rd_full(df, donor, dsz) < 0 || memcmp(donor, "\x94\xEF\x3B\xFF", 4) != 0) {
    free(donor); sceIoClose(df);
    L("obb restore: donor read or magic failed");
    return;
  }
  sceIoClose(df);
  SceUID fd = sceIoOpen(obb_resolve(), SCE_O_RDWR, 0);
  if (fd < 0) { free(donor); L("obb restore: no obb at %s", obb_resolve()); return; }
  uint8_t *toc = malloc(3500 * 16);
  if (!toc) { free(donor); sceIoClose(fd); return; }
  sceIoLseek(fd, 0x800, SCE_SEEK_SET);
  if (rd_full(fd, toc, 3500 * 16) < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb restore: toc read failed");
    return;
  }
  // find the crowd entry by its crc - the diet never changed the toc crcs
  int found = -1;
  uint32_t last = 0;
  for (uint32_t i = 0; i < 3500; i++) {
    if (ru32(toc + i * 16 + 4) != 0) break;
    uint32_t crc = ru32(toc + i * 16 + 8);
    if (crc < last) break;
    last = crc;
    if (crc == 0x1E947EE5) { found = (int)i; break; }
  }
  if (found < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb restore: crowd entry (crc 1E947EE5) not found");
    return;
  }
  uint32_t old_size = ru32(toc + found * 16);
  uint32_t sector = ru32(toc + found * 16 + 12);
  uint32_t need = (dsz + 2047) / 2048;
  // nobody else may live inside the span we're about to fill
  for (uint32_t i = 0; i < 3500; i++) {
    if (ru32(toc + i * 16 + 4) != 0) break;
    if ((int)i == found) continue;
    uint32_t sz = ru32(toc + i * 16), sec = ru32(toc + i * 16 + 12);
    if (!sz || !sec) continue;
    uint32_t end = sec + (sz + 2047) / 2048;
    if (sec < sector + need && end > sector) {
      free(donor); free(toc); sceIoClose(fd);
      L("obb restore: entry %u (sector %u..%u) overlaps the target span, aborting", i, sec, end);
      return;
    }
  }
  sceIoLseek(fd, (SceOff)sector * 2048, SCE_SEEK_SET);
  if (wr_full(fd, donor, dsz) < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb restore: data write failed");
    return;
  }
  wu32(toc + found * 16, dsz);
  sceIoLseek(fd, 0x800 + (SceOff)found * 16, SCE_SEEK_SET);
  if (wr_full(fd, toc + found * 16, 16) < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb restore: toc patch failed");
    return;
  }
  free(donor); free(toc); sceIoClose(fd);
  L("obb restore: crowd entry %d, %u -> %u bytes at sector %u, toc patched",
    found, old_size, dsz, sector);
  SceUID m = sceIoOpen(DATA_PATH "/obb_restore.done",
                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (m >= 0) { sceIoWrite(m, "crowd restored\n", 14); sceIoClose(m); }
}

void obb_diet_run(void) {
  // the crowd archive's diet reversal runs first if asked: it changes the
  // obb itself, before the scan-based passes get their say
  if (config_int("obb_diet_restore", 0)) obb_restore_run();
  // repair v3 marker: each version bump re-arms the pass after the
  // previous one's marker (v1 of the repair healed nothing - the +-8 span
  // test missed the padding; v2 detected but its rebuild was rejected -
  // our level-6 recompress outgrew the span). a fixed pass must never be
  // locked out by its predecessor's marker
  int repair = config_int("obb_diet_repair", 0);
  if (!repair && !config_int("obb_diet", 1)) return;
  const char *marker = repair ? DATA_PATH "/obb_repair3.done" : DATA_PATH "/obb_diet.done";
  SceIoStat st;
  if (sceIoGetstat(marker, &st) >= 0 && !config_int("obb_diet_force", 0))
    return;

  SceUID fd = sceIoOpen(obb_resolve(), SCE_O_RDWR, 0);
  if (fd < 0) {
    L("obb diet: no obb at %s (0x%08X)", obb_resolve(), fd);
    return;
  }
  uint8_t *toc = malloc(3500 * 16);
  if (!toc) { sceIoClose(fd); return; }
  sceIoLseek(fd, 0x800, SCE_SEEK_SET);
  if (rd_full(fd, toc, 3500 * 16) < 0) {
    L("obb diet: toc read failed");
    free(toc);
    sceIoClose(fd);
    return;
  }
  uint32_t count = 3500, last = 0, n_entries = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (ru32(toc + i * 16 + 4) != 0) break;             // the hi word
    uint32_t crc = ru32(toc + i * 16 + 8);
    if (crc < last) break;                              // sorted ascending
    last = crc;
    n_entries++;
  }
  L("obb diet: %u toc entries, scanning", n_entries);

  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  uint32_t dieted = 0, saved = 0;
  int funnel[7] = {0};   // headfail, magic, readfail, parsed, txtr, qualified, anomaly log budget
  // the first-boot UI: draws after the display's up (main.c runs us there)
  extern void bootscreen_progress(const char *h, const char *l, uint32_t d, uint32_t t) __attribute__((weak));
  extern void gl_boot_ensure(void);
  if (bootscreen_progress) gl_boot_ensure();
  SceUInt64 last_draw = 0;
  char line[96];
  for (uint32_t i = 0; i < n_entries; i++) {
    uint32_t size = ru32(toc + i * 16);
    uint32_t crc = ru32(toc + i * 16 + 8);
    uint32_t sector = ru32(toc + i * 16 + 12);
    if (diet_one(fd, i, size, crc, sector, &saved, funnel, repair)) dieted++;
    if (bootscreen_progress) {
      SceUInt64 now = sceKernelGetProcessTimeWide();
      if (now - last_draw > 250 * 1000 || i + 1 == n_entries) {
        snprintf(line, sizeof(line), "%s  %u / %u",
                 repair ? "REPAIRING GAME DATA" : "OPTIMIZING GAME DATA", i + 1, n_entries);
        bootscreen_progress("PREPARING GAME DATA", line, i + 1, n_entries);
        last_draw = now;
      }
    }
  }
  L("obb diet funnel: headfail %d, magic %d, readfail %d, parsed %d, txtr %d, qualified %d",
    funnel[0], funnel[1], funnel[2], funnel[3], funnel[4], funnel[5]);
  free(toc);
  sceIoClose(fd);
  SceUInt64 ms = (sceKernelGetProcessTimeWide() - t0) / 1000;
  L("obb diet: %u archives dieted, %uK saved, %u.%u s",
    dieted, saved / 1024, (unsigned)(ms / 1000), (unsigned)(ms % 1000));

  if (dieted) {
    SceUID m = sceIoOpen(marker,
                         SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (m >= 0) {
      char buf[128];
      snprintf(buf, sizeof(buf), "%u archives, %uK saved\n", dieted, saved / 1024);
      sceIoWrite(m, buf, strlen(buf));
      sceIoClose(m);
    }
  } else {
    // nothing to do - still mark it so we never scan again
    SceUID m = sceIoOpen(marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (m >= 0) { sceIoWrite(m, "nothing to do\n", 13); sceIoClose(m); }
    L("obb diet: nothing qualified, marked done");
  }
}
