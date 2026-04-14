/* =============================================================================
 * GraphiOS Atom Beta 1 — Linker Script (linker.ld)
 *
 * This script tells the linker (ld) where to place each section of the kernel
 * binary in physical memory.
 *
 * Why 1 MB (0x100000)?
 *   The first megabyte of RAM is occupied by the BIOS, VGA buffers, and
 *   legacy hardware mappings. The conventional start address for hobby
 *   kernels — and what GRUB's multiboot spec targets — is 0x100000 (1 MB).
 *
 * Section order matters:
 *   .multiboot must come first so GRUB finds the magic number quickly.
 *   .text (code) before .rodata (constants) before .data before .bss.
 * ============================================================================= */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    /* Load the kernel at 1 MB physical address */
    . = 1M;

    /* The Multiboot2 header must be within the first 32 KB of the image.
     * By placing it first, this is guaranteed. */
    .multiboot : {
        KEEP(*(.multiboot))
    }

    /* Executable machine code */
    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    /* Read-only data: string literals, const arrays, vtables */
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    /* Initialised writable data: global variables with an initial value */
    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    /*
     * Uninitialised / zero-initialised data (Block Started by Symbol).
     * The bootloader/GRUB guarantees these are zeroed before _start runs.
     * No bytes are written into the ELF file for .bss — only the size is
     * recorded, saving disk/ISO space.
     */
    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }
}
