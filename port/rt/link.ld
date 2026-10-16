/* link.ld - mapa de um programa do SF-100: código em 0x1000 (IPL_LOAD_ADDR), heap logo após o BSS. */
ENTRY(_start)
SECTIONS
{
  . = 0x1000;
  .text : { *(.text.start) *(.text .text.*) }
  . = ALIGN(16);
  __code_end = .;
  .rodata : { *(.rodata .rodata.* .rodata1) }
  . = ALIGN(4);
  .data : { *(.data .data.*) }
  . = ALIGN(4);
  __bss_start = .;
  .bss : { *(.bss .bss.*) *(COMMON) }
  . = ALIGN(16);
  _end = .;
  /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
