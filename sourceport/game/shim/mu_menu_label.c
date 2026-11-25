// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <mu_native.h>
#include <sysdolphin/baselib/sislib.h>
#include "mu_menu_label.h"

/* Keep the game's text object, camera, position, box and font parameters.
 * Replace only its words, after copying the actual catalog stream's prefix.
 * The normal HSD_Text destructor owns both allocations, including notices
 * repeated while the user remains on the same row. */
void mu_slippi_menu_description(HSD_Text* text, const char* message)
{
    unsigned char encoded[1024], stream[128 + sizeof encoded];
    SisBuffer* alloc;
    int prefix, n, i, length;
    if (!text || !message || strlen(message) > 100) return;
    prefix = mu_menu_description_prefix(text->sis_buffer);
    if (!prefix) return;
    memcpy(stream, text->sis_buffer, prefix);
    length = HSD_SisLib_803A67EC(encoded, (u8*) message);
    n = prefix;
    /* The editable ASCII encoder inserts its own 0x0a/0x0b spacing overrides.
     * Keep glyphs/spaces only: the row's SIS prefix supplies the kerning. */
    for (i = 0; i < length;) {
        unsigned op = encoded[i];
        int size = op >= 0x20 ? 2 : mu_menu_sis_command_size(op);
        if (op >= 0x20 || op == 0x1a) {
            memcpy(stream + n, encoded + i, size);
            n += size;
        }
        i += size;
    }
    stream[n] = 0;
    alloc = (SisBuffer*) HSD_SisLib_Alloc(sizeof *alloc);
    if (!alloc) return;
    alloc->data = (u8*) HSD_SisLib_Alloc(n + 1);
    if (!alloc->data) { HSD_SisLib_Free(alloc); return; }
    memcpy(alloc->data, stream, n + 1);
    alloc->end = alloc->data + n;
    alloc->size = n + 1;
    alloc->count = 0;
    if (text->alloc_data) {
        HSD_SisLib_Free(text->alloc_data->data);
        HSD_SisLib_Free(text->alloc_data);
    }
    text->alloc_data = alloc;
    text->sis_buffer = alloc->data;
    text->x60 = NULL;
}
