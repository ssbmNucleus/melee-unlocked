// An effect file's slots (runtime/host/effect_slots.h): which bytes each slot owns, which slots a mod
// changes, whether it keeps the disc file's layout, and a file made slot by slot.
//   effect_slots_test             a small effect file made here (always)
//   effect_slots_test --real      the real files: the disc's (MELEE_TEST_ISO: every file the table
//                                 covers) and the effects study's (MU_FX_SLOTS_DIR, the folder holding
//                                 retail\, real-files\ and standin-data2\); skips (77) without either
// SPDX-License-Identifier: GPL-2.0-or-later
#include "effect_slots.h"
#include "effect_slots_fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace host::cosmetics::effect_slots;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {

using effect_slots_fixture::get32;
using effect_slots_fixture::put32;
using effect_slots_fixture::Made;
using effect_slots_fixture::make;

bool same(const std::vector<std::string>& a, const std::vector<std::string>& b) { return a == b; }

int synthetic() {
  const Made retail = make(0);
  Layout layout;
  std::string error;
  CHECK(map_slots("efcadata.dat", retail.bytes, &layout, &error));
  CHECK(layout.target == "EfCaData.dat");
  CHECK(same(layout.slots, {"Falcon Punch", "Falcon Kick", kSharedSlot, kOtherSlot}));
  CHECK(layout.listed == std::vector<bool>({true, true, true, false}));
  CHECK(layout.owner[retail.gen4001] == 0);
  CHECK(layout.owner[retail.image0] == 2);    // group 0: Kick's 4000 and Punch's 4003
  CHECK(layout.owner[retail.model2] == 1);
  CHECK(layout.owner[retail.shared] == 2);    // models 1 (Punch) and 2 (Kick)
  CHECK(layout.owner[retail.kick] == 1);
  CHECK(layout.owner[4] == 3);                // the archive's header
  CHECK(!map_slots("EfZzData.dat", retail.bytes, &layout, &error));
  CHECK(map_slots("EfCaData.dat", retail.bytes, &layout, &error));

  Diff d;
  // A Punch generator's program: Falcon Punch, same layout.
  const Made punch = make(0, [](std::vector<uint8_t>& b, const Made& m) { b[m.gen4001] ^= 0x55; });
  CHECK(diff(layout, retail.bytes, punch.bytes, &d, &error));
  CHECK(d.keeps_layout && same(d.changed, {"Falcon Punch"}));
  // Kick's own model block and the shared image: Kick and Shared.
  const Made kick = make(0, [](std::vector<uint8_t>& b, const Made& m) { b[m.kick] ^= 1; b[m.image0] ^= 1; });
  CHECK(diff(layout, retail.bytes, kick.bytes, &d, &error));
  CHECK(d.keeps_layout && same(d.changed, {"Falcon Kick", kSharedSlot}));
  // A pointer moved (model 2's own block now model 0's): not the same layout.
  const Made moved = make(0, [](std::vector<uint8_t>& b, const Made& m) {
    put32(b, m.model2 + 0xC, get32(b, m.model2 + 0xC) - 0x70);
  });
  CHECK(diff(layout, retail.bytes, moved.bytes, &d, &error));
  CHECK(!d.keeps_layout && !d.relaid.empty());
  // The generator table's offsets changed: laid out anew.
  const Made table = make(0, [](std::vector<uint8_t>& b, const Made&) { b[0x20 + 0x48 + 12 + 3] ^= 0x40; });
  CHECK(diff(layout, retail.bytes, table.bytes, &d, &error));
  CHECK(!d.keeps_layout && d.relaid == "its generator table differs");
  // Laid out anew (16 bytes after the texture bank, every model pointer moved) with only the Punch
  // generator changed: compared by content, Falcon Punch alone.
  const Made relaid = make(16, [](std::vector<uint8_t>& b, const Made& m) { b[m.gen4001] ^= 0x55; });
  CHECK(diff(layout, retail.bytes, relaid.bytes, &d, &error));
  CHECK(!d.keeps_layout && same(d.changed, {"Falcon Punch"}));
  const Made relaid_kick = make(16, [](std::vector<uint8_t>& b, const Made& m) { b[m.kick] ^= 1; });
  CHECK(diff(layout, retail.bytes, relaid_kick.bytes, &d, &error));
  CHECK(!d.keeps_layout && same(d.changed, {"Falcon Kick"}));
  // Not an effect file.
  std::vector<uint8_t> junk(64, 0);
  CHECK(!diff(layout, retail.bytes, junk, &d, &error));

  // Made slot by slot: Punch from one mod, Kick and Shared from the other.
  std::vector<uint8_t> out;
  CHECK(compose(layout, retail.bytes, {&punch.bytes, &kick.bytes, &kick.bytes, nullptr}, &out, &error));
  CHECK(out[retail.gen4001] == punch.bytes[retail.gen4001] && out[retail.kick] == kick.bytes[retail.kick] &&
        out[retail.image0] == kick.bytes[retail.image0]);
  CHECK(diff(layout, retail.bytes, out, &d, &error));
  CHECK(d.keeps_layout && same(d.changed, {"Falcon Punch", "Falcon Kick", kSharedSlot}));
  // Kick from the Punch mod: the disc's Kick.
  CHECK(compose(layout, retail.bytes, {&punch.bytes, &punch.bytes, nullptr, nullptr}, &out, &error));
  CHECK(out == punch.bytes);
  CHECK(compose(layout, retail.bytes, {nullptr, nullptr, nullptr, nullptr}, &out, &error) && out == retail.bytes);
  CHECK(!compose(layout, retail.bytes, {&relaid.bytes, nullptr, nullptr, nullptr}, &out, &error));
  std::puts("effect_slots: slots owned, changes found by slot, layout kept or not, files made slot by slot");
  return 0;
}

// ---- the real files ----
bool read_file(const fs::path& p, std::vector<uint8_t>* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// A file of a GameCube disc by name (its file table at 0x424).
bool disc_file(const fs::path& iso, const std::string& name, std::vector<uint8_t>* out) {
  std::ifstream in(iso, std::ios::binary);
  if (!in) return false;
  uint8_t head[8];
  in.seekg(0x424);
  in.read(reinterpret_cast<char*>(head), 8);
  const auto be = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
  const uint32_t at = be(head), size = be(head + 4);
  if (!in || size > (16u << 20)) return false;
  std::vector<uint8_t> fst(size);
  in.seekg(at);
  in.read(reinterpret_cast<char*>(fst.data()), size);
  if (!in || size < 12) return false;
  const uint32_t count = be(fst.data() + 8);
  if (uint64_t(count) * 12 > size) return false;
  const size_t names = size_t(count) * 12;
  for (uint32_t i = 1; i < count; ++i) {
    const uint8_t* e = fst.data() + i * 12;
    if (e[0]) continue;
    const size_t n = names + (be(e) & 0xFFFFFF);
    if (n >= size) continue;
    const std::string file(reinterpret_cast<const char*>(fst.data() + n), strnlen(reinterpret_cast<const char*>(fst.data() + n), size - n));
    if (_stricmp(file.c_str(), name.c_str())) continue;
    out->resize(be(e + 8));
    in.seekg(be(e + 4));
    in.read(reinterpret_cast<char*>(out->data()), out->size());
    return bool(in);
  }
  return false;
}

std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
  return s.empty() ? "nothing" : s;
}

int real() {
  const char* iso = std::getenv("MELEE_TEST_ISO");
  const char* dir = std::getenv("MU_FX_SLOTS_DIR");
  if ((!iso || !*iso) && (!dir || !*dir)) {
    std::puts("MELEE_TEST_ISO and MU_FX_SLOTS_DIR unset; skipping the real files");
    return 77;
  }
  std::string error;
  if (iso && *iso) {
    // Every file the table covers maps, and each of its named slots holds something.
    for (const auto& target : table_files()) {
      std::vector<uint8_t> bytes;
      Layout layout;
      CHECK(disc_file(iso, target, &bytes));
      if (!map_slots(target, bytes, &layout, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
      for (const auto& name : table_slots(target)) {
        const int i = layout.slot_index(name);
        if (i < 0 || !layout.listed[size_t(i)]) { std::fprintf(stderr, "%s: %s holds nothing\n", target.c_str(), name.c_str()); return 1; }
      }
      std::string line;
      for (size_t i = 0; i < layout.slots.size(); ++i)
        if (layout.listed[i]) line += (line.empty() ? "" : ", ") + layout.slots[i];
      std::printf("%s: %s\n", target.c_str(), line.c_str());
    }
  }
  if (dir && *dir) {
    const fs::path root = fs::u8path(dir);
    std::vector<uint8_t> ca, fx;
    Layout lca, lfx;
    CHECK(read_file(root / "retail" / "EfCaData.dat", &ca) && read_file(root / "retail" / "EfFxData.dat", &fx));
    CHECK(map_slots("EfCaData.dat", ca, &lca, &error) && map_slots("EfFxData.dat", fx, &lfx, &error));
    const auto check = [&](const Layout& l, const std::vector<uint8_t>& retail, const fs::path& file, Diff* d) {
      std::vector<uint8_t> mod;
      if (!read_file(file, &mod) || !diff(l, retail, mod, d, &error)) return false;
      std::printf("%s: %s, %s%s\n", file.filename().u8string().c_str(), joined(d->changed).c_str(),
                  d->keeps_layout ? "keeps the layout" : "laid out anew: ", d->relaid.c_str());
      return true;
    };
    Diff d;
    // The stand-in's Honduran Falcon Punch: the punch alone.
    CHECK(check(lca, ca, root / "standin-data2" / "files" / "EfCaData.dat", &d));
    CHECK(d.keeps_layout && same(d.changed, {"Falcon Punch"}));
    CHECK(check(lca, ca, root / "standin-data2" / "files" / "EfCaData-crimson.dat", &d));
    CHECK(d.keeps_layout && !d.changed.empty());
    // American Falcon Effects (16884): every slot, same layout; 16643 the same and its textures.
    CHECK(check(lca, ca, root / "real-files" / "16884.dat", &d));
    CHECK(d.keeps_layout);
    for (const auto& name : table_slots("EfCaData.dat")) CHECK(std::find(d.changed.begin(), d.changed.end(), name) != d.changed.end());
    CHECK(check(lca, ca, root / "real-files" / "16643.dat", &d));
    CHECK(d.keeps_layout);
    for (const auto& name : table_slots("EfCaData.dat")) CHECK(std::find(d.changed.begin(), d.changed.end(), name) != d.changed.end());
    // Blue Laser Ring (17101): Fox's Shine and laser ring.
    CHECK(check(lfx, fx, root / "real-files" / "17101.dat", &d));
    CHECK(d.keeps_layout && same(d.changed, {"Shine", "Laser ring"}));
    // The ones laid out anew, whichever file they are.
    for (const char* id : {"16641", "17113", "16765", "16863", "16895", "17117"}) {
      const fs::path file = root / "real-files" / (std::string(id) + ".dat");
      if (check(lca, ca, file, &d) || check(lfx, fx, file, &d)) CHECK(!d.keeps_layout);
      else std::printf("%s.dat: neither EfCaData nor EfFxData (%s)\n", id, error.c_str());
    }
  }
  std::puts("effect_slots: the real files' slots and changes as the study found them");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--real") == 0) return real();
  return synthetic();
}
