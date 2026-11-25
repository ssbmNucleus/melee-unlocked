// Cosmetic reads: the game's file reads served from the published state, and the online state
// that decides which form is served.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {

static OverrideRead read_form(uint32_t vanilla_file_start, uint32_t file_offset, void* dst, uint32_t size,
                              bool online_now);
OverrideRead read(uint32_t vanilla_file_start, uint32_t file_offset, void* dst, uint32_t size) {
  return read_form(vanilla_file_start, file_offset, dst, size, online_active());
}
OverrideRead read_online_form(uint32_t vanilla_file_start, uint32_t file_offset, void* dst, uint32_t size) {
  return read_form(vanilla_file_start, file_offset, dst, size, true);
}
bool has_online_form(uint32_t vanilla_file_start) {
  auto runtime = std::atomic_load(&g_runtime);
  auto found = runtime->by_start.find(vanilla_file_start);
  return found != runtime->by_start.end() && found->second.online_allowed && found->second.online_bytes &&
         found->second.kind == "character_costume";
}
static OverrideRead read_form(uint32_t vanilla_file_start, uint32_t file_offset, void* dst, uint32_t size,
                              bool online_now) {
  auto runtime = std::atomic_load(&g_runtime);
  auto found = runtime->by_start.find(vanilla_file_start);
  if (found == runtime->by_start.end()) return OverrideRead::NotOverridden;
  if (found->second.kind == kVoiceKind && !found->second.bytes) {
    // A bank file while no skin's voice is chosen for it: the disc's own bank, under the one length
    // the file table carries for this file, with zeros past the disc file's end.
    const uint32_t vanilla_size = found->second.vanilla_size;
    const uint64_t exposed_size = found->second.length;
    if (file_offset > exposed_size || (uint64_t)file_offset + size > exposed_size + 31) return OverrideRead::Failed;
    const uint32_t disc_size = file_offset < vanilla_size ? std::min(size, vanilla_size - file_offset) : 0;
    if (disc_size && !host::disc_read(vanilla_file_start + file_offset, dst, disc_size)) return OverrideRead::Failed;
    if (disc_size < size) std::memset((uint8_t*)dst + disc_size, 0, size - disc_size);
    return OverrideRead::Success;
  }
  if (!found->second.online_allowed && online_now) {
    const uint32_t vanilla_size = found->second.vanilla_size;
    const uint64_t exposed_size = std::max<size_t>(vanilla_size, found->second.bytes->size());
    if (file_offset > exposed_size || (uint64_t)file_offset + size > exposed_size + 31)
      return OverrideRead::Failed;
    const uint32_t disc_size = file_offset < vanilla_size ? std::min(size, vanilla_size - file_offset) : 0;
    if (disc_size && !host::disc_read(vanilla_file_start + file_offset, dst, disc_size))
      return OverrideRead::Failed;
    if (disc_size < size) std::memset((uint8_t*)dst + disc_size, 0, size - disc_size);
    // The archive's length field (its first four bytes) says the length the game was given for
    // this file, which is the override's when that is the longer one (apply_to_fst).
    uint8_t length_field[4]; put_be32(length_field, (uint32_t)exposed_size);
    // The exact-look announcer bank is an SSM, not an archive; keep its disc header intact.
    if (found->second.kind != kLookFileKind || found->second.target_path != "nr_name.ssm")
      for (uint32_t at = file_offset; at < 4 && at - file_offset < size; ++at)
        ((uint8_t*)dst)[at - file_offset] = length_field[at];
    return OverrideRead::Success;
  }
  std::shared_ptr<const std::vector<uint8_t>> form =
      found->second.online_bytes && online_now ? found->second.online_bytes : found->second.bytes;
  if (found->second.kind == "stage_visual" || found->second.kind == kStageLengthKind) {
    // One read of a file is one form of it. The game reads a large file in pieces (512 KB at a time),
    // and a random stage pick can be published between two of them: the pieces then came from two
    // different files (the standard stage's header with a skin's body), the archive's offsets pointed
    // into the wrong data, and the game stopped on a missing symbol or wrote over the file next to
    // it (lbarchive.c:112, "Cannot find symbol ftDataNess"). A read from the start takes the form
    // served now; later pieces keep it while it has the same length and online state. An online
    // state change takes the current form immediately, so an online read never keeps offline bytes.
    // Stage files only: a sound bank is streamed from any offset and must follow its new pick at once.
    static std::mutex pin_mutex;
    static std::unordered_map<uint32_t, stage_read_pin::PinState<std::shared_ptr<const std::vector<uint8_t>>>> pinned;
    std::lock_guard<std::mutex> lock(pin_mutex);
    auto& pin = pinned[vanilla_file_start];
    form = stage_read_pin::pick(pin, form, online_now, file_offset, form->size());
  }
  const auto& bytes = *form;
  uint64_t begin = file_offset, end = begin + size;
  if (end < begin || begin > bytes.size()) return OverrideRead::Failed;
  if (end > bytes.size() && end - bytes.size() > 31) return OverrideRead::Failed;
  size_t available = begin < bytes.size() ? bytes.size() - (size_t)begin : 0;
  size_t copy = std::min<size_t>(size, available);
  if (copy) std::memcpy(dst, bytes.data() + (size_t)begin, copy);
  if (copy < size) {
    // DVD callers commonly align the final transfer to 32 bytes. Only that bounded tail is legal.
    std::memset((uint8_t*)dst + copy, 0, size - copy);
  }
  return OverrideRead::Success;
}

std::atomic<OnlineProbe> g_online_probe{nullptr};
void set_online_probe(OnlineProbe probe) { g_online_probe.store(probe); }
bool online_active() {
  if (host::gameplay::vanilla_only()) return true;   // use only the gameplay-safe forms of cosmetics
  if (g_online_freezes.load(std::memory_order_relaxed)) return true;
  // MELEE_TEST_ONLINE_FORMS (tests only, hidden runs): stage skins are served in their online form
  // offline too, so a capture shows what an online opponent's game draws.
  static const bool test_forms = host::test_env::getenv("MELEE_TEST_ONLINE_FORMS") && std::getenv("MELEE_NO_GC_ADAPTER");
  if (test_forms) return true;
  const OnlineProbe probe = g_online_probe.load();
  return probe && probe();
}
bool online_allowed(uint32_t vanilla_file_start) {
  auto runtime = std::atomic_load(&g_runtime);
  auto found = runtime->by_start.find(vanilla_file_start);
  return found == runtime->by_start.end() || found->second.online_allowed;
}
uint32_t swapped_online_count(uint32_t* stages) {
  auto runtime = std::atomic_load(&g_runtime);
  std::map<std::string, bool> costumes, stage_ids;
  for (const auto& entry : runtime->by_start) {
    if (entry.second.online_allowed) continue;
    (entry.second.kind == "stage_visual" ? stage_ids : costumes)[entry.second.id] = true;
  }
  if (stages) *stages = (uint32_t)stage_ids.size();
  return (uint32_t)costumes.size();
}
uint32_t largest_fighter_growth() {
  auto runtime = std::atomic_load(&g_runtime);
  std::vector<uint32_t> growth;
  for (const auto& entry : runtime->by_start) {
    const RuntimeAsset& a = entry.second;
    if (a.kind == "stage_visual" || !a.bytes || a.bytes->size() <= a.vanilla_size) continue;
    growth.push_back((uint32_t)std::min<size_t>(a.bytes->size() - a.vanilla_size, 0x7FFFFFFFu));
  }
  std::sort(growth.begin(), growth.end(), std::greater<uint32_t>());
  uint64_t sum = 0;
  for (size_t i = 0; i < growth.size() && i < 4; ++i) sum += growth[i];
  return (uint32_t)std::min<uint64_t>(sum, 0x7FFFFFFFu);
}
bool stage_skin_active(const char* stage) {
  const std::string wanted = lower(stage);
  auto runtime = std::atomic_load(&g_runtime);
  for (const auto& entry : runtime->by_start) {
    const RuntimeAsset& a = entry.second;
    if (a.kind == "stage_visual" && a.bytes && lower(a.target_path).find(wanted) != std::string::npos) return true;
  }
  return false;
}
void freeze_for_online_session() { g_online_freezes.store(1, std::memory_order_relaxed); }
void thaw_after_online_session() { g_online_freezes.store(0, std::memory_order_relaxed); }
SessionProfile session_profile() {
  auto runtime = std::atomic_load(&g_runtime);
  return {runtime->generation, runtime->fingerprint, runtime->assets,
          g_online_freezes.load(std::memory_order_relaxed) != 0};
}

}  // namespace host::cosmetics
