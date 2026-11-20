// android .so loader. written from scratch for this port.
// flow: so_load -> so_relocate -> so_finalize -> so_run_init

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>

#include "loader/so_loader.h"
#include "loader/imports.h"
#include "util/log.h"
#include "util/abi.h"

// --- tiny ELF32 subset, no need to drag in a full elf.h ---
typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct { int32_t d_tag; uint32_t d_val; } Elf32_Dyn;
typedef struct { uint32_t r_offset, r_info; } Elf32_Rel;
typedef struct {
  uint32_t st_name, st_value, st_size;
  uint8_t st_info, st_other;
  uint16_t st_shndx;
} Elf32_Sym;

#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_ARM_EXIDX 0x70000001
#define PF_X 1
#define PF_W 2

#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_REL 17
#define DT_RELSZ 18
#define DT_INIT 12
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define R_ARM_NONE      0
#define R_ARM_ABS32     2
#define R_ARM_GLOB_DAT  21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE  23

#define STB_WEAK 2
#define SHN_UNDEF 0

#define PAGE 0x1000

static so_module *modules;

static int read_at(SceUID fd, void *dst, size_t len, uint32_t off) {
  uint8_t *p = dst;
  while (len) {
    int chunk = len > 0x100000 ? 0x100000 : (int)len;
    int r = sceIoPread(fd, p, chunk, off);
    if (r <= 0) return -1;
    p += r; off += r; len -= r;
  }
  return 0;
}

int so_load(so_module *mod, const char *path) {
  memset(mod, 0, sizeof(*mod));
  const char *slash = strrchr(path, '/');
  snprintf(mod->name, sizeof(mod->name), "%s", slash ? slash + 1 : path);

  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) {
    L("so_load: can't open %s (0x%08X)", path, fd);
    return -1;
  }

  Elf32_Ehdr eh;
  if (read_at(fd, &eh, sizeof(eh), 0) < 0 || memcmp(eh.e_ident, "\x7f" "ELF", 4) || eh.e_machine != 40) {
    L("so_load: %s isn't an ARM elf", path);
    sceIoClose(fd);
    return -1;
  }

  Elf32_Phdr *ph = malloc(eh.e_phnum * sizeof(Elf32_Phdr));
  read_at(fd, ph, eh.e_phnum * sizeof(Elf32_Phdr), eh.e_phoff);

  // how big is the whole image
  uint32_t top = 0;
  for (int i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > top)
      top = ph[i].p_vaddr + ph[i].p_memsz;
  // reserve in 1MB chunks, kubridge is happiest with that
  mod->size = ALIGN_UP(top, 0x100000);

  void *base = NULL;
  mod->block = kuKernelMemReserve(&base, mod->size, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
  if (mod->block < 0) {
    L("so_load: kuKernelMemReserve(%u) failed 0x%08X. kubridge too old? need one with MemReserve",
      (unsigned)mod->size, mod->block);
    goto fail;
  }
  int r = kuKernelMemCommit(base, mod->size, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, NULL);
  if (r < 0) {
    L("so_load: kuKernelMemCommit failed 0x%08X", r);
    goto fail;
  }
  mod->base = base;
  memset(mod->base, 0, mod->size);

  for (int i = 0; i < eh.e_phnum; i++) {
    Elf32_Phdr *p = &ph[i];
    if (p->p_type == PT_LOAD) {
      if (read_at(fd, mod->base + p->p_vaddr, p->p_filesz, p->p_offset) < 0) {
        L("so_load: short read on %s", path);
        goto fail;
      }
      uintptr_t s = (uintptr_t)mod->base + p->p_vaddr, e = s + p->p_memsz;
      if (p->p_flags & PF_X) { mod->text_start = s; mod->text_end = e; }
      else if (p->p_flags & PF_W) { mod->data_start = s; mod->data_end = e; }
    } else if (p->p_type == PT_ARM_EXIDX) {
      mod->exidx = (uintptr_t)mod->base + p->p_vaddr;
      mod->exidx_count = p->p_memsz / 8;
    }
  }

  // dynamic section lives inside the loaded data segment
  Elf32_Dyn *dyn = NULL;
  for (int i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf32_Dyn *)(mod->base + ph[i].p_vaddr);
  if (!dyn) {
    L("so_load: %s has no PT_DYNAMIC", path);
    goto fail;
  }

  for (; dyn->d_tag != DT_NULL; dyn++) {
    uint8_t *v = mod->base + dyn->d_val;
    switch (dyn->d_tag) {
      case DT_SYMTAB:       mod->dynsym = v; break;
      case DT_STRTAB:       mod->dynstr = (const char *)v; break;
      case DT_HASH:         mod->hash = (const uint32_t *)v; break;
      case DT_REL:          mod->rel = v; break;
      case DT_RELSZ:        mod->nrel = dyn->d_val / sizeof(Elf32_Rel); break;
      case DT_JMPREL:       mod->plt_rel = v; break;
      case DT_PLTRELSZ:     mod->nplt_rel = dyn->d_val / sizeof(Elf32_Rel); break;
      case DT_INIT:         mod->init = (void (*)(void))v; break;
      case DT_INIT_ARRAY:   mod->init_array = (void (**)(void))v; break;
      case DT_INIT_ARRAYSZ: mod->ninit_array = dyn->d_val / 4; break;
    }
  }

  free(ph);
  sceIoClose(fd);

  mod->next = modules;
  modules = mod;
  L("so_load: %s @ %p size 0x%X (text %08X-%08X data %08X-%08X) %u rels, %u plt, %u ctors",
    mod->name, mod->base, (unsigned)mod->size, mod->text_start, mod->text_end,
    mod->data_start, mod->data_end, (unsigned)mod->nrel, (unsigned)mod->nplt_rel,
    (unsigned)mod->ninit_array);
  return 0;

fail:
  free(ph);
  sceIoClose(fd);
  return -1;
}

static uint32_t elf_hash(const char *name) {
  uint32_t h = 0, g;
  while (*name) {
    h = (h << 4) + (uint8_t)*name++;
    if ((g = h & 0xF0000000)) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

uintptr_t so_symbol(so_module *mod, const char *name) {
  if (!mod->hash) return 0;
  const Elf32_Sym *syms = mod->dynsym;
  uint32_t nbucket = mod->hash[0];
  const uint32_t *bucket = &mod->hash[2];
  const uint32_t *chain = &bucket[nbucket];
  for (uint32_t i = bucket[elf_hash(name) % nbucket]; i; i = chain[i]) {
    const Elf32_Sym *s = &syms[i];
    if (s->st_shndx != SHN_UNDEF && !strcmp(mod->dynstr + s->st_name, name))
      return (uintptr_t)mod->base + s->st_value;
  }
  return 0;
}

uintptr_t so_symbol_any(const char *name) {
  for (so_module *m = modules; m; m = m->next) {
    uintptr_t a = so_symbol(m, name);
    if (a) return a;
  }
  return 0;
}

so_module *so_module_for_addr(uintptr_t addr) {
  for (so_module *m = modules; m; m = m->next)
    if (addr >= (uintptr_t)m->base && addr < (uintptr_t)m->base + m->size) return m;
  return NULL;
}

// where does an undefined symbol come from: our shims first, then other libs
static uintptr_t resolve(so_module *mod, const Elf32_Sym *s, const char *name) {
  uintptr_t a = imports_lookup(name);
  if (a) return a;
  for (so_module *m = modules; m; m = m->next) {
    if (m == mod) continue;
    if ((a = so_symbol(m, name))) return a;
  }
  if ((s->st_info >> 4) == STB_WEAK) {
    L("  weak %s -> NULL", name);
    return 0;
  }
  mod->n_unresolved++;
  L("  unresolved: %s", name);
  return imports_trap(name);
}

static int apply_rels(so_module *mod, const Elf32_Rel *rels, size_t n) {
  const Elf32_Sym *syms = mod->dynsym;
  for (size_t i = 0; i < n; i++) {
    uint32_t type = rels[i].r_info & 0xFF;
    uint32_t si = rels[i].r_info >> 8;
    uint32_t *where = (uint32_t *)(mod->base + rels[i].r_offset);

    if (type == R_ARM_RELATIVE) {
      *where += (uint32_t)mod->base;
      continue;
    }
    if (type == R_ARM_NONE) continue;

    uintptr_t sa = 0;
    if (si) {
      const Elf32_Sym *s = &syms[si];
      const char *name = mod->dynstr + s->st_name;
      // DT_SYMBOLIC: our own definition wins
      sa = s->st_shndx != SHN_UNDEF ? (uintptr_t)mod->base + s->st_value : resolve(mod, s, name);
    }

    switch (type) {
      case R_ARM_ABS32:     *where += sa; break;
      case R_ARM_GLOB_DAT:
      case R_ARM_JUMP_SLOT: *where = sa; break;
      default:
        L("so_relocate: %s: unhandled reloc type %u at +0x%X", mod->name, type, rels[i].r_offset);
        return -1;
    }
  }
  return 0;
}

// linux maps "kuser helpers" at fixed addresses up at 0xffff0fxx. bits of
// the game (google play games sdk etc) keep function pointers to them in
// .data. nothing lives there on the vita, so point them at our own.
extern void kuser_dmb(void), kuser_cmpxchg(void), kuser_cmpxchg64(void);

static void fix_kuser_pointers(so_module *mod) {
  static const struct { uint32_t addr; void (*fn)(void); const char *name; } helpers[] = {
    { 0xFFFF0FA0, kuser_dmb, "dmb" },
    { 0xFFFF0FC0, kuser_cmpxchg, "cmpxchg" },
    { 0xFFFF0F60, kuser_cmpxchg64, "cmpxchg64" },
  };
  int n[3] = { 0 };
  for (uint32_t *p = (uint32_t *)ALIGN_UP(mod->data_start, 4); p < (uint32_t *)mod->data_end; p++) {
    for (int h = 0; h < 3; h++) {
      if (*p == helpers[h].addr) {
        *p = (uint32_t)helpers[h].fn;
        n[h]++;
      }
    }
  }
  if (n[0] + n[1] + n[2])
    L("so_relocate: %s: swapped kuser pointers (dmb %d, cmpxchg %d, cmpxchg64 %d)", mod->name, n[0], n[1], n[2]);
}

int so_relocate(so_module *mod) {
  L("so_relocate: %s", mod->name);
  if (apply_rels(mod, mod->rel, mod->nrel) < 0) return -1;
  if (apply_rels(mod, mod->plt_rel, mod->nplt_rel) < 0) return -1;
  fix_kuser_pointers(mod);
  L("so_relocate: %s done, %d unresolved", mod->name, mod->n_unresolved);
  return 0;
}

int so_finalize(so_module *mod) {
  uintptr_t s = ALIGN_DOWN(mod->text_start, PAGE);
  uintptr_t e = ALIGN_UP(mod->text_end, PAGE);
  // never flip a page that also holds data to RX
  if (e > ALIGN_DOWN(mod->data_start, PAGE)) e = ALIGN_DOWN(mod->data_start, PAGE);
  int r = kuKernelMemProtect((void *)s, e - s, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
  if (r < 0) {
    L("so_finalize: kuKernelMemProtect failed 0x%08X", r);
    return -1;
  }
  kuKernelFlushCaches(mod->base, mod->size);
  return 0;
}

void so_run_init(so_module *mod) {
  L("so_run_init: %s (%u ctors)", mod->name, (unsigned)mod->ninit_array);
  if (mod->init) mod->init();
  for (size_t i = 0; i < mod->ninit_array; i++) {
    uintptr_t f = (uintptr_t)mod->init_array[i];
    if (f && f != (uintptr_t)-1) ((void (*)(void))f)();
  }
  L("so_run_init: %s done", mod->name);
}

int so_write(uintptr_t addr, const void *data, size_t len) {
  int r = kuKernelCpuUnrestrictedMemcpy((void *)addr, data, len);
  kuKernelFlushCaches((void *)addr, len);
  return r;
}

int so_hook(uintptr_t func, uintptr_t dst) {
  // thumb funcs have bit 0 set, the real code starts at func & ~1.
  // for thumb we need a thumb-mode stub: ldr.w pc, [pc, #0]; .word dst
  // (has to be 4-byte aligned for the literal, pad with a nop if not)
  // literal for ldr.w is at Align(insn+4, 4), so if we start 2 off we
  // drop a nop first and the literal lands right after the ldr.
  static const uint16_t nop = 0xBF00, ldr_pc[2] = { 0xF8DF, 0xF000 };
  static const uint32_t arm_ldr_pc = 0xE51FF004; // ldr pc, [pc, #-4]
  uint8_t buf[12];
  size_t len = 0;
  uintptr_t at = func & ~1;
  if (func & 1) {
    if (at & 2) { memcpy(buf, &nop, 2); len = 2; }
    memcpy(buf + len, ldr_pc, 4); len += 4;
  } else {
    memcpy(buf, &arm_ldr_pc, 4); len = 4;
  }
  memcpy(buf + len, &dst, 4); len += 4;
  return so_write(at, buf, len);
}

uintptr_t so_find_exidx(uintptr_t pc, int *count) {
  so_module *m = so_module_for_addr(pc);
  if (!m || !m->exidx) {
    *count = 0;
    return 0;
  }
  *count = (int)m->exidx_count;
  return m->exidx;
}

void so_backtrace(const char *why) {
  // poor man's backtrace: anything on the stack that points into a
  // module's code is probably a return address. good enough to find callers.
  uintptr_t *sp = __builtin_frame_address(0);
  int shown = 0;
  // stop at the top of whatever block the stack lives in. on hw the page
  // past a thread stack isn't mapped (vita3k maps it, so it never showed)
  int n = 512;
  SceKernelMemBlockInfo mi;
  memset(&mi, 0, sizeof(mi));
  mi.size = sizeof(mi);
  if (sceKernelGetMemBlockInfoByAddr(sp, &mi) >= 0) {
    uintptr_t top = (uintptr_t)mi.mappedBase + mi.mappedSize;
    int left = (int)((top - (uintptr_t)sp) / sizeof(uintptr_t));
    if (left < n) n = left;
  } else {
    n = 64;  // don't know the bounds, stay close
  }
  L("backtrace (%s):", why);
  for (int i = 0; i < n && shown < 12; i++) {
    so_module *m = so_module_for_addr(sp[i]);
    if (m && sp[i] >= m->text_start && sp[i] < m->text_end) {
      char w[96];
      L("  #%d %s", shown++, so_describe(sp[i], w, sizeof(w)));
    }
  }
}

const char *so_describe(uintptr_t addr, char *buf, size_t len) {
  so_module *m = so_module_for_addr(addr);
  if (m) snprintf(buf, len, "%s+0x%X", m->name, (unsigned)(addr - (uintptr_t)m->base));
  else snprintf(buf, len, "0x%08X", (unsigned)addr);
  return buf;
}
