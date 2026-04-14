# =============================================================================
# GraphiOS Atom Beta 1 — Build System (Makefile)
#
# Targets:
#   make          — Build everything → graphios_atom.iso
#   make run      — Build + run in QEMU (requires display server)
#   make run-nox  — Build + run in QEMU (no display, serial output)
#   make debug    — Build + run in QEMU with GDB server on :1234
#   make clean    — Remove all build artifacts
#
# Requirements (install via:  sudo ./setup.sh):
#   nasm, x86_64-elf-gcc (or gcc), ld, grub-mkrescue, xorriso, qemu-system-x86_64
# =============================================================================

# ---- Detect cross-compiler or fall back to system GCC ----------------------
# A proper cross-compiler (x86_64-elf-gcc) avoids any host-system assumptions.
# setup.sh will install it or patch this file to use system gcc if unavailable.
CC       := x86_64-elf-gcc
LD       := x86_64-elf-ld
AS       := nasm
MKISO    := grub-mkrescue
QEMU     := qemu-system-x86_64

# ---- Compiler flags ---------------------------------------------------------
# -ffreestanding   : No standard library. We provide everything ourselves.
# -fno-stack-protector : __stack_chk_fail doesn't exist without libc.
# -mno-red-zone    : x86_64 ABI reserves 128 bytes below RSP ("red zone").
#                    Kernel code (and especially interrupt handlers) must not
#                    use the red zone — disable it entirely.
# -nostdlib        : Don't link crt0, libc, etc.
# -O2              : Optimise for speed (safe for kernel code).
# -Wall -Wextra    : Enable comprehensive warnings.
# -std=c11         : Use C11 standard.
# -mcmodel=kernel  : Tell GCC that code lives in the upper 2 GB of the
#                    address space (required for kernel position assumptions).
CFLAGS   := \
    -ffreestanding        \
    -fno-stack-protector  \
    -mno-red-zone         \
    -nostdlib             \
    -O2                   \
    -Wall                 \
    -Wextra               \
    -std=c11              \
    -m64                  \
    -mcmodel=kernel

# ---- Assembler flags --------------------------------------------------------
# -f elf64 : Output a 64-bit ELF object file.
ASFLAGS  := -f elf64

# ---- Linker flags -----------------------------------------------------------
# -T linker.ld : Use our custom linker script.
# -nostdlib    : No startup files or standard libraries.
LDFLAGS  := -T linker.ld -nostdlib

# ---- Source and output paths ------------------------------------------------
SRC_BOOT := src/boot/boot.asm
SRC_KERN := src/kernel/kernel.c

OBJ_BOOT := build/boot.o
OBJ_KERN := build/kernel.o

ELF      := build/graphios_atom.elf
ISO      := graphios_atom.iso

# =============================================================================
# DEFAULT TARGET
# =============================================================================
.PHONY: all
all: $(ISO)

# =============================================================================
# STEP 1: Assemble the bootloader
# =============================================================================
$(OBJ_BOOT): $(SRC_BOOT) | build/
	@echo "  [AS]  $<"
	$(AS) $(ASFLAGS) $< -o $@

# =============================================================================
# STEP 2: Compile the C kernel
# =============================================================================
$(OBJ_KERN): $(SRC_KERN) | build/
	@echo "  [CC]  $<"
	$(CC) $(CFLAGS) -c $< -o $@

# =============================================================================
# STEP 3: Link boot + kernel into a single ELF binary
# =============================================================================
$(ELF): $(OBJ_BOOT) $(OBJ_KERN) linker.ld | build/
	@echo "  [LD]  $@"
	$(LD) $(LDFLAGS) $(OBJ_BOOT) $(OBJ_KERN) -o $@
	@echo "  [OK]  ELF binary: $@"

# =============================================================================
# STEP 4: Create bootable ISO with GRUB
# grub-mkrescue builds an El Torito ISO that most VMs (QEMU, VirtualBox,
# VMware) can boot directly.
# =============================================================================
$(ISO): $(ELF)
	@echo "  [ISO] Building bootable image..."
	@mkdir -p iso/boot/grub
	@cp $(ELF) iso/boot/graphios_atom.elf
	@cp grub/grub.cfg iso/boot/grub/grub.cfg
	@$(MKISO) -o $(ISO) iso 2>/dev/null
	@echo ""
	@echo "  ============================================="
	@echo "  GraphiOS Atom Beta 1 — Build complete"
	@echo "  Output : $(ISO)"
	@echo "  Run    : make run"
	@echo "  ============================================="

# =============================================================================
# RUN TARGETS
# =============================================================================

# Run with a display window (requires X11 or Wayland)
.PHONY: run
run: $(ISO)
	$(QEMU) \
	    -cdrom $(ISO)       \
	    -m 128M             \
	    -name "GraphiOS Atom Beta 1"

# Run without a display (pure serial — VGA output won't be visible)
.PHONY: run-nox
run-nox: $(ISO)
	$(QEMU) \
	    -cdrom $(ISO)       \
	    -m 128M             \
	    -nographic          \
	    -serial mon:stdio

# Run with GDB remote debug server suspended at entry point.
# In a second terminal: gdb build/graphios_atom.elf
#   (gdb) target remote localhost:1234
#   (gdb) b kernel_main
#   (gdb) continue
.PHONY: debug
debug: $(ISO)
	@echo "  [DBG] QEMU paused. Connect: gdb build/graphios_atom.elf"
	@echo "        Then: target remote localhost:1234"
	$(QEMU) \
	    -cdrom $(ISO)       \
	    -m 128M             \
	    -s -S               \
	    -nographic          \
	    -serial mon:stdio

# =============================================================================
# UTILITY TARGETS
# =============================================================================

# Create the build output directory
build/:
	@mkdir -p build

# Remove all generated files
.PHONY: clean
clean:
	@echo "  [CLN] Removing build artifacts..."
	@rm -rf build/ iso/ $(ISO)
	@echo "  [OK]  Clean."

# Show build size information
.PHONY: size
size: $(ELF)
	@echo "  Kernel ELF sections:"
	@size $(ELF)
