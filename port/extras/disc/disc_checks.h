// Disc image checks a launcher runs before it starts the game, taken out of the old Windows
// launcher (port/app/launcher.cpp) unchanged. Not built by this repository; see ../README.md.
#pragma once
#include <string>

namespace extras::disc {

// Header check: game id GALE01 at offset 0, revision byte 2 at offset 7 (NTSC 1.02), and at least
// a full disc's size. On false, *why says what is wrong in a short phrase.
bool verify_iso(const std::string& path, std::string* why);

// Why the chosen disc cannot be played as the base disc, or empty when it can: a compressed image
// (RVZ, GCZ, WIA, CISO), another game, another revision, a modded main.dol (SHA-1 of the retail
// main.dol, the same check host.cpp disc_has_vanilla_dol makes at startup), or an m-ex mod disc
// (its file table names MxDt.dat). The texts are the ones the launcher showed. Windows only (BCrypt).
std::wstring base_disc_problem(const std::string& path_utf8);

// An NKit image: the retail disc trimmed and repacked, neither the 1.02 ISO nor a mod.
bool nkit_image(const std::wstring& path);

}  // namespace extras::disc
