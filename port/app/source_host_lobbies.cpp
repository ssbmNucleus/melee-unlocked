// Source Port host: doubles lobbies from SSBM Nucleus's directory (MU_CMD_LOBBY).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- doubles lobbies (shim/mu_lobby_list.c, runtime/host/lobby_directory.h) ----
// A lobby is a Teams code from SSBM Nucleus's directory; the game types it into its normal Teams
// search. Off in a Vanilla run and in a replay. What the directory gets of this player: the display
// name (filtered; never the connect code), the game build and the client, nothing else. (MU_CMD_LOBBY)
#ifndef MELEE_NO_SLIPPI
host::lobby_directory::Identity lobby_identity() {
  host::lobby_directory::Identity me;
  std::string code, name;
  slippi::online::local_identity(&code, &name);
  me.name = host::lobby_directory::host_name_from(name, code);
  // The game Teams plays (shim/mu_slippi_menu.c MU_SLP_MODE_TEAMS = 3), as its search will say it.
  const auto build = content_build_for_mode(3);
  me.build = host::lobby_directory::build_string(build.mod_view, build.fingerprint);
  me.client = host::lobby_directory::client_string(product::kWireName, MELEE_PORT_VERSION);
  return me;
}
#endif
void lobby_command(const uint8_t* p, size_t n, std::vector<uint8_t>& reply) {
#ifndef MELEE_NO_SLIPPI
  const bool allowed = !host::gameplay::vanilla_only() && !host::options.vanilla_game && !g_replaying;
  host::lobby_directory::command(p, n, allowed, lobby_identity, reply);
#else
  host::lobby_directory::command(p, n, false, nullptr, reply);
#endif
}

}  // namespace host_internal
}  // namespace source_port
