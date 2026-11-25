/* The Mods menu's list and viewer: their requests to the host (MU_CMD_MOD_BROWSER 9 to 14; the host's
 * side is port/runtime/host/mods_menu.cpp), which answers at once and never blocks. The subs are named
 * in mu_mods_gallery_internal.h; each one's bytes are written where it is asked.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <mu_mods_gallery.h>
#include <mu_commands.h>

#include "mu_mods_gallery_internal.h"

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
enum { MAX_SEARCH = 24 };      /* the words of a search at most (the host keeps as many) */

unsigned char mu_mods_reply[REPLY_MIN];

int mu_mods_ask(const unsigned char* payload, unsigned int n, unsigned char* out, unsigned int capacity,
                unsigned int* got)
{
    *got = 0;
    return mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, n, out, capacity, got) == 0;
}

int mu_mods_u16_at(const unsigned char* p) { return p[0] << 8 | p[1]; }
unsigned int mu_mods_u32_at(const unsigned char* p)
{
    return (unsigned int) p[0] << 24 | (unsigned int) p[1] << 16 | (unsigned int) p[2] << 8 | p[3];
}

int mu_mods_host_change_row(int op, int row)
{
    unsigned char p[4];
    unsigned int got;
    p[0] = SUB_CHANGE;
    p[1] = (unsigned char) op;
    p[2] = (unsigned char) (row >> 8);
    p[3] = (unsigned char) row;
    return mu_mods_ask(p, row < 0 ? 2 : 4, mu_mods_reply, sizeof mu_mods_reply, &got) && got >= 1 && mu_mods_reply[0] == 1;
}

int mu_mods_host_change(int op) { return mu_mods_host_change_row(op, -1); }

/* Browse's search: the host shows the posts with `words` from its first page; "" ends the search. */
int mu_mods_gallery_search(const char* words)
{
    unsigned char p[2 + MAX_SEARCH];
    unsigned int got, n = 0;
    p[0] = SUB_CHANGE;
    p[1] = CHANGE_SEARCH;
    while (words != NULL && words[n] != 0 && n < MAX_SEARCH) {
        p[2 + n] = (unsigned char) mu_mods_drawable(words[n]);
        n++;
    }
    return mu_mods_ask(p, 2 + n, mu_mods_reply, sizeof mu_mods_reply, &got) && got >= 1 && mu_mods_reply[0] == 1;
}
