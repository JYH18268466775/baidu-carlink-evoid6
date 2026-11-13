/* Freestanding ARMv7-A ELF probe for static inspection.
 * QNX ThreadDestroy takes tid, priority, and exit status in r0-r2.
 * No startup code, C library, stack access, or device access is required.
 */
.syntax unified
.arch armv7-a
.arm

.section .text, "ax", %progbits
.global _start
.type _start, %function
_start:
    mvn r0, #0
    mov r1, r0
    eor r2, r2, r2
    mov r12, #0x2f
    svc #0
    udf #0                  /* A successful process exit does not return. */
    udf #0                  /* QNX syscall error return skips one instruction. */
.size _start, . - _start

/* Reproduce the owner/type/descriptor observed in the supplied QNX samples. */
.section .note.qnx.ident, "a", %note
.balign 4
.long 4, 8, 3
.asciz "QNX"
.long 0, 0x1000

.section .note.GNU-stack, "", %progbits
