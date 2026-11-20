#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>

#include "loader/imports.h"
#include "loader/exports.h"
#include "loader/so_loader.h"
#include "util/log.h"

// every shim file's table. add new ones here.
#define TABLES(X) \
  X(libc) X(pthread) X(stdio) X(io) X(math) X(wchar) X(net) X(zlib) \
  X(android) X(looper) X(input) X(opensles) X(egl) X(gles) X(curl) X(ssl)

#define DECL(t) extern const export_t t##_exports[]; extern const unsigned t##_exports_count;
TABLES(DECL)
#undef DECL

static export_t *all;
static unsigned n_all;

static int cmp_exp(const void *a, const void *b) {
  return strcmp(((const export_t *)a)->name, ((const export_t *)b)->name);
}

void imports_init(void) {
  unsigned total = 0;
#define COUNT(t) total += t##_exports_count;
  TABLES(COUNT)
#undef COUNT
  all = malloc(total * sizeof(export_t));
#define COPY(t) memcpy(all + n_all, t##_exports, t##_exports_count * sizeof(export_t)); n_all += t##_exports_count;
  TABLES(COPY)
#undef COPY
  qsort(all, n_all, sizeof(export_t), cmp_exp);
  for (unsigned i = 1; i < n_all; i++)
    if (!strcmp(all[i].name, all[i - 1].name))
      L("imports: %s is exported twice, one of them wins at random", all[i].name);
  L("imports: %u shim symbols", n_all);
}

uintptr_t imports_lookup(const char *name) {
  export_t key = { name, 0 };
  export_t *e = bsearch(&key, all, n_all, sizeof(export_t), cmp_exp);
  return e ? (uintptr_t)e->addr : 0;
}

// --- traps for stuff we haven't written yet ---
// each stub is ARM code:  ldr r0,[pc]; ldr pc,[pc]; .word rec; .word handler
// so the handler gets the record in r0 and lr still points into the game.

typedef struct {
  const char *name;
  int hits;
} trap_rec;

#define MAX_TRAPS 1024
static uint32_t *trap_code;
static SceUID trap_block = -1;
static trap_rec trap_recs[MAX_TRAPS];
static int n_traps;

static int trap_handler(trap_rec *r) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  if (r->hits++ < 8) {
    char where[96];
    L("TRAP: game called unimplemented %s from %s", r->name, so_describe(lr, where, sizeof(where)));
  }
  return 0;
}

uintptr_t imports_trap(const char *name) {
  if (trap_block < 0) {
    void *base = NULL;
    trap_block = kuKernelMemReserve(&base, 0x100000, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    if (trap_block < 0 || kuKernelMemCommit(base, 0x100000, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, NULL) < 0)
      fatal("imports_trap: couldn't get memory for trap stubs (0x%08X)", trap_block);
    trap_code = base;
  }
  // same name twice? reuse the stub
  for (int i = 0; i < n_traps; i++)
    if (!strcmp(trap_recs[i].name, name)) return (uintptr_t)&trap_code[i * 4];
  if (n_traps >= MAX_TRAPS) fatal("imports_trap: too many traps");

  int i = n_traps++;
  trap_recs[i].name = strdup(name);
  uint32_t *c = &trap_code[i * 4];
  c[0] = 0xE59F0000; // ldr r0, [pc, #0]  -> c[2]
  c[1] = 0xE59FF000; // ldr pc, [pc, #0]  -> c[3]
  c[2] = (uint32_t)&trap_recs[i];
  c[3] = (uint32_t)&trap_handler;
  return (uintptr_t)c;
}

void imports_finalize(void) {
  if (trap_block < 0) return;
  kuKernelMemProtect(trap_code, 0x100000, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
  kuKernelFlushCaches(trap_code, 0x100000);
  L("imports: %d trap stubs", n_traps);
}
