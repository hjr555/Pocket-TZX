/* Linker script for standalone ATJ2157 TZX Mode foreground application (P/01).
 * Mirrors UDISK.AP's exact single-segment flat resident SRAM layout inside
 * 0x00108800..0x0010E800 (24 KB, strictly below APENC.AL top 0x0010EA28,
 * AUD_DEV.DRV DMA ringbuffers 0x0010F000+, and OS exception stubs 0x0010FC00),
 * leaving 0x00200000..0x00205FFF (FAT32N/KERNEL/LCD/APP_GLOBE_DATA/KEY/CARD/UI)
 * 100% untouched while using the foreground stack top ptos = 0x00206AC0.
 */
ENTRY(_start)

MEMORY
{
    RSINGLE (rwx) : ORIGIN = 0x00108800, LENGTH = 0x00006000
}

SECTIONS
{
    .text 0x00108800 : ALIGN(4)
    {
        __code_start = .;
        KEEP(*(.text.crt0))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        __code_end = .;
    } > RSINGLE

    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RSINGLE

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RSINGLE

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment*)
        *(.note*)
    }
}
