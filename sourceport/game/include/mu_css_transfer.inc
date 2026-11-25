/* Live pick previews (shim/mu_slippi_css.c): door `door`'s pick as character select shows it now,
 * read only. *ck: the fighter under the coin while the coin is down, else 0xFF (carried, or no
 * fighter). *color: the costume the door's portrait shows while it shows that fighter (X/Y change
 * it at once; players[].color only follows later), else players[port].color. */
void mu_css_pick_preview(int door, int port, unsigned char* ck, unsigned char* color)
{
    PlayerInitData* p;
    CSSDoor* d;
    int held;
    *ck = 0xFF;
    *color = 0;
    if (mnCharSel_804D6CB0 == NULL || door < 0 || door > 3 || port < 0 || port > 3) return;
    p = &mnCharSel_804D6CB0->vs.start.players[port];
    d = &mnCharSel_803F0DFC.doors[door];
    held = mnCharSel_804A0BD0[door] != NULL && mnCharSel_804A0BD0[door]->x5 != 0;
    *color = p->color;
    if (p->ckind >= ChKind_None) return;
    if (d->sel_icon < CSS_NONE && icons[d->sel_icon].char_kind == p->ckind) *color = d->costume;
    if (!held) *ck = (unsigned char) p->ckind;
}
