; =============================================================================
; GraphiOS Atom Beta 1 — Bootloader (boot.asm)
; Architecture: x86_64
; Assembled with: nasm -f elf64
;
; WHAT THIS FILE DOES (for C#/Python devs):
;   Think of this as the "static constructor" that runs before main().
;   There is no OS under us. GRUB drops us here in 32-bit protected mode
;   and we must upgrade the CPU ourselves to 64-bit long mode before
;   calling our C kernel.
;
; EXECUTION FLOW:
;   GRUB → _start (32-bit) → setup_paging → enable_lm → lm_entry (64-bit) → kernel_main
; =============================================================================

[bits 32]

global _start
extern kernel_main      ; Defined in kernel.c

; =============================================================================
; SECTION: .multiboot
; The Multiboot2 header tells GRUB how to load us.
; It must appear within the first 32KB of the kernel image.
; The magic number 0xE85250D6 is how GRUB recognises a Multiboot2 kernel.
; =============================================================================
section .multiboot
align 8

mb_header_start:
    dd 0xE85250D6                                           ; Multiboot2 magic
    dd 0                                                    ; Architecture: x86 (i386)
    dd mb_header_end - mb_header_start                      ; Total header size
    ; Checksum: all four fields must sum to zero (mod 2^32)
    dd -(0xE85250D6 + 0 + (mb_header_end - mb_header_start))

    ; --- End tag: tells GRUB there are no more tags ---
    align 8
    dw 0        ; type = 0 (end)
    dw 0        ; flags
    dd 8        ; size of this tag
mb_header_end:


; =============================================================================
; SECTION: .bss  (Block Started by Symbol — zero-initialised, no space in ELF)
; Stack memory + page table buffers live here.
; Like C# fields initialized to default(T), or Python's None.
; =============================================================================
section .bss
align 16
global stack_top, stack_bottom

stack_bottom:
    resb 32768      ; 32 KB kernel stack
stack_top:          ; Stack grows DOWN, so we load esp/rsp = stack_top

; ------- 4-Level Paging structures (each must be 4096-byte aligned) ----------
; PML4  → PDPT  → PDT  → (2 MB huge pages, no PT needed)
; This identity-maps the first 1 GB of RAM: virtual addr == physical addr.
align 4096
pml4:   resb 4096   ; Page Map Level 4   (top-level, CR3 points here)
pdpt:   resb 4096   ; Page Directory Pointer Table
pdt:    resb 4096   ; Page Directory Table (each entry covers 2 MB)


; =============================================================================
; SECTION: .data  — The 64-bit Global Descriptor Table (GDT)
;
; In 64-bit mode the CPU requires a valid GDT with at least:
;   - A null descriptor  (mandatory, index 0)
;   - A code descriptor  (DPL0, 64-bit)
;   - A data descriptor  (DPL0)
;
; Segment base/limit are ignored in 64-bit mode for CS/DS/ES/SS.
; The bit flags that matter are: P (present), S (not system), L (long mode).
; =============================================================================
section .data
align 8

gdt64:
    ; --- [0x00] Null descriptor (required by spec) ---
    dq 0

    ; --- [0x08] Code segment ---
    ; Bit 43: Execute  (code)
    ; Bit 44: S=1      (not a system descriptor)
    ; Bit 47: P=1      (present)
    ; Bit 53: L=1      (64-bit long mode code segment)
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)

    ; --- [0x10] Data segment ---
    ; Bit 41: Write    (read/write data)
    ; Bit 44: S=1      (not a system descriptor)
    ; Bit 47: P=1      (present)
.data: equ $ - gdt64
    dq (1<<41) | (1<<44) | (1<<47)

; GDTR (GDT Register) descriptor — loaded with LGDT instruction
; Format: 2-byte limit (size - 1) + 8-byte base address
gdt64_ptr:
    dw gdt64_ptr - gdt64 - 1    ; limit = last valid byte offset
    dq gdt64                    ; 64-bit base address of GDT


; =============================================================================
; SECTION: .text — Executable code starts here
; =============================================================================
section .text

; -----------------------------------------------------------------------------
; _start — GRUB entry point (CPU is in 32-bit protected mode here)
;
; On entry:
;   EAX = 0x36d76289 (Multiboot2 magic — verify this!)
;   EBX = physical address of Multiboot2 info struct
; -----------------------------------------------------------------------------
_start:
    cli                     ; Disable hardware interrupts (we have no IDT yet)
    cld                     ; Clear direction flag (string ops go forward)
    mov esp, stack_top      ; Set up our temporary 32-bit stack

    ; Save the Multiboot2 info pointer — we'll pass it to kernel_main later.
    ; We push it onto the stack and retrieve it after entering 64-bit mode.
    push ebx

    ; --- Verify we were actually loaded by a Multiboot2-compliant bootloader ---
    cmp eax, 0x36d76289
    jne .error_no_multiboot

    ; --- Phase 1: Build identity-mapped page tables ---
    call setup_paging

    ; --- Phase 2: Enable long mode in the CPU's MSR ---
    call enable_long_mode

    ; --- Phase 3: Load our 64-bit GDT ---
    ; Note: in 32-bit mode, LGDT reads 2-byte limit + 4-byte base (6 bytes).
    ; Our GDT is within the first 4 GB, so the truncated 32-bit base is correct.
    lgdt [gdt64_ptr]

    ; --- Phase 4: Far-jump to the 64-bit code segment ---
    ; This flushes the instruction pipeline and activates 64-bit decode.
    ; "gdt64.code" = 0x08 (offset of code descriptor in our GDT).
    jmp gdt64.code:lm_entry

.error_no_multiboot:
    ; Write "ER" in bright red to the top-left of the VGA buffer so at least
    ; something visible happens if GRUB didn't set up multiboot correctly.
    mov dword [0xB8000], 0x4F524F45     ; 'ER' with red background (0x4F = white on red)
    mov dword [0xB8004], 0x4F214F52     ; 'R!'
    hlt


; -----------------------------------------------------------------------------
; setup_paging — Build minimal identity-mapped page tables
;
; Creates a 1 GB identity map using 2 MB huge pages (no level-4 PT needed).
; "Identity mapped" means: virtual address 0x100000 == physical address 0x100000.
; This keeps things simple for a proof-of-concept kernel.
;
; In Python terms: this is like dict comprehension { v: v for v in range(0, 1GB, 2MB) }
; -----------------------------------------------------------------------------
setup_paging:
    ; PML4[0] → pdpt  (flags: Present=1, ReadWrite=1)
    mov eax, pdpt
    or  eax, 0b11
    mov [pml4], eax

    ; PDPT[0] → pdt   (flags: Present=1, ReadWrite=1)
    mov eax, pdt
    or  eax, 0b11
    mov [pdpt], eax

    ; Fill all 512 PDT entries with consecutive 2 MB huge pages
    ; Each entry: (ecx * 2MB) | PageSize=1 | ReadWrite=1 | Present=1
    xor ecx, ecx
.pd_fill:
    mov eax, 0x200000       ; 2 MB
    mul ecx                 ; EAX = ecx * 2MB (physical base of this page)
    or  eax, 0b10000011     ; Huge(bit7) | ReadWrite(bit1) | Present(bit0)
    mov [pdt + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .pd_fill
    ret


; -----------------------------------------------------------------------------
; enable_long_mode — Flip the CPU from 32-bit protected mode to 64-bit
;
; Steps (hardware-mandated sequence, cannot be reordered):
;   1. Load CR3 with the PML4 physical address
;   2. Enable PAE (Physical Address Extension) in CR4
;   3. Set the LME (Long Mode Enable) bit in the IA32_EFER MSR (0xC0000080)
;   4. Enable paging in CR0 — this activates long mode
; -----------------------------------------------------------------------------
enable_long_mode:
    ; 1. Point CR3 at our PML4 table
    mov eax, pml4
    mov cr3, eax

    ; 2. Enable PAE in CR4 (bit 5)
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; 3. Set LME in the Extended Feature Enable Register (MSR 0xC0000080)
    mov ecx, 0xC0000080
    rdmsr               ; Read MSR into EDX:EAX
    or  eax, (1 << 8)   ; Set LME bit
    wrmsr               ; Write back

    ; 4. Enable paging (CR0.PG) and ensure protected mode is on (CR0.PE)
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)
    mov cr0, eax
    ; CPU is now in compatibility mode — it becomes true 64-bit after the far-jmp
    ret


; =============================================================================
; 64-BIT LONG MODE CODE
; After the far-jmp above, the CPU decodes instructions as 64-bit.
; =============================================================================
[bits 64]

; -----------------------------------------------------------------------------
; lm_entry — First 64-bit code
; Load segment registers, then call into the C kernel.
; -----------------------------------------------------------------------------
lm_entry:
    ; Reload all data segment registers with our 64-bit data descriptor.
    ; In 64-bit mode the base/limit are ignored for these; only the descriptor
    ; attributes matter (present, DPL, type).
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Retrieve the Multiboot2 info pointer we saved before.
    ; It was pushed as a 32-bit value, so mask the high 32 bits.
    pop rdi                     ; RDI = first argument (System V AMD64 ABI)
    and rdi, 0x00000000FFFFFFFF ; Zero-extend from 32-bit to 64-bit safely

    ; Jump into the C kernel. This will never return.
    call kernel_main

.halt:
    ; If kernel_main ever returns (it shouldn't), halt the CPU.
    hlt
    jmp .halt
