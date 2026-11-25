// The replay arguments: the -i communication file reader, .slpz to .slp for --replay, and the
// --compress-replays worker. Moved out of main.cpp unchanged.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "replay_args.h"
#include <windows.h>
#include "host.h"
#include "replay_control.h"
#include "replay_slpz.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
// --replay with a .slpz (the slpz tool's compressed replays, port/app/replay_slpz.h): every reader
// here takes a .slp, so it is decompressed once into a scratch file and that is played. The online
// trace beside the original ("<replay>.trace", for "as experienced") is kept by naming it.
// -i <file>: a Slippi communication file (Slippi's COMM_SPEC, as slp2mp4 and Dolphin take it): the replay
// to play is its "replay", or the first "path" of its "queue". Read by hand (two string fields), so the
// argument parser needs no JSON library. Empty when the file names none.
std::string comm_file_replay(const std::string& file) {
  std::ifstream f(std::filesystem::u8path(file), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto field = [&](const char* key, size_t from) -> std::string {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = text.find(k, from);
    if (at == std::string::npos) return {};
    at = text.find(':', at + k.size());
    if (at == std::string::npos) return {};
    at = text.find('"', at);
    if (at == std::string::npos) return {};
    std::string out;
    for (size_t i = at + 1; i < text.size() && text[i] != '"'; ++i) {
      if (text[i] == '\\' && i + 1 < text.size()) { ++i; out += text[i] == 'n' ? '\n' : text[i]; }
      else out += text[i];
    }
    return out;
  };
  std::string replay = field("replay", 0);
  if (replay.empty()) { const size_t queue = text.find("\"queue\""); if (queue != std::string::npos) replay = field("path", queue); }
  if (replay.empty()) std::fprintf(stderr, "-i %s names no replay (\"replay\" or a \"queue\" entry's \"path\")\n", file.c_str());
  return replay;
}

std::string playable_replay(const std::string& path) {
  namespace fs = std::filesystem;
  std::vector<uint8_t> head(24);
  {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f.read((char*)head.data(), (std::streamsize)head.size())) return path;
  }
  if (!slpz::is_slpz(head.data(), head.size())) return path;
  std::vector<uint8_t> slp;
  if (!slpz::read_replay(path, slp)) {
    std::fprintf(stderr, "replay: %s is a damaged .slpz\n", path.c_str());
    return path;
  }
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / "MeleeUnlocked" / "slpz";
  fs::create_directories(dir, ec);
  fs::path out = dir / fs::u8path(path).filename();
  out.replace_extension(".slp");
  {
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write((const char*)slp.data(), (std::streamsize)slp.size());
    if (!f) return path;
  }
  fs::path trace = fs::u8path(path);
  trace.replace_extension(".trace");
  if (fs::exists(trace, ec)) replay_control::set_trace_file(trace.u8string());
  return out.u8string();
}

// "Save replays compressed" (--compress-replays, which a launcher passes for it): replays of
// earlier sessions are turned into .slpz (port/app/replay_slpz.h), 8 to 12 times smaller, once the game has
// started, on one idle thread. Each is checked to read back as the same bytes before its .slp goes;
// unfinished recordings stay .slp. Never this session's: the match report, a launcher and Slippi's tools
// read a new .slp right after the game.
bool g_compress_replays = false;
void compress_earlier_replays(const std::string& replay_dir) {
  if (!g_compress_replays) return;
  if (replay_dir.empty()) return;
  const auto started = std::filesystem::file_time_type::clock::now();
  std::thread([replay_dir, started] {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
#endif
    namespace fs = std::filesystem;
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::u8path(replay_dir), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
      std::error_code fe;
      if (!it->is_regular_file(fe) || fe || it->path().extension() != ".slp") continue;
      if (it->last_write_time(fe) >= started - std::chrono::minutes(1)) continue;   // not one still being written
      found.push_back(it->path());
    }
    size_t done = 0;
    uintmax_t before = 0, after = 0;
    for (const auto& p : found) {
      std::error_code fe;
      const uintmax_t size = fs::file_size(p, fe);
      const std::string out = slpz::compress_file(p.u8string());
      if (out.empty()) continue;
      ++done; before += size; after += fs::file_size(fs::u8path(out), fe);
    }
    if (done) host::log("replays: %zu earlier replays compressed to .slpz (%.1f MB to %.1f MB)", done, before / 1048576.0, after / 1048576.0);
  }).detach();
}
