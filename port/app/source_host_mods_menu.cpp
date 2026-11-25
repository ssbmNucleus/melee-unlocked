// Source Port host: the in-game mod browser and the Mods menu (MU_CMD_MOD_BROWSER).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"
#include "player_fx_test.h"   // MU_CMD_MOD_BROWSER 19: per-player effect files (prototype test switch)

namespace source_port {
namespace host_internal {
// ---- the in-game mod browser (shim/mu_mod_browser.c, runtime/host/mod_browser.h) ----
// payload[0] 0: the page to show. payload: tab, page (big-endian u16), sort, fighter (an index of
//   host::mod_browser::fighters(), 0 all), then the search text (0 to 24 ASCII bytes).
//   reply: status (0 loading, 1 ready, 2 error), tab, page u16, pages u16, has_next, row count,
//   a 64-byte message, then per row: id u32, state, downloads u32, title[44], author[20], note[40].
// payload[0] 2: the fighter filter's names. reply: a count, then 20 bytes a name.
// payload[0] 1: download and install a mod. payload: id u32. reply: 1 when it was queued.
// Text fields are ASCII, ended by 0 and filled with 0. (MU_CMD_MOD_BROWSER)
void put_text(std::vector<uint8_t>& out, const std::string& text, size_t width) {
  const size_t start = out.size();
  out.resize(start + width, 0);
  size_t at = 0;
  for (size_t i = 0; i < text.size() && at + 1 < width; ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (c >= 0x80) {
      // The game's menu font is ASCII here: one '?' for a whole multi-byte character.
      if ((c & 0xC0) != 0x80) out[start + at++] = '?';
    } else if (c >= 32) {
      out[start + at++] = c;
    }
  }
}
void mod_browser_command(const uint8_t* p, size_t n, std::vector<uint8_t>& reply) {
  reply.assign(1, 0);
  host::mod_browser::set_blocked(g_replaying || host::cosmetics::online_active());
  if (p[0] == 1 && n == 5) {
    const int id = (int)((uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4]);
    reply[0] = host::mod_browser::download(id) ? 1 : 0;
    host::log("mod browser: download of mod %d %s", id, reply[0] ? "queued" : "refused");
    return;
  }
  if (p[0] == 2 && n == 1) {
    const auto& names = host::mod_browser::fighters();
    reply.assign(1, (uint8_t)names.size());
    for (const auto& name : names) put_text(reply, name.empty() ? "All" : name, 20);
    return;
  }
  if (p[0] == 8 && n == 1) {
    // The Mods menu in Trophies' place (shim/mu_mods_menu.c): 1 on, 0 Trophies as the game has it
    // (a Vanilla run, a replay). Then the categories this release has (release_scope.h mask(), bit c
    // category c): the game dims the others' rows, their words read "Coming soon." and Manage leaves
    // them out.
    reply = {(uint8_t)(host::gameplay::vanilla_only() || host::options.vanilla_game || g_replaying ? 0 : 1),
             host::release_scope::mask()};
    // (asked at each main menu load: an install that ended after its list was left is published here)
    if (reply[0] && !host::cosmetics::online_active()) republish_from_mods_menu();
    static bool logged = false;
    if (!logged) {
      logged = true;
      host::log("mods menu: %s (vanilla only %d, vanilla game %d, replay %d)", reply[0] ? "on" : "off",
                (int)host::gameplay::vanilla_only(), (int)host::options.vanilla_game, (int)g_replaying);
      host::log("mods menu: categories: %s", host::release_scope::describe().c_str());
    }
    return;
  }
  if (p[0] == 19 && n == 1) {
    // The CSS is about to open its archive. An install can finish after the main menu's check.
    if (!g_replaying && !host::cosmetics::online_active()) publish_menu_mods(false);
    return;
  }
  // The doubles lobby list runs on the same list screen (shim/mu_lobby_list.c, host/lobby_directory.h).
  if (p[0] >= 9 && p[0] <= 14 && host::lobby_directory::gallery(p, n, reply)) return;
  if (p[0] >= 9 && p[0] <= 14) {
    // The Mods menu's lists, viewer and installs (shim/mu_mods_gallery.c, host/mods_menu.h). Off in a
    // Vanilla run and in a replay; nothing changes while an online session is queued or running.
    if (host::gameplay::vanilla_only() || host::options.vanilla_game || g_replaying) { reply = {0}; return; }
    // A category this release does not have (release_scope.h): nothing opens, lists or installs, even
    // asked directly (the game never opens one: A on its row gives the menus' error sound).
    static bool category_off = false;
    if (p[0] == 9 && (n == 3 || n == 4))
      category_off = !host::release_scope::gate(p[2], "its list and installs are refused");
    if (category_off) { reply = {0}; return; }
    host::mods_menu::set_blocked(host::cosmetics::online_active());
    republish_from_mods_menu();
    const int row = n >= 3 ? (int)p[1] << 8 | p[2] : -1;
    switch (p[0]) {
    case 9:   // open: mode (0 Browse, 1 Manage), category (0 Characters, 1 Stages, 2 Effects, 3 Fighters), [1: the same
              // list again, back from the search's name entry]. reply: 1
      // Fighters install into this run's fighters folder (empty in a Vanilla launch or with --fighters-dir none).
      if (n == 3 || n == 4) host::mods_menu::set_fighters_folder(roster_served() ? installed_fighters_folder().u8string() : std::string());
      if (n == 3 || n == 4) { host::mods_menu::open(p[1], p[2], n == 4 && p[3] == 1); reply = {1}; }
      return;
    case 10:  // the list: first row (u16), how many (at most 60). reply: status u8, rows u16, the row to
              // start on u16 (the fighters' list: the one chosen last), 0 u16 (one continuous list: no
              // pages), flags (1 the fighters' list, 2 more posts after its end, still to read, 4 a
              // character filter, 8 a pack's cards, 16 the site was slow, 32 a search, 64 more on their
              // way, 128 more failed), filter[20], message[48] (the bar's count), pack title[48], then per
              // row name[40], right column[12], state u8 (| 0x40 a pack, | 0x80 "Install all")
      if (n == 4) host::mods_menu::list(row, p[3], reply);
      return;
    case 11:  // change: 1 read the posts after the list's end (again after a failure), 2/3 previous/next
              // character, 4 try again, 5 open the pack of a row (u16 after the op), 6 back from a pack,
              // 7 install all; 8 Browse's search, its words after the op (at most 32 bytes, none to end
              // it); 9 the fighters' list: that row's fighter; 10 back to the fighters. reply: 1 changed
      if (n >= 2 && p[1] == 8 && n <= 2 + 32)
        reply = {(uint8_t)(host::mods_menu::search(std::string((const char*)p + 2, n - 2)) ? 1 : 0)};
      else if (n == 2) reply = {(uint8_t)(host::mods_menu::change(p[1]) ? 1 : 0)};
      else if (n == 4) reply = {(uint8_t)(host::mods_menu::change(p[1], (int)p[2] << 8 | p[3]) ? 1 : 0)};
      return;
    case 12:  // the viewer: row (u16). reply: state, picture status, picture generation u32, name[48],
              // kind[48], author[48], downloads[48], action line[48], description[400], the card's file
              // id u32; 0xFF no such row (or not a card)
      if (n == 3) host::mods_menu::detail(row, reply);
      // [1]: a pack's row. reply: 1, picture status, picture generation u32, name[48], by[48],
      // downloads[48], its count[48], the post's description[400]; 0xFF not a pack's row
      else if (n == 4 && p[3] == 1) host::mods_menu::pack_detail(row, reply);
      return;
    case 13:  // the viewer's picture: row (u16). reply: 0 not yet, or 1, generation u32, size u32, the archive
      if (n == 3) host::mods_menu::picture(row, reply);
      // [1]: a pack's row's picture, the same reply
      else if (n == 4 && p[3] == 1) host::mods_menu::pack_picture(row, reply);
      // [2, line]: the list's cursor is on that row, line `line` of the screen: the pictures near it are
      // fetched ahead of the viewer. reply: 1
      else if (n == 5 && p[3] == 2) {
        host::mods_menu::prefetch(row, p[4]);
        reply = {1};
      }
      return;
    case 14:  // A in the viewer: install (Browse) or remove (Manage) that row; [the file id u32 the viewer
              // showed: refused when the row is another card now]. reply: 1 queued
      if (n == 3) reply = {(uint8_t)(host::mods_menu::action(row) ? 1 : 0)};
      else if (n == 7)
        reply = {(uint8_t)(host::mods_menu::action(row, (int)((uint32_t)p[3] << 24 | (uint32_t)p[4] << 16 |
                                                              (uint32_t)p[5] << 8 | p[6]))
                               ? 1
                               : 0)};
      return;
    }
    return;
  }
  if (p[0] == 3 && n == 1) {
    // The fighters installed beside the game, for a disc with no m-ex table (shim/mu_mex_roster.c):
    // how many, and a number that changes with the set. None in a replay: it plays the disc it names.
    // Then the serial of the sound files served (fighter_sound.h; 0 the disc's own): the game loads
    // smash2.sem and nr_name.ssm again when it is not the one it has.
    uint32_t count = 0, generation = 0;
    if (roster_served() && !g_replaying && !installed_fighters_folder().empty()) {
      publish_roster();
      count = (uint32_t)std::min<size_t>(host::roster::fighters().size(), 16);
      generation = count ? host::roster::generation() : 0;
    }
    reply = {(uint8_t)count, (uint8_t)(generation >> 24), (uint8_t)(generation >> 16), (uint8_t)(generation >> 8),
             (uint8_t)generation};
    append_be32(reply, roster_served() ? g_sound_serial : 0u);
    return;
  }
  if (p[0] == 4 && n == 2) {
    // An installed fighter's record, then what its sounds add (fighter_sound.h), as one reply.
    const auto& list = host::roster::fighters();
    if (roster_served() && p[1] < list.size()) {
      reply = list[p[1]].record;
      if (p[1] < g_sound_tails.size() && reply.size() + g_sound_tails[p[1]].size() <= 4096)
        reply.insert(reply.end(), g_sound_tails[p[1]].begin(), g_sound_tails[p[1]].end());
    }
    return;
  }
  if (p[0] == 5 && n == 7) {
    // A part of an installed fighter's select screen picture (host/roster.h): fighter, which picture,
    // costume, the offset wanted. The reply: 1, width, height, the whole size, then bytes from there.
    const host::roster::Image* image = roster_served() ? host::roster::image(p[1], p[2], p[3]) : nullptr;
    const size_t at = (size_t)p[4] << 16 | (size_t)p[5] << 8 | p[6];
    if (!image || image->data.empty() || at >= image->data.size()) { reply = {0}; return; }
    const size_t size = image->data.size(), part = std::min<size_t>(size - at, 3840);
    reply = {1, (uint8_t)(image->width >> 8), (uint8_t)image->width, (uint8_t)(image->height >> 8),
             (uint8_t)image->height, (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
    reply.insert(reply.end(), image->data.begin() + at, image->data.begin() + at + part);
    return;
  }
  if (p[0] == 6 && n == 1) {
    // Where the fighter plugins come from (mexft/mexft_plugins.c, at each scan). reply[0]: 0 the
    // game library's own rule (MELEE_FIGHTER_PLUGINS, else the fighters folder beside it), 1 none
    // this run, 2 the folder that follows, in the process's code page (UTF-8 with the manifest),
    // which the library's file functions take.
    if (host::gameplay::vanilla_only() || host::options.vanilla_game) { reply = {1}; return; }
#ifdef _WIN32
    // Always a folder of the host's on Windows, never 0: the library's own rule would read a folder
    // nobody checked. It holds only the plugins that passed the approval gate (plugin_gate.h).
    const std::filesystem::path folder = gated_plugin_folder(false);
#else
    if (!host::options.fighters_dir_set) { reply = {0}; return; }
    const std::filesystem::path folder = installed_fighters_folder();
#endif
    if (folder.empty()) { reply = {1}; return; }
    std::string text;
#ifdef _WIN32
    const std::wstring wide = folder.wstring();
    BOOL lossy = FALSE;
    const int bytes = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
      text.resize((size_t)bytes);
      WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(), text.data(), bytes, nullptr,
                          GetACP() == CP_UTF8 ? nullptr : &lossy);
    }
    if (lossy) text.clear();
#else
    text = folder.u8string();
#endif
    // The library's paths are MAX_PATH long with room for a file name; a path it cannot hold is none.
    if (text.empty() || text.size() > 200) {
      host::log("fighters: the plugin folder %s cannot be handed to the game library; no fighter plugins this run",
                folder.u8string().c_str());
      reply = {1};
      return;
    }
    reply.assign(1, 2);
    reply.insert(reply.end(), text.begin(), text.end());
    return;
  }
  if (p[0] == 7 && n >= 4 && n <= 3 + 31) {
    // One port's own costume (shim/mu_port_costume.c; host/opponent_skins.h served_costume): op, port,
    // then the costume file the game would load ("PlFxGr.dat"). op 0 asks for the size: reply 1 and
    // the size (big-endian u32), or 0 for the file as before. op 1 asks for the bytes of the file the
    // last op 0 for that port answered: the reply is the file. Never in a replay, which plays the disc.
    const int port = p[2];
    std::string slot;
    for (size_t i = 3; i < n; ++i) {
      if (p[i] < 33 || p[i] > 126) { reply = {0}; return; }
      slot.push_back((char)p[i]);
    }
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    const bool online = slippi::online::session_mode() >= 0;
    if (g_replaying || port > 3 || !host::skins::served_costume(port, slot, online, &bytes) || !bytes || bytes->empty()) {
      reply = {0};
      return;
    }
    if (p[1] == 0) {
      const uint32_t size = (uint32_t)bytes->size();
      reply = {1, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
    } else if (p[1] == 1) {
      reply.assign(bytes->begin(), bytes->end());
    } else {
      reply = {0};
    }
    return;
  }
  if (p[0] == 19) {
    // Per-player effect files, the prototype's test switch (host/player_fx_test.h; shim/mu_player_fx.c).
    host::player_fx::command(p, n, !g_replaying && slippi::online::session_mode() < 0 && !source_p2p::requested(), reply);
    return;
  }
  if (p[0] >= 15 && p[0] <= 18) {
    // Installed skins as costume slots (host/costume_slots.h; shim/mu_css_skins.c). None in a Vanilla
    // run or a replay.
    if (g_replaying || host::gameplay::vanilla_only() || host::options.vanilla_game ||
        !host::cosmetics::imports_as_costume_slots()) {
      reply = {0};
      return;
    }
    // A picture in parts, as sub-command 5 hands a roster picture over: 1, width, height, the whole
    // size, then bytes from `at`.
    const auto picture_part = [&](const std::vector<uint8_t>* px, int width, int height, size_t at) {
      if (!px || px->empty() || at >= px->size()) { reply = {0}; return; }
      const size_t size = px->size(), part = std::min<size_t>(size - at, 3840);
      reply = {1, (uint8_t)(width >> 8), (uint8_t)width, (uint8_t)(height >> 8), (uint8_t)height, (uint8_t)(size >> 16),
               (uint8_t)(size >> 8), (uint8_t)size};
      reply.insert(reply.end(), px->begin() + (std::ptrdiff_t)at, px->begin() + (std::ptrdiff_t)(at + part));
    };
    switch (p[0]) {
    case 15:   // a fighter's slots: character. reply: 1, count, generation u32, per slot: costume, flags
               // (1 a portrait, 2 a stock icon), name[24]
      if (n == 2) {
        uint32_t generation = 0;
        const auto slots = host::costume_slots::list(p[1], &generation);
        reply = {1, (uint8_t)slots.size(), (uint8_t)(generation >> 24), (uint8_t)(generation >> 16), (uint8_t)(generation >> 8),
                 (uint8_t)generation};
        for (const auto& s : slots) {
          reply.push_back((uint8_t)s.costume);
          reply.push_back((uint8_t)((s.has_csp ? 1 : 0) | (s.has_stock ? 2 : 0)));
          put_text(reply, host::mods_menu::drawable(s.name, 23), 24);
        }
      } else reply = {0};
      return;
    case 16:   // a pick: port, character, slot (0 the retail colour), flags (1 the online screen's own
               // door). {16, 0xFF}: every pick forgotten. reply: 1 and the costume it keeps, or 0
      if (n == 2 && p[1] == 0xFF) {
        host::costume_slots::clear();
        reply = {1};
      } else if (n == 5) {
        int costume = -1;
        std::string why;
        const bool ok = host::costume_slots::pick(p[1], p[2], p[3], (p[4] & 1) != 0, &costume, &why);
        reply = {(uint8_t)(ok ? 1 : 0), (uint8_t)(costume < 0 ? 0xFF : costume)};
      } else reply = {0};
      return;
    case 17:   // a slot's picture: character, slot, what (1 portrait 136x188, 2 stock icon 24x24), offset u24
      if (n == 7 && (p[3] == 1 || p[3] == 2))
        picture_part(host::costume_slots::picture(p[1], p[2], p[3]), p[3] == 1 ? 136 : 24, p[3] == 1 ? 188 : 24,
                     (size_t)p[4] << 16 | (size_t)p[5] << 8 | p[6]);
      else reply = {0};
      return;
    case 18:   // the stock icon of the skin a port's fighter wears: port, offset u24. The player's own
               // skin's (its costume slot), else an opponent's skin's (host/opponent_skins.h worn_stock).
               // reply {2}: that icon is still on its way, ask again later; {0}: none, the retail icon.
      if (n == 5 && p[1] < 4) {
        const size_t at = (size_t)p[2] << 16 | (size_t)p[3] << 8 | p[4];
        if (const auto* own = host::costume_slots::worn_stock(p[1])) {
          picture_part(own, 24, 24, at);
          return;
        }
        std::shared_ptr<const std::vector<uint8_t>> remote;
        const auto state = host::skins::worn_stock(p[1], &remote);
        if (state == host::skins::PortState::kReady) picture_part(remote.get(), 24, 24, at);
        else reply = {(uint8_t)(state == host::skins::PortState::kWorking ? 2 : 0)};
      } else reply = {0};
      return;
    }
    return;
  }
  if (p[0] != 0 || n < 6 || n > 6 + (size_t)host::mod_browser::kMaxSearch) return;
  // Empty in a Vanilla launch or with --fighters-dir none: the browser then installs no fighter.
  host::mod_browser::set_fighters_folder(installed_fighters_folder().u8string());
  host::mod_browser::Query query;
  query.tab = p[1];
  query.sort = p[4];
  query.fighter = p[5];
  for (size_t i = 6; i < n; ++i) {
    if (p[i] >= 32 && p[i] < 127) query.search += (char)p[i];
  }
  const auto view = host::mod_browser::view(query, (p[2] << 8) | p[3]);
  reply.clear();
  reply.push_back((uint8_t)view.status);
  reply.push_back((uint8_t)query.tab);
  reply.push_back((uint8_t)(view.page >> 8)); reply.push_back((uint8_t)view.page);
  const int pages = std::min(view.pages, 0xFFFF);
  reply.push_back((uint8_t)(pages >> 8)); reply.push_back((uint8_t)pages);
  reply.push_back(view.has_next ? 1 : 0);
  const size_t rows = std::min<size_t>(view.rows.size(), host::mod_browser::kRowsPerPage);
  reply.push_back((uint8_t)rows);
  put_text(reply, view.message, 64);
  for (size_t i = 0; i < rows; ++i) {
    const auto& row = view.rows[i];
    const uint32_t id = (uint32_t)row.id, downloads = (uint32_t)std::max(row.downloads, 0);
    reply.insert(reply.end(), {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)row.state,
                               (uint8_t)(downloads >> 24), (uint8_t)(downloads >> 16), (uint8_t)(downloads >> 8), (uint8_t)downloads});
    put_text(reply, row.title, 44);
    put_text(reply, row.author, 20);
    put_text(reply, row.note, 40);
  }
}

}  // namespace host_internal
}  // namespace source_port
