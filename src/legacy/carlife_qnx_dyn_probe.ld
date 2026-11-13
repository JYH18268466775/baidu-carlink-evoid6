/* Place the ELF and program headers in the executable load segment, as in
 * nbt-aad. Keep the dynamic table and the PLT's writable GOT separate.
 */
ENTRY(_start)

PHDRS
{
    headers PT_PHDR PHDRS;
    interpreter PT_INTERP FLAGS(4);
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
    notes PT_NOTE FLAGS(4);
    stack 0x6474e551 FLAGS(6);
}

SECTIONS
{
    . = 0x100000 + SIZEOF_HEADERS;
    .interp : { *(.interp) } :text :interpreter
    .note.qnx.ident : { *(.note.qnx.ident) } :text :notes
    .hash : { *(.hash) } :text
    .dynsym : { *(.dynsym) } :text
    .dynstr : { *(.dynstr) } :text
    .rel.plt : { *(.rel.plt) } :text
    .plt : { *(.plt) } :text
    .text : { *(.text) } :text

    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got.plt) *(.got) } :data

    .ARM.attributes 0 : { *(.ARM.attributes) } :NONE
    /DISCARD/ : { *(.note.GNU-stack) }
}
