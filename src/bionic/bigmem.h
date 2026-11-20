#pragma once
#include <stddef.h>

// overflow for big allocations once newlib's heap runs low. each block is
// its own memblock: free main memory outside the heap first, then CDRAM
// (the L1-cached flavour if the kernel gives it to us). reserves are kept
// back for thread stacks and GL textures.

void bigmem_init(void);
void *bigmem_alloc(size_t n);       // NULL if CDRAM is too tight
void *bigmem_alloc_ex(size_t n, int urgent);  // urgent: dip into the reserves
int bigmem_owns(const void *p);
size_t bigmem_size(const void *p);  // requested size, 0 if not ours
void bigmem_free(void *p);
void bigmem_report(void);
