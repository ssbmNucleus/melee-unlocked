// SPDX-License-Identifier: GPL-2.0-or-later
#include "optional_downloads.h"
#include "product_identity.h"
#include "archive_unpack.h"
#include "host.h"
#include "http_client.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace host::downloads {
namespace {
// ================ Every optional download: its address and the SHA-256 of that file ================
// The one table of what this program downloads for its optional parts. Nothing is fetched that is not
// here, and every row is pinned: the file at that address must have exactly that SHA-256, checked as
// it arrives and before anything is put beside the game. Every row is AMD's FSR, from AMD's own SDK
// repository at a pinned tag; nothing is fetched from Melee Unlocked's GitHub any more.
enum class Part { Fsr };
struct Pinned {
  Part part;
  const char* url;
  const char* sha256;     // of the whole file at `url`
};
constexpr Pinned kPinned[] = {
    // AMD's signed FSR DLLs (FSR SDK 2.4.0, port/third_party/fsr), loose files on AMD's own repository.
    {Part::Fsr, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll",
     "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"},
    {Part::Fsr, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll",
     "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"},
    {Part::Fsr, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll",
     "e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6"},
    {Part::Fsr, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll",
     "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"},
    {Part::Fsr, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll",
     "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"},
    {Part::Fsr, "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll",
     "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"},
};

constexpr bool is_sha256(const char* s) {
  int n = 0;
  for (; s[n]; ++n)
    if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return false;
  return n == 64;
}
constexpr bool is_https(const char* s) {
  const char* head = "https://";
  for (int i = 0; head[i]; ++i) if (s[i] != head[i]) return false;
  return true;
}
constexpr bool every_row_pinned() {
  for (const Pinned& p : kPinned) if (!is_https(p.url) || !is_sha256(p.sha256)) return false;
  return true;
}
static_assert(every_row_pinned(), "every optional download needs an HTTPS address and the SHA-256 of its file");

// The last segment of a row's address: the file's own name.
std::string file_name(const Pinned& p) {
  const std::string url = p.url;
  return url.substr(url.rfind('/') + 1);
}

using archive::place_error;
using archive::place_file;

std::mutex g_mutex;   // the status messages below

// The most any one download may be: well over the largest part.
constexpr uint64_t kMaxDownload = uint64_t(1) << 30;

// One row of kPinned into `file`, through the host's one HTTP client (http_client.h): HTTPS only,
// redirects followed (GitHub release assets redirect to a CDN) with each hop checked and never down to
// HTTP, at most kMaxDownload bytes, written to disk as it arrives and hashed on the way; a file whose
// SHA-256 is not the row's is deleted, never handed on. `progress` hears (bytes so far, total or 0)
// about four times a second; false stops the download.
host::http::Response fetch_pinned(const Pinned& pin, const std::filesystem::path& file,
                                  std::function<bool(uint64_t, uint64_t)> progress = {}) {
  host::http::Request req;
  req.url = pin.url;
  req.max_bytes = kMaxDownload;
  req.expected_sha256 = pin.sha256;
  req.resolve_ms = 8000;
  req.connect_ms = 8000;
  req.send_ms = 30000;
  req.receive_ms = 120000;
  req.progress = std::move(progress);
  req.log_label = "download";
  return host::http::fetch_to_file(req, file);
}

// The status line for a failed fetch_pinned.
std::string download_failure(const std::string& label, const host::http::Response& r, const std::filesystem::path& work) {
  using host::http::Error;
  switch (r.error) {
  case Error::HashMismatch: return label + ": the download does not match the release; not installed";
  case Error::Status: return label + " download failed (HTTP " + std::to_string(r.status) + ")";
  case Error::TooLarge: return label + ": the download is larger than any optional part; not installed";
  case Error::Write: return label + ": cannot save the download in " + work.u8string() + ". Check the free space on that drive.";
  default: return label + " download failed (no connection or cut off); try again";
  }
}

// Downloads unpack in a work folder of the player's own (LOCALAPPDATA\<data folder>\downloads), away
// from the game's folder: a game kept on the Desktop, in Documents or Downloads can sit in a folder
// Windows protects (Ransomware protection), and antivirus can hold a file it is still scanning. Only the
// finished files go beside the game, and that move is tried for a few seconds before it gives up.
std::filesystem::path download_work(const char* name) {
#ifdef _WIN32
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::filesystem::path base = n && n < MAX_PATH ? std::filesystem::path(buf) : std::filesystem::temp_directory_path();
#else
  const char* data = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  std::filesystem::path base = data && *data ? std::filesystem::path(data)
                             : home && *home ? std::filesystem::path(home) / ".local" / "share" : std::filesystem::temp_directory_path();
#endif
#ifdef _WIN32
  return base / product::kDataFolder / "downloads" / name;
#else
  return base / product::kDataFolderUnix / "downloads" / name;
#endif
}
}  // namespace

// ---------------- optional parts ----------------
namespace {
// One optional part's download and its status.
struct PartFetch {
  std::atomic<bool> busy{false}, failed{false};
  std::string message;
  std::thread thread;
};
// Never destroyed: a download still running at exit is detached (shutdown) and may touch its
// PartFetch until the process ends.
PartFetch& g_fsr = *new PartFetch;
void part_message(PartFetch& p, const std::string& m) { std::lock_guard<std::mutex> lk(g_mutex); p.message = m; }
std::string part_message(PartFetch& p) { std::lock_guard<std::mutex> lk(g_mutex); return p.message; }
}  // namespace

bool fsr_present(const std::string& game_dir_utf8) {
  std::error_code ec;
  for (const Pinned& f : kPinned)
    if (f.part == Part::Fsr && !std::filesystem::is_regular_file(std::filesystem::u8path(game_dir_utf8) / file_name(f), ec)) return false;
  return true;
}

// Each file on its own (AMD publishes them loose, not as a zip), checked, staged in the work folder and
// then put beside the game, so a cut download never leaves half a DLL in place.
void fsr_download(const std::string& game_dir_utf8) {
  if (g_fsr.busy.exchange(true)) return;
  if (g_fsr.thread.joinable()) g_fsr.thread.join();
  g_fsr.failed = false;
  part_message(g_fsr, std::string("Downloading AMD FSR (") + kFsrSize + ")...");
  g_fsr.thread = std::thread([game_dir_utf8] {
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(game_dir_utf8);
    std::error_code ec;
    auto fail = [&](const std::string& why) {
      part_message(g_fsr, why);
      host::log("fsr download: %s", why.c_str());
      g_fsr.failed = true;
      g_fsr.busy = false;
    };
    for (const Pinned& f : kPinned) {
      if (f.part != Part::Fsr) continue;
      const std::string name = file_name(f);
      const fs::path to = dir / name;
      const fs::path staged = download_work("fsr") / name;
      fs::create_directories(staged.parent_path(), ec);
      const host::http::Response got = fetch_pinned(f, staged);
      if (got.error == host::http::Error::HashMismatch) return fail("AMD FSR: " + name + " does not match AMD's build; not installed");
      if (!got.ok()) return fail(download_failure("AMD FSR", got, staged.parent_path()));
      if (!place_file(staged, to, ec) || !fs::is_regular_file(to, ec)) return fail(place_error("AMD FSR could not be put", dir, ec));
    }
    part_message(g_fsr, "AMD FSR is installed. It starts with the game's next start.");
    host::log("fsr download: AMD's FSR DLLs installed beside the game");
    g_fsr.busy = false;
  });
}

ModelState fsr_state(const std::string& game_dir_utf8) {
  if (g_fsr.busy.load()) return ModelState::Downloading;
  if (fsr_present(game_dir_utf8)) return ModelState::Present;
  return g_fsr.failed.load() ? ModelState::Failed : ModelState::Missing;
}
std::string fsr_message() { return part_message(g_fsr); }

void shutdown() {
  // Every part download: a std::thread still joinable when its static is destroyed at exit calls
  // std::terminate (B-905: quitting after a download finished aborted). A finished download is
  // joined; one still running is left to end with the process, so quitting never waits for it.
  for (PartFetch* p : {&g_fsr}) {
    if (!p->thread.joinable()) continue;
    if (p->busy.load()) p->thread.detach();
    else p->thread.join();
  }
}
}  // namespace host::downloads
