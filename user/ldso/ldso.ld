OUTPUT_ARCH( "riscv" )
ENTRY( _dlstart )

SECTIONS
{
  /* the loader is a position-independent executable of its own: the
     kernel maps it wherever it likes and it relocates itself. */
  . = 0x0 + SIZEOF_HEADERS;

  .text : {
    *(.text .text.*)
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  . = ALIGN(0x1000);
  .dynamic : { *(.dynamic) }
  .rela.dyn : { *(.rela.dyn) *(.rela.*) }
  /* pointers that need relocating are writable until the loader is
     done with them, so they belong with the data, not the text. */
  .data.rel.ro : {
    . = ALIGN(16);
    *(.data.rel.ro .data.rel.ro.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
    *(.got .got.plt)
  }
  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    *(COMMON)
  }
}
