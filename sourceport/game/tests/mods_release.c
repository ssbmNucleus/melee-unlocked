/* The Mods menu's release switches, the game's half (shim/mu_mods_menu.c, the production functions): the
 * host hands over which categories are on (MU_CMD_MOD_BROWSER 8's second byte, release_scope.h mask()).
 * With the release's defaults (0x03: Characters, Stages) Browse keeps its six rows, the other four
 * unavailable (A: the error sound) and reading "Coming soon."; Manage has the two that are on. A
 * switch turned on (Particles: 0x07) makes its row like the others'. The host's half (the table, the
 * settings lines, the byte, the online bit): port/tests/release_scope_test.cpp. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct HSD_Text { int unused; } HSD_Text;
typedef struct MuModsCategory {
    const char* name;
    const char* browse_words;
    const char* manage_words;
} MuModsCategory;
enum { MU_MODS_CATEGORIES = 6 };
#define CATEGORY_ROWS MU_MODS_CATEGORIES
#define MODS_KIND 35

static const MuModsCategory characters = {"Characters", "Skins for the fighters.", "Character skins you installed."},
                            stages = {"Stages", "Skins for the stages.", "Stage skins you installed."},
                            effects = {"Effects", "Effects for the fighters.", "Effects you installed."},
                            fighters = {"Fighters", "New fighters to play offline.", "Fighters you installed."},
                            new_stages = {"New Stages", "New stages for versus.", "New stages you installed."},
                            menus = {"Menus", "Character select screens.", "Menu mods you installed."};
static const MuModsCategory* const mu_mods_categories[MU_MODS_CATEGORIES] = {&characters, &stages, &effects,
                                                                             &fighters, &new_stages, &menus};

static int enabled = 1, manage;
static unsigned char categories_on;
static const char* said;   /* the description line last written */
static int said_from;      /* the catalog text it was made to start from (0: the row's own) */
#define SOON_TEXT 0x96
static void HSD_SisLib_803A6368(HSD_Text* text, int sis_idx)
{
    (void) text;
    said_from = sis_idx;
}
static void mu_slippi_menu_description(HSD_Text* text, const char* words)
{
    (void) text;
    said = words;
}

#include "category_on_body.h"
#include "row_category_body.h"
#include "mu_mods_menu_rows_body.h"
#include "mu_mods_menu_available_body.h"
#include "mu_mods_menu_describe_body.h"

static const char* words_of(int row)
{
    HSD_Text text;
    said = NULL;
    said_from = 0;
    mu_mods_menu_describe(&text, MODS_KIND, row);
    return said ? said : "";
}

int main(void)
{
    int row;
    /* The release's defaults (release_scope.h kTable: Characters and Stages on). */
    categories_on = 0x03;
    manage = 0;
    assert(mu_mods_menu_rows() == 6);   /* Browse: every row shows, off ones too */
    assert(mu_mods_menu_available(0) && mu_mods_menu_available(1));
    assert(strcmp(words_of(0), "Skins for the fighters.") == 0 && said_from == 0);   /* a row that is on: its own text */
    for (row = 2; row < 6; ++row) {
        assert(!mu_mods_menu_available(row));                 /* A: the error sound, nothing opens */
        assert(strcmp(words_of(row), "Coming soon.") == 0);   /* Particles, Fighters, New Stages, Menus */
        assert(said_from == SOON_TEXT);                       /* all four in one form, whatever the row's own text */
    }
    manage = 1;
    assert(mu_mods_menu_rows() == 2);   /* Manage: the ones on only */
    assert(row_category(0) == 0 && row_category(1) == 1 && row_category(2) == -1);
    assert(strcmp(words_of(1), "Stage skins you installed.") == 0);

    /* "modsparticles 1": Particles is a row like the others. */
    categories_on = 0x07;
    manage = 0;
    assert(mu_mods_menu_available(2) && strcmp(words_of(2), "Effects for the fighters.") == 0 && said_from == 0);
    assert(!mu_mods_menu_available(3) && strcmp(words_of(3), "Coming soon.") == 0);
    manage = 1;
    assert(mu_mods_menu_rows() == 3 && row_category(2) == 2);

    /* Fighters alone over the defaults: Manage's third row is Fighters, not Particles. */
    categories_on = 0x0B;
    assert(mu_mods_menu_rows() == 3 && row_category(2) == 3);
    assert(strcmp(words_of(2), "Fighters you installed.") == 0);

    /* Every category on (an automated run's default): as before the switches. */
    categories_on = 0x3F;
    manage = 0;
    for (row = 0; row < 6; ++row) assert(mu_mods_menu_available(row) && strcmp(words_of(row), "Coming soon.") != 0);
    manage = 1;
    assert(mu_mods_menu_rows() == 6);
    printf("mods release switches (game): all checks passed\n");
    return 0;
}
