OUTPUT_ARCH( "riscv" )
ENTRY( start )

SECTIONS
{
 /* the ELF header and the program headers are part of the first
    segment, so that a program can find its own PT_TLS and PT_DYNAMIC
    through the auxiliary vector. */
 . = 0x0 + SIZEOF_HEADERS;
 
  .text : {
    *(.text .text.*)
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .eh_frame : {
       *(.eh_frame)
       *(.eh_frame.*)
   }

  . = ALIGN(0x1000);
  /* thread-local storage: .tdata first and .tbss after it, which is
     the order the TLS segment's offsets are computed in. left to
     itself the linker orphans them the other way round. */
  .tdata : {
    . = ALIGN(16);
    *(.tdata .tdata.*)
  }
  .tbss : {
    *(.tbss .tbss.*)
    *(.tcommon)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  }

  PROVIDE(end = .);
}
