// dumps regs as lib+offset when something blows up, way faster to debug
// than a raw coredump address.
#include <psp2/kernel/processmgr.h>
#include <kubridge.h>

#include "loader/so_loader.h"
#include "util/log.h"

static const char *const kinds[] = { "data abort", "prefetch abort", "undefined instruction" };

static void on_exception(KuKernelExceptionContext *c) {
  char pc[96], lr[96];
  L("!!! CRASH: %s", c->exceptionType < 3 ? kinds[c->exceptionType] : "?");
  L("  pc %s", so_describe(c->pc, pc, sizeof(pc)));
  L("  lr %s", so_describe(c->lr, lr, sizeof(lr)));
  L("  far 0x%08X fsr 0x%08X sp 0x%08X", c->FAR, c->FSR, c->sp);
  L("  r0 %08X r1 %08X r2 %08X r3 %08X", c->r0, c->r1, c->r2, c->r3);
  L("  r4 %08X r5 %08X r6 %08X r7 %08X", c->r4, c->r5, c->r6, c->r7);
  L("  r8 %08X r9 %08X r10 %08X r11 %08X r12 %08X", c->r8, c->r9, c->r10, c->r11, c->r12);
  // a few words off the stack, return addresses tend to be in there
  uint32_t *sp = (uint32_t *)c->sp;
  for (int i = 0; i < 16; i++) {
    char w[96];
    if (so_module_for_addr(sp[i])) L("  sp[%d] %s", i, so_describe(sp[i], w, sizeof(w)));
  }
  // returning would just re-run the faulting insn, so bail
  sceKernelExitProcess(0);
}

void crash_init(void) {
  for (int t = 0; t < 3; t++) {
    int r = kuKernelRegisterExceptionHandler(t, on_exception, NULL, NULL);
    if (r < 0) L("crash: couldn't hook exception %d (0x%08X), no crash logs", t, r);
  }
}
