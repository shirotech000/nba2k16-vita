// ADTS splitter, written from the ISO 14496-3 ADTS header layout:
//  syncword 12 | id 1 | layer 2 | protection_absent 1 | profile 2 | sf_index 4 |
//  private 1 | channel_cfg 3 | orig 1 | home 1 | copyright id/start 2 |
//  frame_length 13 | buffer_fullness 11 | raw_blocks 2 | (crc 16)
#include <string.h>
#include "android/aac_adts.h"

static const uint32_t sr_table[13] = {
  96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

// sync + layer 00. id can be either (mpeg-4 or mpeg-2)
static inline int is_sync(const uint8_t *p) {
  return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
}

int adts_parse(const uint8_t *p, uint32_t n, adts_header *h) {
  if (n < 2) return 0;
  if (!is_sync(p)) return -1;
  if (n < ADTS_MIN_HEADER) return 0;
  uint32_t sri = (p[2] >> 2) & 0xF;
  if (sri >= 13) return -1;
  h->header_len = (p[1] & 1) ? 7 : 9;
  h->object_type = (p[2] >> 6) + 1;
  h->sr_index = sri;
  h->rate = sr_table[sri];
  h->channels = ((p[2] & 1) << 2) | (p[3] >> 6);
  h->frame_len = ((uint32_t)(p[3] & 3) << 11) | ((uint32_t)p[4] << 3) | (p[5] >> 5);
  h->blocks = (p[6] & 3) + 1;
  if (h->frame_len <= h->header_len) return -1;
  return 1;
}

// id, layer, protection_absent | profile, sf_index, private, ch hi | ch lo, orig, home
static inline int same_fixed(const uint8_t *a, const uint8_t *b) {
  return a[0] == b[0] && a[1] == b[1] && (a[2] & 0xF0) == (b[2] & 0xF0);
}

// start out hunting: the first frame only counts once the second lines up
// behind it (or the stream ends). costs one frame of lookahead.
void adts_reset(adts_splitter *s) {
  s->len = 0;
  s->synced = 0;
  s->have_fixed = 0;
}

uint32_t adts_room(const adts_splitter *s) {
  return ADTS_FIFO - s->len;
}

uint32_t adts_push(adts_splitter *s, const void *data, uint32_t n) {
  uint32_t room = ADTS_FIFO - s->len;
  if (n > room) n = room;
  memcpy(s->buf + s->len, data, n);
  s->len += n;
  return n;
}

void adts_pop(adts_splitter *s, uint32_t n) {
  if (n >= s->len) {
    s->len = 0;
  } else {
    memmove(s->buf, s->buf + n, s->len - n);
    s->len -= n;
  }
  s->synced = 1;
}

uint32_t adts_pcm_ahead(const adts_splitter *s) {
  uint32_t off = 0, bytes = 0;
  adts_header h;
  while (adts_parse(s->buf + off, s->len - off, &h) == 1 && off + h.frame_len <= s->len) {
    bytes += 1024 * h.blocks * (h.channels ? h.channels : 2) * 2;
    off += h.frame_len;
  }
  return bytes;
}

static void drop(adts_splitter *s, uint32_t n) {
  adts_pop(s, n);
  s->dropped += n;
  s->synced = 0;
}

int adts_next(adts_splitter *s, adts_header *h, int final) {
  for (;;) {
    int r = adts_parse(s->buf, s->len, h);
    if (r == 0) return 0;
    if (r < 0) {
      // junk, skip ahead to the next 0xFF
      const uint8_t *ff = s->len > 1 ? memchr(s->buf + 1, 0xFF, s->len - 1) : NULL;
      drop(s, ff ? (uint32_t)(ff - s->buf) : s->len);
      continue;
    }
    if (s->len < h->frame_len) {
      if (!final) return 0; // rest still on its way
      drop(s, 1);           // EOS, it never will be
      continue;
    }

    // same stream as before (back to back, or found again after junk):
    // sync + 28 fixed-header bits agreeing is plenty
    int trusted = s->have_fixed ? same_fixed(s->buf + 1, s->fixed) : s->synced;
    if (!trusted) {
      // fresh stream, or after junk with a header that changed. 0xFFF turns
      // up in random data plenty, so the next frame has to line up behind it
      if (s->len >= h->frame_len + 4) {
        const uint8_t *n = s->buf + h->frame_len;
        if (!is_sync(n) || !same_fixed(n + 1, s->buf + 1)) {
          drop(s, 1);
          continue;
        }
      } else if (!final) {
        return 0;
      } else if (s->len != h->frame_len) {
        drop(s, 1); // last one has to end right at the end
        continue;
      }
    }
    memcpy(s->fixed, s->buf + 1, 3);
    s->have_fixed = 1;
    return 1;
  }
}
