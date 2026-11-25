/* The Mods menu's words, as the game's text objects get them: every character mapped to one the
 * font draws (drawable), lines written into the text's own stream (set_words), broken at spaces
 * (wrap), cut in the middle (middle_cut), and the viewer's title fitted to its box (fit_name).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <sysdolphin/baselib/sislib.h>

#include "mu_menu_label.h"
#include "mu_mods_gallery_internal.h"

/* One character the font draws, or a space (the encoder reads any other byte as half of a
 * Shift-JIS pair). */
char mu_mods_drawable(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return c;
    switch (c) {
    case ' ': case '.': case ',': case '\'': case '"': case ':': case '-': return c;
    default: return ' ';
    }
}

/* A fixed-width field of the host's reply as a string of drawable characters; a line end stays (the
 * host sends one only where a line must end: an effect's notice, one line per effect it overrides). */
void mu_mods_field(char* out, const unsigned char* in, int width)
{
    int i;
    for (i = 0; i < width - 1 && in[i] != 0; i++) out[i] = in[i] == '\n' ? '\n' : mu_mods_drawable((char) in[i]);
    out[i] = 0;
}

/* ---- words into the game's text objects ---- */

static int prefix_of(const unsigned char* stream)
{
    int n = 0;
    if (stream == NULL) return 0;
    while (n < 128) {
        const unsigned op = stream[n];
        if (op >= 0x20 || op == 0x1a) return n;
        if (op == 0 || op == 8 || op == 9) return 0;
        n += mu_menu_sis_command_size(op);
    }
    return 0;
}

/* `message` (lines split at '\n', each read up to 100 characters) in place of the text's words,
 * after the formatting its stream starts with (colour, scale, alignment). */
void mu_mods_set_words(HSD_Text* text, const char* message)
{
    static unsigned char stream[3072];
    unsigned char encoded[1024];
    char line[104];
    const char* p = message;
    SisBuffer* alloc;
    int prefix, n, i, length;
    if (text == NULL || message == NULL) return;
    prefix = prefix_of(text->sis_buffer);
    memcpy(stream, text->sis_buffer, prefix);
    n = prefix;
    for (;;) {
        int k = 0;
        while (*p && *p != '\n' && k < 100) line[k++] = mu_mods_drawable(*p++);
        line[k] = 0;
        while (*p && *p != '\n') p++;
        if (k > 0) {
            length = HSD_SisLib_803A67EC(encoded, (u8*) line);
            for (i = 0; i < length;) {
                const unsigned op = encoded[i];
                const int size = op >= 0x20 ? 2 : mu_menu_sis_command_size(op);
                if ((op >= 0x20 || op == 0x1a) && n + size < (int) sizeof stream - 2) {
                    memcpy(stream + n, encoded + i, size);
                    n += size;
                }
                i += size;
            }
        }
        if (*p != '\n') break;
        p++;
        if (n < (int) sizeof stream - 2) stream[n++] = 0x03;   /* a new line */
    }
    stream[n] = 0;
    alloc = (SisBuffer*) HSD_SisLib_Alloc(sizeof *alloc);
    if (alloc == NULL) return;
    alloc->data = (u8*) HSD_SisLib_Alloc(n + 1);
    if (alloc->data == NULL) {
        HSD_SisLib_Free(alloc);
        return;
    }
    memcpy(alloc->data, stream, n + 1);
    alloc->end = alloc->data + n;
    alloc->size = n + 1;
    alloc->count = 0;
    if (text->alloc_data != NULL) {
        HSD_SisLib_Free(text->alloc_data->data);
        HSD_SisLib_Free(text->alloc_data);
    }
    text->alloc_data = alloc;
    text->sis_buffer = alloc->data;
    text->x60 = NULL;
}

/* `in` broken into lines of at most `width` characters at spaces (and where it has a line end), at
 * most `max_lines` of them, after `out`'s first `at` bytes. */
int mu_mods_wrap(char* out, int at, int cap, const char* in, int width, int max_lines)
{
    int lines = 0;
    while (*in == ' ') in++;
    while (*in && lines < max_lines && at < cap - 2) {
        int take = 0, cut, i;
        while (in[take] && in[take] != '\n' && take < width) take++;
        cut = take;
        if (in[take] && in[take] != '\n') {
            while (cut > 0 && in[cut] != ' ') cut--;
            if (cut == 0) cut = take;
        }
        if (lines + 1 == max_lines && in[cut]) {
            /* the last line that fits: what is left ends in "..." */
            if (cut > width - 3) cut = width - 3;
            for (i = 0; i < cut && at < cap - 5; i++) out[at++] = in[i];
            out[at++] = '.'; out[at++] = '.'; out[at++] = '.';
            lines++;
            break;
        }
        for (i = 0; i < cut && at < cap - 2; i++) out[at++] = in[i];
        in += cut;
        if (*in == '\n') in++;
        while (*in == ' ') in++;
        lines++;
        if (*in && lines < max_lines) out[at++] = '\n';
    }
    out[at] = 0;
    return at;
}

/* `in` in at most `room` characters: whole, or its start and its end around "...", so names that
 * begin alike (a pack's "Monochrome Animelee ...") still read apart by how they end. */
void mu_mods_middle_cut(char* out, const char* in, int room)
{
    const int n = (int) strlen(in);
    int start, end, at;
    if (n <= room) {
        strcpy(out, in);
        return;
    }
    end = (room - 3) * 2 / 5;
    start = room - 3 - end;
    at = start;
    memcpy(out, in, start);
    while (at > 0 && out[at - 1] == ' ') at--;
    memcpy(out + at, "...", 3);
    at += 3;
    in += n - end;
    while (*in == ' ') in++;
    strcpy(out + at, in);
}

/* The viewer's name: the title at the game's size while it stays legible (the text is drawn narrower to
 * fit its box: at most NAME_ONE_LINE characters, about three quarters of its width); a longer one on two
 * lines at a smaller size, split at the space nearest its middle, each line cut in the middle past
 * NAME_TWO_LINES. */
enum { NAME_ONE_LINE = 26, NAME_TWO_LINES = 32 };
#define NAME_SIZE_X 0.044f
#define NAME_SIZE_Y 0.103f
#define NAME_SMALL_X 0.034f
#define NAME_SMALL_Y 0.056f
void mu_mods_fit_name(HSD_Text* text, const char* title)
{
    char both[2 * NAME_TWO_LINES + 8], first[48], second[48], cut[NAME_TWO_LINES + 4];
    const int n = (int) strlen(title);
    int split = -1, i;
    if (text == NULL) return;
    if (n <= NAME_ONE_LINE) {
        text->font_size.x = NAME_SIZE_X;
        text->font_size.y = NAME_SIZE_Y;
        mu_mods_set_words(text, title);
        return;
    }
    for (i = 0; i < n; i++) {
        const int d = i > n / 2 ? i - n / 2 : n / 2 - i;
        if (title[i] == ' ' && (split < 0 || d < (split > n / 2 ? split - n / 2 : n / 2 - split))) split = i;
    }
    if (split < 0) split = n / 2;
    memcpy(first, title, split);
    first[split] = 0;
    strcpy(second, title + split + (title[split] == ' ' ? 1 : 0));
    mu_mods_middle_cut(cut, first, NAME_TWO_LINES);
    strcpy(both, cut);
    strcat(both, "\n");
    mu_mods_middle_cut(cut, second, NAME_TWO_LINES);
    strcat(both, cut);
    text->font_size.x = NAME_SMALL_X;
    text->font_size.y = NAME_SMALL_Y;
    mu_mods_set_words(text, both);
}
