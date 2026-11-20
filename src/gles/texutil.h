#pragma once
// cpu-side texture helpers: gxm swizzle order, layout sizes, mip box filter.
// plain C with no gxm/gl headers so tools/tex_test.c can build it on the host.
#include <stdint.h>

// gxm swizzled / cube texel order (pow2 w,h). morton with y in bit 0 and x in
// bit 1. on non-square textures the leftover high bits of the longer axis sit
// above the interleaved part. same order vita3k unswizzles with.
uint32_t tu_morton(unsigned x, unsigned y, unsigned w, unsigned h);

// copy a w x h rect of linear pixels into a swizzled lw x lh level
void tu_swizzle_rect(uint8_t *level, unsigned lw, unsigned lh, unsigned bpp,
                     unsigned x0, unsigned y0, unsigned w, unsigned h,
                     const uint8_t *src, unsigned src_stride);
// whole swizzled level -> linear
void tu_unswizzle(const uint8_t *level, unsigned lw, unsigned lh, unsigned bpp,
                  uint8_t *dst, unsigned dst_stride);

// layout sizes (bpp in bytes)
// linear level: rows padded to 8 pixels
uint32_t tu_linear_level_size(unsigned w, unsigned h, unsigned bpp);
// one cube face incl. its whole chain down to 1x1, padded to gxm's face alignment
uint32_t tu_cube_face_size(unsigned size, unsigned bpp);
unsigned tu_log2(unsigned v);          // floor
unsigned tu_pow2_ceil(unsigned v);
static inline int tu_is_pow2(unsigned v) { return v && !(v & (v - 1)); }

// mip box filter. kinds describe the texel bits, not the channel meaning.
enum {
  TU_BOX_NONE = 0,
  TU_BOX_8,      // 1 byte per texel
  TU_BOX_88,     // 2 bytes, 2 x 8 bit
  TU_BOX_8888,   // 4 bytes, 4 x 8 bit (any channel order)
  TU_BOX_565,    // 16 bit packed
  TU_BOX_4444,
  TU_BOX_5551,
};
// src sw x sh -> dst dw x dh (normally half size, min 1). 2x2 average,
// edge texels repeat on odd sizes.
void tu_box_down(int kind, const uint8_t *src, unsigned sw, unsigned sh, unsigned sstride,
                 uint8_t *dst, unsigned dw, unsigned dh, unsigned dstride);

// nearest neighbour resample (npot cube faces -> pow2)
void tu_resample_nearest(const uint8_t *src, unsigned sw, unsigned sh, unsigned sstride,
                         uint8_t *dst, unsigned dw, unsigned dh, unsigned dstride, unsigned bpp);
