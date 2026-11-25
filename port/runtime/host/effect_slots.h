// An effect file's named slots (Falcon Punch, Falcon Kick, ...): which model effects, particle
// generators and particle texture groups of a fighter's EfXxData.dat make each effect a player sees,
// which bytes of the disc's file each slot owns, and which slots a mod's file changes. A mod that keeps
// the disc file's layout (every table, offset and pointer the same; only the bytes of effects differ)
// can be merged with others slot by slot; one laid out anew is taken whole ("All effects").
//
// The file (ef/efasync.c, sysdolphin/baselib/particle.c): the root "eff...DataTable" holds word 0 the
// particle command bank, word 1 the particle texture bank, then EF_EffectDesc records (0x14 bytes:
// lifetime, joint, animjoint, matanim, shapeanim), one per model effect. The command bank: u16 version,
// the first generator id (bank * 1000) and a count, then bank-relative offsets of the generator records
// (a 0x3C head, its texture group at +2, then bytecode). The texture bank: a count, then bank-relative
// offsets of the groups (a 0x18 head and a table of image and palette offsets).
//
// The table of slots was read off the game's code (tools/effect_slot_survey.py): the requests a move
// makes (efSync/efAlt/efAsync), the model effects and generators each spawns, the generators a model's
// particle keys start and those a generator's program starts. A generator two slots start belongs to
// "Shared effects"; a texture group belongs to the one slot whose generators draw from it, else to
// "Shared effects"; whatever no slot names is "Other effects".
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace host::cosmetics::effect_slots {

inline constexpr const char* kSharedSlot = "Shared effects";
inline constexpr const char* kOtherSlot = "Other effects";
inline constexpr const char* kAllSlots = "All effects";   // a whole file (the menu's row; never a slot)

// The table's named slots of a disc file ("EfCaData.dat", any case), in the menu's order; empty when the
// table has no entry for it.
std::vector<std::string> table_slots(const std::string& target);
// The disc files the table covers ("EfCaData.dat", ...).
std::vector<std::string> table_files();

// The disc file's bytes by slot.
struct Layout {
  std::string target;                 // the disc file, as the table spells it
  std::vector<std::string> slots;     // named slots with bytes, then "Shared effects" (when it has bytes),
                                      // then "Other effects" (always: the file's tables)
  std::vector<bool> listed;           // a slot the menu shows: one that holds a model effect, a
                                      // generator or a texture group (Other only when the table left one)
  std::vector<uint8_t> owner;         // per byte of the file, an index into slots
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> ranges;   // per slot: its bytes, [begin, end)
  // The parts' slots, for comparing a file laid out anew by content: per model effect, per generator
  // id, per texture group; `other` is Other's index.
  std::vector<int> model_slot;
  std::map<int, int> gen_slot;
  std::vector<int> group_slot;
  int other = -1;
  int slot_index(const std::string& name) const;
};

// The slots of `retail` (the disc's own file `target`). False (with a reason) when the table has no
// entry for it or the file is not an effect file this reads.
bool map_slots(const std::string& target, const std::vector<uint8_t>& retail, Layout* out, std::string* error);

// What a mod's file changes against the disc's.
struct Diff {
  bool keeps_layout = false;          // mergeable slot by slot
  std::string relaid;                 // why not (empty when it keeps the layout)
  std::vector<std::string> changed;   // the slots whose bytes (or, laid out anew, content) differ, in order
};
// `layout` from map_slots over `retail`. False (with a reason) when `mod` is not the same effect file.
bool diff(const Layout& layout, const std::vector<uint8_t>& retail, const std::vector<uint8_t>& mod, Diff* out,
          std::string* error);

// How many model effects (EF_EffectDesc) an effect file holds, empty ones at the table's end left out;
// -1 when it is not an effect file this reads. The game takes a model effect's descriptor by its id with
// no bound (ef/eflib.c, efLib_Create's `desc`), so a file with fewer than the disc's has it read past
// its table: a crash, or garbage drawn.
int model_effects(const std::vector<uint8_t>& file);

// The fields of root `root`'s first `size` bytes (a structure of pointers and words: ftData) that differ
// between two files, each pointer's target compared as a graph (a file laid out anew still compares, and a
// block's zero padding up to the next is not a change): their offsets. False (with a reason) when either is
// not an archive with that root.
bool changed_fields(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::string& root,
                    uint32_t size, std::vector<uint32_t>* changed, std::string* error);

// The served file from slot sources: `retail` with each listed slot's bytes copied from its source (a
// file that keeps the layout; nullptr for the disc's own). `sources` is indexed like layout.slots.
bool compose(const Layout& layout, const std::vector<uint8_t>& retail,
             const std::vector<const std::vector<uint8_t>*>& sources, std::vector<uint8_t>* out,
             std::string* error);

}  // namespace host::cosmetics::effect_slots
