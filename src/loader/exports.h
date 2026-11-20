#pragma once
// every shim file hands the loader a table of {name, address} it can
// resolve the game's imports against. data symbols work too (address of
// the variable), that's how SL_IID_* / __sF / _ctype_ get wired up.
typedef struct {
  const char *name;
  void *addr;
} export_t;

#define EXP(sym)         { #sym, (void *)&(sym) }
#define EXP_AS(name, fn) { name, (void *)&(fn) }

// use at file scope:  EXPORT_TABLE(libc) = { EXP(memcpy), ... };
#define EXPORT_TABLE(tag)                                  \
  extern const export_t tag##_exports[];                   \
  extern const unsigned tag##_exports_count;               \
  const export_t tag##_exports[]

#define EXPORT_TABLE_END(tag) \
  const unsigned tag##_exports_count = sizeof(tag##_exports) / sizeof(export_t)
