// The Mods menu's Effects (particles, the category menu's third row): an effect's preview picture.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"
#include "mods_menu_stb.h"

namespace host::mods_menu {
namespace menu_internal {

// An effect installed through its own download_url (a DAT alone): its picture for the lists, the first
// of the card's pictures (its preview, then its mod's screenshots) that is a PNG the hardened reader
// decodes in full, at most kMaxPictureBytes.
void attach_effect_preview(const std::string& asset_id, const Card& card) {
  std::vector<std::string> urls = card.pictures;
  bool asked_mod = false;
  for (size_t i = 0;; ++i) {
    if (i == urls.size() && !asked_mod && card.mod_id > 0 && !g_blocked.load()) {
      // None of the card's own: its mod's screenshots (/mods/{id}), once.
      asked_mod = true;
      json j;
      std::string error;
      int status = 0;
      if (get_json(host::mod_browser::api_address() + "/mods/" + std::to_string(card.mod_id), &j, &status, &error) &&
          j.is_object() && j.contains("data") && j["data"].is_object())
        if (const auto list = j["data"].find("screenshots"); list != j["data"].end() && list->is_array())
          for (const auto& s : *list)
            if (s.is_string() && urls.size() < card.pictures.size() + 3 && picture_address(s.get<std::string>()))
              urls.push_back(s.get<std::string>());
    }
    if (i >= urls.size() || g_blocked.load()) break;
    const std::string url = urls[i];
    std::string body, error;
    int status = 0;
    if (!host::mod_browser::get_text(url, &body, kMaxPictureBytes, &error, &status, host::mod_browser::kWaitPicture)) continue;
    const auto* data = reinterpret_cast<const stbi_uc*>(body.data());
    int w = 0, h = 0, n = 0;
    if (body.size() < 8 || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0 ||
        !stbi_info_from_memory(data, (int)body.size(), &w, &h, &n) || w <= 0 || h <= 0 || w > 4096 || h > 4096)
      continue;
    stbi_uc* px = stbi_load_from_memory(data, (int)body.size(), &w, &h, &n, 4);
    if (!px) continue;
    stbi_image_free(px);
    std::error_code ec;
    const fs::path folder = fs::temp_directory_path(ec) / L"MeleeUnlockedNucleus" / (L"pic_" + std::to_wstring(card.file_id));
    fs::create_directories(folder, ec);
    const fs::path file = folder / L"preview.png";
    {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out.write(body.data(), (std::streamsize)body.size());
    }
    const auto result = host::cosmetics::set_effect_preview(asset_id, file.u8string());
    host::log("mods menu: card %d: its picture (%dx%d, from %s) %s%s", card.file_id, w, h, log_host(url).c_str(),
              result.ok ? "kept with " : "not kept: ", result.ok ? asset_id.c_str() : result.message.c_str());
    fs::remove_all(folder, ec);
    if (result.ok) return;
  }
  host::log("mods menu: card %d: no PNG picture for it", card.file_id);
}

// ---- an effect card the effect checks refuse (host::cosmetics::check_effect: broken, not_an_effect) ----
// <settings dir>/CosmeticMods/effect-verdicts.json: per file SHA-256 (the site's, which the download was
// checked against), {"verdict", "reason", "checks"}; one of another checks version is not read, so a later
// build looks again. A card the site gives no hash for is kept by its file id, for the session.
namespace {
std::map<std::string, std::pair<std::string, std::string>> g_refused_files;   // sha256 -> verdict, reason
std::map<int, std::string> g_refused_cards;                                   // file id -> reason
bool g_refused_read = false;
fs::path refused_path() {
  const fs::path cache = fs::u8path(host::skins::cache_folder());
  return cache.empty() ? fs::path() : cache.parent_path() / L"CosmeticMods" / L"effect-verdicts.json";
}
void read_refused_locked() {
  if (g_refused_read) return;
  g_refused_read = true;
  const fs::path path = refused_path();
  std::error_code ec;
  if (path.empty() || fs::file_size(path, ec) > (1u << 20) || ec) return;
  std::ifstream in(path, std::ios::binary);
  const json doc = json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), nullptr, false);
  if (!doc.is_object()) return;
  for (auto it = doc.begin(); it != doc.end(); ++it)
    if (it.key().size() == 64 && it->is_object() && it->value("checks", 0) == host::cosmetics::kEffectChecksVersion &&
        host::cosmetics::effect_verdict_refuses(it->value("verdict", std::string())))
      g_refused_files[it.key()] = {it->value("verdict", std::string()), drawable(it->value("reason", std::string()), 80)};
}
}  // namespace

const std::string* effect_refused_locked(const Card& card) {
  if (card.kind != "effect_visual") return nullptr;
  if (const auto it = g_refused_cards.find(card.file_id); it != g_refused_cards.end()) return &it->second;
  read_refused_locked();
  if (const auto it = g_refused_files.find(lower(card.sha256)); !card.sha256.empty() && it != g_refused_files.end())
    return &it->second.second;
  return nullptr;
}

void effect_refuse_locked(const Card& card, const std::string& verdict, const std::string& reason) {
  g_refused_cards[card.file_id] = reason;
  host::log("mods menu: card %d is an effect the checks refuse (%s): %s; not installable", card.file_id, verdict.c_str(),
            reason.c_str());
  if (card.sha256.size() != 64) return;
  read_refused_locked();
  g_refused_files[lower(card.sha256)] = {verdict, reason};
  const fs::path path = refused_path();
  if (path.empty()) return;
  json doc = json::object();
  for (const auto& [digest, entry] : g_refused_files)
    doc[digest] = {{"verdict", entry.first}, {"reason", entry.second}, {"checks", host::cosmetics::kEffectChecksVersion}};
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path staged = fs::path(path).concat(L".tmp");
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out << doc.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    if (!out) { fs::remove(staged, ec); return; }
  }
  fs::rename(staged, path, ec);
  if (ec) fs::remove(staged, ec);
}

}  // namespace menu_internal
}  // namespace host::mods_menu
