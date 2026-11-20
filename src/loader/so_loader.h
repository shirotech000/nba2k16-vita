#pragma once
#include <stddef.h>
#include <stdint.h>
#include <psp2/types.h>

// our own android .so loader. reserves one VA range per lib so the
// text->data distance stays exactly what the linker baked in.

typedef struct so_module {
  char name[64];
  uint8_t *base;
  size_t size;
  SceUID block;

  uintptr_t text_start, text_end;   // the RX segment
  uintptr_t data_start, data_end;   // the RW segment (incl. bss)

  const void *dynsym;               // Elf32_Sym*
  const char *dynstr;
  const uint32_t *hash;             // nbucket, nchain, buckets..., chains...
  const void *rel;                  // Elf32_Rel*
  size_t nrel;
  const void *plt_rel;
  size_t nplt_rel;

  void (*init)(void);
  void (**init_array)(void);
  size_t ninit_array;

  uintptr_t exidx;                  // ARM.exidx, for the unwinder
  size_t exidx_count;

  int n_unresolved;
  struct so_module *next;
} so_module;

int so_load(so_module *mod, const char *path);
int so_relocate(so_module *mod);
int so_finalize(so_module *mod);    // text -> RX + cache flush
void so_run_init(so_module *mod);

// defined (exported) symbol lookup in one module, 0 if missing
uintptr_t so_symbol(so_module *mod, const char *name);
// same but searches every loaded module
uintptr_t so_symbol_any(const char *name);
so_module *so_module_for_addr(uintptr_t addr);

// write over game code (hooks/patches). handles RX pages + cache flush.
int so_write(uintptr_t addr, const void *data, size_t len);
// replace a function with an ARM-mode "ldr pc, [pc, #-4]; .word dst" jump
int so_hook(uintptr_t func, uintptr_t dst);

// exidx lookup for gnustl's unwinder
uintptr_t so_find_exidx(uintptr_t pc, int *count);

// pretty "lib+0x1234" for crash logs
const char *so_describe(uintptr_t addr, char *buf, size_t len);
