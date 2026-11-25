/* Loads the fighter plugins (mexft_plugin.h) and hands their fighters to the registry
 * (akaneia/mu_ak_fighters.c find_fighter) and their variables to the rollback snapshot
 * (shim/mu_exclusions.c mu_snapshot_ranges).
 *
 * Where: the folder the host names (--fighters-dir), none in a Vanilla launch; else the folder
 * MELEE_FIGHTER_PLUGINS names, or "fighters" beside the game library. Every
 * *.mufighter in it is tried, in name order. A plugin that is refused says why in the log and its
 * fighter stays locked, as a fighter with no native code is.
 *
 * A plugin is a relocatable object, linked here: its sections are placed in memory next to the
 * game image, its references to the game are resolved by name from melee_game.syms (beside the
 * library, made with it and carrying its build id), and its relocations are applied. Code and
 * constants end read-only; the variables are one span the snapshot holds.
 *
 * This file uses the operating system and none of the game's headers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mexft_plugin.h"
#include "mexft_abi.h" /* generated: MEXFT_ABI_BYTES */

/* The game's include path has its own C library headers, which the system's do not mix with:
 * what this file needs of either is declared here. */
typedef unsigned long long uintptr_t;
typedef __SIZE_TYPE__ size_t;
#define NULL ((void*) 0)
char* getenv(const char* name);
void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
size_t strlen(const char* s);
char* strcpy(char* to, const char* from);
char* strcat(char* to, const char* from);
char* strrchr(const char* s, int c);
int memcmp(const void* a, const void* b, size_t n);
void* memcpy(void* to, const void* from, size_t n);
void* memset(void* to, int value, size_t n);
unsigned long long strtoull(const char* text, char** end, int base);

void OSReport(const char* format, ...);

enum { MAX_PLUGINS = 64, MAX_FIGHTERS = 128 };

/* What a plugin must be built for, for a host that has not started the game: melee_source
 * --print-info (port/app/main.cpp print_info) maps this library without running any of it (no
 * DllMain, no imports resolved) and reads this export. Data only, with no pointers, so nothing in it
 * needs the library to be set up; the host reads a library whose size field is at least its own. */
#include "mexft_linked.h" /* generated: MEXFT_LINKED_FIGHTERS, the m-ex fighters linked in, by name */
#include <mu_commands.h>
typedef struct MuFighterPluginInfo {
    unsigned int size;
    unsigned int format;  /* MU_FIGHTER_PLUGIN_VERSION */
    char abi[36];         /* MEXFT_ABI_TEXT: the ABI id in hex */
    char linked[512];     /* comma-separated */
} MuFighterPluginInfo;
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const MuFighterPluginInfo mu_fighter_plugin_info = {
    sizeof(MuFighterPluginInfo), MU_FIGHTER_PLUGIN_VERSION, MEXFT_ABI_TEXT, MEXFT_LINKED_FIGHTERS
};

/* A descriptor starts with its display name and its fighter file (MuAkFighter). */
typedef struct Descriptor {
    const char* name;
    const char* file;
} Descriptor;

typedef struct Region {
    void* address;
    unsigned int size;
} Region;

static Region variables[MAX_PLUGINS];
static int plugin_count;
static MuFighterPluginEntry fighters[MAX_FIGHTERS];
static unsigned int fighter_digest[MAX_FIGHTERS];
static int fighter_count;
static int loaded;

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int same_text(const char* a, const char* b)
{
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

#ifdef _WIN32
/* kernel32 and the PE and COFF fields read below (winnt.h's names for them in the comments). */
typedef void* HANDLE;
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE) (long long) -1)
typedef struct WIN32_FIND_DATAA {
    unsigned int attributes;
    unsigned int times[6];
    unsigned int size_high, size_low, reserved0, reserved1;
    char cFileName[MAX_PATH];
    char alternate[14];
} WIN32_FIND_DATAA;
__declspec(dllimport) unsigned int __stdcall GetLastError(void);
__declspec(dllimport) unsigned int __stdcall GetModuleFileNameA(HANDLE module, char* path, unsigned int size);
__declspec(dllimport) HANDLE __stdcall FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* found);
__declspec(dllimport) int __stdcall FindNextFileA(HANDLE find, WIN32_FIND_DATAA* found);
__declspec(dllimport) int __stdcall FindClose(HANDLE find);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char* path, unsigned int access, unsigned int share,
                                                   void* security, unsigned int creation, unsigned int flags,
                                                   HANDLE template_file);
__declspec(dllimport) int __stdcall ReadFile(HANDLE file, void* buffer, unsigned int size, unsigned int* read,
                                             void* overlapped);
__declspec(dllimport) unsigned int __stdcall GetFileSize(HANDLE file, unsigned int* high);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE handle);
__declspec(dllimport) void* __stdcall VirtualAlloc(void* address, size_t size, unsigned int type, unsigned int protect);
__declspec(dllimport) int __stdcall VirtualFree(void* address, size_t size, unsigned int type);
__declspec(dllimport) int __stdcall VirtualProtect(void* address, size_t size, unsigned int protect,
                                                   unsigned int* old);
#define MEM_COMMIT_RESERVE 0x3000u
#define MEM_RELEASE 0x8000u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

extern char __ImageBase[];

static unsigned int le16(const unsigned char* p)
{
    return p[0] | (unsigned int) p[1] << 8;
}

static unsigned int le32(const unsigned char* p)
{
    return p[0] | (unsigned int) p[1] << 8 | (unsigned int) p[2] << 16 | (unsigned int) p[3] << 24;
}

static unsigned long long le64(const unsigned char* p)
{
    return le32(p) | (unsigned long long) le32(p + 4) << 32;
}

static void put32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

static void put64(unsigned char* p, unsigned long long v)
{
    put32(p, (unsigned int) v);
    put32(p + 4, (unsigned int) (v >> 32));
}

static void hex(char* out, const unsigned char* id)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) {
        out[i * 2] = digits[id[i] >> 4];
        out[i * 2 + 1] = digits[id[i] & 15];
    }
    out[32] = '\0';
}

/* The running game library's build id: the CodeView record of its debug directory. */
static int game_id(unsigned char id[16])
{
    const unsigned char* base = (const unsigned char*) __ImageBase;
    const unsigned char* nt = base + le32(base + 0x3C);             /* e_lfanew */
    const unsigned char* dir = nt + 24 + 112 + 6 * 8;               /* DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] */
    const unsigned int rva = le32(dir), size = le32(dir + 4);
    unsigned int i;
    for (i = 0; rva != 0 && i < size / 28; i++) {                   /* IMAGE_DEBUG_DIRECTORY */
        const unsigned char* entry = base + rva + i * 28;
        const unsigned char* record = base + le32(entry + 20);      /* AddressOfRawData */
        if (le32(entry + 12) == 2 && le32(entry + 16) >= 20 &&      /* IMAGE_DEBUG_TYPE_CODEVIEW, SizeOfData */
            memcmp(record, "RSDS", 4) == 0)
        {
            memcpy(id, record + 4, 16);
            return 1;
        }
    }
    return 0;
}

/* A whole file, in memory of its own (VirtualFree releases it); one 0 byte follows the contents. */
static unsigned char* read_file(const char* path, unsigned int* size)
{
    HANDLE file = CreateFileA(path, 0x80000000u /* GENERIC_READ */, 1 /* FILE_SHARE_READ */, NULL,
                              3 /* OPEN_EXISTING */, 0, NULL);
    unsigned char* data = NULL;
    unsigned int high = 0, got = 0, length;
    if (file == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    length = GetFileSize(file, &high);
    if (high == 0 && length != 0xFFFFFFFFu && length < 64u * 1024u * 1024u) {
        data = VirtualAlloc(NULL, (size_t) length + 1, MEM_COMMIT_RESERVE, PAGE_READWRITE);
        if (data != NULL && (!ReadFile(file, data, length, &got, NULL) || got != length)) {
            VirtualFree(data, 0, MEM_RELEASE);
            data = NULL;
        }
    }
    CloseHandle(file);
    *size = length;
    return data;
}

/* ---- the game's symbols (melee_game.syms: "# build <id>", then "name address" lines, sorted
 * by name as strcmp orders them) ---- */
static unsigned char* syms_text;
static const char** sym_name;
static unsigned int* sym_address;
static int sym_count;

static void syms_free(void)
{
    if (syms_text != NULL) {
        VirtualFree(syms_text, 0, MEM_RELEASE);
    }
    if (sym_name != NULL) {
        VirtualFree((void*) sym_name, 0, MEM_RELEASE);
    }
    if (sym_address != NULL) {
        VirtualFree(sym_address, 0, MEM_RELEASE);
    }
    syms_text = NULL;
    sym_name = NULL;
    sym_address = NULL;
    sym_count = 0;
}

static int syms_load(const char* library_dir)
{
    char path[MAX_PATH * 2], want[33], have[33];
    unsigned char id[16];
    unsigned int size = 0, i, lines = 0;
    char* p;
    strcpy(path, library_dir);
    strcat(path, "\\melee_game.syms");
    syms_text = read_file(path, &size);
    if (syms_text == NULL) {
        OSReport("[plugin] no fighter plugin is loaded: %s is missing\n", path);
        return 0;
    }
    if (!game_id(id)) {
        OSReport("[plugin] no fighter plugin is loaded: this game library has no build id\n");
        return 0;
    }
    hex(have, id);
    if (size < 41 || memcmp(syms_text, "# build ", 8) != 0 || memcmp(syms_text + 8, have, 32) != 0) {
        memcpy(want, size >= 41 ? (const char*) syms_text + 8 : "?", size >= 41 ? 32 : 2);
        want[size >= 41 ? 32 : 1] = '\0';
        OSReport("[plugin] no fighter plugin is loaded: melee_game.syms is of build %s, the library of %s\n", want,
                 have);
        return 0;
    }
    for (i = 0; i < size; i++) {
        lines += syms_text[i] == '\n';
    }
    sym_name = VirtualAlloc(NULL, (size_t) (lines + 1) * sizeof *sym_name, MEM_COMMIT_RESERVE, PAGE_READWRITE);
    sym_address = VirtualAlloc(NULL, (size_t) (lines + 1) * sizeof *sym_address, MEM_COMMIT_RESERVE, PAGE_READWRITE);
    if (sym_name == NULL || sym_address == NULL) {
        return 0;
    }
    for (p = (char*) syms_text; *p != '\0';) {
        char* line = p;
        char* space = NULL;
        while (*p != '\0' && *p != '\n') {
            if (*p == ' ' && space == NULL) {
                space = p;
            }
            p++;
        }
        if (*p == '\n') {
            *p++ = '\0';
        }
        if (line[0] != '#' && space != NULL) {
            *space = '\0';
            sym_name[sym_count] = line;
            sym_address[sym_count] = (unsigned int) strtoull(space + 1, NULL, 16);
            sym_count++;
        }
    }
    return sym_count > 0;
}

static int sym_find(const char* name, uintptr_t* address)
{
    int lo = 0, hi = sym_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = strcmp(name, sym_name[mid]);
        if (c == 0) {
            *address = sym_address[mid];
            return 1;
        }
        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

/* ---- linking one object ---- */
enum {
    SCN_CODE = 0x20, SCN_DATA = 0x40, SCN_BSS = 0x80, SCN_INFO = 0x200, SCN_REMOVE = 0x800,
    SCN_DISCARDABLE = 0x02000000, SCN_WRITE = (int) 0x80000000u,
    REL_ADDR64 = 1, REL_ADDR32 = 2, REL_REL32 = 4, REL_REL32_5 = 9,
};

/* Where the plugins go: from the end of the range the host holds for the game image
 * (port/app/source_host.cpp), upward, 64 KB steps. */
static uintptr_t place_next = 0x84000000u;
#define PLACE_END 0xC0000000u

typedef struct Object {
    const char* file;
    const unsigned char* data;
    unsigned int size;
    const unsigned char* sections;
    unsigned int section_count;
    const unsigned char* symbols;
    unsigned int symbol_count;
    const char* strings;
    unsigned int strings_size;
    uintptr_t* section_at; /* 0: not loaded */
} Object;

static const char* long_name(const Object* o, const unsigned char* field, char* small)
{
    if (field[0] == '/') { /* a section's "/123" */
        const unsigned long long at = strtoull((const char*) field + 1, NULL, 10);
        return at < o->strings_size ? o->strings + at : "";
    }
    memcpy(small, field, 8);
    small[8] = '\0';
    return small;
}

static const char* symbol_name(const Object* o, const unsigned char* symbol, char* small)
{
    if (le32(symbol) == 0) {
        const unsigned int at = le32(symbol + 4);
        return at < o->strings_size ? o->strings + at : "";
    }
    memcpy(small, symbol, 8);
    small[8] = '\0';
    return small;
}

static int loads(const Object* o, const unsigned char* section)
{
    char small[9];
    const unsigned int flags = le32(section + 36);
    const char* name = long_name(o, section, small);
    if ((flags & (SCN_INFO | SCN_REMOVE | SCN_DISCARDABLE)) != 0 || (flags & (SCN_CODE | SCN_DATA | SCN_BSS)) == 0) {
        return 0;
    }
    /* Unwind tables: nothing here unwinds through a plugin. */
    return strncmp(name, ".pdata", 6) != 0 && strncmp(name, ".xdata", 6) != 0;
}

static int constructors(const Object* o, const unsigned char* section)
{
    char small[9];
    return strncmp(long_name(o, section, small), ".ctors", 6) == 0;
}

static unsigned int section_size(const unsigned char* section)
{
    const unsigned int raw = le32(section + 16), virt = le32(section + 8);
    return raw > virt ? raw : virt;
}

static uintptr_t align_up(uintptr_t v, uintptr_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* The address of symbol `index`: its section's place plus its value, or the game's address of
 * that name. */
static int symbol_address(const Object* o, unsigned int index, uintptr_t* address)
{
    char small[9];
    const unsigned char* symbol;
    const char* name;
    int section;
    if (index >= o->symbol_count) {
        OSReport("[plugin] %s: refused, a relocation names symbol %u of %u\n", o->file, index, o->symbol_count);
        return 0;
    }
    symbol = o->symbols + (size_t) index * 18;
    section = (short) le16(symbol + 12);
    name = symbol_name(o, symbol, small);
    if (section > 0) {
        if ((unsigned int) section > o->section_count || o->section_at[section - 1] == 0) {
            OSReport("[plugin] %s: refused, %s is in a section that is not loaded\n", o->file, name);
            return 0;
        }
        *address = o->section_at[section - 1] + le32(symbol + 8);
        return 1;
    }
    if (section == -1) { /* absolute */
        *address = le32(symbol + 8);
        return 1;
    }
    if (section == 0 && le32(symbol + 8) == 0 && sym_find(name, address)) {
        return 1;
    }
    OSReport("[plugin] %s: refused, this game has no symbol %s\n", o->file, name);
    return 0;
}

static int relocate(const Object* o, unsigned int index)
{
    const unsigned char* section = o->sections + (size_t) index * 40;
    const unsigned int count = le16(section + 32);
    const unsigned char* reloc = o->data + le32(section + 24);
    const uintptr_t at = o->section_at[index];
    unsigned int i;
    if (count == 0) {
        return 1;
    }
    if (le32(section + 24) > o->size || (size_t) count * 10 > o->size - le32(section + 24) || count == 0xFFFF) {
        OSReport("[plugin] %s: refused, a relocation table runs past the file\n", o->file);
        return 0;
    }
    for (i = 0; i < count; i++, reloc += 10) {
        const unsigned int offset = le32(reloc), type = le16(reloc + 8);
        unsigned char* where = (unsigned char*) at + offset;
        uintptr_t target;
        if ((unsigned long long) offset + (type == REL_ADDR64 ? 8 : 4) > section_size(section)) {
            OSReport("[plugin] %s: refused, a relocation is outside its section\n", o->file);
            return 0;
        }
        if (!symbol_address(o, le32(reloc + 4), &target)) {
            return 0;
        }
        if (type == REL_ADDR64) {
            put64(where, le64(where) + target);
        } else if (type == REL_ADDR32) {
            const unsigned long long v = (unsigned long long) le32(where) + target;
            if (v > 0xFFFFFFFFull) {
                OSReport("[plugin] %s: refused, a 32-bit address does not fit\n", o->file);
                return 0;
            }
            put32(where, (unsigned int) v);
        } else if (type >= REL_REL32 && type <= REL_REL32_5) {
            /* Relative to the end of the field, plus 0 to 5 bytes that follow it in the instruction. */
            const long long v = (long long) (int) le32(where) + (long long) target -
                                (long long) ((uintptr_t) where + 4 + (type - REL_REL32));
            if (v < -0x80000000ll || v > 0x7FFFFFFFll) {
                OSReport("[plugin] %s: refused, a 32-bit offset does not reach\n", o->file);
                return 0;
            }
            put32(where, (unsigned int) (int) v);
        } else {
            OSReport("[plugin] %s: refused, relocation type %u is not handled\n", o->file, type);
            return 0;
        }
    }
    return 1;
}

/* Links the object into memory of its own. On success: the plugin's record, its variables' span,
 * and its constructors run. On failure nothing stays. */
static const MuFighterPlugin* link_object(Object* o, Region* writable)
{
    char small[9];
    uintptr_t read_only = 0, written = 0, total, base = 0, at;
    const MuFighterPlugin* plugin = NULL;
    unsigned int i, old = 0;
    int pass, ok = 1;

    if (o->size < 20 || le16(o->data) != 0x8664 /* IMAGE_FILE_MACHINE_AMD64 */ || le16(o->data + 16) != 0) {
        OSReport("[plugin] %s: refused, not an x86-64 object\n", o->file);
        return NULL;
    }
    o->section_count = le16(o->data + 2);
    o->sections = o->data + 20;
    o->symbol_count = le32(o->data + 12);
    if (le32(o->data + 8) > o->size || (size_t) o->symbol_count * 18 + 4 > o->size - le32(o->data + 8) ||
        (size_t) o->section_count * 40 + 20 > o->size)
    {
        OSReport("[plugin] %s: refused, its tables run past the file\n", o->file);
        return NULL;
    }
    o->symbols = o->data + le32(o->data + 8);
    o->strings = (const char*) o->symbols + (size_t) o->symbol_count * 18;
    o->strings_size = le32((const unsigned char*) o->strings);
    if (o->strings_size > o->size - (unsigned int) ((const unsigned char*) o->strings - o->data)) {
        OSReport("[plugin] %s: refused, its names run past the file\n", o->file);
        return NULL;
    }
    o->section_at = VirtualAlloc(NULL, (size_t) (o->section_count + 1) * sizeof *o->section_at, MEM_COMMIT_RESERVE,
                                 PAGE_READWRITE);
    if (o->section_at == NULL) {
        return NULL;
    }

    /* Sizes: the read-only part (code, constants, the constructor list), then the variables on
     * a page of their own. Pass 0 measures, pass 1 places. */
    for (pass = 0; pass < 2; pass++) {
        uintptr_t ro = pass ? base : 0, rw = pass ? base + align_up(read_only, 0x1000) : 0;
        for (i = 0; i < o->section_count; i++) {
            const unsigned char* section = o->sections + (size_t) i * 40;
            const unsigned int flags = le32(section + 36), bits = (flags >> 20) & 15;
            const uintptr_t alignment = bits ? (uintptr_t) 1 << (bits - 1) : 16;
            uintptr_t* cursor;
            if (!loads(o, section)) {
                continue;
            }
            cursor = (flags & SCN_WRITE) != 0 && !constructors(o, section) ? &rw : &ro;
            *cursor = align_up(*cursor, alignment);
            if (pass) {
                o->section_at[i] = *cursor;
                if (le32(section + 20) != 0 && (flags & SCN_BSS) == 0) {
                    if (le32(section + 20) > o->size || le32(section + 16) > o->size - le32(section + 20)) {
                        OSReport("[plugin] %s: refused, a section runs past the file\n", o->file);
                        ok = 0;
                        break;
                    }
                    memcpy((void*) *cursor, o->data + le32(section + 20), le32(section + 16));
                }
            }
            *cursor += section_size(section);
        }
        if (!ok) {
            break;
        }
        if (pass == 0) {
            read_only = ro;
            written = rw;
            total = align_up(align_up(read_only, 0x1000) + written, 0x10000);
            if (total == 0 || total > 16u * 1024u * 1024u) {
                OSReport("[plugin] %s: refused, %u bytes to load\n", o->file, (unsigned int) total);
                ok = 0;
                break;
            }
            for (at = place_next; at + total <= PLACE_END; at += 0x10000) {
                base = (uintptr_t) VirtualAlloc((void*) at, total, MEM_COMMIT_RESERVE, PAGE_EXECUTE_READWRITE);
                if (base != 0) {
                    break;
                }
            }
            if (base == 0) {
                OSReport("[plugin] %s: not loaded, no room for %u bytes below %08X\n", o->file, (unsigned int) total,
                         PLACE_END);
                ok = 0;
                break;
            }
        }
    }
    for (i = 0; ok && i < o->section_count; i++) {
        if (o->section_at[i] != 0) {
            ok = relocate(o, i);
        }
    }
    /* The record: the one global the object defines under this name. */
    for (i = 0; ok && plugin == NULL && i < o->symbol_count; i += 1 + o->symbols[(size_t) i * 18 + 17]) {
        const unsigned char* symbol = o->symbols + (size_t) i * 18;
        if ((short) le16(symbol + 12) > 0 && symbol[16] == 2 /* IMAGE_SYM_CLASS_EXTERNAL */ &&
            strcmp(symbol_name(o, symbol, small), MU_FIGHTER_PLUGIN_SYMBOL) == 0)
        {
            uintptr_t address = 0;
            if (symbol_address(o, i, &address)) {
                plugin = (const MuFighterPlugin*) address;
            }
        }
    }
    if (ok && (plugin == NULL || plugin->magic != MU_FIGHTER_PLUGIN_MAGIC ||
               plugin->version != MU_FIGHTER_PLUGIN_VERSION))
    {
        OSReport("[plugin] %s: refused, not a fighter plugin of format %u\n", o->file, MU_FIGHTER_PLUGIN_VERSION);
        ok = 0;
    }
    if (ok) {
        static const unsigned char abi[16] = { MEXFT_ABI_BYTES };
        if (memcmp(plugin->abi, abi, 16) != 0) {
            char want[33], have[33];
            hex(want, plugin->abi);
            hex(have, abi);
            OSReport("[plugin] %s: refused, built for fighter layouts %s and this game has %s; build it again "
                     "for this version\n", o->file, want, have);
            ok = 0;
        }
    }
    if (ok && (plugin->fighter_count <= 0 || fighter_count + plugin->fighter_count > MAX_FIGHTERS)) {
        OSReport("[plugin] %s: refused, %d fighters\n", o->file, plugin->fighter_count);
        ok = 0;
    }
    if (ok) {
        /* Constructors (the FtState to MotionState conversion), last to first as the C runtime
         * runs a .ctors list, then code and constants are no longer writable. */
        for (i = o->section_count; i-- > 0;) {
            const unsigned char* section = o->sections + (size_t) i * 40;
            if (o->section_at[i] != 0 && constructors(o, section)) {
                unsigned int k;
                for (k = section_size(section) / 8; k-- > 0;) {
                    void (*run)(void) = *(void (**)(void)) (o->section_at[i] + (uintptr_t) k * 8);
                    if (run != NULL && (uintptr_t) run != (uintptr_t) -1) {
                        run();
                    }
                }
            }
        }
        if (read_only != 0) {
            VirtualProtect((void*) base, align_up(read_only, 0x1000), PAGE_EXECUTE_READ, &old);
        }
        writable->address = (void*) (base + align_up(read_only, 0x1000));
        writable->size = (unsigned int) written;
        place_next = base + total;
    } else if (base != 0) {
        VirtualFree((void*) base, 0, MEM_RELEASE);
        plugin = NULL;
    }
    VirtualFree(o->section_at, 0, MEM_RELEASE);
    return ok ? plugin : NULL;
}

static void load_one(const char* path, const char* name)
{
    Object o;
    Region writable = { NULL, 0 };
    const MuFighterPlugin* p;
    unsigned int i;
    int k;
    if (plugin_count >= MAX_PLUGINS) {
        OSReport("[plugin] %s: not loaded, %d plugins are the most\n", name, MAX_PLUGINS);
        return;
    }
    memset(&o, 0, sizeof o);
    o.file = name;
    o.data = read_file(path, &o.size);
    if (o.data == NULL) {
        OSReport("[plugin] %s: not loaded, the file cannot be read (system error %u)\n", name, GetLastError());
        return;
    }
    p = link_object(&o, &writable);
    if (p != NULL) {
        /* FNV-1a over the name and the contents. */
        unsigned int h = 2166136261u;
        for (i = 0; name[i] != '\0'; i++) {
            h = (h ^ (unsigned char) lower(name[i])) * 16777619u;
        }
        for (i = 0; i < o.size; i++) {
            h = (h ^ o.data[i]) * 16777619u;
        }
        h = h != 0 ? h : 1;
        variables[plugin_count++] = writable;
        for (k = 0; k < p->fighter_count; k++) {
            const Descriptor* d = (const Descriptor*) p->fighters[k].descriptor;
            fighter_digest[fighter_count] = h;
            fighters[fighter_count++] = p->fighters[k];
            OSReport("[plugin] %s: %s (%s%s) at %08X, %u bytes of variables, file %08X\n", name,
                     d->name != NULL ? d->name : "?", p->fighters[k].mex_name != NULL ? "by name, built as " : "",
                     d->file != NULL ? d->file : "?",
                     (unsigned int) ((uintptr_t) writable.address & ~(uintptr_t) 0xFFFF), writable.size, h);
        }
    }
    VirtualFree((void*) o.data, 0, MEM_RELEASE);
}

static int by_name(const void* a, const void* b)
{
    return strcmp((const char*) a, (const char*) b);
}

/* The files already tried, by name and by what the folder says of them (size and time of the last
 * write): a scan tries a file once, and again only when it has been replaced. */
typedef struct Seen {
    char name[MAX_PATH];
    unsigned int stamp[3];
} Seen;
static Seen seen[MAX_PLUGINS];
static int seen_count;

static int was_seen(const WIN32_FIND_DATAA* found, int remember)
{
    const unsigned int stamp[3] = { found->size_low, found->times[4], found->times[5] };
    int i;
    for (i = 0; i < seen_count; i++) {
        if (same_text(seen[i].name, found->cFileName)) {
            if (memcmp(seen[i].stamp, stamp, sizeof stamp) == 0) {
                return 1;
            }
            if (remember) {
                memcpy(seen[i].stamp, stamp, sizeof stamp);
            }
            return 0;
        }
    }
    if (remember && seen_count < MAX_PLUGINS) {
        strcpy(seen[seen_count].name, found->cFileName);
        memcpy(seen[seen_count].stamp, stamp, sizeof stamp);
        seen_count++;
    }
    return 0;
}

/* The host's answer to where this run's plugins are (port/app/source_host.cpp, MU_CMD_MOD_BROWSER 6):
 * 1 and the folder in dir, 0 for none this run (a Vanilla launch, or --fighters-dir none), -1 when
 * the host leaves it to this library (no --fighters-dir, or a host too old to say). */
int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
static int host_folder(char* dir)
{
    static unsigned char reply[4096]; /* the host's least reply buffer (MU_SLIPPI_RESPONSE_CAPACITY) */
    const unsigned char ask[1] = { 6 };
    unsigned int got = 0;
    if (mu_online_abi_command(MU_CMD_MOD_BROWSER, ask, 1, reply, sizeof reply, &got) != 0 || got < 1 ||
        reply[0] == 0) {
        return -1;
    }
    if (reply[0] != 2 || got < 2 || got - 1 >= MAX_PATH - 16) {
        return 0;
    }
    memcpy(dir, reply + 1, got - 1);
    dir[got - 1] = '\0';
    return 1;
}

/* Loads the plugins in the folder that have not been tried yet; returns how many it loaded. The
 * first scan is the one at startup. A later one (mexft_plugins_rescan) finds a file put there
 * while the game runs. */
static int scan(void)
{
    char library_dir[MAX_PATH], dir[MAX_PATH], pattern[MAX_PATH + 16], path[MAX_PATH * 2];
    static char names[MAX_PLUGINS][MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE find;
    char* slash;
    int count = 0, i, from_host;
    const int before = plugin_count;
    const char* named = getenv("MELEE_FIGHTER_PLUGINS");
    const unsigned int n = GetModuleFileNameA((HANDLE) __ImageBase, library_dir, MAX_PATH - 16);
    if (n == 0 || n >= MAX_PATH - 16 || (slash = strrchr(library_dir, '\\')) == NULL) {
        return 0;
    }
    *slash = '\0';
    from_host = host_folder(dir);
    if (from_host == 0) {
        static int said;
        if (!said) {
            said = 1;
            OSReport("[plugin] no fighter plugins this run (the host turned installed fighters off)\n");
        }
        return 0;
    }
    if (from_host < 0 && named != NULL && named[0] != '\0' && strlen(named) < MAX_PATH - 16) {
        strcpy(dir, named);
    } else if (from_host < 0) {
        strcpy(dir, library_dir);
        strcat(dir, "\\fighters");
    }
    strcpy(pattern, dir);
    strcat(pattern, "\\*" MU_FIGHTER_PLUGIN_EXTENSION);
    find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (count < MAX_PLUGINS && strlen(found.cFileName) < MAX_PATH && !was_seen(&found, 1)) {
            strcpy(names[count++], found.cFileName);
        }
    } while (FindNextFileA(find, &found));
    FindClose(find);
    if (count == 0) {
        return 0;
    }
    if (syms_load(library_dir)) {
        qsort(names, (size_t) count, sizeof names[0], by_name);
        for (i = 0; i < count; i++) {
            strcpy(path, dir);
            strcat(path, "\\");
            strcat(path, names[i]);
            load_one(path, names[i]);
        }
    }
    syms_free();
    OSReport("[plugin] %d of %d new fighter plugins loaded from %s (%d loaded in all)\n", plugin_count - before, count,
             dir, plugin_count);
    return plugin_count - before;
}
#else
/* The ELF builds link their fighters in for now. */
static int scan(void)
{
    return 0;
}
#endif

const void* mexft_plugin_fighter(const char* file, const char* mex_name)
{
    int i;
    if (!loaded) {
        loaded = 1;
        scan();
    }
    /* The fighter's own name first: a build may give the fighter any slot, and so any file name. */
    for (i = 0; mex_name != NULL && mex_name[0] != '\0' && i < fighter_count; i++) {
        if (fighters[i].mex_name != NULL && same_text(fighters[i].mex_name, mex_name)) {
            return fighters[i].descriptor;
        }
    }
    for (i = 0; file != NULL && i < fighter_count; i++) {
        const Descriptor* d = (const Descriptor*) fighters[i].descriptor;
        if (fighters[i].mex_name == NULL && d->file != NULL && same_text(d->file, file)) {
            return d;
        }
    }
    return NULL;
}

int mexft_plugins_rescan(void)
{
    if (!loaded) {
        return 0; /* the registry has not asked yet: its first lookup scans */
    }
    return scan();
}

unsigned int mexft_plugin_ranges(void* out, unsigned int capacity)
{
    Region* regions = (Region*) out;
    unsigned int count = 0;
    int i;
    for (i = 0; i < plugin_count; i++) {
        if (variables[i].size == 0) {
            continue;
        }
        if (regions != NULL && count < capacity) {
            regions[count] = variables[i];
        }
        count++;
    }
    return count;
}

unsigned int mexft_plugin_digest(const void* descriptor)
{
    int i;
    for (i = 0; descriptor != NULL && i < fighter_count; i++) {
        if (fighters[i].descriptor == descriptor) {
            return fighter_digest[i];
        }
    }
    return 0;
}
