/* Loads the fighter plugins (mexft_plugin.h) and hands their fighters to the registry
 * (akaneia/mu_ak_fighters.c find_fighter) and their variables to the rollback snapshot
 * (shim/mu_exclusions.c mu_snapshot_ranges).
 *
 * Where: the folder MELEE_FIGHTER_PLUGINS names, or "fighters" beside the game library. Every
 * *.mufighter in it is tried, in name order. A plugin that is refused says why in the log and its
 * fighter stays locked, as a fighter with no native code is.
 *
 * This file uses the operating system and none of the game's headers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mexft_plugin.h"

/* The game's include path has its own C library headers, which the system's do not mix with:
 * what this file needs of either is declared here. */
typedef unsigned long long uintptr_t;
typedef __SIZE_TYPE__ size_t;
#define NULL ((void*) 0)
char* getenv(const char* name);
void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));
int strcmp(const char* a, const char* b);
size_t strlen(const char* s);
char* strcpy(char* to, const char* from);
char* strcat(char* to, const char* from);
char* strrchr(const char* s, int c);
int memcmp(const void* a, const void* b, size_t n);
void* memcpy(void* to, const void* from, size_t n);

void OSReport(const char* format, ...);

enum { MAX_PLUGINS = 64, MAX_FIGHTERS = 128 };

/* A descriptor starts with its display name and its fighter file (MuAkFighter). */
typedef struct Descriptor {
    const char* name;
    const char* file;
} Descriptor;

typedef struct Region {
    void* address;
    unsigned int size;
} Region;

static const MuFighterPlugin* plugins[MAX_PLUGINS];
static int plugin_count;
static const Descriptor* fighters[MAX_FIGHTERS];
static int fighter_count;
static int loaded;

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int same_file(const char* a, const char* b)
{
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

#ifdef _WIN32
/* kernel32 and the PE header fields read below (winnt.h's names for them in the comments). */
typedef void* HMODULE;
typedef void* HANDLE;
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE) (long long) -1)
#define DONT_RESOLVE_DLL_REFERENCES 1u
typedef struct WIN32_FIND_DATAA {
    unsigned int attributes;
    unsigned int times[6];
    unsigned int size_high, size_low, reserved0, reserved1;
    char cFileName[MAX_PATH];
    char alternate[14];
} WIN32_FIND_DATAA;
__declspec(dllimport) HMODULE __stdcall LoadLibraryExA(const char* path, HANDLE file, unsigned int flags);
__declspec(dllimport) int __stdcall FreeLibrary(HMODULE module);
__declspec(dllimport) void* __stdcall GetProcAddress(HMODULE module, const char* name);
__declspec(dllimport) unsigned int __stdcall GetLastError(void);
__declspec(dllimport) unsigned int __stdcall GetModuleFileNameA(HMODULE module, char* path, unsigned int size);
__declspec(dllimport) HANDLE __stdcall FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* found);
__declspec(dllimport) int __stdcall FindNextFileA(HANDLE find, WIN32_FIND_DATAA* found);
__declspec(dllimport) int __stdcall FindClose(HANDLE find);

extern char __ImageBase[];

static unsigned int le32(const unsigned char* p)
{
    return p[0] | (unsigned int) p[1] << 8 | (unsigned int) p[2] << 16 | (unsigned int) p[3] << 24;
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

static void load_one(const char* path, const char* name, const unsigned char game[16])
{
    HMODULE module;
    const MuFighterPlugin* p;
    char want[33], have[33];
    int i;
    if (plugin_count >= MAX_PLUGINS) {
        OSReport("[plugin] %s: not loaded, %d plugins are the most\n", name, MAX_PLUGINS);
        return;
    }
    /* No relocations and no imports: the system maps it at its own address or not at all. */
    module = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (module == NULL) {
        OSReport("[plugin] %s: not loaded (system error %u; its address may be taken)\n", name,
                 (unsigned int) GetLastError());
        return;
    }
    p = (const MuFighterPlugin*) (void*) GetProcAddress(module, MU_FIGHTER_PLUGIN_SYMBOL);
    if (p == NULL || p->magic != MU_FIGHTER_PLUGIN_MAGIC || p->version != MU_FIGHTER_PLUGIN_VERSION) {
        OSReport("[plugin] %s: refused, not a fighter plugin of format %u\n", name, MU_FIGHTER_PLUGIN_VERSION);
        FreeLibrary(module);
        return;
    }
    if ((uintptr_t) module != p->base || p->base > 0xFFFF0000ull) {
        OSReport("[plugin] %s: refused, mapped at %p and linked for %08X\n", name, (void*) module,
                 (unsigned int) p->base);
        FreeLibrary(module);
        return;
    }
    if (memcmp(p->game_id, game, 16) != 0) {
        hex(want, p->game_id);
        hex(have, game);
        OSReport("[plugin] %s: refused, built for game build %s and this is %s\n", name, want, have);
        FreeLibrary(module);
        return;
    }
    if (p->fighter_count <= 0 || fighter_count + p->fighter_count > MAX_FIGHTERS) {
        OSReport("[plugin] %s: refused, %d fighters\n", name, p->fighter_count);
        FreeLibrary(module);
        return;
    }
    if (p->init != NULL) {
        p->init();
    }
    plugins[plugin_count++] = p;
    for (i = 0; i < p->fighter_count; i++) {
        const Descriptor* d = (const Descriptor*) p->fighters[i];
        fighters[fighter_count++] = d;
        OSReport("[plugin] %s: %s (%s) at %08X, %u bytes of variables\n", name, d->name != NULL ? d->name : "?",
                 d->file != NULL ? d->file : "?", (unsigned int) p->base,
                 (unsigned int) ((p->data_hi - p->data_lo) + (p->bss_hi - p->bss_lo)));
    }
}

static int by_name(const void* a, const void* b)
{
    return strcmp((const char*) a, (const char*) b);
}

static void load_all(void)
{
    char dir[MAX_PATH], pattern[MAX_PATH + 16], path[MAX_PATH * 2];
    static char names[MAX_PLUGINS][MAX_PATH];
    unsigned char game[16];
    WIN32_FIND_DATAA found;
    HANDLE find;
    int count = 0, i;
    const char* named = getenv("MELEE_FIGHTER_PLUGINS");
    if (named != NULL && named[0] != '\0' && strlen(named) < MAX_PATH - 16) {
        strcpy(dir, named);
    } else {
        char* slash;
        unsigned int n = GetModuleFileNameA((HMODULE) __ImageBase, dir, MAX_PATH - 16);
        if (n == 0 || n >= MAX_PATH - 16) {
            return;
        }
        slash = strrchr(dir, '\\');
        if (slash == NULL) {
            return;
        }
        strcpy(slash + 1, "fighters");
    }
    strcpy(pattern, dir);
    strcat(pattern, "\\*" MU_FIGHTER_PLUGIN_EXTENSION);
    find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (count < MAX_PLUGINS && strlen(found.cFileName) < MAX_PATH) {
            strcpy(names[count++], found.cFileName);
        }
    } while (FindNextFileA(find, &found));
    FindClose(find);
    if (!game_id(game)) {
        OSReport("[plugin] %d fighter plugins in %s are not loaded: this game library has no build id\n", count, dir);
        return;
    }
    qsort(names, (size_t) count, sizeof names[0], by_name);
    for (i = 0; i < count; i++) {
        strcpy(path, dir);
        strcat(path, "\\");
        strcat(path, names[i]);
        load_one(path, names[i], game);
    }
    OSReport("[plugin] %d of %d fighter plugins loaded from %s\n", plugin_count, count, dir);
}
#else
/* The ELF builds link their fighters in for now. */
static void load_all(void) {}
#endif

const void* mexft_plugin_fighter(const char* file)
{
    int i;
    if (!loaded) {
        loaded = 1;
        load_all();
    }
    for (i = 0; i < fighter_count; i++) {
        if (fighters[i]->file != NULL && same_file(fighters[i]->file, file)) {
            return fighters[i];
        }
    }
    return NULL;
}

unsigned int mexft_plugin_ranges(void* out, unsigned int capacity)
{
    Region* regions = (Region*) out;
    unsigned int count = 0;
    int i;
    for (i = 0; i < plugin_count; i++) {
        const MuFighterPlugin* p = plugins[i];
        if (regions != NULL && count < capacity) {
            regions[count].address = p->data_lo;
            regions[count].size = (unsigned int) (p->data_hi - p->data_lo);
        }
        count++;
        if (regions != NULL && count < capacity) {
            regions[count].address = p->bss_lo;
            regions[count].size = (unsigned int) (p->bss_hi - p->bss_lo);
        }
        count++;
    }
    return count;
}
