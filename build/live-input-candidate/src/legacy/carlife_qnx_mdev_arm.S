/* QNX ARMv7-A soft-float mdev transport, based on nbt-aad's call sites.
 *
 * int carlink_mdev_arm_open(void);  // connection ID, or -1 with errno
 * int carlink_mdev_arm_sendv(int coid, const iov_t *iov, int parts);
 * int carlink_mdev_arm_close(int coid);
 *
 * Each iov_t is an ARM32 pointer followed by a 32-bit length. Two parts
 * are used for setup; three for frames. sendv returns 0 or a positive
 * error code. All libc bindings are external and resolved by the caller.
 */
.syntax unified
.arch armv7-a
.arm
.section .text, "ax", %progbits

.global carlink_mdev_arm_open
.type carlink_mdev_arm_open, %function
carlink_mdev_arm_open:
    adr r0, .Lmdev_path
    mov r1, #0
    b name_open
.size carlink_mdev_arm_open, . - carlink_mdev_arm_open

.global carlink_mdev_arm_close
.type carlink_mdev_arm_close, %function
carlink_mdev_arm_close:
    b name_close
.size carlink_mdev_arm_close, . - carlink_mdev_arm_close

.global carlink_mdev_arm_sendv
.type carlink_mdev_arm_sendv, %function
carlink_mdev_arm_sendv:
    cmp r2, #2
    cmpne r2, #3
    movne r0, #22             /* EINVAL */
    bxne lr

    push {r4-r8, lr}
    sub sp, sp, #16
    mov r4, r0                /* coid */
    mov r5, r1                /* iov */
    mov r6, r2                /* parts */
    mov r8, #0

    bl __get_errno_ptr
    mov r7, r0
    str r8, [r7]
    cmp r6, #2
    movweq r0, #0xca00        /* Setup: 1,000,000,000 ns. */
    movteq r0, #0x3b9a
    movwne r0, #0xb280        /* Frame: 250,000,000 ns. */
    movtne r0, #0x0ee6
    str r0, [sp, #8]
    str r8, [sp, #12]
    str r8, [sp]
    mov r0, #2                /* CLOCK_MONOTONIC */
    mov r1, #0x50             /* SEND | REPLY */
    mov r2, r8                /* no event */
    add r3, sp, #8
    bl TimerTimeout
    cmn r0, #1
    beq .Lerrno

    str r8, [r7]
    str r8, [sp]              /* reply size */
    mov r0, r4
    mov r1, r5
    mov r2, r6
    mov r3, r8                /* no reply buffer */
    bl MsgSendvs_r
    cmp r0, #0
    rsble r0, r0, #0          /* 0 -> success, -errno -> errno */
    ble .Lreturn
.Lerrno:
    ldr r0, [r7]
    cmp r0, #0
    moveq r0, #5              /* EIO when no error code was supplied */
.Lreturn:
    add sp, sp, #16
    pop {r4-r8, pc}
.size carlink_mdev_arm_sendv, . - carlink_mdev_arm_sendv

.balign 4
.Lmdev_path:
    .asciz "/dev/name/local/mdev"

.section .note.GNU-stack, "", %progbits
