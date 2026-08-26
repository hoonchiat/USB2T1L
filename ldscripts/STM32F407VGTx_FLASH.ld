/*
 * Linker script for STM32F407VGTx
 *   FLASH : 896 KB @ 0x08000000  (sectors 0..10; code/rodata/data)
 *   CONFIG:  128 KB @ 0x080E0000  (sector 11; reserved for the production
 *                                  record, written out-of-band — never by the
 *                                  build, so no section is placed here)
 *   RAM   :  128 KB @ 0x20000000  (main SRAM: SRAM1+SRAM2)
 *   CCMRAM:   64 KB @ 0x10000000  (core-coupled, not DMA/USB accessible)
 */
ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* top of RAM (stack grows down) */

_Min_Heap_Size = 0x400;    /* newlib heap (mostly unused; FreeRTOS heap_4) */
_Min_Stack_Size = 0x800;   /* main()/ISR stack before scheduler starts     */

MEMORY
{
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 896K
  CONFIG (r)   : ORIGIN = 0x080E0000, LENGTH = 128K
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

/* Reserved production-record sector (must match PRODINFO_FLASH_ADDR). */
_prodinfo_start = ORIGIN(CONFIG);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* Core-coupled RAM: usable for fast data that DMA/USB do not touch. */
  _siccmram = LOADADDR(.ccmram);
  .ccmram :
  {
    . = ALIGN(4);
    _sccmram = .;
    *(.ccmram)
    *(.ccmram*)
    . = ALIGN(4);
    _eccmram = .;
  } >CCMRAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
