/* Exercise the production serializer against known console bytes and its reader. */
#include "../shim/mu_replay.c"
void mu_replay_abi_log(const char* text) { (void) text; }
#ifdef MU_AKANEIA_FIGHTERS
/* This fixture uses retail player ids. The separate character-map fixture covers m-ex ids. */
int mu_ak_ckind_from_mex(int ext) { return ext; }
#endif

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

/* What emit_game_start (the Game Start event) reads from the rest of the game, for the identity check. */
HSD_PadStatus HSD_PadCopyStatus[4];
static u32 test_seed = 0x12345678;
u32* HSD_RandSeedPtr = &test_seed;
u8 gm_GetNumCostumesForCKind(u8 kind) { (void) kind; return 6; }
char* GetNameText(int slot) { (void) slot; return NULL; }
u8 gm_GetCurrentSceneIndex(void) { return 2; }
u8 gm_GetCurrentGameMode(void) { return 8; }   /* the online VS scene */
enum_t lbLang_GetSavedLanguage(void) { return 1; }
int mu_general_codes(void) { return 0; }
int mu_ps_frozen_toggle(void) { return 0; }
#if MU_MAKER
int mu_bm_maker(void) { return 0; }
void BAM_ReplayBuildsStamp(u8* info) { (void) info; }
#endif
#ifdef MU_AKANEIA_FIGHTERS
int mu_ak_mex_external(int ckind) { return ckind; }
#endif
static unsigned int test_game, test_tiebreak;
void mu_online_record_game_numbers(unsigned int* game, unsigned int* tiebreak)
{
    *game = test_game;
    *tiebreak = test_tiebreak;
}
static u8 event[GS_SIZE];
static unsigned int event_command, event_size;
void mu_replay_abi_event(unsigned int command, const unsigned char* payload, unsigned int size)
{
    event_command = command;
    event_size = size;
    memcpy(event, payload, size < sizeof event ? size : sizeof event);
}

/* An online match state block (Slippi's MSRB) with each port's Shift-JIS name, connect code (the
 * full-width '#' is 0x81 0x94) and UID, the match id and the ranked info as the host writes them. */
static void fill_msrb(u8* msrb)
{
    static const char* names[4] = {"SwagPlayer", "Noob", "Ann", "Bob"};
    static const char* codes[4] = {"SWAG\x81\x94" "123", "NOOB\x81\x94" "456", "AAA\x81\x94" "1", "BBBBBBB\x81\x94" "2"};
    static const char* uids[4] = {"uidSwagPlayer0000000000000a", "uidNoob000000000000000000b",
                                  "uidAnn00000000000000000001", "uidBob00000000000000000002"};
    int i;
    memset(msrb, 0, 1100);
    for (i = 0; i < 4; ++i) {
        memcpy(msrb + MSRB_P1_NAME + 31 * i, names[i], strlen(names[i]));
        memcpy(msrb + MSRB_P1_CONNECT_CODE + 10 * i, codes[i], strlen(codes[i]));
        memcpy(msrb + MSRB_P1_SLIPPI_UID + 29 * i, uids[i], strlen(uids[i]));
    }
    memcpy(msrb + MSRB_MATCH_ID, "mode.direct-2026-11-25T10:00:00.00Z-0-v3", 40);
    for (i = 0; i < 16 * 4 + 4; ++i) msrb[MSRB_RANKED_PLAYER_INFO + i] = (u8) (i + 1);
}

static int zero(const u8* p, int n)
{
    while (n-- > 0) if (*p++ != 0) return 0;
    return 1;
}

/* One Game Start with `humans` players (the rest empty slots): every identity field of every port. */
static int check_identity(int humans, const u8* msrb, unsigned int game, unsigned int tiebreak)
{
    static StartMeleeData data;
    int i;
    memset(&data, 0, sizeof data);
    for (i = 0; i < 6; ++i) {
        data.players[i].slot_type = i < humans ? 0 : 3;
        data.players[i].ckind = i < humans ? 2 + i : 0x21;
        data.players[i].nametag = 0x78;
    }
    test_game = game;
    test_tiebreak = tiebreak;
    event_command = event_size = 0;
    emit_game_start(&data, msrb);
    CHECK(event_command == 0x36 && event_size == GS_SIZE - 1);   /* 828 bytes, replay 3.20.0 */
    CHECK(event[0] == 3 && event[1] == 20 && event[2] == 0);
    CHECK(event[GS_MAJOR - 1] == 8 && event[GS_LANGUAGE - 1] == 1);
    for (i = 0; i < 4; ++i) {
        const u8* name = event + GS_NAME - 1 + 31 * i;
        const u8* code = event + GS_CODE - 1 + 10 * i;
        const u8* uid = event + GS_UID - 1 + 29 * i;
        if (msrb != NULL && i < humans) {
            CHECK(memcmp(name, msrb + MSRB_P1_NAME + 31 * i, 31) == 0 && name[0] != 0);
            CHECK(memcmp(code, msrb + MSRB_P1_CONNECT_CODE + 10 * i, 10) == 0 && code[0] != 0);
            CHECK(memcmp(uid, msrb + MSRB_P1_SLIPPI_UID + 29 * i, 29) == 0 && uid[0] != 0);
        } else {
            CHECK(zero(name, 31) && zero(code, 10) && zero(uid, 29));
        }
    }
    if (msrb != NULL) {
        CHECK(memcmp(event + GS_MATCH_ID - 1, msrb + MSRB_MATCH_ID, 51) == 0);
        CHECK(be32(event + GS_GAME_NUMBER - 1) == game && be32(event + GS_TIEBREAK - 1) == tiebreak);
        CHECK(memcmp(event + GS_RANKED_PLAYER_INFO - 1, msrb + MSRB_RANKED_PLAYER_INFO, 16 * 4 + 4) == 0);
    } else {   /* offline: SendGameInfo writes zeroes there */
        CHECK(zero(event + GS_MATCH_ID - 1, 51 + 4 + 4 + 16 * 4 + 4));
    }
    return 0;
}

int main(void)
{
    {
        static u8 msrb[1100];
        int line;
        fill_msrb(msrb);
        /* Direct / Unranked (two players), Teams (four), a Ranked tiebreak, and offline. */
        if ((line = check_identity(2, msrb, 2, 0)) != 0) return line;
        CHECK(memcmp(event + GS_CODE - 1, "SWAG\x81\x94" "123\0", 10) == 0);
        CHECK(memcmp(event + GS_NAME - 1 + 31, "Noob\0", 5) == 0);
        if ((line = check_identity(4, msrb, 1, 0)) != 0) return line;
        CHECK(memcmp(event + GS_CODE - 1 + 30, "BBBBBBB\x81\x94" "2", 10) == 0);
        if ((line = check_identity(2, msrb, 3, 1)) != 0) return line;
        if ((line = check_identity(2, NULL, 5, 2)) != 0) return line;
    }
    StartMeleeRules r = {0}, decoded = {0};
    PlayerInitData p = {0}, player = {0};
    u8 rules[0x60], again[0x60], bytes[0x24], twice[0x24];
    r.match_kind = 1; r.x0_3 = 4; r.timer_enabled = 1;
    r.friendly_fire = 1; r.is_stock = 1; r.disable_pausing = 1; r.is_vs = 1;
    r.stkind = 31; r.time_limit = 480; r.item_freq = -1; r.sd_penalty = -2;
    r.x20 = 0x0123456789ABCDEFULL; r.x28 = -1;
    r.x30 = 1.0f; r.game_speed = 1.0f;
    r.on_frame_start = (void*) 0x12345678;
    record_rules(rules, &r);
    CHECK(rules[0] == 0x32 && rules[1] == 1 && rules[2] == 0x88 && rules[4] == 0x40);
    CHECK(rules[11] == 255 && rules[12] == 254 && rules[14] == 0 && rules[15] == 31);
    CHECK(be32(rules + 16) == 480 && be32(rules + 0x20) == 0x01234567);
    CHECK(be32(rules + 0x24) == 0x89ABCDEF && be32(rules + 0x30) == 0x3F800000);
    for (int i = 0x38; i < 0x60; ++i) CHECK(rules[i] == 0);
    convert_rules(&decoded, rules); record_rules(again, &decoded);
    CHECK(memcmp(rules, again, sizeof rules) == 0);
    p.ckind = 2; p.slot_type = 1; p.stocks = 4; p.color = 3; p.slot = 1;
    p.spawn_pos = -1; p.spawn_dir = -1; p.handicap = 9; p.team = 2;
    p.rumble_enabled = 1; p.vs_metal = 1; p.vs_invisible = 1; p.xD_b7 = 1;
    p.cpu_kind = 4; p.cpu_level = 9; p.damage = 123; p.damage1 = 456; p.hp = 150;
    p.attack_ratio = 1.0f; p.defense_ratio = 2.0f; p.model_scale = 0.5f;
    record_player(bytes, &p);
    CHECK(be32(bytes) == 0x02010403 && bytes[5] == 255 && bytes[6] == 255);
    CHECK(bytes[12] == 0xA8 && bytes[13] == 1 && be16(bytes + 18) == 456);
    CHECK(be32(bytes + 24) == 0x3F800000 && be32(bytes + 32) == 0x3F000000);
    convert_player(&player, bytes); record_player(twice, &player);
    CHECK(memcmp(bytes, twice, sizeof bytes) == 0);
    return 0;
}
