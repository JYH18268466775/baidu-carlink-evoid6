/* QNX ARM process entry, using the loader ABI verified by the Screen probe. */
#ifndef CARLINK_PROBE_ENTRY
#define CARLINK_PROBE_ENTRY carlink_mmr_connect_probe
#endif
.syntax unified
.arm
.text
.global _start
.type _start, %function
_start:
    mov r7, r0
    ldr r4, [sp]
    add r5, sp, #4
    add r6, r5, r4, lsl #2
    add r6, r6, #4
    mov r3, r6
1:  ldr r12, [r3], #4
    cmp r12, #0
    bne 1b
    bic sp, sp, #7
    sub sp, sp, #8
    str r7, [sp]
    mov r0, r4
    mov r1, r5
    mov r2, r6
    bl _init_libc
    mov r0, r4
    mov r1, r5
    bl CARLINK_PROBE_ENTRY
    bl exit
    udf #0
.size _start, . - _start

.section .note.qnx.ident, "a", %note
.balign 4
.long 4, 8, 3
.asciz "QNX"
.long 0, 0x1000

.section .note.GNU-stack, "", %progbits
