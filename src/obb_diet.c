// obb_diet.c - on-device OBB surgery, rewritten from the validated grammar.
// runs once at boot (obb_diet.done marker), before the game loads, against
// the user's own OBB. no game data ships - only this algorithm.
//
// OBB container: TOC at 0x800, 16B entries {u32 size, u32 hi(0), u32 name_crc
// ascending, u32 sector}; entries live at 2048B sector boundaries.
//
// IFF archive (magic 0xFF3BEF94 LE): data_start u32 LE @+4, total u32 BE @+8,
// block_count @+0x10, dir_count @+0x18, block table @+0x20 (0x20/entry:
// ha, hb, align, dsz, codec, off, ssz, res). a block is "ZLIB" + BE dsz +
// BE csz + u32, then the raw zlib stream. the record area [0x60, data_start)
// holds a pointer table at T (first entry dir_count*4+1, ptr[i] =
// R_i-(T-1)-4*i) then the records, flush at data_start. record: {file_id,
// type_id, block_index, meta_off} 16B, + payload_off u32 when block_index==2.
//
// TXTR meta (in b0, 0xC0 bytes): format enum @+0x08 (1=RGBA8, 2=RGB565,
// 16=L8/A8), dims u16 pair @+0xA0/+0xA2, LOD-bank offset @+0x88 (companion
// 0x7F @+0x8C). TXTR payload (in b1) is a raw mip chain, w*h*bpp*4/3 bytes.
//
// the diet: every qualifying TXTR gets its top mip level(s) excised from b1
// and its dims scaled down - the game's own next mip becomes the new base.
// scene archives (any SCNE record) are never cut: their metadata carries
// baked payload offsets (hw65), they go through the donor channel instead.
//
// every absolute b1 offset is rebased with the strictly-before rule:
//   - record payload_off fields
//   - the head descriptor region [0x60, T) - packed (count<<16|tag) fields
//     (low half 0x80..0x83) are data and never shift (hw86's 2.7GB ask)
//   - b0 LOD-bank pointers into the tail span [max_po, b1sz) - the last
//     payload record's span when it isn't a texture - except meta data slots
// nothing else moves: data_start and the pointer table stay put, so a rebase
// can't corrupt the head. the rebuilt archive is fully re-parsed and verified
// in memory before a single byte hits the OBB; any doubt skips the entry.
//
// config.txt: obb_diet=0 disables, obb_diet_force=1 re-arms the marker,
// obb_diet_scale=2|4 (mip levels dropped per texture), obb_diet_min_span.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>
#include <zlib.h>

#include "util/log.h"
#include "util/paths.h"
#include "game/patches.h"

#define TXTR_CRC 0x5C369069u
#define SCNE_CRC 0xE26C9B5Du
#define MAX_ENTRY (8u * 1024 * 1024)
#define MAX_RECS 2048
#define MAX_BANK 8192
#define TOC_OFF 0x800
#define TOC_MAX 3500
#define SECTOR 2048
#define NO_PO 0xFFFFFFFFu

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

// ---- the archive ----------------------------------------------------------

typedef struct {
  uint8_t *b0, *b1;
  uint32_t b0sz, b1sz;
  uint32_t data_start, dir_count;
  uint32_t ha[2], hb[2], align[2], codec[2];
} arc_t;

static void arc_free(arc_t *a) { free(a->b0); free(a->b1); a->b0 = a->b1 = NULL; }

// strict: magic, exactly 2 ZLIB blocks, wrapper dsz sane, inflate == dsz
static int arc_parse(const uint8_t *d, uint32_t n, arc_t *a) {
  memset(a, 0, sizeof(*a));
  if (n < 0x90 || ru32(d) != 0xFF3BEF94u) return -1;
  a->data_start = ru32(d + 0x04);
  uint32_t block_count = ru32(d + 0x10);
  a->dir_count = ru32(d + 0x18);
  if (block_count != 2 || a->data_start >= n || a->data_start < 0x60) return -1;
  for (int i = 0; i < 2; i++) {
    const uint8_t *e = d + 0x20 + i * 0x20;
    a->ha[i] = ru32(e); a->hb[i] = ru32(e + 4); a->align[i] = ru32(e + 8);
    a->codec[i] = ru32(e + 0x10);
    uint32_t off = ru32(e + 0x14), ssz = ru32(e + 0x18);
    if ((uint64_t)off + ssz > n || memcmp(d + off, "ZLIB", 4) != 0) {
      arc_free(a);
      return -1;
    }
    // the wrapper's stored csz overruns the stream on some originals - hand
    // inflate the whole span as capacity, it stops at stream end
    uint32_t dsz = (uint32_t)d[off + 4] << 24 | (uint32_t)d[off + 5] << 16 |
                   (uint32_t)d[off + 6] << 8 | d[off + 7];
    if (!dsz || dsz > 48u * 1024 * 1024) {
      arc_free(a);
      return -1;
    }
    uint8_t *out = malloc(dsz);
    if (!out) {
      arc_free(a);
      return -1;
    }
    uLongf got = dsz;
    if (uncompress(out, &got, d + off + 16, ssz - 16) != Z_OK || got != dsz) {
      free(out);
      arc_free(a);
      return -1;
    }
    if (i == 0) { a->b0 = out; a->b0sz = dsz; } else { a->b1 = out; a->b1sz = dsz; }
  }
  return 0;
}

// ---- the records ----------------------------------------------------------

typedef struct {
  uint32_t pos;    // the record's own file offset
  uint32_t type, blk, meta, po;   // po = NO_PO when blk != 2
} rec_t;

// locate the pointer table by its signature and validate the whole walk:
// every pointer resolves, records end flush at data_start. returns the
// record count, or -1. *T_out gets the table offset.
static int walk_records(const uint8_t *d, const arc_t *a, rec_t *recs, int cap,
                        uint32_t *T_out) {
  if (a->dir_count == 0 || (int)a->dir_count > cap) return -1;
  uint32_t want = a->dir_count * 4 + 1;
  for (uint32_t T = 0x60; T + 4 <= a->data_start; T += 4) {
    if (ru32(d + T) != want) continue;
    uint32_t pos = T + a->dir_count * 4;
    int n = 0, ok = 1;
    for (uint32_t i = 0; i < a->dir_count; i++) {
      if (pos + 12 > a->data_start) { ok = 0; break; }
      if (ru32(d + T + 4 * i) != pos - (T - 1) - 4 * i) { ok = 0; break; }
      rec_t r;
      r.pos = pos;
      r.type = ru32(d + pos + 4);
      r.blk = ru32(d + pos + 8);
      r.meta = ru32(d + pos + 12);
      r.po = NO_PO;
      if (r.blk == 2) {
        if (pos + 20 > a->data_start) { ok = 0; break; }
        r.po = ru32(d + pos + 16);
        pos += 20;
      } else if (r.blk == 1) {
        pos += 16;
      } else if (r.blk == 0) {
        pos += 12;
      } else {
        ok = 0;
        break;
      }
      if (n < cap) recs[n] = r;
      n++;
    }
    if (!ok || pos != a->data_start) continue;
    *T_out = T;
    return n;
  }
  return -1;
}

// the payload span of a block-2 record: to the next bigger po, else b1's end
static uint32_t rec_span(const rec_t *recs, int n, int ri, uint32_t b1sz) {
  uint32_t span = b1sz - recs[ri].po;
  for (int j = 0; j < n; j++)
    if (recs[j].blk == 2 && recs[j].po > recs[ri].po && recs[j].po - recs[ri].po < span)
      span = recs[j].po - recs[ri].po;
  return span;
}

// the mip chain is w*h*bpp*4/3; originals carry packer padding, so a 4%
// window (a halved chain sits a factor of 4 away - no confusion)
static int fits_span_loose(uint32_t w, uint32_t h, uint32_t span) {
  for (uint32_t bpp = 2; bpp <= 4; bpp += 2) {
    uint64_t want = (uint64_t)w * h * bpp * 4 / 3;
    uint64_t slack = want / 25;
    if (span >= want - slack && span <= want + slack) return (int)bpp;
  }
  return 0;
}

static int is_pow2(uint32_t v) { return v >= 64 && v <= 2048 && (v & (v - 1)) == 0; }

// ---- the cut plan ---------------------------------------------------------

typedef struct {
  uint32_t po, drop;      // excise [po, po+drop) from b1
  uint32_t meta;          // b0 dims slot to scale
  uint16_t nw, nh;
} cut_t;

// strictly-before shift: total drop of all cuts starting before v
static uint32_t shift_of(const cut_t *cuts, int ncut, uint32_t v) {
  uint32_t dl = 0;
  for (int i = 0; i < ncut; i++)
    if (cuts[i].po < v) dl += cuts[i].drop;
  return dl;
}

static int cut_cmp(const void *a, const void *b) {
  uint32_t pa = ((const cut_t *)a)->po, pb = ((const cut_t *)b)->po;
  return pa < pb ? -1 : pa > pb ? 1 : 0;
}

// ---- one OBB entry --------------------------------------------------------

static int diet_entry(SceUID fd, uint32_t idx, uint32_t size, uint32_t crc,
                      uint32_t sector, uint32_t *saved, int *funnel) {
  if (size > MAX_ENTRY || size < 0x90) return 0;
  sceIoLseek(fd, (SceOff)sector * SECTOR, SCE_SEEK_SET);
  uint8_t head[4];
  if (rd_full(fd, head, 4) < 0 || memcmp(head, "\x94\xEF\x3B\xFF", 4) != 0) {
    funnel[0]++;
    return 0;
  }
  funnel[1]++;
  uint8_t *d = malloc(size);
  if (!d) return 0;
  sceIoLseek(fd, (SceOff)sector * SECTOR, SCE_SEEK_SET);
  if (rd_full(fd, d, size) < 0) {
    funnel[2]++;
    free(d);
    return 0;
  }
  funnel[3]++;

  arc_t a;
  if (arc_parse(d, size, &a) < 0) {
    free(d);
    return 0;
  }
  funnel[4]++;

  rec_t *recs = malloc(MAX_RECS * sizeof(rec_t));
  uint32_t T = 0x60;
  int nrec = walk_records(d, &a, recs, MAX_RECS, &T);
  if (nrec < 0) {
    L("obb diet: entry %u (crc %08X) record walk failed, skipping", idx, crc);
    free(recs);
    arc_free(&a);
    free(d);
    return 0;
  }

  // scene archives carry baked payload offsets in their metadata (hw65) -
  // cutting them kills the stored-table path. donor channel only.
  int scene = 0;
  for (int i = 0; i < nrec; i++)
    if (recs[i].type == SCNE_CRC) { scene = 1; break; }
  if (scene) {
    free(recs);
    arc_free(&a);
    free(d);
    return 0;
  }

  int scale = config_int("obb_diet_scale", 2);
  if (scale != 4) scale = 2;
  uint32_t min_span = (uint32_t)config_int("obb_diet_min_span", 128 * 1024);

  cut_t *cuts = malloc(MAX_RECS * sizeof(cut_t));
  int ncut = 0;
  for (int i = 0; i < nrec; i++) {
    if (recs[i].type != TXTR_CRC || recs[i].blk != 2) continue;
    if (recs[i].meta + 0xC0 > a.b0sz || recs[i].po >= a.b1sz) continue;
    funnel[5]++;
    uint32_t span = rec_span(recs, nrec, i, a.b1sz);
    uint16_t w = ru16(a.b0 + recs[i].meta + 0xA0), h = ru16(a.b0 + recs[i].meta + 0xA2);
    int bpp = fits_span_loose(w, h, span);
    if (!(is_pow2(w) && is_pow2(h) && span >= min_span && bpp)) {
      if (span >= min_span && funnel[6]++ < 3)
        L("obb diet: entry %u near-miss %ux%u span %u bpp %d", idx, w, h, span, bpp);
      continue;
    }
    uint32_t drop = (uint32_t)w * h * (uint32_t)bpp;
    if (scale == 4) drop += (uint32_t)(w / 2) * (h / 2) * (uint32_t)bpp;
    if (drop >= span) continue;
    cuts[ncut].po = recs[i].po;
    cuts[ncut].drop = drop;
    cuts[ncut].meta = recs[i].meta;
    cuts[ncut].nw = (uint16_t)(w / scale);
    cuts[ncut].nh = (uint16_t)(h / scale);
    ncut++;
  }
  if (ncut == 0) {
    free(cuts);
    free(recs);
    arc_free(&a);
    free(d);
    return 0;
  }
  qsort(cuts, (size_t)ncut, sizeof(cut_t), cut_cmp);
  uint32_t first_cut = cuts[0].po;
  uint32_t sum_drop = 0;
  for (int i = 0; i < ncut; i++) sum_drop += cuts[i].drop;

  // new b1: the kept segments between the cuts
  uint32_t nb1sz = a.b1sz - sum_drop;
  uint8_t *nb1 = malloc(nb1sz);
  if (!nb1) goto bail;
  {
    uint32_t cur = 0, w = 0;
    for (int i = 0; i < ncut; i++) {
      memcpy(nb1 + w, a.b1 + cur, cuts[i].po - cur);
      w += cuts[i].po - cur;
      cur = cuts[i].po + cuts[i].drop;
    }
    memcpy(nb1 + w, a.b1 + cur, a.b1sz - cur);
  }

  // dims in b0
  for (int i = 0; i < ncut; i++) {
    wu16(a.b0 + cuts[i].meta + 0xA0, cuts[i].nw);
    wu16(a.b0 + cuts[i].meta + 0xA2, cuts[i].nh);
  }

  // b0 LOD-bank pointers: every u32 in b0 whose value lands in the tail span
  // [tail_start, b1sz) is a pointer into the last payload record's payload
  // (the LOD-bank store) - unless it's meta data. the tail exists only when
  // the last payload record isn't itself a texture.
  uint32_t tail_start = a.b1sz;
  {
    int lasti = -1;
    for (int i = 0; i < nrec; i++)
      if (recs[i].blk == 2 && (lasti < 0 || recs[i].po > recs[lasti].po)) lasti = i;
    if (lasti >= 0 && recs[lasti].type != TXTR_CRC) tail_start = recs[lasti].po;
  }
  // meta extents: 1 = inside a TXTR meta, 2 = meta data slot, 3 = the +0x88 slot
  uint8_t *mflag = calloc(a.b0sz, 1);
  if (!mflag) goto bail;
  for (int i = 0; i < nrec; i++) {
    if (recs[i].type != TXTR_CRC || recs[i].meta + 0xC0 > a.b0sz) continue;
    memset(mflag + recs[i].meta, 1, 0xC0);
    mflag[recs[i].meta + 0x00] = 2;   // name crc can land in-tail numerically
    mflag[recs[i].meta + 0xA0] = 2;   // packed dims pair - data, not a pointer
    mflag[recs[i].meta + 0xA4] = 2;
    mflag[recs[i].meta + 0x88] = 3;   // the LOD-bank offset slot
  }
  typedef struct { uint32_t p, oldv, newv; } bank_t;
  bank_t *bank = malloc(MAX_BANK * sizeof(bank_t));
  int nbank = 0, abort_arc = 0;
  if (tail_start < a.b1sz) {
    for (uint32_t p = 0; p + 4 <= a.b0sz; p += 4) {
      uint32_t v = ru32(a.b0 + p);
      int intail = (v >= tail_start && v < a.b1sz) ||
                   ((v & 1) && v >= tail_start + 1 && v - 1 < a.b1sz);
      if (!intail) continue;
      if (mflag[p] == 2) continue;
      if (mflag[p] == 1) { abort_arc = 1; break; }   // in-tail meta value that isn't the bank slot: human review
      if (mflag[p] == 3 && ru32(a.b0 + p + 4) != 0x7F) { abort_arc = 1; break; }
      if (nbank < MAX_BANK) {
        bank[nbank].p = p;
        bank[nbank].oldv = v;
        bank[nbank].newv = v - shift_of(cuts, ncut, v);
        nbank++;
      }
    }
  }
  if (abort_arc) {
    L("obb diet: entry %u (crc %08X) unexpected in-tail meta value, skipping", idx, crc);
    free(mflag); free(bank); free(nb1); free(cuts); free(recs);
    arc_free(&a); free(d);
    return 0;
  }
  for (int i = 0; i < nbank; i++)
    wu32(a.b0 + bank[i].p, bank[i].newv);

  // recompress both blocks
  uLongf z0cap = compressBound(a.b0sz);
  uint8_t *z0 = malloc(z0cap + 16);
  uLongf z1cap = compressBound(nb1sz);
  uint8_t *z1 = malloc(z1cap + 16);
  if (!z0 || !z1) {
    free(z0); free(z1); free(mflag); free(bank); free(nb1);
    goto bail;
  }
  uLongf z0n = z0cap, z1n = z1cap;
  if (compress2(z0 + 16, &z0n, a.b0, a.b0sz, 9) != Z_OK ||
      compress2(z1 + 16, &z1n, nb1, nb1sz, 6) != Z_OK) {
    free(z0); free(z1); free(mflag); free(bank); free(nb1);
    goto bail;
  }
  memcpy(z0, "ZLIB", 4); wbe32(z0 + 4, a.b0sz); wbe32(z0 + 8, (uint32_t)z0n); wu32(z0 + 12, 0);
  memcpy(z1, "ZLIB", 4); wbe32(z1 + 4, nb1sz); wbe32(z1 + 8, (uint32_t)z1n); wu32(z1 + 12, 0);

  uint32_t total = a.data_start + (uint32_t)(16 + z0n) + (uint32_t)(16 + z1n);
  uint32_t cap = (size + SECTOR - 1) & ~(uint32_t)(SECTOR - 1);
  if (total >= cap) {
    L("obb diet: entry %u rebuild %u >= span cap %u, skipping", idx, total, cap);
    free(z0); free(z1); free(mflag); free(bank); free(nb1);
    goto bail;
  }

  // assemble: header + block table + head (with rebases) + the two blocks.
  // data_start never moves, so the pointer table needs no repair
  uint8_t *out = malloc(total);
  if (!out) {
    free(z0); free(z1); free(mflag); free(bank); free(nb1);
    goto bail;
  }
  memcpy(out, d, 0x20);
  wu32(out + 0x04, a.data_start);
  wbe32(out + 0x08, total);
  {
    uint32_t dszv[2] = { a.b0sz, nb1sz };
    uint32_t offv[2] = { a.data_start, a.data_start + (uint32_t)(16 + z0n) };
    uint32_t sszv[2] = { (uint32_t)(16 + z0n), (uint32_t)(16 + z1n) };
    for (int i = 0; i < 2; i++) {
      uint8_t *te = out + 0x20 + i * 0x20;
      wu32(te, a.ha[i]); wu32(te + 4, a.hb[i]); wu32(te + 8, a.align[i]);
      wu32(te + 12, dszv[i]); wu32(te + 16, a.codec[i]); wu32(te + 20, offv[i]);
      wu32(te + 24, sszv[i]); wu32(te + 28, 0);
    }
  }
  memcpy(out + 0x60, d + 0x60, a.data_start - 0x60);
  // record payload_off fields
  for (int i = 0; i < nrec; i++) {
    if (recs[i].blk != 2) continue;
    uint32_t nv = recs[i].po - shift_of(cuts, ncut, recs[i].po);
    if (nv != recs[i].po) wu32(out + recs[i].pos + 16, nv);
  }
  // head descriptor region [0x60, T): genuine b1 offsets shift, packed
  // (count<<16|tag) fields (low half 0x80..0x83, count >= 0x20) never do
  for (uint32_t off = 0x60; off + 4 <= T; off += 4) {
    uint32_t v = ru32(out + off);
    if (v < first_cut || v >= a.b1sz) continue;
    uint32_t lo = v & 0xFFFF;
    if (lo >= 0x80 && lo <= 0x83 && (v >> 16) >= 0x20) continue;
    uint32_t dl = shift_of(cuts, ncut, v);
    if (dl) wu32(out + off, v - dl);
  }
  memcpy(out + a.data_start, z0, 16 + z0n);
  memcpy(out + a.data_start + 16 + z0n, z1, 16 + z1n);

  // ---- verify the rebuild in memory before touching the OBB ----
  {
    arc_t v;
    int bad = 0;
    if (arc_parse(out, total, &v) < 0) {
      L("obb diet: entry %u rebuild doesn't parse, skipping", idx);
      bad = 1;
    } else {
      rec_t *vr = malloc(MAX_RECS * sizeof(rec_t));
      uint32_t vT = 0x60;
      int vn = walk_records(out, &v, vr, MAX_RECS, &vT);
      if (vn != nrec || vT != T) {
        L("obb diet: entry %u walk mismatch (%d/%u T %#x/%#x), skipping", idx, vn, nrec, vT, T);
        bad = 1;
      } else {
        // every record's po must equal the planned shifted value
        for (int i = 0; i < vn && !bad; i++) {
          if (vr[i].blk != 2) continue;
          uint32_t want = NO_PO;
          for (int j = 0; j < nrec; j++)
            if (recs[j].pos == vr[i].pos) { want = recs[j].po - shift_of(cuts, ncut, recs[j].po); break; }
          if (want != NO_PO && vr[i].po != want) bad = 1;
        }
        // every TXTR's span must fit its (new) dims
        int consistent = 0;
        for (int i = 0; i < vn && !bad; i++) {
          if (vr[i].type != TXTR_CRC || vr[i].blk != 2) continue;
          if (vr[i].meta + 0xC0 > v.b0sz || vr[i].po >= v.b1sz) { bad = 1; break; }
          uint32_t span = rec_span(vr, vn, i, v.b1sz);
          uint16_t w = ru16(v.b0 + vr[i].meta + 0xA0), h = ru16(v.b0 + vr[i].meta + 0xA2);
          if (fits_span_loose(w, h, span)) consistent++;
          else if (funnel[6]++ < 10)
            L("obb diet: entry %u verify: tex %ux%u span %u inconsistent", idx, w, h, span);
        }
        if (!bad && consistent < ncut) {
          L("obb diet: entry %u verify: %d consistent < %d cut, skipping", idx, consistent, ncut);
          bad = 1;
        }
        // bank contents must be byte-identical at the shifted targets
        for (int i = 0; i < nbank && !bad; i++) {
          uint32_t ov = bank[i].oldv, nv = bank[i].newv;
          uint32_t n = 8;
          if (ov + n > a.b1sz) n = a.b1sz - ov;
          if (nv + n > v.b1sz || n < 4 || memcmp(a.b1 + ov, v.b1 + nv, n) != 0) {
            L("obb diet: entry %u bank slot %#x content moved, skipping", idx, bank[i].p);
            bad = 1;
          }
        }
      }
      free(vr);
      arc_free(&v);
    }
    if (bad) {
      free(out); free(z0); free(z1); free(mflag); free(bank); free(nb1);
      goto bail;
    }
  }

  // write the data first, the TOC last
  sceIoLseek(fd, (SceOff)sector * SECTOR, SCE_SEEK_SET);
  if (wr_full(fd, out, total) < 0) {
    L("obb diet: entry %u data write failed", idx);
    free(out); free(z0); free(z1); free(mflag); free(bank); free(nb1);
    goto bail;
  }
  {
    uint8_t toc[16];
    wu32(toc, total); wu32(toc + 4, 0); wu32(toc + 8, crc); wu32(toc + 12, sector);
    sceIoLseek(fd, TOC_OFF + (SceOff)idx * 16, SCE_SEEK_SET);
    if (wr_full(fd, toc, 16) < 0) {
      L("obb diet: entry %u TOC write failed", idx);
      free(out); free(z0); free(z1); free(mflag); free(bank); free(nb1);
      goto bail;
    }
  }
  L("obb diet: entry %u (crc %08X) %d textures cut, %uK freed, %u -> %u bytes",
    idx, crc, ncut, sum_drop / 1024, size, total);
  *saved += size - total;
  free(out); free(z0); free(z1); free(mflag); free(bank); free(nb1);
  free(cuts); free(recs);
  arc_free(&a);
  free(d);
  return 1;

bail:
  free(cuts); free(recs);
  arc_free(&a);
  free(d);
  return 0;
}

// ---- the donor channel ----------------------------------------------------
// any donor_<8hex>.iff in the data dir replaces the whole OBB entry the crc
// names (the PC-built resizes - crowd scene etc.). the donor lands at the
// entry's original sector, only the TOC size word changes. a marker records
// the applied donor's size+fnv, so re-pushing a changed donor re-applies.

static uint32_t donor_fnv(const uint8_t *d, uint32_t n) {
  uint32_t h = 0x811C9DC5;
  while (n--) { h ^= *d++; h *= 0x01000193; }
  return h;
}

static void obb_donor_apply(const char *path, uint32_t crc) {
  SceUID df = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (df < 0) return;
  SceOff dn = sceIoLseek(df, 0, SCE_SEEK_END);
  sceIoLseek(df, 0, SCE_SEEK_SET);
  if (dn <= 0 || dn > (SceOff)MAX_ENTRY) {
    sceIoClose(df);
    L("obb donor: %s size weird (%lld), skipping", path, (long long)dn);
    return;
  }
  uint32_t dsz = (uint32_t)dn;
  uint8_t *donor = malloc(dsz);
  if (!donor) { sceIoClose(df); return; }
  if (rd_full(df, donor, dsz) < 0 || memcmp(donor, "\x94\xEF\x3B\xFF", 4) != 0) {
    free(donor); sceIoClose(df);
    L("obb donor: %s read or magic failed", path);
    return;
  }
  sceIoClose(df);
  uint32_t fnv = donor_fnv(donor, dsz);

  char mk[64];
  snprintf(mk, sizeof(mk), DATA_PATH "/obb_donor_%08X.done", crc);
  SceUID mf = sceIoOpen(mk, SCE_O_RDONLY, 0);
  if (mf >= 0) {
    char prev[64];
    SceSSize got = sceIoRead(mf, prev, sizeof(prev) - 1);
    sceIoClose(mf);
    if (got > 0) {
      prev[got] = 0;
      uint32_t psz = 0, pfn = 0;
      if (sscanf(prev, "%u %x", &psz, &pfn) == 2 && psz == dsz && pfn == fnv) {
        free(donor);
        return;  // unchanged donor, already applied
      }
    }
  }

  SceUID fd = sceIoOpen(obb_resolve(), SCE_O_RDWR, 0);
  if (fd < 0) { free(donor); L("obb donor: no obb at %s", obb_resolve()); return; }
  uint8_t *toc = malloc(TOC_MAX * 16);
  if (!toc) { free(donor); sceIoClose(fd); return; }
  sceIoLseek(fd, TOC_OFF, SCE_SEEK_SET);
  if (rd_full(fd, toc, TOC_MAX * 16) < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb donor: toc read failed");
    return;
  }
  int found = -1;
  uint32_t last = 0;
  for (uint32_t i = 0; i < TOC_MAX; i++) {
    if (ru32(toc + i * 16 + 4) != 0) break;
    uint32_t c = ru32(toc + i * 16 + 8);
    if (c < last) break;
    last = c;
    if (c == crc) { found = (int)i; break; }
  }
  if (found < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb donor: entry (crc %08X) not found", crc);
    return;
  }
  uint32_t old_size = ru32(toc + found * 16);
  uint32_t sec = ru32(toc + found * 16 + 12);
  uint32_t need = (dsz + SECTOR - 1) / SECTOR;
  // nobody else may live inside the span we're about to fill
  for (uint32_t i = 0; i < TOC_MAX; i++) {
    if (ru32(toc + i * 16 + 4) != 0) break;
    if ((int)i == found) continue;
    uint32_t sz = ru32(toc + i * 16), sc = ru32(toc + i * 16 + 12);
    if (!sz || !sc) continue;
    uint32_t end = sc + (sz + SECTOR - 1) / SECTOR;
    if (sc < sec + need && end > sec) {
      free(donor); free(toc); sceIoClose(fd);
      L("obb donor: entry %u (sector %u..%u) overlaps, aborting", i, sc, end);
      return;
    }
  }
  sceIoLseek(fd, (SceOff)sec * SECTOR, SCE_SEEK_SET);
  if (wr_full(fd, donor, dsz) < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb donor: data write failed");
    return;
  }
  wu32(toc + found * 16, dsz);
  sceIoLseek(fd, TOC_OFF + (SceOff)found * 16, SCE_SEEK_SET);
  if (wr_full(fd, toc + found * 16, 16) < 0) {
    free(donor); free(toc); sceIoClose(fd);
    L("obb donor: toc patch failed");
    return;
  }
  free(donor); free(toc); sceIoClose(fd);
  L("obb donor: crc %08X entry %d, %u -> %u bytes at sector %u, toc patched",
    crc, found, old_size, dsz, sec);
  SceUID m = sceIoOpen(mk, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (m >= 0) {
    char note[64];
    int wl = snprintf(note, sizeof(note), "%u %08x\n", dsz, fnv);
    sceIoWrite(m, note, wl);
    sceIoClose(m);
  }
}

static void obb_donor_scan(void) {
  SceUID df = sceIoDopen(DATA_PATH);
  if (df < 0) return;
  SceIoDirent de;
  while (1) {
    memset(&de, 0, sizeof(de));
    if (sceIoDread(df, &de) <= 0) break;
    const char *dn = de.d_name;
    size_t len = strlen(dn);
    if (len != 18 || memcmp(dn, "donor_", 6) != 0 || memcmp(dn + 14, ".iff", 4) != 0)
      continue;
    uint32_t crc = 0;
    int ok = 1;
    for (int i = 0; i < 8; i++) {
      char c = dn[6 + i];
      crc <<= 4;
      if (c >= '0' && c <= '9') crc |= c - '0';
      else if (c >= 'a' && c <= 'f') crc |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') crc |= c - 'A' + 10;
      else { ok = 0; break; }
    }
    if (!ok) continue;
    char path[300];
    snprintf(path, sizeof(path), DATA_PATH "/%s", dn);
    obb_donor_apply(path, crc);
  }
  sceIoDclose(df);
}

// ---- the entry point ------------------------------------------------------

void obb_diet_run(void) {
  // donors first: a pushed donor is the intent, the markers make it
  // self-limiting, and the diet then sees the replaced entries as-is
  obb_donor_scan();

  if (!config_int("obb_diet", 1)) return;
  SceIoStat st;
  if (sceIoGetstat(DATA_PATH "/obb_diet.done", &st) >= 0 && !config_int("obb_diet_force", 0))
    return;

  SceUID fd = sceIoOpen(obb_resolve(), SCE_O_RDWR, 0);
  if (fd < 0) {
    L("obb diet: no obb at %s (0x%08X)", obb_resolve(), fd);
    return;
  }
  uint8_t *toc = malloc(TOC_MAX * 16);
  if (!toc) { sceIoClose(fd); return; }
  sceIoLseek(fd, TOC_OFF, SCE_SEEK_SET);
  if (rd_full(fd, toc, TOC_MAX * 16) < 0) {
    L("obb diet: toc read failed");
    free(toc);
    sceIoClose(fd);
    return;
  }
  uint32_t last = 0, n_entries = 0;
  for (uint32_t i = 0; i < TOC_MAX; i++) {
    if (ru32(toc + i * 16 + 4) != 0) break;             // the hi word
    uint32_t crc = ru32(toc + i * 16 + 8);
    if (crc < last) break;                              // sorted ascending
    last = crc;
    n_entries++;
  }
  L("obb diet: %u toc entries, scanning", n_entries);

  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  uint32_t dieted = 0, saved = 0;
  int funnel[7] = {0};   // magicfail, iff, readfail, readok, parsed, txtr, log budget
  extern void bootscreen_progress(const char *h, const char *l, uint32_t d, uint32_t t) __attribute__((weak));
  extern void gl_boot_ensure(void);
  if (bootscreen_progress) gl_boot_ensure();
  SceUInt64 last_draw = 0;
  for (uint32_t i = 0; i < n_entries; i++) {
    uint32_t size = ru32(toc + i * 16);
    uint32_t crc = ru32(toc + i * 16 + 8);
    uint32_t sector = ru32(toc + i * 16 + 12);
    if (diet_entry(fd, i, size, crc, sector, &saved, funnel)) dieted++;
    if (bootscreen_progress) {
      SceUInt64 now = sceKernelGetProcessTimeWide();
      if (now - last_draw > 250 * 1000 || i + 1 == n_entries) {
        char line[96];
        snprintf(line, sizeof(line), "OPTIMIZING GAME DATA  %u / %u", i + 1, n_entries);
        bootscreen_progress("PREPARING GAME DATA", line, i + 1, n_entries);
        last_draw = now;
      }
    }
  }
  L("obb diet funnel: magicfail %d iff %d readfail %d readok %d parsed %d txtr %d",
    funnel[0], funnel[1], funnel[2], funnel[3], funnel[4], funnel[5]);
  free(toc);
  sceIoClose(fd);
  SceUInt64 ms = (sceKernelGetProcessTimeWide() - t0) / 1000;
  L("obb diet: %u archives dieted, %uK saved, %u.%u s",
    dieted, saved / 1024, (unsigned)(ms / 1000), (unsigned)(ms % 1000));

  SceUID m = sceIoOpen(DATA_PATH "/obb_diet.done", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (m >= 0) {
    char buf[128];
    int wl = snprintf(buf, sizeof(buf), "%u archives, %uK saved\n", dieted, saved / 1024);
    sceIoWrite(m, buf, wl);
    sceIoClose(m);
  }
}
