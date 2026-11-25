/* Another player's code or a lobby's code as melee_port.log writes it (the game library's side of
 * host::public_build::log_id, port/runtime/host/public_build.h). A development build writes the text
 * itself; a public build (NUCLEUS_PUBLIC_BUILD) writes "id-" and six hex digits of the same FNV-1a
 * hash, so the host's lines and the game's name one code the same way.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_LOG_ID_H
#define MU_LOG_ID_H

/* `out` holds at least 10 bytes; the text is returned unchanged when there is nothing to hide. */
static inline const char* mu_log_id(const char* text, char* out)
{
#ifdef NUCLEUS_PUBLIC_BUILD
    static const char hex[] = "0123456789abcdef";
    const unsigned char* p = (const unsigned char*) text;
    unsigned int h = 2166136261u;
    int i;
    if (text == 0 || *text == 0) {
        return text;
    }
    while (*p != 0) {
        h ^= *p++;
        h *= 16777619u;
    }
    out[0] = 'i';
    out[1] = 'd';
    out[2] = '-';
    for (i = 0; i < 6; i++) {
        out[3 + i] = hex[(h >> (20 - 4 * i)) & 15u];
    }
    out[9] = 0;
    return out;
#else
    (void) out;
    return text;
#endif
}

#endif
