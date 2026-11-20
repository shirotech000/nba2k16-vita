#pragma once
#include <stddef.h>

// small allocs (<64K) that newlib's full heap refused, carved from 1MB
// bigmem blocks. 16-byte aligned.
void *spill_alloc(size_t n);    // NULL if even bigmem's floors are hit
int spill_owns(const void *p);
size_t spill_usable(const void *p);
void spill_free(void *p);
