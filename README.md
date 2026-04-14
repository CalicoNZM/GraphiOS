# GraphiOS Atom — Beta 1
### A Semantic Operating System Proof of Concept
#### Architecture: x86_64 · Freestanding C · Multiboot2 · GRUB

---

## What Is This?

GraphiOS Atom is a bootable hobby OS kernel demonstrating the **Semantic OS** concept:
instead of a File Allocation Table (FAT) with paths and extensions, the kernel stores
data as **Atoms** — self-describing objects with UUIDs, semantic types, and tags.

You query by *meaning*, not by location.

This is a real kernel. It:
- Boots on bare metal x86_64 hardware or any VM (QEMU, VirtualBox, VMware)
- Transitions the CPU from 32-bit protected mode → 64-bit long mode
- Drives the VGA text buffer directly (no GPU driver needed)
- Reads the PS/2 keyboard via I/O ports (no OS below it)
- Manages an in-RAM graph of Data Atoms with UUIDs and semantic tags
- Exposes a small interactive shell

---

## File Structure

```
graphios-atom/
│
├── src/
│   ├── boot/
│   │   └── boot.asm          ← NASM: Multiboot2 header + 32→64-bit boot
│   └── kernel/
│       └── kernel.c          ← C: VGA, keyboard, Atoms, shell
│
├── linker.ld                 ← Tells the linker where to place each section
├── grub/
│   └── grub.cfg              ← GRUB menu configuration
│
├── Makefile                  ← Build system
├── setup.sh                  ← One-shot dependency installer
└── README.md                 ← This file
```

---

## Quick Start (Ubuntu 22.04 / 24.04)

### Step 1: Install dependencies

```bash
chmod +x setup.sh
./setup.sh
```

This installs NASM, QEMU, GRUB tools, xorriso, and builds an `x86_64-elf` cross-compiler
if one isn't already present. Takes ~10 minutes the first time (cross-compiler build).

### Step 2: Build the ISO

```bash
make
```

Output: `graphios_atom.iso`

### Step 3: Run in QEMU

```bash
make run          # Opens a display window (requires a desktop environment)
# — or —
make run-nox      # Serial only, works over SSH / headless VMs
```

---

## Running in VirtualBox or VMware

1. Create a new VM:
   - Type: **Other** / **Other (64-bit)**
   - RAM: **128 MB** (minimum)
   - No hard disk needed

2. Mount `graphios_atom.iso` as the **primary optical drive**

3. Boot the VM

GRUB will appear with a 5-second countdown, then GraphiOS loads.

---

## The Shell

Once booted, you get an interactive prompt:

```
  atom>
```

| Command | Description |
|---|---|
| `help` | List all commands |
| `ls` | List all atoms in the store |
| `atom <n>` | Inspect atom #n (UUID, type, tags, content preview) |
| `new <label>` | Create a new THOUGHT atom |
| `tag <n> <tag>` | Add a semantic tag to atom #n |
| `write <n> <text>` | Append text to atom #n payload |
| `about` | System info (architecture, atom count, tick counter) |
| `clear` | Clear the screen |

### Example session

```
  atom> ls
  atom> atom 0
  atom> new Grocery List
  atom> tag 3 food
  atom> write 3 Mangoes, bread, avocado
  atom> atom 3
```

---

## Architecture Notes (for C#/Python developers)

### Why Assembly for the boot file?

The C compiler generates code that assumes a working stack, zero-initialised
globals, and a 64-bit CPU. None of those are true when GRUB first hands us
control. Assembly lets us set up those preconditions manually before calling
into C.

### Why no `#include <stdio.h>`?

We are the OS. There is no OS below us to call. `printf` ultimately calls
`write()`, which is a system call to a kernel — but we *are* the kernel.
Everything the screen, keyboard, and memory do must be implemented from scratch.

### The 32 → 64-bit transition

Modern x86 CPUs start in **real mode** (16-bit, 1986-era). GRUB does us the
favour of getting to **protected mode** (32-bit). We still need to:
1. Build page tables and point CR3 at them (memory mapping)
2. Enable PAE and set the LME bit (tell the CPU to enter long mode)
3. Enable paging (activates long mode)
4. Far-jump with a 64-bit code segment selector (switches the decoder to 64-bit)

### Data Atoms vs. Files

| Traditional FS | GraphiOS Atom |
|---|---|
| Filename: `notes.txt` | Label: `Grocery List` |
| Extension: `.txt` | Type: `ATOM_TEXT` |
| Path: `/home/user/docs/` | UUID: `A701BE74-0001-...` |
| No metadata | Tags: `[food][personal]` |
| Modified date | `modified_tick` counter |

---

## Build System Reference

```bash
make              # Build ISO
make run          # Build + run QEMU with display
make run-nox      # Build + run QEMU headless (serial)
make debug        # Build + run QEMU with GDB server on :1234
make size         # Show ELF section sizes
make clean        # Remove build/ iso/ and the ISO file
```

---

## Extending GraphiOS

Beta 1 is intentionally minimal. The natural next steps are:

| Feature | Where to implement |
|---|---|
| IDT + interrupt handlers (real keyboard IRQ) | `src/kernel/idt.c` |
| Physical memory manager (bitmap allocator) | `src/kernel/pmm.c` |
| Heap allocator (so `atom_create` uses dynamic memory) | `src/kernel/heap.c` |
| Persistent Atom store (ATA/AHCI disk driver) | `src/drivers/disk.c` |
| Semantic query engine (tag-based search) | `src/kernel/query.c` |
| ACPI parser (graceful shutdown) | `src/kernel/acpi.c` |

---

## License

GraphiOS Atom Beta 1 is released for educational use.
Build something interesting.
