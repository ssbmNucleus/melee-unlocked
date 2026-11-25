// Replay argument helpers (replay_args.cpp), moved out of main.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
std::string comm_file_replay(const std::string& file);    // -i <comm file>: the replay it names
std::string playable_replay(const std::string& path);     // a .slpz is unpacked to a scratch .slp
extern bool g_compress_replays;                            // --compress-replays
void compress_earlier_replays(const std::string& replay_dir);
