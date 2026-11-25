// Cosmetic portraits and stock icons: names, install, the select screen's pictures read from a
// disc, a skin's own pictures, the game's standard ones.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

// "PlFxGr.dat" from "PlFxGr.dat#portrait"
std::string portrait_slot(const std::string& target) {
  const size_t at = target.rfind(kPortraitSuffix);
  return at == std::string::npos ? target : target.substr(0, at);
}

// Whether a picture's name says it is a stock icon: "stock" in it, or the SSBM Nucleus exports'
// "stc.png" / "<costume>_stc.png" (the site's "Add to Nucleus" zips and the Vault's).
bool stock_picture_name(const std::string& raw) {
  std::string name = lower(raw);
  if (name.find("stock") != std::string::npos) return true;
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name.erase(0, slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) name.resize(dot);
  return name == "stc" || (name.size() > 4 && name.compare(name.size() - 4, 4, "_stc") == 0);
}

// The slot a picture's name identifies. First the costume's own file code ("plfxgr"), which is
// what packs made beside a skin use; otherwise one fighter name and one color word. A name that
// fits two costumes, or a color the fighter does not have, identifies nothing.
bool portrait_slot_from_name_impl(const std::string& raw, std::string* slot, std::string* kind) {
  std::string name = lower(raw);
  const size_t dot = name.find_last_of('.');
  const size_t slash = name.find_last_of("/\\");
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) name.resize(dot);
  auto word_at = [&](size_t at, size_t length) {
    return (at == 0 || !std::isalnum((unsigned char)name[at - 1])) &&
           (at + length >= name.size() || !std::isalnum((unsigned char)name[at + length]));
  };
  *kind = stock_picture_name(raw) ? "stock" : "csp";
  std::vector<SlotName> found;
  auto add = [&](const SlotName& candidate) {
    for (const auto& have : found)
      if (have.family == candidate.family && have.color == candidate.color) return;
    found.push_back(candidate);
  };
  for (size_t at = name.find("pl"); at != std::string::npos; at = name.find("pl", at + 1)) {
    if (at + 6 > name.size() || !word_at(at, 6)) continue;
    for (const auto& family : families) {
      if (lower(family.file_code) != name.substr(at + 2, 2)) continue;
      for (const auto& color : color_names)
        if (lower(color.code) == name.substr(at + 4, 2) && family_has_color(family, color.code))
          add({&family, &color});
    }
  }
  if (found.size() == 1) { *slot = slot_file(found[0]); return true; }
  if (found.size() > 1) return false;

  struct Alias { const char* text; const char* code; };
  // Longest first: a match is blanked out, so "falcon" never also reads as "falco", nor "dr mario"
  // as "mario".
  static constexpr Alias fighters[] = {
      {"mr. game & watch", "Gw"}, {"mr game and watch", "Gw"}, {"game and watch", "Gw"}, {"game & watch", "Gw"},
      {"captain falcon", "Ca"}, {"ice climbers", "Pp"}, {"iceclimbers", "Pp"}, {"donkey kong", "Dk"},
      {"young link", "Cl"}, {"gameandwatch", "Gw"}, {"jigglypuff", "Pr"}, {"younglink", "Cl"},
      {"ganondorf", "Gn"}, {"gamewatch", "Gw"}, {"dr. mario", "Dr"}, {"dr mario", "Dr"}, {"c.falcon", "Ca"},
      {"cfalcon", "Ca"}, {"drmario", "Dr"}, {"pikachu", "Pk"}, {"captain", "Ca"}, {"bowser", "Kp"},
      {"falcon", "Ca"}, {"mewtwo", "Mt"}, {"donkey", "Dk"}, {"jiggly", "Pr"}, {"ylink", "Cl"}, {"clink", "Cl"},
      {"ganon", "Gn"}, {"kirby", "Kb"}, {"luigi", "Lg"}, {"mario", "Mr"}, {"marth", "Ms"}, {"pichu", "Pc"},
      {"peach", "Pe"}, {"purin", "Pr"}, {"sheik", "Sk"}, {"samus", "Ss"}, {"yoshi", "Ys"}, {"zelda", "Zd"},
      {"falco", "Fc"}, {"koopa", "Kp"}, {"link", "Lk"}, {"ness", "Ns"}, {"nana", "Nn"}, {"popo", "Pp"},
      {"puff", "Pr"}, {"g&w", "Gw"}, {"gnw", "Gw"}, {"roy", "Fe"}, {"fox", "Fx"}, {"doc", "Dr"}, {"ics", "Pp"},
      {"dk", "Dk"},
  };
  static constexpr Alias colors[] = {
      {"lavender", "La"}, {"default", "Nr"}, {"neutral", "Nr"}, {"original", "Nr"}, {"vanilla", "Nr"},
      {"normal", "Nr"}, {"orange", "Or"}, {"yellow", "Ye"}, {"purple", "La"}, {"black", "Bk"}, {"white", "Wh"},
      {"green", "Gr"}, {"blue", "Bu"}, {"pink", "Pi"}, {"aqua", "Aq"}, {"cyan", "Aq"}, {"gray", "Gy"},
      {"grey", "Gy"}, {"red", "Re"},
  };
  auto scan = [&](const Alias* aliases, size_t count, std::vector<std::string>* codes) {
    for (size_t i = 0; i < count; ++i) {
      const std::string text = aliases[i].text;
      for (size_t at = name.find(text); at != std::string::npos; at = name.find(text, at + 1)) {
        if (!word_at(at, text.size())) continue;
        if (std::find(codes->begin(), codes->end(), aliases[i].code) == codes->end()) codes->push_back(aliases[i].code);
        name.replace(at, text.size(), text.size(), ' ');
      }
    }
  };
  std::vector<std::string> fighter_codes, color_codes;
  scan(fighters, std::size(fighters), &fighter_codes);
  scan(colors, std::size(colors), &color_codes);
  if (fighter_codes.size() != 1 || color_codes.size() != 1) return false;
  for (const auto& family : families) {
    if (fighter_codes[0] != family.file_code) continue;
    for (const auto& color : color_names)
      if (color_codes[0] == color.code && family_has_color(family, color.code)) {
        *slot = slot_file({&family, &color}); return true;
      }
  }
  return false;
}

// One entry per costume slot holds its portrait and its stock icon. Setting a picture again
// replaces that picture and keeps the other.
ImportResult install_portrait_locked(const std::string& slot_target, const std::string& kind,
                                     const std::vector<uint8_t>& png, const std::string& source_name) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  SlotName slot;
  if (!find_slot(slot_target, &slot)) {
    result.message = "That is not a costume of the game: " + slot_target; return result;
  }
  if (kind != "csp" && kind != "stock") { result.message = "A picture is a portrait or a stock icon."; return result; }
  if (!png_companion(png, &error)) { result.message = error; return result; }
  const std::string digest = sha256(png);
  if (digest.empty()) { result.message = "The picture could not be hashed."; return result; }
  const std::string file = slot_file(slot);
  const std::string id = "portrait-" + file.substr(0, file.size() - 4);
  const std::string target = file + kPortraitSuffix;
  AssetRecord::Companion picture;
  picture.kind = kind;
  picture.stored_path = (fs::path(L"assets") / fs::u8path(id) / L"companions" /
                         (kind == "csp" ? L"csp.png" : L"stock.png")).generic_u8string();
  picture.sha256 = digest;
  picture.source_member = source_name;

  auto existing = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) { return a.info.id == id; });
  const bool had = existing != g_assets.end();
  AssetRecord asset = had ? *existing : AssetRecord{};
  bool same_picture = false;
  for (const auto& companion : asset.companions) same_picture |= companion.kind == kind && companion.sha256 == digest;
  asset.companions.erase(std::remove_if(asset.companions.begin(), asset.companions.end(),
      [&](const AssetRecord::Companion& c) { return c.kind == kind; }), asset.companions.end());
  asset.companions.push_back(picture);
  std::stable_sort(asset.companions.begin(), asset.companions.end(),
      [](const AssetRecord::Companion& a, const AssetRecord::Companion& b) { return a.kind == "csp" && b.kind != "csp"; });
  asset.info.id = id;
  asset.info.kind = kPortraitKind;
  asset.info.target_path = target;
  asset.info.character = slot.family->display_name;
  asset.info.costume = slot.color->label;
  if (asset.info.name.empty()) asset.info.name = std::string(slot.family->display_name) + ", " + slot.color->label + " pictures";
  asset.info.available = true;
  asset.info.availability_message.clear();
  asset.info.roots.clear();
  asset.info.unsupported_companions.clear();
  for (const auto& companion : asset.companions)
    asset.info.unsupported_companions.push_back(std::string(companion.kind == "csp" ? "Portrait: " : "Stock icon: ") +
                                               companion.source_member);
  // The entry's own file is its first picture (the portrait when it has one): what the catalog
  // checks when it lists the entry.
  asset.stored_path = asset.companions.front().stored_path;
  asset.info.sha256 = asset.companions.front().sha256;
  asset.source_kind = "png";
  asset.source_name = source_name;

  if (!write_atomic(g_root / fs::u8path(picture.stored_path), png.data(), png.size(), &error)) {
    result.message = error; return result;
  }
  const AssetRecord previous = had ? *existing : AssetRecord{};
  if (had) *existing = asset; else g_assets.push_back(asset);
  if (!save_catalog_locked(&error)) {
    if (had) *std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) { return a.info.id == id; }) = previous;
    else g_assets.pop_back();
    result.message = error; return result;
  }
  const auto current = g_profile.selections.find(target);
  if (current == g_profile.selections.end() || current->second != id) {
    Profile before = g_profile;
    g_profile.selections[target] = id;
    ++g_profile.generation;
    if (!save_profile_locked(&error)) {
      g_profile = std::move(before);
      result.message = "The picture was saved, but its selection could not be: " + error; return result;
    }
  } else if (!same_picture) {
    ++g_profile.generation;   // the same entry, a different picture
    save_profile_locked(&error);
  }
  g_message = std::string(kind == "csp" ? "Portrait" : "Stock icon") + " for " + slot.family->display_name + ", " +
              slot.color->label + " set from " + source_name + ".";
  result.ok = true; result.already_present = same_picture; result.asset_id = id; result.message = g_message;
  return result;
}

// ---- a pack's own portraits ----
// The character select file keeps every portrait in one texture animation: an image table, a
// palette table, and a track of keys that says which table entry each frame shows. The game shows
// a costume's portrait by setting that animation to frame "fighter column + 30 * costume"
// (mn/mncharsel.c), so the keys, not the order of the table, name the picture of a slot. This is
// the reading tools/companion_texture_diagnostic.py does to make companion_texture_map.h.
constexpr uint32_t kPortraitWidth = 136, kPortraitHeight = 188, kPortraitFormat = 9;   // 8 bit palette indices
struct SelectPortrait {
  uint32_t pixels = 0, colors = 0, palette_format = 0, palette_entries = 0;
  uint32_t width = kPortraitWidth, height = kPortraitHeight, format = kPortraitFormat;
};

// Frame -> value of one animation track that holds whole-number keys. `block` is the archive's data
// block; every read stays inside it.
bool texture_track_keys(const uint8_t* block, size_t size, uint32_t track, std::map<int, int>* keys) {
  keys->clear();
  if ((uint64_t)track + 0x14 > size) return false;
  const uint32_t length = be32(block + track + 4);
  const uint8_t value_format = block[track + 0xD], slope_format = block[track + 0xE];
  size_t at = be32(block + track + 0x10);
  if (at > size || length > size - at) return false;
  const size_t end = at + length;
  static constexpr uint32_t sizes[] = {4, 2, 2, 1, 1};   // float, s16, u16, s8, u8
  const uint32_t kind = value_format >> 5, slope_kind = slope_format >> 5;
  if (kind > 4) return false;
  const double scale = (double)(1u << (value_format & 31));
  uint64_t frame = 0;
  while (at < end) {
    uint8_t byte = block[at++];
    const uint32_t opcode = byte & 0xF;
    if (opcode < 1 || opcode > 6) return false;
    uint32_t count = (byte >> 4) & 7, shift = 3;
    while (byte & 0x80) {
      if (at >= end || shift > 24) return false;
      byte = block[at++];
      count |= (uint32_t)(byte & 0x7F) << shift; shift += 7;
    }
    for (uint32_t i = 0; i <= count; ++i) {
      bool has_value = false;
      double value = 0;
      if (opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 6) {
        if (end - at < sizes[kind]) return false;
        const uint8_t* raw = block + at;
        at += sizes[kind];
        switch (kind) {
          case 0: { const uint32_t bits = le32(raw); float real = 0; std::memcpy(&real, &bits, 4); value = real; break; }
          case 1: value = (int16_t)le16(raw) / scale; break;
          case 2: value = le16(raw) / scale; break;
          case 3: value = (int8_t)raw[0] / scale; break;
          default: value = raw[0] / scale; break;
        }
        has_value = true;
      }
      if (opcode == 4 || opcode == 5) {
        if (slope_kind > 4 || end - at < sizes[slope_kind]) return false;
        at += sizes[slope_kind];
      }
      uint32_t wait = 0, wait_shift = 0;
      for (;;) {
        if (at >= end || wait_shift > 28) return false;
        byte = block[at++];
        wait |= (uint32_t)(byte & 0x7F) << wait_shift; wait_shift += 7;
        if (!(byte & 0x80)) break;
      }
      if (has_value) {
        if (!std::isfinite(value) || value < 0 || value > 65535 || frame > 65535 || keys->size() >= 4096) return false;
        (*keys)[(int)frame] = (int)value;
      }
      frame += wait;
    }
  }
  return !keys->empty();
}

// Frame -> portrait of a character select file, from the first texture animation whose tables hold
// portraits. False when the file has no such animation (not a character select file, or one built
// another way): then no portrait is taken from it.
bool texture_animation_frames(const std::vector<uint8_t>& dat, uint32_t mex_animation, std::map<int, SelectPortrait>* frames,
                              uint32_t want_w = kPortraitWidth, uint32_t want_h = kPortraitHeight);

bool select_portraits(const std::vector<uint8_t>& dat, std::map<int, SelectPortrait>* frames, int* mex_stride = nullptr) {
  frames->clear();
  if (mex_stride) *mex_stride = 0;
  disc_metadata::Archive archive(dat);
  if (!archive.parse()) return false;
  uint32_t mex_root = 0, mex_animation = UINT32_MAX;
  int stride = 0;
  if (archive.root("mexSelectChr", &mex_root)) {
    uint32_t material;
    if (!archive.bounds(mex_root, 20) || !archive.pointer(mex_root + 12, &material) ||
        !archive.bounds(material, 12) || !archive.pointer(material + 8, &mex_animation)) return false;
    stride = (int)archive.word(mex_root + 16);
    if (stride < 26 || stride > 512) return false;
  }
  if (!texture_animation_frames(dat, mex_animation, frames)) return false;
  if (mex_stride) *mex_stride = stride;
  return true;
}

// The m-ex stock icons of a HUD file (IfAll): "Stc_icns" = {reserved frames, frames per costume row,
// material animation joint}. A fighter's icon is frame reserved + costume * stride + m-ex internal id.
bool mex_stock_icons(const std::vector<uint8_t>& dat, std::map<int, SelectPortrait>* frames, int* reserved, int* stride) {
  frames->clear();
  disc_metadata::Archive archive(dat);
  uint32_t root = 0, joint = 0, material = 0, animation = 0;
  if (!archive.parse() || !archive.root("Stc_icns", &root) || !archive.bounds(root, 8) ||
      !archive.pointer(root + 4, &joint) || !archive.bounds(joint, 12) || !archive.pointer(joint + 8, &material) ||
      !archive.bounds(material, 12) || !archive.pointer(material + 8, &animation)) return false;
  const uint32_t head = archive.word(root);
  *reserved = (int)(head >> 16); *stride = (int)(head & 0xFFFF);
  if (*stride < 26 || *stride > 512) return false;
  return texture_animation_frames(dat, animation, frames);
}

// Frame -> picture of one texture animation: the one at mex_animation, or with UINT32_MAX the first whose
// tables hold retail character select portraits.
bool texture_animation_frames(const std::vector<uint8_t>& dat, uint32_t mex_animation, std::map<int, SelectPortrait>* frames,
                              uint32_t want_w, uint32_t want_h) {
  frames->clear();
  const int stride = mex_animation != UINT32_MAX;   // a named animation: any picture size
  const uint64_t data_size = be32(dat.data() + 4), relocations = be32(dat.data() + 8);
  if (0x20ull + data_size + relocations * 4ull > dat.size()) return false;
  const uint8_t* block = dat.data() + 0x20;
  std::vector<uint32_t> offsets((size_t)relocations);
  for (size_t i = 0; i < offsets.size(); ++i) offsets[i] = be32(block + data_size + i * 4);
  std::vector<uint32_t> sorted = offsets;
  std::sort(sorted.begin(), sorted.end());
  auto relocated = [&](uint32_t at) { return std::binary_search(sorted.begin(), sorted.end(), at); };
  auto word = [&](uint64_t at) { return be32(block + at); };
  for (const uint32_t pointer : offsets) {
    // pointer is the image table field of a texture animation:
    // {next, id, animation, image table, palette table, images (16 bit), palettes (16 bit)}
    if (pointer < 0xC || (uint64_t)pointer + 12 > data_size) continue;
    const uint32_t anim = pointer - 0xC;
    if (stride && anim != mex_animation) continue;  // the named CSP animation, never an unrelated menu texture
    if (!relocated(pointer + 4) || !relocated(anim + 8)) continue;
    const uint32_t images = word(pointer), palettes = word(pointer + 4);
    const uint32_t image_count = be16(block + pointer + 8), palette_count = be16(block + pointer + 10);
    if (image_count < (stride ? 1u : 20u) || image_count > 4096 || !palette_count || palette_count > 4096) continue;
    if ((uint64_t)images + 4ull * image_count > data_size || (uint64_t)palettes + 4ull * palette_count > data_size) continue;
    std::vector<SelectPortrait> entries(image_count);
    std::vector<bool> valid(image_count, false);
    size_t good = 0;
    bool broken = false;
    for (uint32_t i = 0; i < image_count; ++i) {
      const uint32_t image = word(images + 4ull * i);
      if ((uint64_t)image + 24 > data_size) { broken = true; break; }
      SelectPortrait entry;
      entry.pixels = word(image);
      entry.width = be16(block + image + 4); entry.height = be16(block + image + 6); entry.format = word(image + 8);
      if (!entry.width || !entry.height || entry.width > 1024 || entry.height > 1024 ||
          (!stride && (entry.width != want_w || entry.height != want_h)) ||
          (entry.format != 8 && entry.format != 9) ||
          (uint64_t)entry.pixels + gx_image_size(entry.width, entry.height, entry.format, 0, 0) > data_size) continue;
      entries[i] = entry; valid[i] = true; ++good;
    }
    if (broken || good * 5 < (size_t)image_count * 4) continue;
    // animation -> its first track; the image index track is type 1
    const uint32_t animation = word(anim + 8);
    if ((uint64_t)animation + 12 > data_size) continue;
    std::map<int, int> keys, palette_keys;
    bool have_keys = false, have_palette_keys = false, bad_track = false;
    uint32_t track = word(animation + 8);
    for (int guard = 0; track && guard < 64; ++guard) {
      if ((uint64_t)track + 0x14 > data_size) { have_keys = false; break; }
      if (block[track + 0xC] == 1) {
        have_keys = texture_track_keys(block, (size_t)data_size, track, &keys);
        bad_track |= !have_keys;
      }
      if (block[track + 0xC] == 2) {
        have_palette_keys = texture_track_keys(block, (size_t)data_size, track, &palette_keys);
        bad_track |= !have_palette_keys;
      }
      track = word(track);
    }
    if (!have_keys || bad_track || track) continue;
    bool in_table = true;
    for (const auto& key : keys) in_table &= key.second >= 0 && (uint32_t)key.second < image_count;
    if (!in_table) continue;
    for (const auto& key : palette_keys) in_table &= key.second >= 0 && (uint32_t)key.second < palette_count;
    if (!in_table || (!have_palette_keys && palette_count != 1 && palette_count != image_count)) continue;
    std::set<int> keyframes;
    for (const auto& key : keys) keyframes.insert(key.first);
    for (const auto& key : palette_keys) keyframes.insert(key.first);
    for (const int frame : keyframes) {
      auto image_key = keys.upper_bound(frame);
      if (image_key == keys.begin()) continue;
      const int image_index = std::prev(image_key)->second;
      if (!valid[(size_t)image_index]) continue;
      int palette_index = palette_count == 1 ? 0 : image_index;
      if (have_palette_keys) {
        auto palette_key = palette_keys.upper_bound(frame);
        if (palette_key == palette_keys.begin()) continue;
        palette_index = std::prev(palette_key)->second;
      }
      const uint32_t palette = word(palettes + 4ull * palette_index);
      if ((uint64_t)palette + 14 > data_size) continue;
      SelectPortrait entry = entries[(size_t)image_index];
      entry.colors = word(palette); entry.palette_format = word(palette + 4);
      entry.palette_entries = be16(block + palette + 12);
      if (entry.palette_format > 2 || !entry.palette_entries || entry.palette_entries > (entry.format == 8 ? 16u : 256u) ||
          (uint64_t)entry.colors + 2ull * entry.palette_entries > data_size) continue;
      (*frames)[frame] = entry;
    }
    return !frames->empty();
  }
  return false;
}

// The frame that shows a costume slot's portrait, or -1 for a slot with no cell of its own: Nana
// (the Ice Climbers' cell is Popo's), Sheik (Zelda's cell), and Mr. Game & Watch (four cells, one
// costume file).
int portrait_frame(const SlotName& slot) {
  static constexpr const char* columns[] = {"Ca", "Dk", "Fx", "Gw", "Kb", "Kp", "Lk", "Lg", "Mr", "Ms", "Mt", "Ns", "Pe",
                                            "Pk", "Pp", "Pr", "Ss", "Ys", "Zd", "Fc", "Cl", "Dr", "Fe", "Pc", "Gn"};
  const std::string code = slot.family->file_code;
  if (code == "Gw") return -1;
  const std::string colors = slot.family->colors;   // the game's costume order
  const size_t at = (std::string(" ") + colors).find(std::string(" ") + slot.color->code);
  if (at == std::string::npos) return -1;
  for (size_t column = 0; column < std::size(columns); ++column)
    if (code == columns[column]) return (int)column + (int)(at / 3) * 30;
  return -1;
}

// One portrait as 8 bit RGBA, top row first. The image is rows of 8x4 texel blocks of palette
// indices; the palette is 16 bit colors in one of the console's three palette formats.
std::vector<uint8_t> decode_portrait(const uint8_t* block, const SelectPortrait& portrait) {
  std::vector<uint8_t> rgba((size_t)portrait.width * portrait.height * 4, 0);
  const uint8_t* source = block + portrait.pixels;
  const uint32_t block_height = portrait.format == 8 ? 8 : 4;
  size_t texel = 0;
  for (uint32_t block_y = 0; block_y < portrait.height; block_y += block_height)
    for (uint32_t block_x = 0; block_x < portrait.width; block_x += 8)
      for (uint32_t y = 0; y < block_height; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
          const uint32_t index = portrait.format == 8 ? (source[texel / 2] >> (texel % 2 ? 0 : 4)) & 15 : source[texel];
          ++texel;
          if (block_x + x >= portrait.width || block_y + y >= portrait.height) continue;
          if (index >= portrait.palette_entries) continue;   // no such color: left transparent
          const uint32_t color = be16(block + portrait.colors + 2 * index);
          uint32_t r, g, b, a = 255;
          if (portrait.palette_format == 0) {          // intensity and alpha
            r = g = b = color & 0xFF; a = color >> 8;
          } else if (portrait.palette_format == 1) {   // 5:6:5
            r = (color >> 11) & 31; g = (color >> 5) & 63; b = color & 31;
            r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
          } else if (color & 0x8000) {                 // 5:5:5, opaque
            r = (color >> 10) & 31; g = (color >> 5) & 31; b = color & 31;
            r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
          } else {                                     // 4:4:4 with 3 bits of alpha
            a = (color >> 12) & 7; r = (color >> 8) & 15; g = (color >> 4) & 15; b = color & 15;
            a = (a << 5) | (a << 2) | (a >> 1); r *= 17; g *= 17; b *= 17;
          }
          uint8_t* out = rgba.data() + ((size_t)(block_y + y) * portrait.width + block_x + x) * 4;
          out[0] = (uint8_t)r; out[1] = (uint8_t)g; out[2] = (uint8_t)b; out[3] = (uint8_t)a;
        }
  return rgba;
}

// A PNG of 8 bit RGBA pixels. The picture is small and written once per scan, so the data is
// stored without compression: no encoder to depend on, and any PNG reader accepts it.
std::vector<uint8_t> rgba_png(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  std::vector<uint8_t> raw;
  raw.reserve(((size_t)width * 4 + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);   // filter: none
    raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + (size_t)(y + 1) * width * 4);
  }
  std::vector<uint8_t> packed{0x78, 0x01};
  uint32_t sum_a = 1, sum_b = 0;
  for (const uint8_t byte : raw) { sum_a = (sum_a + byte) % 65521u; sum_b = (sum_b + sum_a) % 65521u; }
  for (size_t at = 0; at < raw.size();) {
    const size_t chunk = std::min<size_t>(raw.size() - at, 65535);
    packed.push_back(at + chunk == raw.size() ? 1 : 0);
    packed.push_back((uint8_t)chunk); packed.push_back((uint8_t)(chunk >> 8));
    packed.push_back((uint8_t)~chunk); packed.push_back((uint8_t)(~chunk >> 8));
    packed.insert(packed.end(), raw.begin() + at, raw.begin() + at + chunk);
    at += chunk;
  }
  const uint32_t checksum = (sum_b << 16) | sum_a;
  for (int shift = 24; shift >= 0; shift -= 8) packed.push_back((uint8_t)(checksum >> shift));
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    uint8_t word[4];
    put_be32(word, (uint32_t)data.size()); png.insert(png.end(), word, word + 4);
    png.insert(png.end(), body.begin(), body.end());
    put_be32(word, crc32(body.data(), body.size())); png.insert(png.end(), word, word + 4);
  };
  std::vector<uint8_t> header(13, 0);
  put_be32(header.data(), width); put_be32(header.data() + 4, height);
  header[8] = 8; header[9] = 6;   // 8 bits a channel, RGBA
  chunk("IHDR", header);
  chunk("IDAT", packed);
  chunk("IEND", {});
  return png;
}

size_t pack_portraits_locked(const DiscCandidate& screen, const disc_metadata::MexCostumes& costumes,
                            std::map<std::string, AssetRecord>* fresh) {
  if (screen.member.empty()) return 0;
  uint32_t retail_offset = 0, retail_size = 0;
  std::vector<uint8_t> pack, retail;
  if (!host::disc_find_file("MnSlChr.usd", &retail_offset, &retail_size) || !retail_size || retail_size > kMaxAssetBytes ||
      !read_range(screen.file, screen.offset, screen.length, &pack)) return 0;
  retail.resize(retail_size);
  if (!host::disc_read(retail_offset, retail.data(), retail_size) || pack == retail) return 0;
  std::map<int, SelectPortrait> ours, theirs;
  int stride = 0;
  if (!select_portraits(retail, &ours) || !select_portraits(pack, &theirs, &stride)) return 0;
  size_t stored = 0;
  for (auto& item : *fresh) {
    AssetRecord& asset = item.second;
    SlotName slot;
    if (asset.info.kind != "character_costume" || !find_slot(asset.info.target_path, &slot)) continue;
    const int standard_frame = portrait_frame(slot);
    int frame = standard_frame;
    const auto origin = costumes.find(lower(asset.source_member));
    if (stride && origin != costumes.end() && origin->second.family == slot.family->file_code)
      frame = (int)origin->second.external + stride * (int)origin->second.costume;
    else if (!asset.info.variant.empty()) continue;  // no explicit picture for this alternate
    else if (stride && frame >= 0)
      frame = frame / 30 * stride + frame % 30 + (frame % 30 >= 19 ? 1 : 0);
    const auto standard = ours.find(standard_frame), picture = theirs.find(frame);
    if (frame < 0 || picture == theirs.end()) continue;
    const std::vector<uint8_t> rgba = decode_portrait(pack.data() + 0x20, picture->second);
    if (standard != ours.end() && picture->second.width == standard->second.width && picture->second.height == standard->second.height &&
        rgba == decode_portrait(retail.data() + 0x20, standard->second)) continue;
    const std::vector<uint8_t> png = rgba_png(rgba, picture->second.width, picture->second.height);
    AssetRecord::Companion companion;
    companion.kind = "csp";
    companion.sha256 = sha256(png);
    companion.source_member = screen.member;
    companion.stored_path = (fs::path(L"assets") / fs::u8path(asset.info.id) / L"companions" / L"csp.png").generic_u8string();
    std::string write_error;
    if (companion.sha256.empty() ||
        !write_atomic(g_root / fs::u8path(companion.stored_path), png.data(), png.size(), &write_error)) continue;
    asset.companions.push_back(std::move(companion));
    ++stored;
  }
  return stored;
}

// Gives each m-ex costume the stock icon the pack's HUD file shows for it (mex_stock_icons), as the
// skin's "stock" companion. m-ex picks the icon by its own ids, which the standard game cannot read,
// so without this an m-ex pack's stock icons never show outside the full mod.
size_t pack_stock_icons_locked(const DiscCandidate& hud, const disc_metadata::MexCostumes& costumes,
                               std::map<std::string, AssetRecord>* fresh) {
  if (hud.member.empty() || costumes.empty() || !hud.length || hud.length > kMaxAssetBytes) return 0;
  std::vector<uint8_t> pack;
  std::map<int, SelectPortrait> icons;
  int reserved = 0, stride = 0;
  if (!read_range(hud.file, hud.offset, hud.length, &pack) || !mex_stock_icons(pack, &icons, &reserved, &stride)) return 0;
  size_t stored = 0;
  for (auto& item : *fresh) {
    AssetRecord& asset = item.second;
    SlotName slot;
    if (asset.info.kind != "character_costume" || !find_slot(asset.info.target_path, &slot)) continue;
    const auto origin = costumes.find(lower(asset.source_member));
    if (origin == costumes.end() || origin->second.family != slot.family->file_code ||
        origin->second.internal >= (uint32_t)stride) continue;
    const auto picture = icons.find(reserved + stride * (int)origin->second.costume + (int)origin->second.internal);
    if (picture == icons.end()) continue;
    const std::vector<uint8_t> png =
        rgba_png(decode_portrait(pack.data() + 0x20, picture->second), picture->second.width, picture->second.height);
    AssetRecord::Companion companion;
    companion.kind = "stock";
    companion.sha256 = sha256(png);
    companion.source_member = hud.member;
    companion.stored_path = (fs::path(L"assets") / fs::u8path(asset.info.id) / L"companions" / L"stock.png").generic_u8string();
    std::string write_error;
    if (companion.sha256.empty() ||
        !write_atomic(g_root / fs::u8path(companion.stored_path), png.data(), png.size(), &write_error)) continue;
    asset.companions.push_back(std::move(companion));
    ++stored;
  }
  return stored;
}

}  // namespace cosmetic_internal

ImportResult import_portrait(const std::string& png_path, const std::string& slot, const std::string& kind) {
  const fs::path path = fs::u8path(png_path);
  std::vector<uint8_t> bytes;
  std::string error;
  if (!read_bounded(path, 16ull * 1024 * 1024, &bytes, &error)) return {false, false, {}, error};
  std::lock_guard<std::mutex> lock(g_mutex);
  ImportResult result = install_portrait_locked(slot, kind, bytes, path_filename_utf8(path));
  if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
  g_message = result.message;
  return result;
}

// ---- a skin's own portrait and stock icon ----

static const char* picture_word(const std::string& kind) { return kind == "csp" ? "portrait" : "stock icon"; }
static AssetRecord* skin_locked(const std::string& skin_id) {
  for (auto& item : g_assets)
    if (item.info.id == skin_id && item.info.kind == "character_costume") return &item;
  return nullptr;
}
static const AssetRecord::Companion* picture_of(const AssetRecord& asset, const std::string& kind) {
  for (const auto& companion : asset.companions) if (companion.kind == kind) return &companion;
  return nullptr;
}
// The fighter a skin or a costume's picture entry belongs to; null for stages and effects.
static const FighterFamily* entry_family(const AssetRecord& asset) {
  SlotName slot;
  if (asset.info.kind != "character_costume" && asset.info.kind != kPortraitKind) return nullptr;
  return find_slot(portrait_slot(asset.info.target_path), &slot) ? slot.family : nullptr;
}

// Stores `png` as the skin's own picture of that kind. The file is written under a new name first
// and the record changed after, so a failed save leaves the old picture as it was.
static ImportResult attach_picture_locked(const std::string& skin_id, const std::string& kind,
                                          const std::vector<uint8_t>& png, const std::string& source_name,
                                          const std::string& from_entry = std::string()) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  if (kind != "csp" && kind != "stock") { result.message = "A picture is a portrait or a stock icon."; return result; }
  AssetRecord* asset = skin_locked(skin_id);
  if (!asset) { result.message = "A picture belongs to an installed skin; that skin is not in the list."; return result; }
  if (!png_companion(png, &error)) { result.message = error; return result; }
  const std::string digest = sha256(png);
  if (digest.empty()) { result.message = "The picture could not be hashed."; return result; }
  AssetRecord::Companion picture;
  picture.kind = kind;
  picture.stored_path = (fs::path(L"assets") / fs::u8path(asset->info.id) / L"companions" /
                         fs::u8path(player_picture_file(kind, digest))).generic_u8string();
  picture.sha256 = digest;
  picture.source_member = source_name;
  const AssetRecord::Companion* had = picture_of(*asset, kind);
  const bool same = had && had->sha256 == digest;
  const std::string old_path = had ? had->stored_path : std::string();
  const std::string old_source = had ? had->source_member : std::string();
  if (!write_atomic(g_root / fs::u8path(picture.stored_path), png.data(), png.size(), &error)) { result.message = error; return result; }
  const AssetRecord previous = *asset;
  const Profile previous_profile = g_profile;
  asset->companions.erase(std::remove_if(asset->companions.begin(), asset->companions.end(),
      [&](const AssetRecord::Companion& c) { return c.kind == kind; }), asset->companions.end());
  asset->companions.push_back(picture);
  // The portrait first: the list's preview is the first picture it finds.
  std::stable_sort(asset->companions.begin(), asset->companions.end(),
      [](const AssetRecord::Companion& a, const AssetRecord::Companion& b) { return a.kind == "csp" && b.kind != "csp"; });
  // The note about the picture this one replaces goes with it.
  if (!old_source.empty()) {
    auto& notes = asset->info.unsupported_companions;
    const std::string tail = ": " + old_source;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const std::string& note) {
      return note.size() >= tail.size() && note.compare(note.size() - tail.size(), tail.size(), tail) == 0;
    }), notes.end());
  }
  if (!same) ++g_profile.generation;   // the same skin, another picture
  // A costume's picture entry for this skin's own costume, whose every picture the skin now carries,
  // has done its job: it is switched off, so the standard costume shows the game's picture again.
  bool retired = false;
  const auto entry = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return !from_entry.empty() && item.info.id == from_entry && item.info.kind == kPortraitKind &&
           item.info.target_path == asset->info.target_path + kPortraitSuffix;
  });
  if (entry != g_assets.end()) {
    const auto pick = g_profile.selections.find(entry->info.target_path);
    const bool carried = std::all_of(entry->companions.begin(), entry->companions.end(),
        [&](const AssetRecord::Companion& c) {
          const AssetRecord::Companion* mine = picture_of(*asset, c.kind);
          return (c.kind != "csp" && c.kind != "stock") || (mine && mine->sha256 == c.sha256);
        });
    if (pick != g_profile.selections.end() && pick->second == from_entry && carried) {
      pick->second = kVanillaSelection;
      ++g_profile.generation;
      retired = true;
    }
  }
  if (!save_state_locked(&error)) {
    *asset = previous; g_profile = previous_profile;
    if (picture.stored_path != old_path) DeleteFileW((g_root / fs::u8path(picture.stored_path)).c_str());
    result.message = error; return result;
  }
  if (!old_path.empty() && old_path != picture.stored_path) DeleteFileW((g_root / fs::u8path(old_path)).c_str());
  g_message = std::string(kind == "csp" ? "Portrait" : "Stock icon") + " of " + asset->info.name + " set from " +
              source_name + "." + (retired ? " The standard costume shows its own picture again." : "");
  result.ok = true; result.already_present = same; result.asset_id = asset->info.id; result.message = g_message;
  return result;
}

ImportResult set_skin_portrait(const std::string& skin_id, const std::string& png_path, const std::string& kind) {
  const fs::path path = fs::u8path(png_path);
  std::vector<uint8_t> bytes;
  std::string error;
  if (!read_bounded(path, 16ull * 1024 * 1024, &bytes, &error)) return {false, false, {}, error};
  std::lock_guard<std::mutex> lock(g_mutex);
  ImportResult result = attach_picture_locked(skin_id, kind, bytes, path_filename_utf8(path));
  if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
  g_message = result.message;
  return result;
}

bool set_skin_portrait_from(const std::string& skin_id, const std::string& source_asset_id,
                            const std::string& kind, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  const AssetRecord* skin = skin_locked(skin_id);
  const auto source = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == source_asset_id;
  });
  const AssetRecord::Companion* picture = source == g_assets.end() ? nullptr : picture_of(*source, kind);
  if (!skin || !picture || source_asset_id == skin_id || !entry_family(*source) ||
      entry_family(*source) != entry_family(*skin)) {
    *error = std::string("That entry has no ") + picture_word(kind) + " for this skin."; return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_bounded(g_root / fs::u8path(picture->stored_path), 16ull * 1024 * 1024, &bytes, error) ||
      sha256(bytes) != picture->sha256) {
    *error = "The " + std::string(picture_word(kind)) + " of " + source->info.name + " is missing or changed."; return false;
  }
  const std::string from = source->info.name;   // attach_picture_locked changes the list's records
  ImportResult result = attach_picture_locked(skin_id, kind, bytes, from, source_asset_id);
  if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
  if (result.ok) g_message = result.message; else *error = result.message;
  return result.ok;
}

bool clear_skin_portrait(const std::string& skin_id, const std::string& kind, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  AssetRecord* asset = skin_locked(skin_id);
  const AssetRecord::Companion* had = asset ? picture_of(*asset, kind) : nullptr;
  if (!had) { *error = std::string("That skin has no ") + picture_word(kind) + " of its own."; return false; }
  const AssetRecord previous = *asset;
  const Profile previous_profile = g_profile;
  const fs::path stored = g_root / fs::u8path(had->stored_path);
  const std::string tail = ": " + had->source_member;
  auto& notes = asset->info.unsupported_companions;
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const std::string& note) {
    return tail.size() > 2 && note.size() >= tail.size() && note.compare(note.size() - tail.size(), tail.size(), tail) == 0;
  }), notes.end());
  asset->companions.erase(std::remove_if(asset->companions.begin(), asset->companions.end(),
      [&](const AssetRecord::Companion& c) { return c.kind == kind; }), asset->companions.end());
  ++g_profile.generation;
  if (!save_state_locked(error)) { *asset = previous; g_profile = previous_profile; return false; }
  DeleteFileW(stored.c_str());
  g_message = asset->info.name + " has no " + picture_word(kind) + " of its own now" +
              (std::atomic_load(&g_runtime)->initialized ? "; restart to apply it." : ".");
  return true;
}

std::vector<PortraitChoice> portrait_choices(const std::string& skin_id, const std::string& kind) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<PortraitChoice> out;
  const AssetRecord* skin = skin_locked(skin_id);
  const FighterFamily* family = skin ? entry_family(*skin) : nullptr;
  if (!family) return out;
  for (const auto& item : g_assets)
    if (item.info.id != skin_id && entry_family(item) == family && picture_of(item, kind))
      out.push_back({item.info.id, item.info.name});
  return out;
}

PortraitSource skin_portrait_source(const std::string& skin_id, const std::string& kind) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const AssetRecord* skin = skin_locked(skin_id);
  if (!skin) return PortraitSource::Standard;
  if (picture_of(*skin, kind)) return PortraitSource::Own;
  // The costume's added picture counts while its entry is switched on.
  const std::string target = skin->info.target_path + kPortraitSuffix;
  const auto pick = g_profile.selections.find(target);
  if (pick == g_profile.selections.end() || pick->second == kVanillaSelection) return PortraitSource::Standard;
  for (const auto& item : g_assets)
    if (item.info.id == pick->second && item.info.kind == kPortraitKind && item.info.target_path == target &&
        picture_of(item, kind)) return PortraitSource::Costume;
  return PortraitSource::Standard;
}

// The game's own stock icon of a fighter and costume: the HUD file's (IfAll.usd) icon animation, the
// frame the game picks (gm_80168B34): the fighter's column, then 30 frames a costume row. Columns are
// the character select numbers, Sheik after Ganondorf (25), everyone after her one to the left.
bool standard_stock_icon(int css_character, int costume, std::vector<uint8_t>* rgba, int* width, int* height) {
  static std::mutex lock;
  static bool tried = false;
  static std::vector<uint8_t> file;
  static std::map<int, SelectPortrait> frames;
  std::lock_guard<std::mutex> lk(lock);
  if (!tried) {
    tried = true;
    uint32_t offset = 0, size = 0;
    if ((host::disc_find_file("IfAll.usd", &offset, &size) || host::disc_find_file("IfAll.dat", &offset, &size)) && size &&
        size <= kMaxAssetBytes) {
      file.resize(size);
      if (!host::disc_read(offset, file.data(), size) || !texture_animation_frames(file, UINT32_MAX, &frames, 24, 24)) {
        file.clear();
        frames.clear();
      }
    }
    host::log("cosmetics: %zu stock icon frames in the HUD file", frames.size());
  }
  if (frames.empty() || css_character < 0 || css_character > 25 || costume < 0 || costume > 5) return false;
  const int column = css_character == 19 ? 25 : css_character > 19 ? css_character - 1 : css_character;
  const auto it = frames.find(column + costume * 30);
  if (it == frames.end()) return false;
  *rgba = decode_portrait(file.data() + 0x20, it->second);
  *width = (int)it->second.width;
  *height = (int)it->second.height;
  return !rgba->empty();
}

bool standard_portrait(int css_character, int costume, std::vector<uint8_t>* rgba, int* width, int* height) {
  static std::mutex lock;
  static bool tried = false;
  static std::vector<uint8_t> file;
  static std::map<int, SelectPortrait> frames;
  std::lock_guard<std::mutex> lk(lock);
  if (!tried) {
    tried = true;
    uint32_t offset = 0, size = 0;
    if (host::disc_find_file("MnSlChr.usd", &offset, &size) && size && size <= kMaxAssetBytes) {
      file.resize(size);
      if (!host::disc_read(offset, file.data(), size) || !select_portraits(file, &frames)) { file.clear(); frames.clear(); }
    }
  }
  if (frames.empty() || costume < 0) return false;
  if (css_character == 19) css_character = 18;   // Sheik is in Zelda's cell
  int frame = -1;
  if (css_character == 3) {
    frame = 3 + 30 * std::min(costume, 3);       // Mr. Game & Watch: one costume file, a cell per colour
  } else {
    SlotName slot;
    if (!find_slot(costume_slot_file(css_character, costume), &slot)) return false;
    frame = portrait_frame(slot);
  }
  const auto it = frames.find(frame);
  if (frame < 0 || it == frames.end()) return false;
  *rgba = decode_portrait(file.data() + 0x20, it->second);
  *width = (int)it->second.width;
  *height = (int)it->second.height;
  return !rgba->empty();
}

}  // namespace host::cosmetics
