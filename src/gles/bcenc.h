#pragma once
// bc1 / bc3 (dxt1 / dxt5) block encoder + decoder and the etc -> bc level
// transcoder. our own code, written from the public format descriptions.
// no gxm headers, tools/tex_test.c builds it on the host.
#include <stdint.h>
#include "gles/gl_defs.h"

// one 4x4 block of rgba8 pixels (rows `stride` bytes apart) -> 8 bytes bc1.
// punch = 0: opaque, always 4 colour mode (or c0 == c1 with index 0), never
//            index 3, so it reads the same with and without dxt1 alpha.
// punch = 1: texels with alpha < 128 turn into the transparent index (3 colour
//            mode, only in blocks that have such texels)
void bc1_encode(const uint8_t *rgba, unsigned stride, uint8_t out[8], int punch);
// -> 16 bytes bc3: interpolated alpha block, then a 4 colour bc1 block
void bc3_encode(const uint8_t *rgba, unsigned stride, uint8_t out[16]);
// bs = 8 (bc1) or 16 (bc3). spec decode, thirds rounded to nearest.
void bc_decode_block(unsigned bs, const uint8_t *blk, uint8_t *rgba, unsigned stride);

// bc block size an etc format transcodes to: 8, 16, or 0 (r11/rg11, not etc)
unsigned bc_bytes_for_etc(GLenum etc);

// transcode etc blocks covering w x h pixels into a swizzled bc level.
// level = start of the level's memory, gw x gh = its block grid (pow2, see
// tu_bc_dim), blocks land at tu_morton(bx0 + bx, by0 + by, gw, gh). only whole
// 32 bit words get written to level (it's uncached gpu memory on the vita).
// tmp = cached scratch, ((w + 3) & ~3) * 16 bytes.
void bc_from_etc(GLenum etc, const uint8_t *src, unsigned w, unsigned h,
                 uint8_t *level, unsigned gw, unsigned gh, unsigned bx0, unsigned by0,
                 uint8_t *tmp);
// swizzled bc level -> linear rgba8, w x h pixels
void bc_level_to_rgba(unsigned bs, const uint8_t *level, unsigned gw, unsigned gh,
                      unsigned w, unsigned h, uint8_t *dst, unsigned dstride);
