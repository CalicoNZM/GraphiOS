#!/usr/bin/env bash
# =============================================================================
# GraphiOS Atom Beta 1 — Development Environment Setup
# Tested on: Ubuntu 22.04 LTS / 24.04 LTS / Debian 12
#
# What this script does:
#   1. Installs NASM, QEMU, GRUB tools, xorriso via apt
#   2. Checks for a cross-compiler (x86_64-elf-gcc)
#   3. If not found, downloads and builds one from source (GCC + Binutils)
#      into $HOME/opt/cross — this takes ~10 minutes, done once.
#   4. Adds the cross-compiler to your PATH in ~/.bashrc
#
# Run as: chmod +x setup.sh && ./setup.sh
# =============================================================================

set -euo pipefail

CROSS_PREFIX="$HOME/opt/cross"
CROSS_BIN="$CROSS_PREFIX/bin"
TARGET="x86_64-elf"

GCC_VER="13.2.0"
BINUTILS_VER="2.41"

BOLD="\033[1m"
GREEN="\033[1;32m"
YELLOW="\033[1;33m"
RED="\033[1;31m"
RESET="\033[0m"

info()  { echo -e "${GREEN}  [OK]${RESET} $1"; }
warn()  { echo -e "${YELLOW}  [??]${RESET} $1"; }
error() { echo -e "${RED}  [!!]${RESET} $1"; exit 1; }
step()  { echo -e "\n${BOLD}==> $1${RESET}"; }

# -----------------------------------------------------------------------------
step "GraphiOS Atom Beta 1 — Setup"
echo "  Target machine: $(uname -m) running $(lsb_release -ds 2>/dev/null || uname -s)"
echo ""

# -----------------------------------------------------------------------------
step "Installing system packages"

sudo apt-get update -q
sudo apt-get install -y \
    build-essential     \
    nasm                \
    grub-pc-bin         \
    grub-common         \
    xorriso             \
    mtools              \
    qemu-system-x86     \
    wget                \
    bison               \
    flex                \
    libgmp-dev          \
    libmpfr-dev         \
    libmpc-dev          \
    texinfo             \
    python3

info "System packages installed."

# -----------------------------------------------------------------------------
step "Checking for x86_64-elf cross-compiler"

if command -v x86_64-elf-gcc &>/dev/null; then
    info "x86_64-elf-gcc already available: $(x86_64-elf-gcc --version | head -1)"
    NEED_CROSS=0
elif [ -x "$CROSS_BIN/x86_64-elf-gcc" ]; then
    info "Cross-compiler found at $CROSS_BIN"
    NEED_CROSS=0
    # Make sure it's on PATH for this session
    export PATH="$CROSS_BIN:$PATH"
else
    warn "x86_64-elf-gcc not found. Will build cross-compiler from source."
    NEED_CROSS=1
fi

# -----------------------------------------------------------------------------
if [ "$NEED_CROSS" -eq 1 ]; then
    step "Building cross-compiler (GCC $GCC_VER + Binutils $BINUTILS_VER)"
    warn "This takes ~10 minutes. Do it once."

    mkdir -p "$CROSS_PREFIX" "$HOME/src/cross-build"
    cd "$HOME/src/cross-build"

    # --- Download sources ---
    if [ ! -f "binutils-$BINUTILS_VER.tar.gz" ]; then
        info "Downloading Binutils $BINUTILS_VER..."
        wget -q "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.gz"
    fi

    if [ ! -f "gcc-$GCC_VER.tar.gz" ]; then
        info "Downloading GCC $GCC_VER..."
        wget -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.gz"
    fi

    # --- Extract ---
    [ -d "binutils-$BINUTILS_VER" ] || tar -xf "binutils-$BINUTILS_VER.tar.gz"
    [ -d "gcc-$GCC_VER"           ] || tar -xf "gcc-$GCC_VER.tar.gz"

    # --- Build Binutils ---
    info "Building Binutils..."
    mkdir -p build-binutils && cd build-binutils
    ../binutils-$BINUTILS_VER/configure \
        --target="$TARGET"              \
        --prefix="$CROSS_PREFIX"        \
        --with-sysroot                  \
        --disable-nls                   \
        --disable-werror                \
        2>&1 | tail -3
    make -j"$(nproc)" 2>&1 | tail -3
    make install 2>&1 | tail -1
    cd ..

    # --- Build GCC (C only, no C++) ---
    info "Building GCC (C frontend only)..."
    mkdir -p build-gcc && cd build-gcc
    ../gcc-$GCC_VER/configure          \
        --target="$TARGET"              \
        --prefix="$CROSS_PREFIX"        \
        --disable-nls                   \
        --enable-languages=c            \
        --without-headers               \
        2>&1 | tail -3
    make -j"$(nproc)" all-gcc 2>&1 | tail -3
    make -j"$(nproc)" all-target-libgcc 2>&1 | tail -3
    make install-gcc install-target-libgcc 2>&1 | tail -1
    cd ..

    export PATH="$CROSS_BIN:$PATH"
    info "Cross-compiler built: $CROSS_BIN/x86_64-elf-gcc"
fi

# -----------------------------------------------------------------------------
step "Configuring PATH"

PATH_LINE="export PATH=\"$CROSS_BIN:\$PATH\""

if ! grep -qF "$CROSS_BIN" "$HOME/.bashrc" 2>/dev/null; then
    echo "" >> "$HOME/.bashrc"
    echo "# GraphiOS Atom cross-compiler" >> "$HOME/.bashrc"
    echo "$PATH_LINE" >> "$HOME/.bashrc"
    info "Added cross-compiler to ~/.bashrc"
else
    info "~/.bashrc already references $CROSS_BIN"
fi

# Also update current session
export PATH="$CROSS_BIN:$PATH"

# -----------------------------------------------------------------------------
step "Patching Makefile if needed"

# If the cross-compiler ended up at a non-standard name, patch the Makefile
if ! command -v x86_64-elf-gcc &>/dev/null; then
    warn "x86_64-elf-gcc still not in PATH — patching Makefile to use system GCC"
    # Return to project root (assume setup.sh is run from there)
    cd "$(dirname "$0")"
    sed -i "s|^CC.*:=.*x86_64-elf-gcc|CC       := gcc|"  Makefile
    sed -i "s|^LD.*:=.*x86_64-elf-ld|LD       := ld|"    Makefile
    # Remove -mcmodel=kernel if using system gcc on a non-kernel setup
    sed -i "s|-mcmodel=kernel||" Makefile
    info "Makefile patched to use system GCC."
fi

# -----------------------------------------------------------------------------
step "Setup complete!"
echo ""
echo "  Next steps:"
echo ""
echo "    cd $(dirname "$0")"
echo "    make              # Build the kernel ISO"
echo "    make run          # Launch in QEMU with a display window"
echo "    make run-nox      # Launch in QEMU serial-only (no display needed)"
echo ""
echo "  If this is a new terminal session, first run:"
echo "    source ~/.bashrc"
echo ""
