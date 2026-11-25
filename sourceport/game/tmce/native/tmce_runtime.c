/* Training Mode CE, native build: what the console's m-ex loader provided.
 *
 * On the console TM-CE's C is compiled into .dat files (TM/eventMenu.dat, TM/lab.dat, ...) and a
 * patched Start.dol loads them, relocates their code and hands the game a table of their functions.
 * Natively the same C is compiled into the game (tools/tmce/link_module.py gives each module its own
 * prefix), so loading a module means: load the .dat file for its data (models, menus: the player's
 * TM-CE disc) and hand back the functions compiled in. Compiled against the native MexTK headers;
 * m-ex's C library and argument-order wrappers are tmce/mex/mex_runtime.c.
 *
 * TM-CE: Training Mode - Community Edition, by UnclePunch and the TM-CE contributors. */
#include "../MexTK/mex.h"
#include "../src/events.h"

/* the event state every module reaches through event_vars (a fixed console address on the console) */
void *mu_tmce_event_vars;

/* ---- the modules ---- */
#define EV_DECL(m)                                                                                  \
    void tmce_##m##_Event_Init(GOBJ *) __attribute__((weak));                                       \
    void tmce_##m##_Event_Update(void) __attribute__((weak));                                       \
    void tmce_##m##_Event_Think(GOBJ *) __attribute__((weak));                                      \
    extern EventMenu *tmce_##m##_Event_Menu;
EV_DECL(lab)
EV_DECL(lcancel)
EV_DECL(ledgedash)
EV_DECL(wavedash)
EV_DECL(powershield)
EV_DECL(dthrowknee)
EV_DECL(edgeguard)
EV_DECL(fc)
EV_DECL(sweetspot)
EV_DECL(laserland)
EV_DECL(eggs)
EV_DECL(techchase)
EV_DECL(slalom)

typedef struct EventModule {
    const char *file;
    evFunction functions;
} EventModule;

#define EV_ROW(m) { "TM/" #m ".dat", { tmce_##m##_Event_Init, tmce_##m##_Event_Update, tmce_##m##_Event_Think, &tmce_##m##_Event_Menu } }
static const EventModule event_modules[] = {
    EV_ROW(lab), EV_ROW(lcancel), EV_ROW(ledgedash), EV_ROW(wavedash), EV_ROW(powershield),
    EV_ROW(dthrowknee), EV_ROW(edgeguard), EV_ROW(fc), EV_ROW(sweetspot), EV_ROW(laserland),
    EV_ROW(eggs), EV_ROW(techchase), EV_ROW(slalom),
};

void tmce_labCSS_OnCSSLoad(HSD_Archive *archive);

int mu_tmce_dvd_entrynum(const char *path) __asm__("DVDConvertPathToEntrynum");

static int same_file(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return 0;
        if (x == 0)
            return 1;
    }
}

/* m-ex's MEX_LoadRelArchive: load the module's file and fill `functions` with its entry points.
 * A module with no data of its own may have no file on the disc (a newer TM-CE than the player's
 * disc): its code is here all the same, and its archive is NULL. */
HSD_Archive *MEX_LoadRelArchive(char *file, void *functions, char *symbol)
{
    HSD_Archive *archive = 0;
    if (mu_tmce_dvd_entrynum(file) >= 0)
        archive = Archive_LoadFile(file);
    if (__builtin_strcmp(symbol, "cssFunction") == 0) {
        *(void **) functions = (void *) tmce_labCSS_OnCSSLoad;
        return archive;
    }
    for (unsigned i = 0; i < sizeof event_modules / sizeof event_modules[0]; i++) {
        if (same_file(event_modules[i].file, file)) {
            __builtin_memcpy(functions, &event_modules[i].functions, sizeof(evFunction));
            return archive;
        }
    }
    OSReport("[tmce] no native module for %s\n", file);
    __builtin_memset(functions, 0, sizeof(evFunction));
    return archive;
}

/* TM-CE's current event page, kept in a spare byte of the save file (MexTK TM_EventPage, twin-mapped) */
int mu_tmce_event_page(void) { return stc_memcard->TM_EventPage; }
void mu_tmce_set_event_page(int page) { stc_memcard->TM_EventPage = (u8) page; }
