/* A game built without Training Mode CE (-DMU_TMCE=OFF): the TM-CE hooks the decomp calls, doing
 * nothing. In place of shim/mu_tmce*.c and TM-CE's modules (sourceport/game/CMakeLists.txt).
 *
 * mu_tmce_active() and mu_tmce_enabled() are 0, so the game plays as it does without TM-CE's disc
 * files. Every other hook is called only after mu_tmce_active(), except the ones the decomp calls
 * unconditionally: mu_tmce_boot, mu_tmce_ev_scene_lists (vanilla tables, never swapped here), the
 * pass-through checks mu_tmce_osd_oo_airborne and mu_tmce_osd_oo_hitstun (their argument back), and
 * MU_TMCE_ON_EXIT's mu_tmce_osd_msid (-1) and *_exit. A body that returns a value gives the vanilla
 * one where the hook is handed it. */
#include <mu_native.h>
#include <mu_tmce_codes.h>
#include <mu_tmce_ess.h>
#include <mu_tmce_osd.h>
#include "mu_tmce_event.h"

/* ---- shim/mu_tmce.c ---- */
int mu_tmce_enabled(void) { return 0; }
int mu_tmce_active(void) { return 0; }
void mu_tmce_boot(void) {}
void mu_tmce_on_scene_change(void) {}
void mu_tmce_on_start_melee(void) {}

/* ---- shim/mu_tmce_menu.c ---- */
void mu_tmce_menu_load(struct HSD_MatAnimJoint* cursor_matanim, struct HSD_MatAnimJoint* panel_matanim,
                       struct HSD_MatAnimJoint* contop_matanim) { (void) cursor_matanim; (void) panel_matanim; (void) contop_matanim; }
void mu_tmce_menu_describe(struct HSD_Text* text, int menu_kind, int selection) { (void) text; (void) menu_kind; (void) selection; }

/* ---- shim/mu_tmce_codes.c (ft/ftdrawcommon.c) ---- */
struct HitCapsule* mu_tmce_code_hitbox_order(struct HitCapsule* hitboxes, int i) { (void) i; return hitboxes; }
void mu_tmce_code_hitbox_extras(struct Fighter* fp) { (void) fp; }
void mu_tmce_code_fighter_hurt_pass(int on) { (void) on; }
void* mu_tmce_code_fod_reflection(void* render_cb) { return render_cb; }

/* ---- shim/mu_tmce_ess.c: the Event Select Screen ---- */
s32 mu_tmce_ess_last_scroll(void) { return 0; }
bool mu_tmce_ess_short_page(struct MnEventData* data, s32 event_idx) { (void) data; (void) event_idx; return false; }
bool mu_tmce_ess_skip_row(struct MnEventData* data, s32 idx, s32 event_id) { (void) data; (void) idx; (void) event_id; return false; }
void mu_tmce_ess_event_name(struct MnEventData* data, s32 idx, s32 event_id, f32 x, f32 y) { (void) data; (void) idx; (void) event_id; (void) x; (void) y; }
void mu_tmce_ess_description(struct MnEventData* data) { (void) data; }
void mu_tmce_ess_page_arrows(void) {}
bool mu_tmce_ess_button_actions(HSD_GObj* ess_gobj) { (void) ess_gobj; return false; }
bool mu_tmce_ess_jump_down(struct MnEventData* data) { (void) data; return false; }
bool mu_tmce_ess_jump_up(struct MnEventData* data) { (void) data; return false; }
void mu_tmce_ess_cursor_to_row(HSD_GObj* ess_gobj) { (void) ess_gobj; }
bool mu_tmce_ess_down_blocked(struct MnEventData* data) { (void) data; return false; }
void mu_tmce_ess_create_page_texts(void) {}
void mu_tmce_ess_remove_page_texts(void) {}
void mu_tmce_ess_remove_event_texture(HSD_Archive* archive) { (void) archive; }
s32 mu_tmce_ess_load_score(s32 event) { (void) event; return 0; }
void mu_tmce_ess_save_score(s32 event, s32 score) { (void) event; (void) score; }
u8 mu_tmce_ess_score_type(u8 event) { (void) event; return 0; }

/* ---- shim/mu_tmce_event.c: event mode and its CSS ---- */
unsigned char mu_tmce_ev_css_enter(struct CSSData* css) { (void) css; return 0; }
void mu_tmce_ev_css_restore(struct CSSData* css, unsigned char css_type, unsigned char port) { (void) css; (void) css_type; (void) port; }
void mu_tmce_ev_css_load_ssm(struct CSSData* css) { (void) css; }
void mu_tmce_ev_css_backup(struct CSSData* css, struct EventData* ev) { (void) css; (void) ev; }
void mu_tmce_ev_css_exit_sss(void) {}
int mu_tmce_ev_enter_vs(struct StartMeleeData* md, int event_id, struct gm_804D6900_t** level_slot) { (void) md; (void) event_id; (void) level_slot; return 0; }
int mu_tmce_ev_ab_reset(void) { return 0; }
void mu_tmce_ev_scene_lists(int active) { (void) active; }
void mu_tmce_ev_css_event_name(void) {}
void mu_tmce_ev_css_icons_init(struct CSSData* css) { (void) css; }
int mu_tmce_ev_css_vs_text(void) { return 0; }
void mu_tmce_ev_css_on_load(void) {}

/* ---- shim/mu_tmce_osd.c and tmce/native/tmce_osd.c: the on-screen displays ---- */
void mu_tmce_osd_change_motion(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_fastfall_reset(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_anim_counts(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_post_hitstun(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_anim_end(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_input(struct HSD_GObj* gobj) { (void) gobj; }
int mu_tmce_osd_ff_tick(struct Fighter* fp) { (void) fp; return 1; }
void mu_tmce_osd_ff_display(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_shield_hit(struct Fighter* attacker, struct Fighter* defender) { (void) attacker; (void) defender; }
void mu_tmce_osd_hit_backup(struct Fighter* victim) { (void) victim; }
void mu_tmce_osd_throw_source(struct Fighter* victim, struct HSD_GObj* thrower) { (void) victim; (void) thrower; }
int mu_tmce_osd_same_projectile(struct Item* ip, struct HSD_GObj* victim) { (void) ip; (void) victim; return 0; }
int mu_tmce_osd_same_move_again(struct Fighter* fp) { (void) fp; return 0; }
int mu_tmce_osd_combo_move(int slot, int victim_slot, int move) { (void) slot; (void) victim_slot; return move; }
void mu_tmce_osd_combo_increment(int slot, int victim_slot, int arg3) { (void) slot; (void) victim_slot; (void) arg3; }
void mu_tmce_osd_sdi_init(struct HSD_GObj* gobj) { (void) gobj; }
int mu_tmce_osd_sdi(struct Fighter* fp) { (void) fp; return 1; }
void mu_tmce_osd_airdodge_angle(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_act_oo_float(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_airdodge_item(struct Fighter* fp, int is_throw) { (void) fp; (void) is_throw; }
void mu_tmce_osd_boost_grab(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_fortress_nil(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_glide_toss(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_grab_breakout(struct Fighter* victim) { (void) victim; }
void mu_tmce_osd_lcancel(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_powershield(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_egg_toss(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_shield_count(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_shield_count_reset(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_dashback(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_cliffwait_frames(struct Fighter* fp) { (void) fp; }
void mu_tmce_osd_galint_aerial(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_galint_laser(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_galint_ledgedash(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_galint_noimpact(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_ledgestall(struct HSD_GObj* gobj) { (void) gobj; }
void mu_tmce_osd_wavedash(struct HSD_GObj* gobj) { (void) gobj; }
int mu_tmce_osd_oo_airborne(int interrupted, struct HSD_GObj* gobj, int which) { (void) gobj; (void) which; return interrupted; }
int mu_tmce_osd_oo_hitstun(int interrupted, struct HSD_GObj* gobj) { (void) gobj; return interrupted; }
int mu_tmce_osd_msid(struct HSD_GObj* gobj) { (void) gobj; return -1; }
void mu_tmce_osd_kneebend_exit(MuTmceExit* e) { (void) e; }
void mu_tmce_osd_guard_iasa_exit(MuTmceExit* e) { (void) e; }
void mu_tmce_osd_damagefall_exit(MuTmceExit* e) { (void) e; }
int mu_tmce_osd_gnw_hammer(int roll) { return roll; }
int mu_tmce_osd_luigi_misfire(int chance) { (void) chance; return 1; }
float mu_tmce_osd_nana_throw(struct Fighter* fp, float roll) { (void) fp; return roll; }
int mu_tmce_osd_peach_fsmash(struct Fighter* fp, int count) { (void) fp; (void) count; return 0; }
int mu_tmce_osd_peach_item(int kind) { return kind; }
int mu_tmce_osd_turnip(int sum) { (void) sum; return 0; }
int mu_tmce_osd_frame_advance(void) { return 0; }
void mu_tmce_osd_frame_counter_init(void) {}
void mu_tmce_osd_frame_counter_inc(void) {}
int mu_tmce_osd_lras_light_press(void) { return 0; }
void mu_tmce_osd_hud_init(int canvas) { (void) canvas; }
void mu_tmce_osd_pause_stick_create(void) {}
void mu_tmce_osd_pause_stick_remove(void) {}
unsigned mu_tmce_osd_enabled(void) { return 0; }
void mu_tmce_osd_set_enabled(unsigned bits) { (void) bits; }
int mu_tmce_osd_position(void) { return 0; }
void mu_tmce_osd_set_position(int position) { (void) position; }
