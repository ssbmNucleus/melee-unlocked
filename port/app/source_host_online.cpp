// Source Port host: online play. Content views per mode, the Direct opponent on Slippi Dolphin,
// --online-test, the online select's options (MU_CMD_ONLINE_OPTION), the Slippi menu switches.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {
// --online-test ranked plays one whole best-of-3 set: after each game the harness
// locks the same character in again, and the run ends once the set's room is done.
constexpr int32_t kGameModeDebugVs = 0x0E;   // GM_DEBUG_VS: the harness's (and Slippi playback's) major

// Matchmaking uses retail gameplay. Direct peers may agree to use the same mod.
// A Direct opponent on Slippi Dolphin (no build message): that match plays the retail game. Set and
// cleared by slippi_online.cpp (set_plain_opponent_handler); g_content_mode is the mode last applied.
bool g_plain_opponent = false;
int g_content_mode = -1;
slippi::online::LocalBuild content_build_for_mode(int mode) {
  const bool mods = !g_view_alias.empty();
  slippi::online::LocalBuild build;
  build.fingerprint = g_direct_fingerprint.empty() ? g_mod_fingerprint : g_direct_fingerprint;
  build.mod_view = slippi::online::native_source_mod_view(mods, mode, g_plain_opponent,
                                                         gx::RenderOptions::live_mods_in_direct(),
                                                         g_cosmetic_pack || g_training_only);
  // Akaneia and ACE: their added fighters are compiled into this game library (an Akaneia disc is
  // refused without them), so Direct and Teams may pick them, as on the Static Recomp. Before this
  // only Static mod boots said so, and the Source Port refused every added fighter online.
  build.extended_content = build.mod_view && g_mod_gameplay_profile == slippi::online::NativeGameplayProfile::Akaneia;
  build.name = g_mod_display_name;
  build.allow_unverified = gx::RenderOptions::live_mods_dolphin_ok();
  return build;
}

void apply_content_mode(int mode) {
  const bool mods = !g_view_alias.empty();
  const auto build = content_build_for_mode(mode);
  const bool retail = mods && !build.mod_view;
  g_content_mode = mode;
  if (retail != g_retail_view)
    host::log("content: %s view (%s)", retail ? "retail" : "mod",
              mode < 0 ? "offline" : g_cosmetic_pack ? "online cosmetic look" :
              mode == 2 && retail ? "Direct, the opponent is on Slippi Dolphin" :
              mode == 2 ? "Direct" :
              "online mode that plays the retail game");
  g_retail_view = retail;
  mods::status().retail_view = retail;   // the panel's notice counts the pack skins swapped in this view
  slippi::online::set_local_build(build);
}

// slippi_online.cpp's word on the Direct opponent (see g_plain_opponent). The view follows at once:
// nothing of the match is loaded yet when the build wait ends.
void plain_opponent(bool plain, const char* why) {
  if (plain == g_plain_opponent) return;
  g_plain_opponent = plain;
  if (!g_view_alias.empty()) apply_content_mode(g_content_mode);
  if (plain && why) host::log("content: standard view for this opponent: %s", why);
}

void prepare_practice_content(int mode, bool activate) {
  if (activate) apply_content_mode(mode);
  else slippi::online::set_local_build(content_build_for_mode(mode));
  host::log("native practice content: mode %d, advertised %s, active files %s%s", mode,
            slippi::online::local_build().mod_view ? "mod" : "retail",
            g_retail_view || g_view_alias.empty() ? "retail" : "mod",
            activate ? ", boundary activation" : "");
}

// --online-test: the harness enters an online match without menus. The first time the game asks,
// matchmaking starts (local test peering, from --local-peer); the game then waits for the match.
int32_t h_online_test_match(MuOnlineMatch* out) {

#ifdef MELEE_NO_SLIPPI
  // A session asks for a match through the neutral state (netplay_state.h). The game calls this at
  // boot, to pick its first scene, and again in that scene before it waits for the match state.
  const host::netplay::MatchRequest request = host::netplay::match_request();
  if (!request.requested) return 0;
  if (!g_online_test_started) {
    g_online_test_started = true;
    apply_content_mode(request.mode);
    host::log("netplay: the game enters a requested match (mode %d, pad port %d)", request.mode, request.local_port);
  }
  if (out) {
    std::memset(out, 0, sizeof *out);
    out->mode = (uint8_t)request.mode;
    out->local_port = (uint8_t)(request.local_port & 3);
  }
  return request.harness ? 1 : 2;
#else
  // A peer-to-peer session asked for the match (--p2p-*, source_p2p.cpp): the same answer as the
  // build without the Slippi layer gives, and Slippi's matchmaking is never started. No such
  // request (every run without --p2p-*): the Slippi path below, unchanged.
  const host::netplay::MatchRequest request = host::netplay::match_request();
  if (request.requested) {
    if (!g_online_test_started) {
      g_online_test_started = true;
      apply_content_mode(request.mode);
      host::log("netplay: the game enters a requested match (mode %d, pad port %d)", request.mode, request.local_port);
    }
    if (out) {
      std::memset(out, 0, sizeof *out);
      out->mode = (uint8_t)request.mode;
      out->local_port = (uint8_t)(request.local_port & 3);
    }
    return request.harness ? 1 : 2;
  }
  const auto& lobby = slippi::online::config();
  const bool from_lobby = !lobby.lobby_code.empty();
  if (g_online_test_mode < 0 && !from_lobby) return 0;
  const int mode = from_lobby ? 2 : g_online_test_mode;
  const uint8_t character = from_lobby ? (uint8_t)lobby.lobby_character : g_online_test_character;
  if (!g_online_test_started) {
    g_online_test_started = true;
    apply_content_mode(mode);   // a launcher or harness match skips the online menu's mode pick
    std::string error;
    // Tests: MELEE_ONLINE_TEST_CODE names the opponent of a harness Direct match.
    const char* test_code = host::test_env::getenv("MELEE_ONLINE_TEST_CODE");
    // Teams solo queue requires an empty code, including when a Direct test code is set.
    const std::string connect_code = from_lobby ? lobby.lobby_code : mode == 3 ? "" : test_code ? test_code : "PEER#001";
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    // Replace only the menu/CSS actions. Game-side selection sends the real lineup command
    // and computes the real disc budget before native_start_match uses the normal search.
    if (!from_lobby && g_online_test_squad.count) {
      int32_t picks[9] = {g_online_test_squad.count};
      for (unsigned i = 0; i < g_online_test_squad.count; ++i) {
        picks[1 + 2*i] = g_online_test_squad.members[i].kind;
        picks[2 + 2*i] = g_online_test_squad.members[i].color;
      }
      if (!g_game.practice || g_game.practice(MU_SQUAD_TEST_SETUP, picks,
                                             1 + 2*g_online_test_squad.count) != 0) {
        host::log("squad: cancel: harness lineup/budget preparation failed");
        host::request_exit(2);
        return 0;
      }
    }
#endif
    if (slippi::online::native_start_match(mode, connect_code, character,
                                           g_online_test_color, &error))
      host::log("online launch: searching (mode %d, character %u, color %u)", mode,
                character, g_online_test_color);
    else {
      host::log("online test: matchmaking did not start: %s", error.c_str());
      if (from_lobby) host::request_exit(2);
    }
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    // Negative fixture: submit an invalid second lineup through the SAME game/host command.
    // Search keeps the valid squad ticket; lock-in then cancels on the production protocol path.
    if (!from_lobby && g_online_test_squad.count && g_online_test_squad_cancel) {
      const uint8_t invalid[] = {1, 1, 26, 0}; // invalid external character
      uint8_t response[MU_SLIPPI_RESPONSE_CAPACITY];
      uint32_t got = 0;
      h_slippi_command(MU_SQUAD_COMMAND, invalid, sizeof invalid, response, sizeof response, &got);
    }
#endif
  } else if (mode == 0 && !from_lobby) {
    // Ranked: the match scene again after a game of the set; the next game needs a lock-in.
    if (slippi::online::native_ranked_next_game(character, g_online_test_color))
      host::log("online test: ranked, locked in for the set's next game");
  }
  if (out) {
    std::memset(out, 0, sizeof *out);
    out->mode = (uint8_t)mode;
    out->local_port = 0;
  }
  return from_lobby ? 2 : 1; // ABI: 1 = diagnostic harness, 2 = user-approved lobby match.
#endif
}

// Slippi's online flow is game mode 8, asked of the game itself (practice bridge, scene query).
bool native_online_mode() {
  if (!g_game.practice) return false;
  int32_t scene[2] = {0, 0};
  return g_game.practice(MU_PRACTICE_SCENE, scene, 2) == 0 && scene[0] == 8;
}

// MU_CMD_ONLINE_OPTION, from h_slippi_command (source_host.cpp).
bool online_option_command(const uint8_t* p, std::vector<uint8_t>& reply) {
  // With two bytes: an online select option (shim/mu_online_options.c), its number and the
  // new value (0xFF only asks). The answer: the value now, and whether the select offers it
  // (not in a Vanilla run or a replay). Option 0 is local only; option 1 (Items) reaches the
  // match only as every player's game agrees on it.
  const bool available = !host::gameplay::vanilla_only() && !host::options.vanilla_game && !g_replaying;
  if (p[0] == 0) {   // 0: opponents' skins (the "opponentskins" setting)
    if (available && p[1] <= 1 && (p[1] != 0) != host::skins::enabled()) {
      host::skins::set_enabled(p[1] != 0);
      host::log("online options: opponents' skins %s (from the character select)", p[1] ? "on" : "off");
      gx::RenderOptions::live_online_option_changes().fetch_add(1);
    }
    reply = {(uint8_t)(host::skins::enabled() ? 1 : 0), (uint8_t)(available ? 1 : 0)};
  }
#ifndef MELEE_NO_SLIPPI
  else if (p[0] == 1) {   // 1: Items in Direct and Teams (the "onlineitems" setting, slippi_online.h)
    // This one reaches the match, only as every player's game agrees (items_option). The answer
    // goes on: the status (ItemsStatus), the port + 1 of the player keeping items off (0 none or
    // this player), and whether this player is locked in (the choice waits). Shown or not, and the
    // choice in effect, both come from items_offered() (a Vanilla run: not shown, and Off is what is
    // sent, whatever "onlineitems" says); a replay never has an online session to send it to.
    if (!g_replaying && p[1] <= 1) {
      const bool was = slippi::online::config().items.load();
      if (slippi::online::set_items_choice(p[1] != 0) && was != slippi::online::config().items.load())
        gx::RenderOptions::live_online_option_changes().fetch_add(1);
    }
    const slippi::online::ItemsOption o = slippi::online::items_option();
    const bool shown = !g_replaying && o.status != slippi::online::ITEMS_OTHER_MODE;
    reply = {(uint8_t)(o.choice && o.status != slippi::online::ITEMS_UNSUPPORTED ? 1 : 0), (uint8_t)(shown ? 1 : 0),
             (uint8_t)o.status, (uint8_t)(o.blocker + 1), (uint8_t)(o.locked ? 1 : 0)};
  }
  else if (p[0] == 2) {   // 2: Squad Strike in Direct (the "onlinesquad" setting), answered as Items: shown
    // or not and the choice in effect come from squad_offered() (a Vanilla run: not shown, Off is sent)
    if (available && p[1] <= 1) {
      const bool was = slippi::online::config().squad.load();
      if (slippi::online::set_squad_choice(p[1] != 0) && was != slippi::online::config().squad.load())
        gx::RenderOptions::live_online_option_changes().fetch_add(1);
    }
    const slippi::online::ItemsOption o = slippi::online::squad_option();
    const bool shown = available && o.status != slippi::online::ITEMS_OTHER_MODE;
    reply = {(uint8_t)(o.choice && o.status != slippi::online::ITEMS_UNSUPPORTED ? 1 : 0), (uint8_t)(shown ? 1 : 0),
             (uint8_t)o.status, (uint8_t)(o.blocker + 1), (uint8_t)(o.locked ? 1 : 0)};
  }
#endif
  return true;
}

// MU_CMD_ONLINE_CONTENT, from h_slippi_command (source_host.cpp): custom fighters online
// (host/online_fighters.h, docs/online-custom-content.md). An op byte first.
//   0: the online select screen's line about this player's own pick. Reply: spinner (0/1), then the
//      text (at most 60).
//   1: which installed fighters a select screen shows, and which of them may be picked now. Reply:
//      online allowed (0/1), the bits of the roster fighters the online select shows (u32 BE), the
//      bits of every installed one (u32 BE), the bits of the shown ones every player in the session
//      has (u32 BE; the rest are greyed out).
//   2 k: the line for roster fighter k while it is greyed out ("P2 does not have Slippy installed.").
bool online_content_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  namespace of = host::online_fighters;
  if (n < 1) return true;
  if (p[0] == 0) {
    bool spinner = false;
    std::string text = of::line(&spinner);
    if (text.size() > 60) text.resize(60);
    reply.push_back(spinner ? 1 : 0);
    reply.insert(reply.end(), text.begin(), text.end());
  } else if (p[0] == 1) {
    const bool allowed = !g_replaying && of::enabled() && of::roster_usable();
    const uint32_t offered = allowed ? of::offered_online() : 0u;
    const uint32_t installed = of::installed_mask();
    const uint32_t selectable = allowed ? of::selectable_mask() & offered : 0u;
    reply = {(uint8_t)(allowed ? 1 : 0)};
    for (const uint32_t v : {offered, installed, selectable})
      for (int s = 24; s >= 0; s -= 8) reply.push_back((uint8_t)(v >> s));
  } else if (p[0] == 2 && n == 2) {
    std::string text = of::reason(p[1]);
    if (text.size() > 60) text.resize(60);
    reply.insert(reply.end(), text.begin(), text.end());
  }
  return true;
}

}  // namespace host_internal


// --match <stage>:<p1>[:<p2>...], each player <kind>[/c<level>][/x<costume>], all numbers decimal
// or 0x-hex. For example "0x14:9:12/c9" is Onett, one human, one level 9 CPU.
bool set_online_test(const char* spec) {
  std::string s = spec ? spec : "";
  std::string mode = s.substr(0, s.find(':'));
  int id = mode == "ranked" ? 0 : mode == "unranked" ? 1 : mode == "direct" ? 2 : mode == "teams" ? 3 : -1;
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  if (mode == "squad") {
    MuSquadSide lineup{};
    if (s.size() < 7 || !slippi::squad_test::parse(s.c_str() + 6, &lineup)) return false;
    g_online_test_squad = lineup;
    g_online_test_mode = 1; // Unranked, exactly the Squad row's mode
    g_online_test_character = lineup.members[0].kind;
    g_online_test_color = lineup.members[0].color;
    return true;
  }
  g_online_test_squad = {};
#endif
  if (id < 0) return false;
  int character = 2, color = 0;
  size_t colon = s.find(':');
  if (colon != std::string::npos) {
    char* end = nullptr;
    character = (int)std::strtol(s.c_str() + colon + 1, &end, 10);
    if (end && *end == ':') color = (int)std::strtol(end + 1, &end, 10);
    if (!end || *end != '\0' || character < 0 || character > 255 || color < 0 || color > 5) return false;
  }
  g_online_test_mode = id;
  g_online_test_character = (uint8_t)character;
  g_online_test_color = (uint8_t)color;
  return true;
}

#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
bool check_squad_test(bool local_peer, bool invalid_lineup, bool net_delay) {
  if ((g_online_test_squad.count || invalid_lineup || net_delay) && !local_peer) return false;
  if ((invalid_lineup || net_delay) && !g_online_test_squad.count) return false;
  g_online_test_squad_cancel = invalid_lineup;
  return true;
}
#endif

void set_slippi_menus(bool on) { g_slippi_menus_requested = on; }
void set_resume_online() { g_resume_online = true; }

}  // namespace source_port
