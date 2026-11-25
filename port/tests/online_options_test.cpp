// Items and Squad Strike online are offered only where their rule exists (slippi_online.h
// items_offered, squad_offered), never in a Vanilla run (--vanilla-only, --vanilla-game). Where a rule
// is not offered, the choice in effect (items_choice, squad_choice: what SendItemsChoice and SendSquad
// send and the verdicts read) is Off whatever the saved "onlineitems"/"onlinesquad" says, the select's
// row is not shown (ITEMS_OTHER_MODE), turning it on is refused, and the saved setting is kept as it is.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hle/slippi_online.h"
#include "host/host.h"
#include "host/gameplay_policy.h"
#include <cstdio>

namespace {
int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
}

int main() {
  namespace online = slippi::online;
  host::options.quiet = true;   // the refusals log; no log file from a test
  online::config().items = true;
  online::config().squad = true;

  // A normal run before any session (the online menus): both offered, the saved choices in effect.
  CHECK(online::items_offered() && online::items_choice());
  CHECK(online::items_option().status != online::ITEMS_OTHER_MODE && online::items_option().choice);
#if MU_SQUAD
  CHECK(online::squad_offered() && online::squad_choice());
  CHECK(online::squad_option().status != online::ITEMS_OTHER_MODE && online::squad_option().choice);
#else
  CHECK(!online::squad_offered() && !online::squad_choice());   // a build without Squad Strike
#endif

  // A Vanilla run, either way: neither is offered, both are Off in effect, and the saved settings stay on.
  for (int which = 0; which < 2; ++which) {
    if (which == 0) host::gameplay::vanilla_only_session = true;   // --vanilla-only
    else host::options.vanilla_game = true;                         // --vanilla-game
    CHECK(!online::items_offered() && !online::items_choice());
    CHECK(!online::squad_offered() && !online::squad_choice());
    const online::ItemsOption items = online::items_option(), squad = online::squad_option();
    CHECK(items.status == online::ITEMS_OTHER_MODE && !items.choice);
    CHECK(squad.status == online::ITEMS_OTHER_MODE && !squad.choice);
    CHECK(!online::set_items_choice(true) && !online::set_squad_choice(true));   // refused: not offered here
    CHECK(online::set_items_choice(false) && online::set_squad_choice(false));   // already Off in effect
    CHECK(online::config().items.load() && online::config().squad.load());
    host::gameplay::vanilla_only_session = false;
    host::options.vanilla_game = false;
  }

  // Back in a normal run the saved choices are in effect again.
  CHECK(online::items_choice());
#if MU_SQUAD
  CHECK(online::squad_choice());
  CHECK(online::set_squad_choice(false) && !online::config().squad.load() && !online::squad_choice());
#endif
  CHECK(online::set_items_choice(false) && !online::config().items.load() && !online::items_choice());

  if (g_failures == 0) std::printf("online options: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
