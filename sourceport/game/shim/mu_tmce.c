/* Training Mode CE, native build: the game-side hooks.
 *
 * On the console TM-CE hooks the game with PowerPC patches (its ASM folder). Natively each of those
 * patches is a call from the decomp at the same point, landing here, and from here into TM-CE's own C
 * (sourceport/game/tmce, compiled against the native MexTK headers). Only while TM-CE is enabled: its
 * data files are on the disc (the TM folder beside the game, built from the pinned upstream, or a
 * Training Mode CE disc in the mod profile), the base is vanilla, and the game mode is the main menu
 * or Event Match (mu_tmce_active).
 *
 * TM-CE: Training Mode - Community Edition, by UnclePunch and the TM-CE contributors. */
#include <mu_native.h>

#include <sysdolphin/baselib/archive.h>
#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbfile.h>

/* values TM-CE's modules pass to each other (spare console memory on the console) */
s8 mu_tmce_onload_fileno = -1;
s8 mu_tmce_onload_slot = -1;

/* ---- TM-CE's own C (prefixed exports of the eventMenu module) ---- */
void tmce_eventMenu_OnFileLoad(void* archive);
void tmce_eventMenu_OnBoot(void);
void tmce_eventMenu_OnSceneChange(void);
void tmce_eventMenu_OnStartMelee(void);
unsigned char gm_GetCurrentGameMode(void);

/* the event menu's file stays loaded for the whole session, as the console loader keeps it */
static u8 tmce_menu_file[256 * 1024] __attribute__((aligned(32)));
static HSD_Archive tmce_menu_archive;
static int tmce_loaded;
static int tmce_refused;

/* Training Mode is the menus' Event Match and nothing else: its hooks run only in the main menu's
 * game mode (where Event Match's list, the 1-P menu's label and the OSD toggles are) and in the event
 * game mode (its character select, stage select and matches). VS, Training, the other 1-P modes, the
 * online modes and replays play exactly as without it (the match digest does not change). */
static int tmce_mode(void)
{
    const unsigned char mode = gm_GetCurrentGameMode();
    return mode == 0x01 /* GM_MENU */ || mode == 0x2B /* GM_EVENT */;
}

int mu_tmce_active(void)
{
    return tmce_loaded && mu_tmce_enabled() && tmce_mode();
}

/* At each mode load (gm_1A3F.c, as m-ex): the first time TM-CE's files are on the disc and a
 * Training Mode game mode starts, load the event menu file and let TM-CE set itself up. A run that
 * goes straight to a VS match never reads it. */
void mu_tmce_boot(void)
{
    size_t length;
    if (tmce_loaded || tmce_refused || !mu_tmce_enabled() || !tmce_mode())
        return;
    if (DVDConvertPathToEntrynum("TM/eventMenu.dat") < 0)
        return;
    length = lbFileGetSize("TM/eventMenu.dat");
    if (length == 0 || length > sizeof tmce_menu_file) {
        OSReport("[tmce] TM/eventMenu.dat is %u bytes (at most %u supported)\n", (unsigned) length,
                 (unsigned) sizeof tmce_menu_file);
        return;
    }
    lbFile_8001668C("TM/eventMenu.dat", tmce_menu_file, &length);
    /* As lbArchive_InitializeDAT, but a file the archive reader refuses leaves Training Mode CE
     * off for the session instead of stopping the game. */
    if (HSD_ArchiveParse(&tmce_menu_archive, tmce_menu_file, length) == -1) {
        tmce_refused = 1;
        OSReport("[tmce] TM/eventMenu.dat is not an archive the game can read; Training Mode CE "
                 "stays off\n");
        return;
    }
    {
        const char* symbol;
        int i;
        for (i = 0; (symbol = HSD_ArchiveGetExtern(&tmce_menu_archive, i)) != NULL; i++) {
            HSD_ArchiveLocateExtern(&tmce_menu_archive, symbol, NULL);
        }
    }
    tmce_loaded = 1;
    tmce_eventMenu_OnFileLoad(&tmce_menu_archive);
    tmce_eventMenu_OnBoot();
    OSReport("[tmce] Training Mode CE loaded (TM/eventMenu.dat, %u bytes)\n", (unsigned) length);
}

void mu_tmce_on_scene_change(void)
{
    /* The console pack brands every scene. In a combined native profile CE owns Event Match;
     * the main menu, ordinary VS and TE's menu should keep their normal presentation. */
    if (gm_GetCurrentGameMode() == 0x2B)
        tmce_eventMenu_OnSceneChange();
}

void mu_tmce_osd_start_melee(void); /* tmce/native/tmce_osd.c */

void mu_tmce_on_start_melee(void)
{
    tmce_eventMenu_OnStartMelee();
    mu_tmce_osd_start_melee(); /* the message manager exists now: the on-screen displays may print */
}
