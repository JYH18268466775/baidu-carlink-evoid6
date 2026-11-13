/* Minimal Screen window lifecycle using the ARM32 calls observed in nbt-aad.
 * int carlink_screen_arm_open(state[3], group, id, width, height);
 * void carlink_screen_arm_close(state[3]);
 * state contains three 32-bit handles: context, window, render buffer.
 * The window is kept hidden. No display selection or layer changes are made.
 */
.syntax unified
.arch armv7-a
.arm
.text

.global carlink_screen_arm_open
.type carlink_screen_arm_open, %function
carlink_screen_arm_open:
    push {r4-r10, lr}
    ldr r8, [sp, #32]
    sub sp, sp, #32
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, r3
    mov r10, #0
    str r10, [r4]
    str r10, [r4, #4]
    str r10, [r4, #8]

    mov r9, #20
    mov r1, #0
    bl screen_create_context
    cmp r0, #0
    bne .Lscreen_fail

    mov r9, #21
    add r0, r4, #4
    ldr r1, [r4]
    mov r2, #0
    bl screen_create_window_type
    cmp r0, #0
    bne .Lscreen_fail

    mov r9, #22
    ldr r0, [r4, #4]
    mov r1, r5
    bl screen_create_window_group
    cmp r0, #0
    bne .Lscreen_fail

    mov r9, #23
    mov r0, r6
    bl strlen
    mov r2, r0
    mov r3, r6
    mov r1, #0x14
    ldr r0, [r4, #4]
    bl screen_set_window_property_cv
    cmp r0, #0
    bne .Lscreen_fail

    mov r9, #24
    mov r3, #8
    str r3, [sp, #8]
    add r2, sp, #8
    mov r1, #0x30
    ldr r0, [r4, #4]
    bl screen_set_window_property_iv
    cmp r0, #0
    bne .Lscreen_fail
    mov r3, #8
    str r3, [sp, #8]
    add r2, sp, #8
    mov r1, #0x0e
    ldr r0, [r4, #4]
    bl screen_set_window_property_iv
    cmp r0, #0
    bne .Lscreen_fail
    str r7, [sp, #8]
    str r8, [sp, #12]
    add r2, sp, #8
    mov r1, #0x28
    ldr r0, [r4, #4]
    bl screen_set_window_property_iv
    cmp r0, #0
    bne .Lscreen_fail
    str r10, [sp, #8]
    add r2, sp, #8
    mov r1, #0x2b
    ldr r0, [r4, #4]
    bl screen_set_window_property_iv
    cmp r0, #0
    bne .Lscreen_fail

    mov r9, #25
    mov r1, #1
    ldr r0, [r4, #4]
    bl screen_create_window_buffers
    cmp r0, #0
    bne .Lscreen_fail
    add r2, r4, #8
    mov r1, #0x25
    ldr r0, [r4, #4]
    bl screen_get_window_property_pv
    cmp r0, #0
    bne .Lscreen_fail
    ldr r1, [r4, #8]
    cmp r1, #0
    beq .Lscreen_fail

    mov r9, #26
    ldr r0, [r4]
    adr r2, .Lfill
    bl screen_fill
    cmp r0, #0
    bne .Lscreen_fail

    mov r9, #27
    str r10, [sp]
    str r10, [sp, #8]
    str r10, [sp, #12]
    str r7, [sp, #16]
    str r8, [sp, #20]
    add r3, sp, #8
    mov r2, #1
    ldr r1, [r4, #8]
    ldr r0, [r4, #4]
    bl screen_post_window
    cmp r0, #0
    bne .Lscreen_fail
    b .Lscreen_return

.Lscreen_fail:
    mov r0, r4
    bl carlink_screen_arm_close
    mov r0, r9
.Lscreen_return:
    add sp, sp, #32
    pop {r4-r10, pc}
.size carlink_screen_arm_open, . - carlink_screen_arm_open

.global carlink_screen_arm_close
.type carlink_screen_arm_close, %function
carlink_screen_arm_close:
    push {r4, lr}
    mov r4, r0
    ldr r0, [r4, #4]
    cmp r0, #0
    blne screen_destroy_window
    ldr r0, [r4]
    cmp r0, #0
    blne screen_destroy_context
    mov r0, #0
    str r0, [r4]
    str r0, [r4, #4]
    str r0, [r4, #8]
    pop {r4, pc}
.size carlink_screen_arm_close, . - carlink_screen_arm_close

.balign 4
.Lfill:
    .long 12, 0x01000000, 0

.section .note.GNU-stack, "", %progbits
