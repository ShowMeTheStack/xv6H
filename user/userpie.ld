OUTPUT_ARCH( "riscv" )
ENTRY( start )

SECTIONS
{
  /* a dynamic program is position independent: the kernel places it
     where it likes and the loader relocates it. the headers belong to
     the first segment so that it can find its own PT_TLS and
     PT_DYNAMIC through the auxiliary vector. */
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

  .eh_frame : {
    *(.eh_frame)
    *(.eh_frame.*)
  }

  . = ALIGN(0x1000);
  .dynamic : { *(.dynamic) }
  .rela.dyn : { *(.rela.dyn) *(.rela.*) }

  /* .tdata before .tbss: that is the order the TLS offsets follow */
  .tdata : {
    . = ALIGN(16);
    *(.tdata .tdata.*)
  }
  .tbss : {
    *(.tbss .tbss.*)
    *(.tcommon)
  }

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

  PROVIDE(end = .);
}
