// The Mods menu's text for the game: reply fields, counts, and drawable() (the font's characters).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {

// A reason as the viewer's one line holds it (about 44 characters with "Failed: "): whole, else cut
// after a word with "..." (the log has it whole).
std::string one_line(const std::string& text, size_t room) {
  if (text.size() <= room) return text;
  size_t cut = text.rfind(' ', room - 3);
  if (cut == std::string::npos || cut < room / 2) cut = room - 3;
  return text.substr(0, cut) + "...";
}

void put(std::vector<uint8_t>& out, const std::string& text, size_t width) {
  const size_t start = out.size();
  out.resize(start + width, 0);
  std::memcpy(out.data() + start, text.data(), std::min(text.size(), width - 1));
}
void put_u16(std::vector<uint8_t>& out, int v) { out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v); }
void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(v >> s));
}

std::string thousands(int n) {
  std::string s = std::to_string(std::max(n, 0)), out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i && (s.size() - i) % 3 == 0) out += ',';
    out += s[i];
  }
  return out;
}
// "1 effect", "3,373 mods": a count with its word, one or many.
std::string counted(int n, const char* word) { return thousands(n) + " " + word + (n == 1 ? "" : "s"); }
// The same for a list row's right column (11 characters): "12345 skins" when the comma would not fit,
// else the number alone.
std::string row_counted(int n, const char* word) {
  std::string s = counted(n, word);
  if (s.size() > 11) s = std::to_string(std::max(n, 0)) + " " + word + (n == 1 ? "" : "s");
  if (s.size() > 11) s = thousands(n);
  return s;
}

// A UTF-8 character the font has no glyph for, as the plain one it reads as: a Latin letter with a
// mark as the letter ("Pokémon" as "Pokemon"), curly quotes and long dashes as the straight ones.
// 0: none (left out). `cp`: the code point.
char plain_character(uint32_t cp) {
  // U+00C0..U+00FF; '?' for the two that are not letters (U+00D7 and U+00F7).
  static const char kLatin1[] = "AAAAAAACEEEEIIII" "DNOOOOO?OUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo?ouuuuyty";
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0] == '?' ? 0 : kLatin1[cp - 0xC0];
  if (cp == 0x2018 || cp == 0x2019) return '\'';
  if (cp == 0x201C || cp == 0x201D) return '"';
  if (cp == 0x2013 || cp == 0x2014) return '-';
  return 0;
}

}  // namespace menu_internal
std::string drawable(const std::string& text, size_t max) {
  std::string out;
  for (size_t i = 0; i < text.size() && out.size() < max; ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (std::isalnum(c) || c == '.' || c == ',' || c == '\'' || c == '"' || c == ':' || c == '-') {
      out += (char)c;
    } else if (c == '&') {
      if (!out.empty() && out.back() != ' ') out += ' ';
      out += "and ";
    } else if (c == '!' || c == '?') {
      out += '.';
    } else if (c >= 0x80) {
      // A whole multi-byte character: its plain letter or mark when it has one, else left out (the
      // font has no glyph for it). A broken sequence is left out the same way.
      const size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      uint32_t cp = length == 4 ? c & 0x07u : length == 3 ? c & 0x0Fu : c & 0x1Fu;
      bool whole = length > 1;
      size_t k = 1;
      for (; k < length && i + k < text.size() && ((unsigned char)text[i + k] & 0xC0) == 0x80; ++k)
        cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3Fu);
      whole = whole && k == length;
      while (i + 1 < text.size() && ((unsigned char)text[i + 1] & 0xC0) == 0x80) ++i;
      if (const char plain = whole ? plain_character(cp) : 0) out += plain;
    } else if (!out.empty() && out.back() != ' ') {
      out += ' ';   // spaces, line ends and every other mark
    }
  }
  while (out.size() > max) out.pop_back();
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace host::mods_menu
