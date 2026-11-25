/* m-ex's runtime for code compiled against the native MexTK headers: the C library with the console's
 * int sizes, and the MexTK prototypes that list a game function's arguments in another order.
 *
 * Training Mode CE (tmce/native, upstream src/) and the m-ex fighters (mexft/) call these, so they are built
 * with or without Training Mode CE (MU_TMCE); a fighter plugin imports them from the game library. */
#include "../MexTK/mex.h"

/* ---- the C library, with the console's int sizes (mex_native_prelude.h renames the calls) ---- */
void mex_memcpy(void *dest, const void *source, int size) { __builtin_memcpy(dest, source, (unsigned) size); }
void mex_memmove(void *dest, const void *source, int size) { __builtin_memmove(dest, source, (unsigned) size); }
void mex_memset(void *dest, int c, int size) { __builtin_memset(dest, c, (unsigned) size); }
int mex_strncmp(const char *a, const char *b, int n) { return __builtin_strncmp(a, b, (unsigned) n); }
int mex_strcpy(const char *dest, const char *source)
{
    __builtin_strcpy((char *) dest, source);
    return 0;
}
int mex_strlen(const char *s) { return (int) __builtin_strlen(s); }
int mu_crt_vsprintf(char *, const char *, va_list) __asm__("vsprintf");   /* MexTK's vsprintf is a macro */
int mex__vsprintf(char *str, int unk, const char *format, va_list arg)
{
    (void) unk;
    return mu_crt_vsprintf(str, format, arg);
}
/* MexTK's blr/blr2: a return instruction in the game, used as an empty callback */
void mex_blr(void) {}

/* m-ex's calloc: the current heap, cleared */
void *mex_calloc(int size)
{
    void *p = HSD_MemAlloc(size);
    if (p != 0)
        __builtin_memset(p, 0, (unsigned) size);
    return p;
}

/* ---- MexTK prototypes that list a function's arguments in another order than the game ----
 * On the console integer and float arguments go in separate register files, so these worked there;
 * natively each argument has one slot, so they call the game's function in its own order
 * (tools/tmce/check_signatures.py finds them; tools/tmce/function_overrides.json routes the calls). */
void mu_g_Fighter_ChangeMotionState(GOBJ *, int, int, float, float, float, GOBJ *) __asm__("Fighter_ChangeMotionState");
void mex_ActionStateChange(float start_frame, float anim_speed, float anim_blend, GOBJ *fighter, int state_id,
                           int flags, GOBJ *alt_state_source)
{
    mu_g_Fighter_ChangeMotionState(fighter, state_id, flags, start_frame, anim_speed, anim_blend, alt_state_source);
}
int mu_g_mpCheckFloor(float, float, float, float, float, Vec3 *, int *, u32 *, Vec3 *, int, int, int, void *, GOBJ *)
    __asm__("mpCheckFloor");
int mex_GrColl_RaycastGround(Vec3 *coll_pos, int *line_index, int *line_kind, Vec3 *unk1, int unk2, int unk3, int unk4,
                             void *cb, float from_x, float from_y, float to_x, float to_y, float unk5)
{
    return mu_g_mpCheckFloor(from_x, from_y, to_x, to_y, unk5, coll_pos, line_index, (u32 *) line_kind, unk1, unk2,
                             unk3, unk4, cb, 0);
}
void mu_g_lbColl_80008FC8(Vec3, Vec3, GXColor *, GXColor *, float) __asm__("lbColl_80008FC8");
void mex_Develop_DrawSphere(float size, Vec3 *pos1, Vec3 *pos2, GXColor *diffuse, GXColor *ambient)
{
    mu_g_lbColl_80008FC8(*pos1, *pos2, diffuse, ambient, size);
}
void mu_g_DevText_SetBGColor(DevText *, GXColor) __asm__("DevText_SetBGColor");
void mex_DevelopText_StoreBGColor(DevText *text, GXColor *rgba)
{
    mu_g_DevText_SetBGColor(text, *rgba);
}
/* the game passes the knockback magnitude; MexTK leaves that argument out (a stale register on the
 * console), so the fighter's own current knockback magnitude is passed */
float mu_g_ftCo_Damage_CalcAngle(FighterData *, float) __asm__("ftCo_Damage_CalcAngle");
float mex_Fighter_GetKnockbackAngle(FighterData *fighter_data)
{
    return mu_g_ftCo_Damage_CalcAngle(fighter_data, fighter_data->dmg.kb_mag);
}
