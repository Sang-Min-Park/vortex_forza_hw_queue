OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  /* HTIF polls RAM: the completion word must bypass the write-back cache. */
  . = 0xF000;
  .tohost : { *(.tohost) }
  . = 0x80000000;
  .text : { *(.text.init) *(.text .text.*) }
  . = ALIGN(64);
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
}
