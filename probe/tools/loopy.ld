/* From Kasami's loopy-homebrew-template (zlib; LICENSE-loopy-template.txt).
   ALTERED: the .onchip section (code copied into the CPU's on-chip RAM at
   boot) and libgcc's helpers in work RAM, as in the author's Loopy ports.

   Symbols ROMBASE, SRAMBASE, SRAMSIZE, STACKSIZE defined in makefile. */

OUTPUT_FORMAT("elf32-sh", "elf32-sh", "elf32-sh")
OUTPUT_ARCH(sh)

MEMORY {
    /* Console side */
    RAM (rwx) : ORIGIN = 0x09000000, LENGTH = 512K - STACKSIZE
    /* Cartridge side */
    ROM (rwx) : ORIGIN = ROMBASE, LENGTH = 4M
    SRAM (rwx) : ORIGIN = SRAMBASE, LENGTH = SRAMSIZE
    /* The CPU's own 1 KB, less the BIOS's last 16 bytes (the Megadoc,
       "RAM Allocation") */
    ONCHIP (rwx) : ORIGIN = 0x0F000000, LENGTH = 0x3F0
}

SECTIONS {
  . = ORIGIN(ROM);

  .header : {
    KEEP(*(.header.pointers))
    KEEP(*(.header.copyright))

    . = 0x80;
  } >ROM =0xFF

  .vectors ALIGN(4) : SUBALIGN(4) {
    KEEP(*(.vectors))

    . = 0x1D0;

    /* 560 bytes spare in vectable, put small functions here */
    KEEP(*(.lowfunc))
    KEEP(*(.smallfunc))
  } >ROM =0xFF

  . = ORIGIN(ROM) + 0x480;

  .text ALIGN(4) : SUBALIGN(4)
  {
    *(.text.startup)
    /* libgcc's shift and multiply helpers go to work RAM instead (see
       .data below). */
    *(EXCLUDE_FILE(*libgcc.a:_ashiftlt.o *libgcc.a:_lshiftrt.o
                   *libgcc.a:_ashiftrt.o *libgcc.a:_ashiftrt_n.o
                   *libgcc.a:_mulsi3.o) .text)
    *(EXCLUDE_FILE(*libgcc.a:_ashiftlt.o *libgcc.a:_lshiftrt.o
                   *libgcc.a:_ashiftrt.o *libgcc.a:_ashiftrt_n.o
                   *libgcc.a:_mulsi3.o) .text.*)

    /*
    This and the following `. = ALIGN(4)` expressions are intended to
    encourage LMA alignment. Without this, the load addresses of AT>
    sections such as .data can be misaligned, causes issues in crt0.c
    if `.data` is copied using 32-bit reads/writes.
    */
    . = ALIGN(4);
  } >ROM =0xFF

  __text_link_start = ADDR(.text);
  __text_link_end = ADDR(.text) + SIZEOF(.text);

  .rodata ALIGN(4) : SUBALIGN(4) {
    *(.rodata)
    *(.rodata.*)

    . = ALIGN(4);
  } >ROM

  __rodata_link_start = ADDR(.rodata);
  __rodata_link_end = ADDR(.rodata) + SIZEOF(.rodata);

  .ctors ALIGN(4) : SUBALIGN(4)
  {
    KEEP(*(.ctors))
    KEEP(*(.ctors.*))

    . = ALIGN(4);
  } >ROM

  __ctors_link_start = ADDR(.ctors);
  __ctors_link_end = ADDR(.ctors) + SIZEOF(.ctors);

  /* reserve 256 bytes at the beginning of RAM for the BIOS */
  . = ORIGIN(RAM) + 0x100;

  .data ALIGN(4) : SUBALIGN(4) {
    *(.data)
    *(.data.*)

    KEEP(*(.ramtext))
    KEEP(*(.ramfunc))

    /*
    The SH-1 shifts by 1, 2, 8 or 16 places and nothing else, so every
    shift by a variable amount is a call to one of these libgcc routines.
    Left in the cartridge, each call pays the cartridge's slow instruction
    fetches.
    */
    *libgcc.a:_ashiftlt.o(.text .text.*)
    *libgcc.a:_lshiftrt.o(.text .text.*)
    *libgcc.a:_ashiftrt.o(.text .text.*)
    *libgcc.a:_ashiftrt_n.o(.text .text.*)
    /* The SH-1 multiplies 16 by 16 bits; a 32-bit product is a call to this. */
    *libgcc.a:_mulsi3.o(.text .text.*)

    . = ALIGN(4);
  } >RAM AT> ROM

  __data_link_start = ADDR(.data);
  __data_link_end = ADDR(.data) + SIZEOF(.data);
  __data_load_start = LOADADDR(.data);

  .bss ALIGN(4) (NOLOAD) : SUBALIGN(4) {
    *(.bss)
    *(.bss.*)
    *(COMMON)

    . = ALIGN(4);
  } >RAM

  __bss_link_start = ADDR(.bss);
  __bss_link_end = ADDR(.bss) + SIZEOF(.bss);

  /*
  The CPU's on-chip RAM: one-cycle 32-bit fetches that leave the bus to
  the code's data. The probe's raster-timing loops live here (src/timing.c,
  src/probes.s); the rest of the 1 KB, after __onchip_link_end, is where
  the memory probes are copied to run (src/memcost.c, src/raster.c).
  Copied up at boot like .data; its load address follows .data's in the
  cartridge.
  */
  .onchip : SUBALIGN(4) {
    KEEP(*(.onchip))
    *(.onchip.*)

    . = ALIGN(4);
  } >ONCHIP AT> ROM

  __onchip_link_start = ADDR(.onchip);
  __onchip_link_end = ADDR(.onchip) + SIZEOF(.onchip);
  __onchip_load_start = LOADADDR(.onchip);

  /* Go back and pad ROM now that we know where .onchip ends. */
  . = LOADADDR(.onchip) + SIZEOF(.onchip);
  .rompad : {
    /* Always at least one byte of padding, LD won't output empty section */
    BYTE(0xFF);
    . = ALIGN(2);
    _even_padded = .;
    . = ALIGN(4096);
  } >ROM =0xFF

  INCLUDE "tools/debug.ld"
}

__clheader_romfirst = ADDR(.vectors);
__clheader_romlast = _even_padded - 2;
__clheader_sramfirst = ORIGIN(SRAM);
__clheader_sramlast = ORIGIN(SRAM) + LENGTH(SRAM) - 1;

__stack_end = ORIGIN(RAM) + LENGTH(RAM) + STACKSIZE;

__vbr_link_start = ADDR(.vectors);

ENTRY(start);

INCLUDE "tools/address_sh7021.ld"
INCLUDE "tools/address_loopy.ld"
