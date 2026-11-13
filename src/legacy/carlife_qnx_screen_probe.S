/* Standalone Screen ABI probe, with an explicit bench argument.
 * Exit codes 20-27 identify the failed stage in carlife_qnx_screen_arm.S.
 * It creates a hidden 64x64 window and closes it immediately after posting.
 */
.syntax unified
.arch armv7-a
.arm
.text
.global _start
.type _start, %function
_start:
    /* Loader entry and _init_libc arguments observed at nbt-aad 0x104b70. */
    mov r7, r0
    ldr r4, [sp]
    add r5, sp, #4
    add r6, r5, r4, lsl #2
    add r6, r6, #4
    mov r3, r6
.Lenv:
    ldr r12, [r3], #4
    cmp r12, #0
    bne .Lenv
    bic sp, sp, #7
    sub sp, sp, #8
    str r7, [sp]
    mov r0, r4
    mov r1, r5
    mov r2, r6
    bl _init_libc
    add sp, sp, #8

.ifdef CARLINK_LOADER_ONLY
    mov r0, #73
    bl _exit
    udf #0
.else
    mov r0, #2
    cmp r4, #2
    bne .Lexit
    ldr r1, [r5, #4]
    adr r2, .Lbench
.Larg:
    ldrb r3, [r1], #1
    ldrb r4, [r2], #1
    cmp r3, r4
    bne .Lexit
    cmp r3, #0
    bne .Larg

    sub sp, sp, #64
    bl getpid
    mov r3, r0
    add r0, sp, #24
    mov r1, #40
    adr r2, .Lgroup
    bl snprintf
    cmp r0, #0
    blt .Lformat_fail
    cmp r0, #40
    bge .Lformat_fail

    mov r3, #64
    str r3, [sp]
    add r0, sp, #8
    add r1, sp, #24
    mov r2, r1
    bl carlink_screen_arm_open
    mov r4, r0
    cmp r0, #0
    bne .Ldone
    add r0, sp, #8
    bl carlink_screen_arm_close
.Ldone:
    mov r0, r4
    b .Lexit
.Lformat_fail:
    mov r0, #19
.Lexit:
    bl exit
    udf #0
.endif
.size _start, . - _start
.ifndef CARLINK_LOADER_ONLY
.Lbench:
    .asciz "--bench"
.Lgroup:
    .asciz "carlink-screen-%d"
.endif

.section .note.qnx.ident, "a", %note
.balign 4
.long 4, 8, 3
.asciz "QNX"
.long 0, 0x1000

.section .note.GNU-stack, "", %progbits
