@ stand-ins for the linux "kuser helpers" (the little functions linux maps
@ at 0xffff0f60..0xffff0fe0). the game keeps pointers to them in its data
@ segment, so_loader swaps those pointers for these.
@ contract (same as the kernel's): return 0 + carry set if the swap happened,
@ nonzero + carry clear if not. only r0, r3, ip and flags get clobbered.

  .syntax unified
  .arm
  .text

  .global kuser_dmb
  .type kuser_dmb, %function
kuser_dmb:
  dmb ish
  bx lr

@ int cmpxchg(int oldval, int newval, volatile int *ptr)
  .global kuser_cmpxchg
  .type kuser_cmpxchg, %function
kuser_cmpxchg:
  dmb ish
1:
  ldrex r3, [r2]
  cmp r3, r0
  bne 2f
  strex r3, r1, [r2]
  cmp r3, #0
  bne 1b               @ lost the reservation, go again
  dmb ish
  mov r0, #0
  cmp r0, #0           @ Z=1, C=1
  bx lr
2:
  clrex
  mov r0, #1
  cmp r0, #2           @ Z=0, C=0
  bx lr

@ int cmpxchg64(const int64_t *oldval, const int64_t *newval, volatile int64_t *ptr)
  .global kuser_cmpxchg64
  .type kuser_cmpxchg64, %function
kuser_cmpxchg64:
  push {r4-r7}
  ldrd r4, r5, [r0]    @ expected
  ldrd r6, r7, [r1]    @ new value
  dmb ish
1:
  ldrexd r0, r1, [r2]
  eors r3, r0, r4
  eorseq r3, r1, r5
  bne 2f
  strexd r3, r6, r7, [r2]
  cmp r3, #0
  bne 1b
  dmb ish
  pop {r4-r7}
  mov r0, #0
  cmp r0, #0
  bx lr
2:
  clrex
  pop {r4-r7}
  mov r0, #1
  cmp r0, #2
  bx lr
