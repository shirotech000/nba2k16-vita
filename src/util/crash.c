// dumps regs as lib+offset when something blows up, way faster to debug
// than a raw coredump address.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>

#include "loader/so_loader.h"
#include "util/log.h"
#include "util/paths.h"

int emu_is_vita3k(void);

static const char *const kinds[] = { "data abort", "prefetch abort", "undefined instruction" };

// kubridge only promises ~0x560 bytes of stack in here, so: no L(), no
// newlib printf, no stack buffers. format into statics with sceClib and
// write straight to the log fd.
static char cbuf[256];
static char dpc[80], dlr[80];
static SceKernelThreadInfo cinfo;
static int console;
static volatile int crash_thid;

static void out(void) {
  int n = sceClibStrnlen(cbuf, sizeof(cbuf));
  log_write_raw(cbuf, n);
  // console copy is for vita3k's stdout (run.ps1). on hw sceClibPrintf's
  // own stack use is unknown, and the file copy is already out anyway
  if (console) sceClibPrintf("%s", cbuf);
}

static const char *desc(SceUInt32 a, char *b, int len) {
  so_module *m = so_module_for_addr(a);
  if (m) sceClibSnprintf(b, len, "%s+0x%X", m->name, (unsigned)(a - (uintptr_t)m->base));
  else sceClibSnprintf(b, len, "0x%08X", (unsigned)a);
  return b;
}

// only touch sp if it sits inside this thread's stack, a blown or junk sp
// would just fault again in here
static int sp_ok(SceUInt32 sp, int thid) {
  if (!sp || (sp & 3)) return 0;
  cinfo.size = sizeof(cinfo);
  if (sceKernelGetThreadInfo(thid, &cinfo) < 0) return 1;  // aligned + non-null is all we've got
  SceUInt32 lo = (SceUInt32)cinfo.stack, hi = lo + (SceUInt32)cinfo.stackSize;
  return sp >= lo && sp < hi && hi - sp >= 16 * 4;
}

// dump memory at a fault-time register value, if it's actually mapped.
// the memblock query is the safe probe: no second fault mid-dump
static SceKernelMemBlockInfo mb;
static void dump_words(const char *tag, SceUInt32 a, int words) {
  mb.size = sizeof(mb);
  if (a < 0x1000 || (a & 3) || sceKernelGetMemBlockInfoByAddr((void *)(uintptr_t)a, &mb) < 0) {
    sceClibSnprintf(cbuf, sizeof(cbuf), "  mem[%s] 0x%08X unmapped\n", tag, a);
    out();
    return;
  }
  SceUInt32 lo = (SceUInt32)(uintptr_t)mb.mappedBase, hi = lo + (SceUInt32)mb.mappedSize;
  if (a < lo) return;
  if ((SceUInt64)a + (SceUInt64)words * 4 > hi) words = (hi - a) / 4;
  if (words <= 0) return;
  const SceUInt32 *p = (const SceUInt32 *)(uintptr_t)a;
  for (int i = 0; i < words; i += 4) {
    if (words - i >= 4)
      sceClibSnprintf(cbuf, sizeof(cbuf), "  mem[%s]+%03X %08X %08X %08X %08X\n",
                      tag, i * 4, p[i], p[i + 1], p[i + 2], p[i + 3]);
    else
      sceClibSnprintf(cbuf, sizeof(cbuf), "  mem[%s]+%03X %08X\n", tag, i * 4, p[i]);
    out();
  }
}

static void on_exception(KuKernelExceptionContext *c) {
  int me = sceKernelGetThreadId();
  if (!__sync_bool_compare_and_swap(&crash_thid, 0, me)) {
    // faulted inside the dump itself: just go
    if (crash_thid == me) sceKernelExitProcess(0);
    // another thread is already dumping, let it finish and exit for us
    for (;;) sceKernelDelayThread(1000 * 1000);
  }
  log_flush_unlocked();
  sceClibSnprintf(cbuf, sizeof(cbuf), "!!! CRASH: %s\n", c->exceptionType < 3 ? kinds[c->exceptionType] : "?");
  out();
  sceClibSnprintf(cbuf, sizeof(cbuf), "  pc %s\n  lr %s\n", desc(c->pc, dpc, sizeof(dpc)), desc(c->lr, dlr, sizeof(dlr)));
  out();
  sceClibSnprintf(cbuf, sizeof(cbuf), "  far 0x%08X fsr 0x%08X sp 0x%08X\n", c->FAR, c->FSR, c->sp);
  out();
  sceClibSnprintf(cbuf, sizeof(cbuf), "  r0 %08X r1 %08X r2 %08X r3 %08X\n", c->r0, c->r1, c->r2, c->r3);
  out();
  sceClibSnprintf(cbuf, sizeof(cbuf), "  r4 %08X r5 %08X r6 %08X r7 %08X\n", c->r4, c->r5, c->r6, c->r7);
  out();
  sceClibSnprintf(cbuf, sizeof(cbuf), "  r8 %08X r9 %08X r10 %08X r11 %08X r12 %08X\n", c->r8, c->r9, c->r10, c->r11, c->r12);
  out();
  // a few words off the stack, return addresses tend to be in there
  if (sp_ok(c->sp, me)) {
    const SceUInt32 *sp = (const SceUInt32 *)c->sp;
    for (int i = 0; i < 16; i++) {
      if (!so_module_for_addr(sp[i])) continue;
      sceClibSnprintf(cbuf, sizeof(cbuf), "  sp[%d] %s\n", i, desc(sp[i], dpc, sizeof(dpc)));
      out();
    }
  } else {
    sceClibSnprintf(cbuf, sizeof(cbuf), "  (sp looks bad, not reading the stack)\n");
    out();
  }
  // live struct pointers at the fault: r2 is usually the deref that died
  // (prints "unmapped" here), r0/r3/r11 are typically the owner, the table
  // and the record of whatever walk blew up. r8/r12 joined for the index
  // hunt: the index header and the sorted-table's raw slots, plus whatever
  // allocation bytes follow them
  dump_words("r2", c->r2, 1);
  dump_words("r0", c->r0, 0x12);
  dump_words("r3", c->r3, 0x48);
  dump_words("r11", c->r11, 0x0A);
  dump_words("r8", c->r8, 0x0C);
  dump_words("r12", c->r12, 0x10);
  // returning would just re-run the faulting insn, so bail
  sceKernelExitProcess(0);
}

void crash_init(void) {
  console = emu_is_vita3k();
  // data + prefetch aborts only. kubridge hands every user UNDEF to the
  // type 2 handler, which may include vfp/neon lazy-enable traps
  for (int t = KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT; t <= KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT; t++) {
    int r = kuKernelRegisterExceptionHandler(t, on_exception, NULL, NULL);
    if (r < 0) L("crash: couldn't hook exception %d (0x%08X), no crash logs", t, r);
  }
}
