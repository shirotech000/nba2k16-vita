#pragma once
#include <stdint.h>

// merges all the shim export tables, call before loading anything
void imports_init(void);
// address of our implementation of `name`, or 0
uintptr_t imports_lookup(const char *name);
// makes a little stub that logs "called unimplemented X" and returns 0
uintptr_t imports_trap(const char *name);
// flips the trap stubs to RX once relocation is done
void imports_finalize(void);
