OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

/*
 * three loadable segments, so that the image says what the page table
 * says: code that can be executed and not written, constants that can
 * be neither, and data that can be written and not executed. Left to
 * itself the linker puts the lot in one segment with all three
 * permissions, which is what every build used to warn about.
 */
PHDRS
{
  text   PT_LOAD FLAGS(5);  /* R E */
  rodata PT_LOAD FLAGS(4);  /* R   */
  data   PT_LOAD FLAGS(6);  /* RW  */
}

SECTIONS
{
  /*
   * ensure that entry.S / _entry is at 0x80000000,
   * where qemu's -kernel jumps.
   */
  . = 0x80000000;

  .text : {
    kernel/entry.o(_entry)
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    _sigtrampsec = .;
    *(sigtrampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _sigtrampsec == 0x1000, "error: signal trampoline larger than one page");
    PROVIDE(etext = .);
  } :text

  /* a page of its own, because the smallest thing a page table can
     say anything about is a page */
  . = ALIGN(0x1000);

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  } :rodata

  . = ALIGN(0x1000);
  PROVIDE(erodata = .);

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  } :data

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  } :data

  PROVIDE(end = .);
}
