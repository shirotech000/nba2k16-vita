#pragma once
// ADTS frame splitter. bytes go in whatever chunks they come in, whole
// frames come out. plain C, no vita bits, so it builds on the host too.
#include <stdint.h>

#define ADTS_MIN_HEADER 7
#define ADTS_MAX_FRAME  8191        // 13-bit frame_length
#define ADTS_FIFO       (16 * 1024) // > 2 max frames, so a frame always fits

typedef struct {
  uint32_t frame_len;   // whole frame, header included
  uint32_t header_len;  // 7, or 9 with crc
  uint32_t rate;        // Hz
  uint32_t sr_index;
  uint32_t channels;    // channel_configuration, 0 = defined by a PCE
  uint32_t object_type; // profile + 1: 1 main, 2 LC, 3 SSR, 4 LTP
  uint32_t blocks;      // raw data blocks in the frame, normally 1
} adts_header;

typedef struct {
  uint8_t buf[ADTS_FIFO];
  uint32_t len;
  uint32_t dropped; // junk bytes skipped while hunting for a sync, running total
  int synced;       // last frame ended right where this one should start
  int have_fixed;
  uint8_t fixed[3]; // fixed-header bits of the last good frame, same for a whole stream
} adts_splitter;

// 1 = valid header at p, 0 = need more bytes, -1 = not a header
int adts_parse(const uint8_t *p, uint32_t n, adts_header *h);

void adts_reset(adts_splitter *s);
uint32_t adts_room(const adts_splitter *s);
// copies in as much as fits, returns how much that was
uint32_t adts_push(adts_splitter *s, const void *data, uint32_t n);
// 1 = a whole frame sits at s->buf, described by h. 0 = need more data.
// final = no more bytes are coming (EOS), so don't wait on the next header
int adts_next(adts_splitter *s, adts_header *h, int final);
// drop n bytes off the front (the frame you just used)
void adts_pop(adts_splitter *s, uint32_t n);
// s16 PCM bytes the whole frames sitting in the FIFO will decode to
// (1024 samples per raw block, header channel count). for read-ahead
uint32_t adts_pcm_ahead(const adts_splitter *s);
