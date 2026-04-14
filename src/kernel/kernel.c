/* =============================================================================
 * GraphiOS Atom Beta 1 — Kernel (kernel.c)
 * Architecture: x86_64  |  Environment: Freestanding (no stdlib)
 *
 * FOR C# / PYTHON DEVELOPERS:
 *   - There is no runtime beneath us. No malloc, no printf, no OS calls.
 *   - Every device (screen, keyboard) is accessed by reading/writing
 *     specific physical memory addresses or CPU I/O ports directly.
 *   - Think of the VGA buffer like a 2D array at a fixed memory address.
 *   - Think of I/O ports like method calls into the hardware itself.
 *
 * ARCHITECTURE OVERVIEW:
 *   kernel_main()
 *     ├── VGA text mode driver  (draws to 0xB8000)
 *     ├── PS/2 keyboard driver  (reads from I/O port 0x60)
 *     ├── Data Atom store       (in-RAM semantic object graph)
 *     └── Shell REPL            (Read-Eval-Print Loop)
 * =============================================================================
 */

/* ---------------------------------------------------------------------------
 * TYPE DEFINITIONS
 * No <stdint.h> in a freestanding environment, so we define our own.
 * These map directly to fixed-width CPU registers.
 * --------------------------------------------------------------------------- */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef uint64_t            size_t;

/* Boolean — equivalent to Python's True/False or C#'s bool */
typedef int bool;
#define true  1
#define false 0

#define NULL ((void*)0)


/* =============================================================================
 * VGA TEXT MODE DRIVER
 *
 * The VGA text buffer lives at physical address 0xB8000.
 * It is a flat array of 80*25 = 2000 entries, each 2 bytes:
 *   Byte 0 (low):  ASCII character code
 *   Byte 1 (high): Colour attribute = (foreground | background << 4)
 *
 * In C# terms: think of it as an unsafe fixed-size struct array.
 * ============================================================================= */

#define VGA_BUFFER  ((volatile uint16_t *)0xB8000)
#define VGA_COLS    80
#define VGA_ROWS    25

/* VGA colour palette (4-bit values, 0–15) */
typedef enum {
    COLOR_BLACK         = 0,
    COLOR_BLUE          = 1,
    COLOR_GREEN         = 2,
    COLOR_CYAN          = 3,
    COLOR_RED           = 4,
    COLOR_MAGENTA       = 5,
    COLOR_BROWN         = 6,
    COLOR_LIGHT_GREY    = 7,
    COLOR_DARK_GREY     = 8,
    COLOR_LIGHT_BLUE    = 9,
    COLOR_LIGHT_GREEN   = 10,
    COLOR_LIGHT_CYAN    = 11,
    COLOR_LIGHT_RED     = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_YELLOW        = 14,
    COLOR_WHITE         = 15,
} VgaColor;

/* Terminal state — the "screen context" (like a static class in C#) */
static int     g_col   = 0;
static int     g_row   = 0;
static uint8_t g_color = 0;

/* Compose a colour byte from foreground and background */
static inline uint8_t make_color(VgaColor fg, VgaColor bg) {
    return (uint8_t)((uint8_t)fg | ((uint8_t)bg << 4));
}

/* Compose a VGA cell from a character and colour byte */
static inline uint16_t make_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* ---------------------------------------------------------------------------
 * vga_move_cursor — Move the blinking hardware cursor to (row, col)
 *
 * The VGA CRTC (CRT Controller) exposes an index port (0x3D4) and a data
 * port (0x3D5). We write the cursor position register index, then the value.
 * Register 0x0E = high byte of cursor position.
 * Register 0x0F = low  byte of cursor position.
 * --------------------------------------------------------------------------- */
static void vga_move_cursor(int row, int col) {
    uint16_t pos = (uint16_t)(row * VGA_COLS + col);
    /* Write low byte */
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x0F), "Nd"((uint16_t)0x3D4));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)(pos & 0xFF)), "Nd"((uint16_t)0x3D5));
    /* Write high byte */
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x0E), "Nd"((uint16_t)0x3D4));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)(pos >> 8)), "Nd"((uint16_t)0x3D5));
}

/* ---------------------------------------------------------------------------
 * term_clear — Fill every cell with spaces of a given background colour
 * --------------------------------------------------------------------------- */
static void term_clear(VgaColor bg) {
    g_color = make_color(COLOR_WHITE, bg);
    uint16_t blank = make_entry(' ', g_color);
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BUFFER[i] = blank;
    }
    g_col = 0;
    g_row = 0;
    vga_move_cursor(0, 0);
}

/* ---------------------------------------------------------------------------
 * term_scroll — Move all rows up by one, blank the last row
 * Called automatically when we write past row 24.
 * --------------------------------------------------------------------------- */
static void term_scroll(void) {
    for (int row = 0; row < VGA_ROWS - 1; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            VGA_BUFFER[row * VGA_COLS + col] =
                VGA_BUFFER[(row + 1) * VGA_COLS + col];
        }
    }
    uint16_t blank = make_entry(' ', g_color);
    for (int col = 0; col < VGA_COLS; col++) {
        VGA_BUFFER[(VGA_ROWS - 1) * VGA_COLS + col] = blank;
    }
    g_row = VGA_ROWS - 1;
}

/* ---------------------------------------------------------------------------
 * term_putchar — Write one character to the terminal at the current position
 * Handles: newline, carriage return, backspace, and ordinary printable chars.
 * --------------------------------------------------------------------------- */
static void term_putchar(char c) {
    if (c == '\n') {
        g_col = 0;
        g_row++;
    } else if (c == '\r') {
        g_col = 0;
    } else if (c == '\b') {
        if (g_col > 0) {
            g_col--;
            VGA_BUFFER[g_row * VGA_COLS + g_col] = make_entry(' ', g_color);
        }
    } else {
        VGA_BUFFER[g_row * VGA_COLS + g_col] = make_entry(c, g_color);
        g_col++;
    }

    if (g_col >= VGA_COLS) {
        g_col = 0;
        g_row++;
    }
    if (g_row >= VGA_ROWS) {
        term_scroll();
    }
    vga_move_cursor(g_row, g_col);
}

/* Set the current drawing colour */
static void term_color_set(VgaColor fg, VgaColor bg) {
    g_color = make_color(fg, bg);
}

/* Write a null-terminated string */
static void kprint(const char *s) {
    for (int i = 0; s[i] != '\0'; i++) {
        term_putchar(s[i]);
    }
}

/* Write a string in a specific colour, then restore the previous colour */
static void kprint_col(const char *s, VgaColor fg, VgaColor bg) {
    uint8_t saved = g_color;
    term_color_set(fg, bg);
    kprint(s);
    g_color = saved;
}

/* Write a horizontal rule (line of dashes) */
static void kprint_rule(char ch, int len, VgaColor fg, VgaColor bg) {
    uint8_t saved = g_color;
    term_color_set(fg, bg);
    for (int i = 0; i < len; i++) term_putchar(ch);
    term_putchar('\n');
    g_color = saved;
}


/* =============================================================================
 * I/O PORT HELPERS
 *
 * x86 I/O ports are a separate address space from RAM, accessed via the
 * IN and OUT instructions. Think of them as direct hardware method calls.
 * In C# it would be something like: [DllImport] extern byte ReadPort(ushort port);
 * ============================================================================= */

/* Read one byte from an I/O port */
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Write one byte to an I/O port */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}


/* =============================================================================
 * NUMBER FORMATTING
 * No sprintf() available, so we roll our own integer-to-string converters.
 * ============================================================================= */

/* Write an unsigned integer in decimal to a char buffer (null-terminates) */
static void fmt_dec(uint64_t n, char *buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[21];
    int i = 0;
    while (n > 0) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    int j = 0;
    for (int k = i - 1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
}

/* Write an unsigned integer in hexadecimal to a char buffer */
static void fmt_hex(uint64_t n, char *buf, int min_digits) {
    const char *hex = "0123456789ABCDEF";
    char tmp[17];
    int i = 0;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = hex[n & 0xF]; n >>= 4; }
    while (i < min_digits) tmp[i++] = '0';
    int j = 0;
    for (int k = i - 1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
}

static void kprint_dec(uint64_t n) {
    char buf[22];
    fmt_dec(n, buf);
    kprint(buf);
}

static void kprint_hex(uint64_t n, int min_digits) {
    char buf[20];
    fmt_hex(n, buf, min_digits);
    kprint(buf);
}


/* =============================================================================
 * MINIMAL STRING UTILITIES  (no string.h in freestanding)
 * ============================================================================= */

static size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int kstrcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int kstrncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!a[i] || a[i] != b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    }
    return 0;
}

static void kmemset(void *ptr, uint8_t val, size_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < len; i++) p[i] = val;
}


/* =============================================================================
 * DATA ATOM — THE SEMANTIC CORE OF GRAPHIOS ATOM
 *
 * GraphiOS replaces the File Allocation Table with a graph of "Data Atoms."
 * An Atom is a self-describing unit of meaning: it carries its own UUID,
 * semantic type, metadata tags, and a raw payload.
 *
 * Analogy for C# devs:   Like a sealed record class with a Guid primary key.
 * Analogy for Python devs: Like a @dataclass with a uuid field and a dict of tags.
 *
 * In a production Semantic OS, Atoms would live on a persistent graph store
 * (think Neo4j, but on-disk). In Beta 1, they live in kernel RAM.
 * ============================================================================= */

#define ATOM_CAPACITY       64      /* Max atoms this beta supports           */
#define ATOM_PAYLOAD_BYTES  512     /* Max bytes of content per atom          */
#define ATOM_TAG_SLOTS      8       /* Max semantic tags per atom             */
#define ATOM_TAG_WIDTH      32      /* Max characters per tag string          */
#define ATOM_LABEL_WIDTH    64      /* Max characters for the human label     */

/* ---- Atom Type: the semantic "class" of this atom ----
 * Unlike file extensions (.txt, .jpg), this is an intrinsic property
 * that the OS itself understands and can query on. */
typedef enum {
    ATOM_EMPTY      = 0,
    ATOM_TEXT       = 1,    /* Human-readable prose                */
    ATOM_NUMERIC    = 2,    /* Quantitative value or measurement   */
    ATOM_BINARY     = 3,    /* Opaque binary blob                  */
    ATOM_REFERENCE  = 4,    /* Semantic link to another atom UUID  */
    ATOM_THOUGHT    = 5,    /* Unstructured note or idea fragment  */
    ATOM_SYSTEM     = 6,    /* Reserved for kernel-generated atoms */
} AtomType;

/* ---- UUID-128: Unique Atom Identifier ----
 * Stored as four 32-bit words. Beta 1 uses a monotonic counter-based
 * generator rather than cryptographic randomness. */
typedef struct {
    uint32_t a, b, c, d;
} AtomUUID;

/* ---- Atom Header: all metadata, no payload ----
 * Analogous to a filesystem inode, but richer. */
typedef struct {
    AtomUUID    uuid;                               /* 128-bit unique ID        */
    AtomType    type;                               /* Semantic classification  */
    uint32_t    payload_size;                       /* Bytes used in payload[]  */
    uint64_t    created_tick;                       /* Monotonic timestamp      */
    uint64_t    modified_tick;                      /* Last-modified timestamp  */
    char        label[ATOM_LABEL_WIDTH];            /* Human-readable name      */
    char        tags[ATOM_TAG_SLOTS][ATOM_TAG_WIDTH]; /* Semantic tag strings   */
    uint8_t     tag_count;                          /* How many tags are active */
    uint8_t     _pad[7];                            /* Alignment padding        */
} AtomHeader;                                       /* sizeof ≈ 408 bytes       */

/* ---- Complete Atom: header + payload ---- */
typedef struct {
    AtomHeader  header;
    uint8_t     payload[ATOM_PAYLOAD_BYTES];
} DataAtom;                                         /* sizeof ≈ 920 bytes       */


/* ---- Atom Store: flat array in kernel BSS ---- */
static DataAtom  g_atoms[ATOM_CAPACITY];
static uint32_t  g_atom_count = 0;
static uint64_t  g_tick       = 0;  /* Monotonic "clock" — incremented each atom op */


/* ---------------------------------------------------------------------------
 * uuid_generate — Produce a new unique 128-bit UUID
 *
 * Format: GRAPHIOS_BETA-0001-TICK_HI-TICK_LO
 * Not cryptographically secure, but unique within a single boot session.
 * --------------------------------------------------------------------------- */
static AtomUUID uuid_generate(void) {
    AtomUUID id;
    id.a = 0xA701BE74;                      /* "ATOM BETA" signature word */
    id.b = 0x00000001;                      /* Beta 1 version word        */
    id.c = (uint32_t)(g_tick >> 32);        /* High 32 bits of tick       */
    id.d = (uint32_t)(g_tick & 0xFFFFFFFF); /* Low  32 bits of tick       */
    return id;
}

/* Print a UUID in the standard xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx format */
static void uuid_print(const AtomUUID *id) {
    kprint_hex(id->a, 8);
    kprint("-");
    kprint_hex((id->b >> 16) & 0xFFFF, 4);
    kprint("-");
    kprint_hex(id->b & 0xFFFF, 4);
    kprint("-");
    kprint_hex((id->c >> 16) & 0xFFFF, 4);
    kprint("-");
    kprint_hex(id->c & 0xFFFF, 4);
    kprint_hex(id->d, 8);
}

/* ---------------------------------------------------------------------------
 * atom_create — Allocate a new atom and populate its header + payload
 * Returns the atom's index in g_atoms[], or -1 if the store is full.
 * --------------------------------------------------------------------------- */
static int atom_create(const char *label, AtomType type, const char *content) {
    if (g_atom_count >= ATOM_CAPACITY) return -1;

    DataAtom *a = &g_atoms[g_atom_count];
    kmemset(a, 0, sizeof(DataAtom));

    a->header.uuid          = uuid_generate();
    a->header.type          = type;
    a->header.created_tick  = g_tick;
    a->header.modified_tick = g_tick;
    g_tick++;

    /* Copy label, truncated to ATOM_LABEL_WIDTH - 1 */
    int i = 0;
    while (label[i] && i < ATOM_LABEL_WIDTH - 1) {
        a->header.label[i] = label[i];
        i++;
    }

    /* Copy content into payload */
    if (content) {
        int j = 0;
        while (content[j] && j < ATOM_PAYLOAD_BYTES - 1) {
            a->payload[j] = (uint8_t)content[j];
            j++;
        }
        a->header.payload_size = (uint32_t)j;
    }

    return (int)g_atom_count++;
}

/* ---------------------------------------------------------------------------
 * atom_tag — Add a semantic tag string to an existing atom
 * --------------------------------------------------------------------------- */
static bool atom_tag(int idx, const char *tag) {
    if (idx < 0 || (uint32_t)idx >= g_atom_count) return false;
    DataAtom *a = &g_atoms[idx];
    if (a->header.tag_count >= ATOM_TAG_SLOTS) return false;

    int i = 0;
    while (tag[i] && i < ATOM_TAG_WIDTH - 1) {
        a->header.tags[a->header.tag_count][i] = tag[i];
        i++;
    }
    a->header.tag_count++;
    return true;
}

/* Human-readable name for each AtomType */
static const char *atom_type_name(AtomType t) {
    switch (t) {
        case ATOM_TEXT:      return "TEXT     ";
        case ATOM_NUMERIC:   return "NUMERIC  ";
        case ATOM_BINARY:    return "BINARY   ";
        case ATOM_REFERENCE: return "REFERENCE";
        case ATOM_THOUGHT:   return "THOUGHT  ";
        case ATOM_SYSTEM:    return "SYSTEM   ";
        default:             return "EMPTY    ";
    }
}


/* =============================================================================
 * PS/2 KEYBOARD DRIVER
 *
 * The PS/2 controller sits at I/O ports:
 *   0x60 — Data port  (read: incoming scan code)
 *   0x64 — Status port (bit 0 = output buffer full → safe to read 0x60)
 *
 * We use scan code set 1 (the original IBM XT set), which QEMU and most
 * emulators default to.  Each key press sends one byte (the "scan code").
 * A key release sends the same code OR-ed with 0x80.
 * ============================================================================= */

#define KB_DATA    0x60
#define KB_STATUS  0x64

/* US QWERTY scan-code-set-1 → ASCII, unshifted (index = scan code) */
static const char KB_MAP_NORMAL[128] = {
/*00*/  0,   27,  '1', '2', '3', '4', '5', '6',
/*08*/ '7', '8', '9', '0', '-', '=', '\b', '\t',
/*10*/ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
/*18*/ 'o', 'p', '[', ']', '\n', 0,  'a', 's',
/*20*/ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
/*28*/ '\'','`',  0,  '\\','z', 'x', 'c', 'v',
/*30*/ 'b', 'n', 'm', ',', '.', '/', 0,   '*',
/*38*/  0,  ' ',  0,   0,   0,   0,   0,   0,
/*40*/  0,   0,   0,   0,   0,   0,   0,  '7',
/*48*/ '8', '9', '-', '4', '5', '6', '+', '1',
/*50*/ '2', '3', '0', '.',  0,   0,   0,   0,
/*58*/  0,   0,   0,   0,   0,   0,   0,   0
};

/* US QWERTY scan-code-set-1 → ASCII, shifted */
static const char KB_MAP_SHIFT[128] = {
/*00*/  0,   27,  '!', '@', '#', '$', '%', '^',
/*08*/ '&', '*', '(', ')', '_', '+', '\b', '\t',
/*10*/ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
/*18*/ 'O', 'P', '{', '}', '\n', 0,  'A', 'S',
/*20*/ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
/*28*/ '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
/*30*/ 'B', 'N', 'M', '<', '>', '?',  0,  '*',
/*38*/  0,  ' ',  0,   0,   0,   0,   0,   0
};

static bool g_shift    = false;
static bool g_capslock = false;

/* Read the next scan code from the PS/2 controller (non-blocking).
 * Returns 0 if no key is ready. */
static uint8_t kb_poll(void) {
    if (!(inb(KB_STATUS) & 0x01)) return 0;
    return inb(KB_DATA);
}

/* Translate a scan code to an ASCII character.
 * Returns 0 for non-printable keys (shift, ctrl, F-keys, etc.) */
static char kb_to_ascii(uint8_t sc) {
    bool key_up = (sc & 0x80) != 0;
    sc &= 0x7F;     /* Strip the key-release bit */

    /* Track Shift state (scan codes 0x2A = left shift, 0x36 = right shift) */
    if (sc == 0x2A || sc == 0x36) {
        g_shift = !key_up;
        return 0;
    }
    /* Toggle Caps Lock on key-press (scan code 0x3A) */
    if (sc == 0x3A && !key_up) {
        g_capslock = !g_capslock;
        return 0;
    }
    if (key_up || sc >= 128) return 0;  /* Ignore all other release events */

    bool upper = g_shift ^ g_capslock;
    return upper ? KB_MAP_SHIFT[sc] : KB_MAP_NORMAL[sc];
}


/* =============================================================================
 * SHELL — Interactive Command Interface
 *
 * A minimal REPL (Read-Eval-Print Loop):
 *   Python devs: like input() in a while-True loop
 *   C# devs:     like Console.ReadLine() with a switch-case dispatcher
 * ============================================================================= */

#define CMD_BUF_LEN 128
static char  g_cmd[CMD_BUF_LEN];
static int   g_cmd_len = 0;

/* ---------------------------------------------------------------------------
 * seed_atoms — Create a few demo atoms at boot so the store isn't empty
 * --------------------------------------------------------------------------- */
static void seed_atoms(void) {
    int i;

    i = atom_create("Hello, GraphiOS", ATOM_TEXT,
        "This is the first Data Atom on this system. "
        "No files. No folders. Only meaning.");
    atom_tag(i, "demo"); atom_tag(i, "greeting");

    i = atom_create("Boot Record", ATOM_SYSTEM,
        "GraphiOS Atom Beta 1 booted successfully. "
        "Long mode active. Semantic graph online.");
    atom_tag(i, "system"); atom_tag(i, "boot"); atom_tag(i, "kernel");

    i = atom_create("Pi", ATOM_NUMERIC,
        "3.14159265358979323846264338327950288419716939937510");
    atom_tag(i, "math"); atom_tag(i, "constant"); atom_tag(i, "irrational");

    i = atom_create("Idea Fragment", ATOM_THOUGHT,
        "What if the OS knew the difference between a shopping list "
        "and a research note — not because of the file extension, "
        "but because it understood the content?");
    atom_tag(i, "concept"); atom_tag(i, "sem-os"); atom_tag(i, "idea");
}

/* ---------------------------------------------------------------------------
 * draw_header — Render the top banner (Neo-Brutalist style)
 * --------------------------------------------------------------------------- */
static void draw_header(void) {
    term_clear(COLOR_BLACK);

    /* Top status bar — inverse colours */
    term_color_set(COLOR_BLACK, COLOR_LIGHT_CYAN);
    kprint("  GRAPHIOS ATOM");
    kprint("  BETA 1   ");
    kprint("                                       ");
    kprint("x86_64  64-BIT  SEMANTIC OS  ");
    term_color_set(COLOR_WHITE, COLOR_BLACK);

    kprint("\n");
    kprint_col(
        "  No files. No folders. Only atoms of meaning.\n\n",
        COLOR_DARK_GREY, COLOR_BLACK
    );
}

/* ---------------------------------------------------------------------------
 * cmd_help — Print the command reference
 * --------------------------------------------------------------------------- */
static void cmd_help(void) {
    kprint("\n");
    kprint_col("  GRAPHIOS ATOM SHELL COMMANDS\n", COLOR_YELLOW, COLOR_BLACK);
    kprint_rule('-', 46, COLOR_DARK_GREY, COLOR_BLACK);

    /* Each entry: command in cyan, description in white */
    struct { const char *cmd; const char *desc; } entries[] = {
        { "  ls                ", "List all atoms in the store"              },
        { "  atom <n>          ", "Inspect atom by index number"             },
        { "  new <label>       ", "Create a new THOUGHT atom"                },
        { "  tag <n> <tag>     ", "Add a semantic tag to atom #n"            },
        { "  write <n> <text>  ", "Append text to atom #n payload"           },
        { "  clear             ", "Clear the screen"                         },
        { "  about             ", "Show system information"                  },
        { "  help              ", "Show this command list"                   },
        { NULL, NULL }
    };

    for (int i = 0; entries[i].cmd; i++) {
        kprint_col(entries[i].cmd, COLOR_LIGHT_CYAN, COLOR_BLACK);
        kprint(entries[i].desc);
        kprint("\n");
    }
    kprint("\n");
}

/* ---------------------------------------------------------------------------
 * cmd_ls — List all atoms in a table
 * --------------------------------------------------------------------------- */
static void cmd_ls(void) {
    if (g_atom_count == 0) {
        kprint_col("  (atom store is empty)\n", COLOR_DARK_GREY, COLOR_BLACK);
        return;
    }

    kprint("\n");
    kprint_col("  #   TYPE       SIZE   LABEL                          TAGS\n",
               COLOR_YELLOW, COLOR_BLACK);
    kprint_rule('-', 75, COLOR_DARK_GREY, COLOR_BLACK);

    for (uint32_t i = 0; i < g_atom_count; i++) {
        const DataAtom *a = &g_atoms[i];

        /* Index */
        kprint("  ");
        if (i < 10) kprint(" ");
        kprint_dec(i);
        kprint("  ");

        /* Type */
        kprint_col(atom_type_name(a->header.type), COLOR_LIGHT_GREEN, COLOR_BLACK);
        kprint("  ");

        /* Size */
        if (a->header.payload_size < 100) kprint(" ");
        if (a->header.payload_size < 10)  kprint(" ");
        kprint_dec(a->header.payload_size);
        kprint("B  ");

        /* Label (padded to 30 chars) */
        kprint(a->header.label);
        int lpad = 30 - (int)kstrlen(a->header.label);
        for (int j = 0; j < lpad; j++) term_putchar(' ');

        /* Tags */
        for (int j = 0; j < a->header.tag_count; j++) {
            kprint_col("[", COLOR_DARK_GREY, COLOR_BLACK);
            kprint_col(a->header.tags[j], COLOR_LIGHT_MAGENTA, COLOR_BLACK);
            kprint_col("]", COLOR_DARK_GREY, COLOR_BLACK);
        }
        kprint("\n");
    }
    kprint("\n");
}

/* ---------------------------------------------------------------------------
 * cmd_atom — Print full details for one atom
 * --------------------------------------------------------------------------- */
static void cmd_atom_detail(int idx) {
    if (idx < 0 || (uint32_t)idx >= g_atom_count) {
        kprint_col("  [!] No atom at that index.\n", COLOR_LIGHT_RED, COLOR_BLACK);
        return;
    }
    const DataAtom *a = &g_atoms[idx];

    kprint("\n");
    kprint_col("  +-- ATOM #", COLOR_YELLOW, COLOR_BLACK);
    kprint_dec((uint64_t)idx);
    kprint_col(" -----------------------------------------------+\n",
               COLOR_DARK_GREY, COLOR_BLACK);

    /* UUID */
    kprint_col("  | UUID     : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_col("", COLOR_LIGHT_CYAN, COLOR_BLACK);
    uuid_print(&a->header.uuid);
    kprint("\n");

    /* Label */
    kprint_col("  | Label    : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_col(a->header.label, COLOR_WHITE, COLOR_BLACK);
    kprint("\n");

    /* Type */
    kprint_col("  | Type     : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_col(atom_type_name(a->header.type), COLOR_LIGHT_GREEN, COLOR_BLACK);
    kprint("\n");

    /* Payload size */
    kprint_col("  | Size     : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_dec(a->header.payload_size);
    kprint(" bytes\n");

    /* Created tick */
    kprint_col("  | Created  : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_col("tick #", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_dec(a->header.created_tick);
    kprint("\n");

    /* Tags */
    kprint_col("  | Tags     : ", COLOR_DARK_GREY, COLOR_BLACK);
    if (a->header.tag_count == 0) {
        kprint_col("(none)\n", COLOR_DARK_GREY, COLOR_BLACK);
    } else {
        for (int i = 0; i < a->header.tag_count; i++) {
            kprint_col("[", COLOR_DARK_GREY, COLOR_BLACK);
            kprint_col(a->header.tags[i], COLOR_LIGHT_MAGENTA, COLOR_BLACK);
            kprint_col("]", COLOR_DARK_GREY, COLOR_BLACK);
            if (i < a->header.tag_count - 1) kprint(" ");
        }
        kprint("\n");
    }

    /* Payload preview (first 60 printable chars) */
    if (a->header.payload_size > 0) {
        kprint_col("  | Content  : ", COLOR_DARK_GREY, COLOR_BLACK);
        int preview = (int)a->header.payload_size;
        if (preview > 60) preview = 60;
        for (int i = 0; i < preview; i++) {
            char c = (char)a->payload[i];
            if (c < 32 || c > 126) c = '.';
            term_putchar(c);
        }
        if (a->header.payload_size > 60) kprint_col("...", COLOR_DARK_GREY, COLOR_BLACK);
        kprint("\n");
    }

    kprint_col("  +-----------------------------------------------------------+\n",
               COLOR_DARK_GREY, COLOR_BLACK);
    kprint("\n");
}

/* ---------------------------------------------------------------------------
 * cmd_about — System information screen
 * --------------------------------------------------------------------------- */
static void cmd_about(void) {
    kprint("\n");
    kprint_col("  GraphiOS Atom  Beta 1\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
    kprint_col("  ============================\n", COLOR_DARK_GREY, COLOR_BLACK);

    kprint_col("  Architecture : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint("x86_64 Long Mode (64-bit)\n");

    kprint_col("  Display      : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint("VGA Text Mode 80x25\n");

    kprint_col("  Input        : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint("PS/2 Keyboard (Scan Set 1)\n");

    kprint_col("  Atom Store   : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_dec(g_atom_count);
    kprint(" atoms /  ");
    kprint_dec(ATOM_CAPACITY);
    kprint(" capacity\n");

    kprint_col("  Atom Size    : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_dec(sizeof(DataAtom));
    kprint(" bytes each\n");

    kprint_col("  Tick Counter : ", COLOR_DARK_GREY, COLOR_BLACK);
    kprint_dec(g_tick);
    kprint("\n");

    kprint("\n");
    kprint_col("  CONCEPT\n", COLOR_YELLOW, COLOR_BLACK);
    kprint_col("  -------\n", COLOR_DARK_GREY, COLOR_BLACK);
    kprint("  GraphiOS replaces the File Allocation Table with a\n");
    kprint("  semantic graph of Data Atoms.  Each atom carries its own\n");
    kprint("  UUID, type, semantic tags, and payload — no file path\n");
    kprint("  needed.  Query by meaning, not by location.\n\n");
}

/* ---------------------------------------------------------------------------
 * exec_command — Dispatch a command string to its handler
 * --------------------------------------------------------------------------- */
static void exec_command(const char *cmd) {
    /* Trim leading spaces */
    while (*cmd == ' ') cmd++;
    if (!*cmd) return;

    if (kstrcmp(cmd, "help") == 0) {
        cmd_help();
    }
    else if (kstrcmp(cmd, "ls") == 0) {
        cmd_ls();
    }
    else if (kstrcmp(cmd, "clear") == 0) {
        draw_header();
    }
    else if (kstrcmp(cmd, "about") == 0) {
        cmd_about();
    }
    else if (kstrncmp(cmd, "atom ", 5) == 0) {
        int idx = 0;
        const char *p = cmd + 5;
        while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
        cmd_atom_detail(idx);
    }
    else if (kstrncmp(cmd, "new ", 4) == 0) {
        const char *label = cmd + 4;
        if (!*label) { kprint_col("  Usage: new <label>\n", COLOR_LIGHT_RED, COLOR_BLACK); return; }
        int idx = atom_create(label, ATOM_THOUGHT, "(empty — use 'write' to add content)");
        if (idx < 0) {
            kprint_col("  [!] Atom store is full.\n", COLOR_LIGHT_RED, COLOR_BLACK);
        } else {
            kprint_col("  [+] Created atom #", COLOR_LIGHT_GREEN, COLOR_BLACK);
            kprint_dec((uint64_t)idx);
            kprint("  uuid: ");
            uuid_print(&g_atoms[idx].header.uuid);
            kprint("\n");
        }
    }
    else if (kstrncmp(cmd, "tag ", 4) == 0) {
        const char *p = cmd + 4;
        int idx = 0;
        while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
        if (*p == ' ') p++;
        if (!*p) { kprint_col("  Usage: tag <n> <tagname>\n", COLOR_LIGHT_RED, COLOR_BLACK); return; }
        if (atom_tag(idx, p)) {
            kprint_col("  [+] Tag added.\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
        } else {
            kprint_col("  [!] Invalid index or tag slots full.\n", COLOR_LIGHT_RED, COLOR_BLACK);
        }
    }
    else if (kstrncmp(cmd, "write ", 6) == 0) {
        const char *p = cmd + 6;
        int idx = 0;
        while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
        if (*p == ' ') p++;
        if (idx < 0 || (uint32_t)idx >= g_atom_count) {
            kprint_col("  [!] No atom at that index.\n", COLOR_LIGHT_RED, COLOR_BLACK);
            return;
        }
        DataAtom *a = &g_atoms[idx];
        uint32_t available = ATOM_PAYLOAD_BYTES - a->header.payload_size;
        if (available == 0) {
            kprint_col("  [!] Atom payload is full.\n", COLOR_LIGHT_RED, COLOR_BLACK);
            return;
        }
        /* Append text with a space separator */
        if (a->header.payload_size > 0 && available > 1) {
            a->payload[a->header.payload_size++] = ' ';
            available--;
        }
        uint32_t written = 0;
        while (*p && written < available - 1) {
            a->payload[a->header.payload_size++] = (uint8_t)*p++;
            written++;
        }
        a->header.modified_tick = g_tick++;
        kprint_col("  [+] Written ", COLOR_LIGHT_GREEN, COLOR_BLACK);
        kprint_dec(written);
        kprint(" bytes to atom #");
        kprint_dec((uint64_t)idx);
        kprint(".\n");
    }
    else {
        kprint_col("  Unknown: '", COLOR_LIGHT_RED, COLOR_BLACK);
        kprint(cmd);
        kprint_col("'  — type 'help'\n", COLOR_LIGHT_RED, COLOR_BLACK);
    }
}

/* Draw the shell prompt line */
static void draw_prompt(void) {
    kprint_col("\n  atom", COLOR_LIGHT_CYAN, COLOR_BLACK);
    kprint_col("> ", COLOR_YELLOW, COLOR_BLACK);
    term_color_set(COLOR_WHITE, COLOR_BLACK);
}


/* =============================================================================
 * KERNEL MAIN — Called from boot.asm after entering 64-bit long mode
 *
 * Parameters:
 *   mb2_info — Physical address of the Multiboot2 information struct.
 *              We don't parse it in Beta 1, but it's available for extension.
 * ============================================================================= */
void kernel_main(uint64_t mb2_info __attribute__((unused))) {

    /* 1. Initialise display and draw banner */
    draw_header();

    /* 2. Print boot diagnostics */
    kprint_col("  [ OK ] 64-bit long mode active\n",        COLOR_LIGHT_GREEN, COLOR_BLACK);
    kprint_col("  [ OK ] VGA text mode: 80x25 @ 0xB8000\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
    kprint_col("  [ OK ] PS/2 keyboard driver initialised\n",COLOR_LIGHT_GREEN, COLOR_BLACK);
    kprint_col("  [ OK ] Atom store online  (64 slots)\n",   COLOR_LIGHT_GREEN, COLOR_BLACK);

    /* 3. Populate demo atoms */
    seed_atoms();
    kprint_col("  [ OK ] Demo atoms seeded  (4 atoms)\n\n",  COLOR_LIGHT_GREEN, COLOR_BLACK);

    kprint_col("  Type 'help' for commands, 'ls' to list atoms.\n",
               COLOR_DARK_GREY, COLOR_BLACK);

    /* 4. Draw the first prompt */
    draw_prompt();

    /* ==========================================================================
     * MAIN EVENT LOOP
     * Poll the keyboard forever, echo characters, execute commands on Enter.
     * In real OS terms: this is the scheduler idling. Interrupts would replace
     * the polling loop in a production kernel.
     * ========================================================================== */
    while (1) {
        uint8_t sc = kb_poll();
        if (sc == 0) {
            /* No key ready — pause the CPU until the next hardware event.
             * HLT stops execution until an interrupt fires (even if masked).
             * This saves power and reduces bus noise. */
            __asm__ volatile("hlt");
            continue;
        }

        char c = kb_to_ascii(sc);
        if (c == 0) continue;

        if (c == '\n') {
            /* Enter: null-terminate buffer, execute, reset */
            g_cmd[g_cmd_len] = '\0';
            term_putchar('\n');
            exec_command(g_cmd);
            g_cmd_len = 0;
            draw_prompt();
        }
        else if (c == '\b') {
            /* Backspace: erase last character if buffer isn't empty */
            if (g_cmd_len > 0) {
                g_cmd_len--;
                term_putchar('\b');
            }
        }
        else {
            /* Printable character: add to buffer and echo */
            if (g_cmd_len < CMD_BUF_LEN - 1) {
                g_cmd[g_cmd_len++] = c;
                term_putchar(c);
            }
        }
    }
}
