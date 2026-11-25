// What this program calls itself, in one place: Nucleus Unlocked, a fork of Melee Unlocked.
//
// The name a player, a website or another copy of the game sees comes from here: the window title,
// message boxes, the log's first line, the default data folder (its own, so it never shares a
// settings file, a remembered disc or a profile with an installed Melee Unlocked), the User-Agent of
// the mod browser and of opponents' skins downloads, the product named in --print-info and --help and
// in the online build message. Melee Unlocked stays named where the credit and the licence belong
// (upstream_name). This program's earlier data folder (when it was called Netlee) and Melee Unlocked's
// are listed so a first start can copy a returning player's profile forward from them
// (disc_memory::copy_forward, never moving, changing or deleting anything there).
//
// Not renamed, on purpose (the owner's call): the executable and game library file names, the
// settings file, CMake targets, environment variable prefixes, the single-instance mutex and window
// class names, and the version number (the VERSION file). The version resource (app/melee_unlocked.rc)
// spells the name itself, because the resource compiler cannot read this file.
//
// Three spellings since the release rename (2026-11-25; "Nucleus" alone also means the SSBM Nucleus
// desktop app, and people mixed the two up):
//   PRODUCT_NAME          "Nucleus Unlocked": what PEOPLE read (message boxes, the settings headings,
//                         the log's first line, --help, crash and disc chooser texts).
//   PRODUCT_WINDOW_TITLE  "SSBM Nucleus Unlocked": the game window's and the settings window's title.
//   PRODUCT_TOKEN         "Nucleus": what PROGRAMS read, unchanged by the rename: the product token of
//                         the three User-Agents (a space there would be wrong, and the site may key on
//                         it) and --print-info's "product". The static_asserts below hold them.
// A new text for a person takes PRODUCT_NAME; nothing new should need PRODUCT_TOKEN.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The name as a string literal, for texts put together at compile time ("..." PRODUCT_NAME "...",
// L"" PRODUCT_NAME). Everything else uses product::kName.
#define PRODUCT_NAME "Nucleus Unlocked"
#define PRODUCT_NAME_UPPER "NUCLEUS UNLOCKED"   // the settings panel's headings
// The game window's title and the start of the settings window's ("... settings").
#define PRODUCT_WINDOW_TITLE "SSBM Nucleus Unlocked"
// The name programs read: the User-Agents' product token and --print-info's "product". One word,
// and not to be changed with the display name: websites and the desktop app see it.
#define PRODUCT_TOKEN "Nucleus"
// Where this program comes from, as named to websites in the User-Agent.
#define PRODUCT_HOME "https://ssbmnucleus.net"
// The User-Agent of every HTTP request this program makes (the Nucleus client, lobbies, opponents'
// skins, optional downloads, one-click looks, Slippi's services), as a string literal:
// PRODUCT_USER_AGENT(MELEE_PORT_VERSION), or L"" PRODUCT_USER_AGENT(MELEE_PORT_VERSION) for WinHTTP.
// It names this program, its version and its home; never upstream's project, accounts or addresses.
#define PRODUCT_USER_AGENT(version) PRODUCT_TOKEN "/" version " (+" PRODUCT_HOME ")"
// The same after a browser-like prefix, for pages that serve browsers only (the one-click looks'
// Mediafire pages): a choice per request (host::http::Agent::Browser, http_client.h).
#define PRODUCT_BROWSER_USER_AGENT(version) "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " PRODUCT_USER_AGENT(version)
// This program speaking Slippi's protocol, to Slippi's services (host::http::Agent::Slippi).
#define PRODUCT_SLIPPI_USER_AGENT(version) PRODUCT_TOKEN "/" version " (Slippi protocol 3.7.0)"

namespace product {

// The name shown to people.
inline constexpr const char kName[] = PRODUCT_NAME;
inline constexpr const wchar_t kNameW[] = L"" PRODUCT_NAME;
// The game window's title (and the settings window's, before " settings").
inline constexpr const wchar_t kWindowTitleW[] = L"" PRODUCT_WINDOW_TITLE;
// The name programs read (--print-info's "product"; the User-Agents spell it through PRODUCT_TOKEN).
inline constexpr const char kToken[] = PRODUCT_TOKEN;
// The project this one is built from: credits, the licence notice, logs that compare the two.
inline constexpr const char kUpstreamName[] = "Melee Unlocked";
// The product word in the online build message's product field, the lobby client string and the
// Discord party id. Builds from before the first rename send "netlee"; nothing compares the word, it
// is only logged.
inline constexpr const char kWireName[] = "nucleus";

// This program's own Discord application (a snowflake from discord.com/developers/applications), for
// Rich Presence and Join (host/discord_presence.cpp). Empty: this build has no presence at all, no
// pipe is opened and nothing is registered, whatever the settings say. Melee Unlocked's application
// is not used. Set it once the owner has registered Nucleus's own application; its art asset for
// the picture beside the presence is named kWireName ("nucleus").
inline constexpr const char kDiscordAppId[] = "";

// %LOCALAPPDATA%\<kDataFolder> on Windows (data, settings, remembered disc, Slippi user folder);
// <XDG data>/<kDataFolderUnix> elsewhere. Not "SSBM Nucleus" or "Nucleus": those belong to the
// Nucleus desktop app.
inline constexpr const wchar_t kDataFolder[] = L"Nucleus Client";
inline constexpr const char kDataFolderUnix[] = "Nucleus Client";
// This program's own earlier data folders (it was called Netlee), newest first: read once, to copy a
// profile forward, before Melee Unlocked's; never written.
inline constexpr const wchar_t* kPreviousDataFolders[] = {L"Netlee"};
inline constexpr const char* kPreviousDataFoldersUnix[] = {"Netlee"};
// Melee Unlocked's own data folders, newest first: read once, to copy a profile forward, never written.
inline constexpr const wchar_t* kUpstreamDataFolders[] = {L"MeleeUnlocked", L"MeleeUnlocked-YouGame"};
inline constexpr const char* kUpstreamDataFoldersUnix[] = {"MeleeUnlocked", "melee-unlocked"};

// What websites and other programs are sent stays byte for byte what it was before the display name
// changed. A build that changes one of these fails here, on purpose.
namespace identity_check {
constexpr bool same(const char* a, const char* b) {
  for (; *a && *a == *b; ++a, ++b) {}
  return *a == *b;
}
static_assert(same(PRODUCT_USER_AGENT("1.2.3"), "Nucleus/1.2.3 (+https://ssbmnucleus.net)"));
static_assert(same(PRODUCT_BROWSER_USER_AGENT("1.2.3"),
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Nucleus/1.2.3 (+https://ssbmnucleus.net)"));
static_assert(same(PRODUCT_SLIPPI_USER_AGENT("1.2.3"), "Nucleus/1.2.3 (Slippi protocol 3.7.0)"));
static_assert(same(kToken, "Nucleus"));     // --print-info "product": the desktop app reads that answer
static_assert(same(kWireName, "nucleus"));  // the online build message
}  // namespace identity_check

}  // namespace product
