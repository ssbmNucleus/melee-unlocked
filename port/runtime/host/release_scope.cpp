// The release switches (release_scope.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "release_scope.h"

#include "host.h"
#include "public_build.h"
#include "test_env.h"

#include <atomic>
#include <cstring>

namespace host::release_scope {
namespace {

// The settings file's switches (reset_settings: the table's defaults).
std::atomic<bool> g_setting[kCategories] = {true, true, false, false, false, false};
std::atomic<uint8_t> g_logged{0};   // gate(): the categories already logged as off

const char* const kScopeVariable = "MELEE_TEST_MODS_SCOPE";

bool always_on(int c) { return kTable[c].setting == nullptr; }

// An automated run's MELEE_TEST_MODS_SCOPE for `c`: 1 on, 0 off, -1 not set (every category).
int test_scope(int c) {
  const char* v = host::test_env::automated_getenv(kScopeVariable);
  if (!v || !*v) return -1;
  int on = g_setting[c].load() ? 1 : 0;   // "release": the defaults and the file's lines
  const char* p = v;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == ';') ++p;
    const char* start = p;
    while (*p && *p != ',' && *p != ' ' && *p != ';') ++p;
    const size_t n = (size_t)(p - start);
    if (n == 0) continue;
    if (n == 3 && std::strncmp(start, "all", 3) == 0) return 1;
    if (std::strlen(kTable[c].key) == n && std::strncmp(start, kTable[c].key, n) == 0) on = 1;
    if (std::strlen(kTable[c].setting) == n && std::strncmp(start, kTable[c].setting, n) == 0) on = 1;
  }
  return on;
}

}  // namespace

bool on(int category) {
  if (category < 0 || category >= kCategories) return false;
  if (always_on(category)) return true;
  // A public build (public_build.h) has no test fixtures: an automated run there is a player's run too,
  // the table plus the settings file, and MELEE_TEST_MODS_SCOPE is never read.
  if (!host::public_build::kOn && host::test_env::automated) {
    const int scope = test_scope(category);
    return scope != 0;   // not set: every category, as every harness from before the switches expects
  }
  return g_setting[category].load();
}

uint8_t mask() {
  uint8_t m = 0;
  for (int c = 0; c < kCategories; ++c)
    if (on(c)) m |= (uint8_t)(1u << c);
  return m;
}

bool gate(int category, const char* what) {
  if (on(category)) return true;
  if (category < 0 || category >= kCategories) return false;
  const uint8_t bit = (uint8_t)(1u << category);
  if (!(g_logged.fetch_or(bit) & bit))
    host::log("release: %s is coming soon in this release (off; the settings line \"%s 1\" turns it on): %s",
              kTable[category].name, kTable[category].setting, what);
  return false;
}

std::string describe() {
  std::string on_list, off_list;
  for (int c = 0; c < kCategories; ++c) {
    std::string& list = on(c) ? on_list : off_list;
    list += (list.empty() ? "" : ", ") + std::string(kTable[c].name);
  }
  std::string out = on_list + " on";
  if (!off_list.empty()) out += "; " + off_list + " coming soon";
  if (!host::public_build::kOn && host::test_env::automated) {
    const char* v = host::test_env::automated_getenv(kScopeVariable);
    out += v && *v ? std::string(" (automated run, ") + kScopeVariable + "=" + v + ")"
                   : std::string(" (automated run: every category; ") + kScopeVariable + "=release for the release's)";
  }
  return out;
}

void reset_settings() {
  for (int c = 0; c < kCategories; ++c) g_setting[c].store(kTable[c].on_by_default);
}

bool load_setting(const std::string& key, const std::string& value) {
  for (int c = 0; c < kCategories; ++c) {
    if (always_on(c)) continue;
    const std::string name = kTable[c].setting;
    std::string v;
    if (key == name) v = value;
    else if (key.size() > name.size() && key.compare(0, name.size(), name) == 0 && key[name.size()] == '=')
      v = key.substr(name.size() + 1);   // "modsparticles=1"
    else continue;
    if (!v.empty()) g_setting[c].store(v == "1");
    return true;
  }
  return false;
}

std::string settings_lines() {
  std::string out;
  for (int c = 0; c < kCategories; ++c)
    if (!always_on(c) && g_setting[c].load() != kTable[c].on_by_default)
      out += std::string("\n") + kTable[c].setting + " " + (g_setting[c].load() ? "1" : "0");
  return out;
}

bool setting(int category) {
  return category >= 0 && category < kCategories && g_setting[category].load();
}

void set_setting(int category, bool value) {
  if (category >= 0 && category < kCategories && !always_on(category)) g_setting[category].store(value);
}

}  // namespace host::release_scope
