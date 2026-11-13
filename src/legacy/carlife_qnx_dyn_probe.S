/* SDK-free ARM dynamic-link probe. Inspect the ELF without running it.
 *
 * Assemble a separate copy with --defsym CARLINK_LINK_STUB=1 to generate
 * the link-only _exit symbol. Give that temporary shared object's SONAME
 * libc.so.3, then link the probe against it with /usr/lib/ldqnx.so.2 as
 * the interpreter. The temporary library is never a runtime dependency
 * file to install; only its SONAME is recorded in the executable.
 */
.syntax unified
.arch armv7-a
.arm
.section .text, "ax", %progbits

.ifdef CARLINK_LINK_STUB
.global _exit
.type _exit, %function
_exit:
    udf #0
.size _exit, . - _exit
.else
.global _start
.type _start, %function
.extern _exit
_start:
    mov r0, #0
    bl _exit
    udf #0
.size _start, . - _start

.section .note.qnx.ident, "a", %note
.balign 4
.long 4, 8, 3
.asciz "QNX"
.long 0, 0x1000
.endif

.section .note.GNU-stack, "", %progbits
