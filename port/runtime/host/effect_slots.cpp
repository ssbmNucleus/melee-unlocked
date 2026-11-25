// An effect file's named slots, the bytes each owns, and which a mod changes (effect_slots.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "effect_slots.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "mod_data_safety.h"

namespace host::cosmetics::effect_slots {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

// ---- the table: per disc file, its slots' model effects ("m") and generators ("g", by id) ----
// From tools/effect_slot_survey.py over the game's code and the disc's files. Clones share a file
// (Fox and Falco EfFxData, Mario and Dr. Mario EfMrData, Link and Young Link EfLkData, Pikachu and Pichu
// EfPkData, Zelda and Sheik EfZdData); Ganondorf has his own (EfGnData). A row named kSharedSlot puts
// generators more than one slot starts in "Shared effects".
struct SlotDef { const char* name; const char* parts; };
struct FileDef { const char* target; std::vector<SlotDef> slots; };

const std::vector<FileDef>& files() {
  static const std::vector<FileDef> table = {
      {"EfCaData.dat", {{"Falcon Punch", "m0-1 g4001-4003,4005,4016"},
                        {"Falcon Kick", "m2 g4000,4004,4006-4009,4011"},
                        {"Raptor Boost", "m3-5 g4012-4015"},
                        {kSharedSlot, "g4010"}}},
      {"EfFxData.dat", {{"Shine", "m0-2 g3000-3003"},
                        {"Fire Fox", "m3-4 g3004-3007,3009,3010"},
                        {"Fox Illusion", "g3008"},
                        {"Laser ring", "m5"}}},
      {"EfMrData.dat", {{"Fireball", "m0 g1001,1003-1007"}, {"Cape", "g1008,1009"}, {"Mario Tornado", "m1"}}},
      {"EfSsData.dat", {{"Charge Shot", "m1 g2000-2010,2018-2021"},
                        {"Missile", "g2011-2017"},
                        {"Screw Attack", "m0"},
                        {"Double Jump", "m2"}}},
      {"EfKbData.dat", {{"Final Cutter", "m0-2"},
                        {"Inhale", "g5003-5006"},
                        {"Stone", "g5007-5018"},
                        {"Dash Attack", "m3 g5000-5002"},
                        {"Rapid Jab", "m8"}}},
      {"EfLkData.dat", {{"Spin Attack", "m0-3"}}},
      {"EfPkData.dat", {{"Thunder Jolt", "g7000-7009"},
                        {"Thunder", "m0"},
                        {"Forward Smash", "m1-2"},
                        {"Pichu Forward Smash", "m3-4"}}},
      {"EfDkData.dat", {{"Giant Punch", "m2-3"}, {"Spinning Kong", "m4"}, {"Hand Slap", "m0-1,6"}}},
      {"EfYsData.dat", {{"Egg Throw", "g9000-9003"}}},
      {"EfNsData.dat", {{"PSI Magnet", "m2"}}},
      {"EfPrData.dat", {{"Sing", "m0"}}},
      {"EfKpData.dat", {{"Fire Breath", "g12005"}, {"Koopa Klaw", "m0"}, {"Whirling Fortress", "m1-2"}}},
      {"EfMtData.dat", {{"Teleport", "m3"}, {"Forward Smash", "m2"}, {"Up Smash", "m1"}}},
      {"EfIcData.dat", {{"Ice Shot", "g14000"}, {"Blizzard", "g14007-14011"}}},
      {"EfPeData.dat", {{"Vegetable", "m0"}}},
      {"EfMsData.dat", {{"Shield Breaker", "m0-1"}, {"Counter", "g16000-16003"}}},
      {"EfZdData.dat", {{"Nayru's Love", "m0-1"},
                        {"Farore's Wind", "m2-3 g17000-17004"},
                        {"Transform", "g17005-17009"},
                        {"Up Smash", "m5-6"},
                        {"Magic Sparkles", "m4"}}},
      {"EfLgData.dat", {{"Fireball", "m0 g18000,18002-18006"}, {"Luigi Cyclone", "m1"}}},
      {"EfGnData.dat", {{"Warlock Punch", "m0-1 g19000-19003"},
                        {"Wizard's Foot", "m2 g19004-19008,19010,19011"},
                        {"Gerudo Dragon", "m3-5 g19012-19016"},
                        {kSharedSlot, "g19009"}}},
      {"EfFeData.dat", {{"Flare Blade", "m0-1"}, {"Counter", "g49000-49003"}}},
  };
  return table;
}

std::string lowered(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

const FileDef* file_def(const std::string& target) {
  const std::string want = lowered(target);
  for (const auto& f : files())
    if (lowered(f.target) == want) return &f;
  return nullptr;
}

// "m0-1 g4001-4003,4005": the model effects and the generator ids.
void parse_parts(const char* parts, std::vector<int>* models, std::vector<int>* gens) {
  std::vector<int>* into = nullptr;
  const char* p = parts;
  while (*p) {
    if (*p == 'm') { into = models; ++p; continue; }
    if (*p == 'g') { into = gens; ++p; continue; }
    if (*p == ' ' || *p == ',') { ++p; continue; }
    int from = 0, to;
    while (*p >= '0' && *p <= '9') from = from * 10 + (*p++ - '0');
    to = from;
    if (*p == '-') {
      ++p;
      to = 0;
      while (*p >= '0' && *p <= '9') to = to * 10 + (*p++ - '0');
    }
    for (int v = from; v <= to && into; ++v) into->push_back(v);
  }
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// ---- the archive: its data block, pointer fields and blocks (a block runs from something pointed at,
// or a root, to the next such place) ----
struct Dat {
  const std::vector<uint8_t>* raw = nullptr;
  uint32_t size = 0;                // the data block's
  uint32_t tables_at = 0;           // where the relocation table starts in the file
  std::vector<uint32_t> relocs;     // sorted pointer fields (data offsets)
  std::vector<uint32_t> starts;     // sorted block starts, then `size`
  uint32_t table = 0, table_end = 0;
  std::string table_name;

  const uint8_t* data() const { return raw->data() + 0x20; }
  uint32_t word(uint32_t o) const { return be32(data() + o); }
  bool fits(uint64_t at, uint64_t n) const { return at <= size && n <= size - at; }
  bool ptr(uint32_t field, uint32_t* target) const {
    if (!std::binary_search(relocs.begin(), relocs.end(), field)) return false;
    *target = word(field);
    return true;
  }
  uint32_t end_of(uint32_t o) const {
    const auto next = std::upper_bound(starts.begin(), starts.end(), o);
    return next == starts.end() ? size : *next;
  }

  // `root`: the root to read (its offset in `table`); else the effect table ("...DataTable").
  bool parse(const std::vector<uint8_t>& bytes, std::string* why, const char* root = nullptr) {
    raw = &bytes;
    if (bytes.size() < 0x20 || be32(bytes.data()) != bytes.size()) { *why = "not an archive"; return false; }
    size = be32(bytes.data() + 4);
    const uint64_t nrel = be32(bytes.data() + 8), nroot = be32(bytes.data() + 12), nref = be32(bytes.data() + 16);
    const uint64_t rel_at = 0x20ull + size, root_at = rel_at + 4 * nrel, names = root_at + 8 * (nroot + nref);
    if (nrel > size / 4 || nroot > 1024 || nref > 1024 || names > bytes.size()) {
      *why = "its tables run past the file"; return false;
    }
    tables_at = uint32_t(rel_at);
    relocs.reserve(size_t(nrel));
    starts.reserve(size_t(nrel + nroot + 1));
    for (uint64_t i = 0; i < nrel; ++i) {
      const uint32_t field = be32(bytes.data() + rel_at + 4 * i);
      if ((field & 3) || !fits(field, 4) || word(field) >= size) { *why = "a pointer lies outside the file"; return false; }
      relocs.push_back(field);
      starts.push_back(word(field));
    }
    std::sort(relocs.begin(), relocs.end());
    if (std::adjacent_find(relocs.begin(), relocs.end()) != relocs.end()) { *why = "a pointer is listed twice"; return false; }
    bool found = false;
    for (uint64_t i = 0; i < nroot; ++i) {
      const uint32_t at = be32(bytes.data() + root_at + 8 * i);
      const uint64_t name = names + be32(bytes.data() + root_at + 8 * i + 4);
      if (at >= size || name >= bytes.size()) { *why = "a root lies outside the file"; return false; }
      starts.push_back(at);
      const char* text = reinterpret_cast<const char*>(bytes.data() + name);
      const std::string n(text, strnlen(text, bytes.size() - size_t(name)));
      if (!found && (root ? n == root : n.size() > 9 && n.compare(n.size() - 9, 9, "DataTable") == 0)) {
        found = true; table = at; table_name = n;
      }
    }
    if (!found || !fits(table, 8)) { *why = root ? std::string("it has no root ") + root : "it has no effect table"; return false; }
    starts.push_back(size);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    table_end = end_of(table);
    return true;
  }
};

// ---- an effect file's parts ----
struct Gen { int id; uint32_t begin, end; };
struct Group { bool present = false; uint32_t head = 0, head_end = 0; std::vector<std::pair<uint32_t, uint32_t>> images; };
struct Model { uint32_t desc = 0; std::array<uint32_t, 4> parts{{kNone, kNone, kNone, kNone}}; };
struct Effects {
  uint32_t commands = kNone, commands_head_end = 0;
  std::vector<Gen> gens;
  uint32_t textures = kNone, textures_head_end = 0;
  std::vector<Group> groups;
  std::vector<Model> models;
};

bool read_effects(const Dat& d, Effects* e, std::string* why) {
  uint32_t bank;
  if (d.ptr(d.table, &bank)) {
    const uint32_t bank_end = d.end_of(bank);
    if (!d.fits(bank, 12) || bank_end < bank + 12) { *why = "its command bank is cut short"; return false; }
    const uint16_t version = be16(d.data() + bank);
    uint32_t first = 0, count, at;
    if (version == 0) { count = d.word(bank + 4); at = 8; }
    else { first = d.word(bank + 4); count = d.word(bank + 8); at = 12; }
    if (count > (bank_end - bank - at) / 4) { *why = "its generator count runs past its bank"; return false; }
    e->commands = bank;
    e->commands_head_end = bank + at + 4 * count;
    std::vector<uint32_t> bounds{bank_end - bank};
    for (uint32_t i = 0; i < count; ++i)
      if (const uint32_t off = d.word(bank + at + 4 * i)) bounds.push_back(off);
    std::sort(bounds.begin(), bounds.end());
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t off = d.word(bank + at + 4 * i);
      if (!off) continue;
      if (off < at + 4 * count || off >= bank_end - bank) { *why = "a generator lies outside its bank"; return false; }
      const uint32_t end = *std::upper_bound(bounds.begin(), bounds.end(), off);
      e->gens.push_back({int(first + i), bank + off, bank + end});
    }
  }
  if (d.ptr(d.table + 4, &bank)) {
    if (!d.fits(bank, 4)) { *why = "its texture bank is cut short"; return false; }
    const uint32_t count = d.word(bank);
    if (count > 4096 || !d.fits(bank, 4ull + 4ull * count)) { *why = "its texture group count runs past the file"; return false; }
    e->textures = bank;
    e->textures_head_end = bank + 4 + 4 * count;
    e->groups.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t off = d.word(bank + 4 + 4 * i);
      if (!off) continue;
      const uint64_t group = uint64_t(bank) + off;
      if (!d.fits(group, 0x18)) { *why = "a texture group lies outside the file"; return false; }
      const uint32_t g = uint32_t(group);
      const uint32_t num = d.word(g), format = d.word(g + 4), width = d.word(g + 0xC), height = d.word(g + 0x10);
      const uint32_t palettes = d.word(g + 0x14), palnum = palettes >> 16, palflag = palettes & 0xFFFF;
      const bool indexed = format == 8 || format == 9 || format == 10;
      const uint64_t entries = !indexed ? num : (palflag & 1) ? uint64_t(num) + 1 : palnum ? uint64_t(num) + palnum
                                                                                           : uint64_t(num) * 2;
      if (entries > 65536 || !d.fits(group + 0x18, entries * 4)) { *why = "a texture table runs past the file"; return false; }
      Group& out = e->groups[i];
      out.present = true;
      out.head = g;
      out.head_end = uint32_t(group + 0x18 + entries * 4);
      const uint64_t image = data_safety::gx_image_bytes(format, width, height);
      const uint64_t palette = data_safety::gx_palette_bytes(format);
      for (uint64_t t = 0; t < entries; ++t) {
        const uint32_t entry = d.word(uint32_t(group + 0x18 + t * 4));
        const uint64_t length = t < num ? image : palette;
        if (!entry || !length) continue;
        if (!d.fits(uint64_t(bank) + entry, length)) { *why = "a texture lies outside the file"; return false; }
        out.images.push_back({bank + entry, uint32_t(bank + entry + length)});
      }
    }
  }
  for (uint32_t desc = d.table + 8; desc + 0x14 <= d.table_end; desc += 0x14) {
    Model m;
    m.desc = desc;
    for (uint32_t k = 0; k < 4; ++k) {
      uint32_t target;
      if (d.ptr(desc + 4 + 4 * k, &target)) m.parts[k] = target;
    }
    e->models.push_back(m);
  }
  return true;
}

// The blocks a model effect's part reaches (its joints, animations, meshes, textures), never the
// table or the banks.
void reach(const Dat& d, uint32_t from, const std::set<uint32_t>& stops, std::set<uint32_t>* blocks) {
  std::vector<uint32_t> stack{from};
  while (!stack.empty()) {
    const uint32_t at = stack.back();
    stack.pop_back();
    if (at == kNone || stops.count(at) || !blocks->insert(at).second) continue;
    const uint32_t end = d.end_of(at);
    for (auto r = std::lower_bound(d.relocs.begin(), d.relocs.end(), at); r != d.relocs.end() && *r < end; ++r)
      stack.push_back(d.word(*r));
  }
}

// Two parts compared as graphs: each block the same length with pointers in the same places and the
// same bytes elsewhere, and what they point to the same in turn (a file laid out anew still compares).
// `padded`: a block may also be followed by zeros up to the next one (writers align blocks differently,
// so a block's end is not its size): the shorter length, the longer one's rest zeros with no pointer in it.
bool same_graph(const Dat& a, uint32_t oa, const Dat& b, uint32_t ob, bool padded = false) {
  if (oa == kNone || ob == kNone) return oa == kNone && ob == kNone;
  const auto zeros = [](const Dat& d, uint32_t at, uint32_t end) {
    if (std::any_of(d.data() + at, d.data() + end, [](uint8_t v) { return v != 0; })) return false;
    const auto r = std::lower_bound(d.relocs.begin(), d.relocs.end(), at);
    return r == d.relocs.end() || *r >= end;
  };
  std::vector<std::pair<uint32_t, uint32_t>> stack{{oa, ob}};
  std::set<std::pair<uint32_t, uint32_t>> seen;
  while (!stack.empty()) {
    const auto [x, y] = stack.back();
    stack.pop_back();
    if (!seen.insert({x, y}).second) continue;
    uint32_t ex = a.end_of(x), ey = b.end_of(y);
    if (ex - x != ey - y) {
      if (!padded) return false;
      const uint32_t n = std::min(ex - x, ey - y);
      if (!zeros(a, x + n, ex) || !zeros(b, y + n, ey)) return false;
      ex = x + n;
      ey = y + n;
    }
    auto ra = std::lower_bound(a.relocs.begin(), a.relocs.end(), x);
    auto rb = std::lower_bound(b.relocs.begin(), b.relocs.end(), y);
    uint32_t at = 0;
    for (;; ++ra, ++rb) {
      const bool ina = ra != a.relocs.end() && *ra < ex, inb = rb != b.relocs.end() && *rb < ey;
      if (ina != inb) return false;
      const uint32_t next = ina ? *ra - x : ex - x;
      if (ina && *rb - y != next) return false;
      if (std::memcmp(a.data() + x + at, b.data() + y + at, next - at)) return false;
      if (!ina) break;
      stack.push_back({a.word(*ra), b.word(*rb)});
      at = next + 4;
    }
  }
  return true;
}

}  // namespace

int Layout::slot_index(const std::string& name) const {
  for (size_t i = 0; i < slots.size(); ++i)
    if (slots[i] == name) return int(i);
  return -1;
}

std::vector<std::string> table_slots(const std::string& target) {
  std::vector<std::string> out;
  if (const FileDef* f = file_def(target))
    for (const auto& s : f->slots)
      if (std::strcmp(s.name, kSharedSlot) != 0) out.push_back(s.name);
  return out;
}

std::vector<std::string> table_files() {
  std::vector<std::string> out;
  for (const auto& f : files()) out.push_back(f.target);
  return out;
}

bool map_slots(const std::string& target, const std::vector<uint8_t>& retail, Layout* out, std::string* error) {
  *out = Layout{};
  const FileDef* f = file_def(target);
  if (!f) { *error = "no slot table for " + target; return false; }
  Dat d;
  Effects e;
  std::string why;
  if (!d.parse(retail, &why) || !read_effects(d, &e, &why)) { *error = target + ": " + why; return false; }

  // Working indices: the named slots, then Shared, then Other.
  std::vector<std::string> names;
  for (const auto& s : f->slots)
    if (std::strcmp(s.name, kSharedSlot) != 0) names.push_back(s.name);
  const int shared = int(names.size()), other = shared + 1;
  std::map<int, int> model_slot, gen_slot;
  for (const auto& s : f->slots) {
    const int index = std::strcmp(s.name, kSharedSlot) == 0
                          ? shared
                          : int(std::find(names.begin(), names.end(), s.name) - names.begin());
    std::vector<int> models, gens;
    parse_parts(s.parts, &models, &gens);
    for (int m : models) model_slot[m] = index;
    for (int g : gens) gen_slot[g] = index;
  }
  const auto slot_of_model = [&](int m) { const auto it = model_slot.find(m); return it == model_slot.end() ? other : it->second; };
  const auto slot_of_gen = [&](int g) { const auto it = gen_slot.find(g); return it == gen_slot.end() ? other : it->second; };

  // Bytes are claimed by slot; a byte two slots claim is Shared; one nobody claims is Other.
  constexpr uint8_t kFree = 0xFF;
  std::vector<uint8_t> owner(retail.size(), kFree);
  std::vector<bool> holds(names.size() + 2, false);   // a model, generator or texture group
  const auto claim = [&](uint32_t begin, uint32_t end, int slot) {   // data offsets
    for (uint32_t i = 0x20 + begin; i < 0x20 + end; ++i)
      owner[i] = owner[i] == kFree || owner[i] == slot ? uint8_t(slot) : uint8_t(shared);
  };
  const std::set<uint32_t> stops{d.table, e.commands, e.textures};
  std::vector<int> group_slot(e.groups.size(), -1);
  for (const Gen& g : e.gens) {
    const int slot = slot_of_gen(g.id);
    claim(g.begin, g.end, slot);
    holds[size_t(slot)] = true;
    if (g.end - g.begin >= 4) {
      const uint16_t group = be16(d.data() + g.begin + 2);
      if (group < group_slot.size())
        group_slot[group] = group_slot[group] < 0 || group_slot[group] == slot ? slot : shared;
    }
  }
  for (size_t i = 0; i < e.groups.size(); ++i) {
    const Group& g = e.groups[i];
    if (!g.present) continue;
    const int slot = group_slot[i] < 0 ? other : group_slot[i];
    claim(g.head, g.head_end, slot);
    for (const auto& image : g.images) claim(image.first, image.second, slot);
    if (slot != other) holds[size_t(slot)] = true;   // (an unused group is no reason to list Other)
  }
  for (size_t i = 0; i < e.models.size(); ++i) {
    const Model& m = e.models[i];
    const int slot = slot_of_model(int(i));
    claim(m.desc, m.desc + 0x14, slot);
    std::set<uint32_t> blocks;
    for (uint32_t part : m.parts) reach(d, part, stops, &blocks);
    for (uint32_t block : blocks) claim(block, d.end_of(block), slot);
    if (m.parts[0] != kNone || m.parts[1] != kNone || m.parts[2] != kNone || m.parts[3] != kNone)
      holds[size_t(slot)] = true;
  }
  for (auto& o : owner)
    if (o == kFree) o = uint8_t(other);

  // The slots that own bytes, in order; Other always (the file's tables).
  std::vector<bool> has(names.size() + 2, false);
  for (uint8_t o : owner) has[o] = true;
  has[size_t(other)] = true;
  std::vector<int> remap(names.size() + 2, -1);
  for (size_t i = 0; i < names.size() + 2; ++i) {
    if (!has[i]) continue;
    remap[i] = int(out->slots.size());
    out->slots.push_back(int(i) < shared ? names[i] : int(i) == shared ? kSharedSlot : kOtherSlot);
    out->listed.push_back(holds[i]);
  }
  out->target = f->target;
  out->owner.resize(owner.size());
  out->ranges.resize(out->slots.size());
  for (size_t i = 0; i < owner.size(); ++i) {
    const uint8_t s = uint8_t(remap[owner[i]]);
    out->owner[i] = s;
    auto& r = out->ranges[s];
    if (!r.empty() && r.back().second == i) ++r.back().second;
    else r.push_back({uint32_t(i), uint32_t(i + 1)});
  }
  for (size_t i = 0; i < e.models.size(); ++i) out->model_slot.push_back(remap[size_t(slot_of_model(int(i)))]);
  for (const Gen& g : e.gens) out->gen_slot[g.id] = remap[size_t(slot_of_gen(g.id))];
  for (size_t i = 0; i < e.groups.size(); ++i)
    out->group_slot.push_back(remap[size_t(group_slot[i] < 0 ? other : group_slot[i])]);
  out->other = remap[size_t(other)];
  return true;
}

bool diff(const Layout& layout, const std::vector<uint8_t>& retail, const std::vector<uint8_t>& mod, Diff* out,
          std::string* error) {
  *out = Diff{};
  Dat a, b;
  Effects ea, eb;
  std::string why;
  if (!a.parse(retail, &why) || !read_effects(a, &ea, &why)) { *error = layout.target + ": " + why; return false; }
  if (!b.parse(mod, &why) || !read_effects(b, &eb, &why)) { *error = "the mod's file: " + why; return false; }
  if (a.table_name != b.table_name) { *error = "the mod's file is not " + layout.target; return false; }
  if (layout.owner.size() != retail.size()) { *error = "the slot map is not of this file"; return false; }

  std::vector<bool> changed(layout.slots.size(), false);
  // The same layout: the same size and tables, every pointer, the generator table and the texture
  // groups' heads the same. Then a byte that differs is its owner's change.
  const auto same_bytes = [&](uint32_t begin, uint32_t end) {   // data offsets
    return std::memcmp(a.data() + begin, b.data() + begin, end - begin) == 0;
  };
  if (retail.size() != mod.size()) out->relaid = "its size differs";
  else if (std::memcmp(retail.data(), mod.data(), 20) || a.tables_at != b.tables_at ||
           std::memcmp(retail.data() + a.tables_at, mod.data() + a.tables_at, retail.size() - a.tables_at))
    out->relaid = "its pointers, roots or names differ";
  else {
    for (uint32_t r : a.relocs)
      if (a.word(r) != b.word(r)) { out->relaid = "a pointer differs"; break; }
  }
  if (out->relaid.empty()) {
    if (ea.commands != eb.commands || ea.commands_head_end != eb.commands_head_end ||
        (ea.commands != kNone && !same_bytes(ea.commands, ea.commands_head_end)))
      out->relaid = "its generator table differs";
    else if (ea.textures != eb.textures || ea.groups.size() != eb.groups.size() ||
             (ea.textures != kNone && !same_bytes(ea.textures, ea.textures_head_end)))
      out->relaid = "its texture table differs";
    else {
      for (const Group& g : ea.groups)
        if (g.present && !same_bytes(g.head, g.head_end)) { out->relaid = "a texture group's size or format differs"; break; }
    }
  }
  out->keeps_layout = out->relaid.empty();

  if (out->keeps_layout) {
    for (size_t i = 0; i < retail.size(); ++i)
      if (retail[i] != mod[i]) changed[layout.owner[i]] = true;
  } else {
    // Laid out anew: compared by content, part by part (a model effect as a graph, a generator by its
    // record, a texture group by its head and images).
    const auto mark = [&](int slot) { changed[size_t(slot < 0 ? layout.other : slot)] = true; };
    for (size_t i = 0; i < std::max(ea.models.size(), eb.models.size()); ++i) {
      const int slot = i < layout.model_slot.size() ? layout.model_slot[i] : layout.other;
      if (i >= ea.models.size() || i >= eb.models.size()) { mark(slot); continue; }
      const Model &x = ea.models[i], &y = eb.models[i];
      bool same = be32(a.data() + x.desc) == be32(b.data() + y.desc);
      for (int k = 0; k < 4 && same; ++k) same = same_graph(a, x.parts[k], b, y.parts[k]);
      if (!same) mark(slot);
    }
    std::map<int, std::pair<const uint8_t*, uint32_t>> ga, gb;
    for (const Gen& g : ea.gens) ga[g.id] = {a.data() + g.begin, g.end - g.begin};
    for (const Gen& g : eb.gens) gb[g.id] = {b.data() + g.begin, g.end - g.begin};
    std::set<int> ids;
    for (const auto& g : ga) ids.insert(g.first);
    for (const auto& g : gb) ids.insert(g.first);
    for (int id : ids) {
      const auto x = ga.find(id), y = gb.find(id);
      const auto slot = layout.gen_slot.find(id);
      if (x == ga.end() || y == gb.end() || x->second.second != y->second.second ||
          std::memcmp(x->second.first, y->second.first, x->second.second))
        mark(slot == layout.gen_slot.end() ? layout.other : slot->second);
    }
    for (size_t i = 0; i < std::max(ea.groups.size(), eb.groups.size()); ++i) {
      const int slot = i < layout.group_slot.size() ? layout.group_slot[i] : layout.other;
      if (i >= ea.groups.size() || i >= eb.groups.size()) { mark(slot); continue; }
      const Group &x = ea.groups[i], &y = eb.groups[i];
      bool same = x.present == y.present && x.images.size() == y.images.size() &&
                  (!x.present || std::memcmp(a.data() + x.head, b.data() + y.head, 0x18) == 0);
      for (size_t t = 0; same && t < x.images.size(); ++t)
        same = x.images[t].second - x.images[t].first == y.images[t].second - y.images[t].first &&
               std::memcmp(a.data() + x.images[t].first, b.data() + y.images[t].first,
                           x.images[t].second - x.images[t].first) == 0;
      if (!same) mark(slot);
    }
    if (std::find(changed.begin(), changed.end(), true) == changed.end() && retail != mod) mark(layout.other);
  }
  for (size_t i = 0; i < changed.size(); ++i)
    if (changed[i]) out->changed.push_back(layout.slots[i]);
  return true;
}

int model_effects(const std::vector<uint8_t>& file) {
  Dat d;
  Effects e;
  std::string why;
  if (!d.parse(file, &why) || !read_effects(d, &e, &why)) return -1;
  int used = 0;
  for (size_t i = 0; i < e.models.size(); ++i) {
    const Model& m = e.models[i];
    const uint8_t* desc = d.data() + m.desc;
    const bool empty = std::all_of(desc, desc + 0x14, [](uint8_t b) { return b == 0; }) &&
                       std::all_of(m.parts.begin(), m.parts.end(), [](uint32_t p) { return p == kNone; });
    if (!empty) used = int(i + 1);
  }
  return used;
}

bool changed_fields(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::string& root,
                    uint32_t size, std::vector<uint32_t>* changed, std::string* error) {
  changed->clear();
  Dat x, y;
  std::string why;
  if (!x.parse(a, &why, root.c_str())) { *error = "the disc's file: " + why; return false; }
  if (!y.parse(b, &why, root.c_str())) { *error = "the mod's file: " + why; return false; }
  if (!x.fits(x.table, size) || !y.fits(y.table, size)) { *error = root + " is cut short"; return false; }
  for (uint32_t field = 0; field + 4 <= size; field += 4) {
    uint32_t px = kNone, py = kNone;
    const bool ax = x.ptr(x.table + field, &px), ay = y.ptr(y.table + field, &py);
    const bool same = ax || ay ? ax && ay && same_graph(x, px, y, py, true) : x.word(x.table + field) == y.word(y.table + field);
    if (!same) changed->push_back(field);
  }
  return true;
}

bool compose(const Layout& layout, const std::vector<uint8_t>& retail,
             const std::vector<const std::vector<uint8_t>*>& sources, std::vector<uint8_t>* out,
             std::string* error) {
  if (layout.owner.size() != retail.size() || sources.size() != layout.slots.size()) {
    *error = "the slot map is not of this file"; return false;
  }
  *out = retail;
  for (size_t s = 0; s < sources.size(); ++s) {
    const std::vector<uint8_t>* source = sources[s];
    if (!source) continue;
    if (source->size() != retail.size()) { *error = "a slot's file is not laid out as the game's"; return false; }
    for (const auto& r : layout.ranges[s])
      std::memcpy(out->data() + r.first, source->data() + r.first, r.second - r.first);
  }
  return true;
}

}  // namespace host::cosmetics::effect_slots
