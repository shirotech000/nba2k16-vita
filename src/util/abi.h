#pragma once
// the game + gnustl are softfp (floats in core regs), we're hardfp.
// anything the game calls (or that we call in the game) that passes a
// float/double by value needs SOFTFP on it. variadic funcs are already
// base-AAPCS on both sides so they don't need it.
#define SOFTFP __attribute__((pcs("aapcs")))

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
