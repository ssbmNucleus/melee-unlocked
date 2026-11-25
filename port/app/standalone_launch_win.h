// Standalone Windows startup and local disc selection.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>   // SHGetFolderPathW: the Documents folder (replay_folder)
#include "disc_memory.h"
#include "mod_scan.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace standalone_launch {
namespace fs = std::filesystem;

inline fs::path exe_dir() {
  std::wstring self(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, self.data(), (DWORD)self.size());
  if (n == 0 || n >= self.size()) return {};
  self.resize(n);
  return fs::path(self).parent_path();
}

inline fs::path data_dir() {
  wchar_t local[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return disc_memory::standalone_dir(fs::path(local));
}

// The Slippi Launcher's Settings file (%APPDATA%\Slippi Launcher\Settings, only read) and the player's
// Documents folder through the shell (OneDrive can move it, and the Launcher follows it there), the
// places a plain start looks for the replay folder (disc_memory::replay_folder).
inline fs::path launcher_settings_file() {
  wchar_t roaming[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", roaming, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return fs::path(roaming) / L"Slippi Launcher" / L"Settings";
}
inline fs::path documents_dir() {
  wchar_t documents[MAX_PATH] = {};
  if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents) != S_OK) return {};
  return fs::path(documents);
}

// Melee NTSC 1.02: game ID GALE01 at the start of the disc, disc revision 2 at offset 7.
inline bool is_melee_102(const fs::path& iso) {
  std::error_code ec;
  if (!fs::is_regular_file(iso, ec)) return false;
  FILE* f = _wfopen(iso.c_str(), L"rb");
  if (!f) return false;
  unsigned char head[8] = {};
  const size_t got = std::fread(head, 1, sizeof head, f);
  std::fclose(f);
  return got == sizeof head && std::memcmp(head, "GALE01", 6) == 0 && head[7] == 2;
}

// Each disc needs the 1.02 header. A disc from
// another run's launcher.ini (possibly a mod run) must also be the unmodified disc, by the same
// whole-disc check the game itself makes for --vanilla-only (mod_scan identify_vanilla).
inline fs::path earlier_disc(const fs::path& local_app_data) {
  const auto decode = [](const std::string& raw, disc_memory::Encoding encoding) -> fs::path {
    if (encoding == disc_memory::Encoding::Utf8) return fs::u8path(raw);
    const int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), wide.data(), n);
    return fs::path(wide);
  };
  return disc_memory::first_verified(
      disc_memory::places(local_app_data), decode, [](const fs::path& disc) { return is_melee_102(disc); },
      [](const fs::path& disc) {
        return source_port::mods::identify_vanilla(disc).kind == source_port::mods::Kind::Vanilla;
      });
}

// The picker and its message boxes, owned by a window of their own centered on the primary
// monitor's work area. With no owner the picker opened at the screen's top-left corner (pc3,
// 11-17). The owner is a one-pixel, fully transparent window: visible, so it has a taskbar button
// that brings the picker back if it ends up behind another window.
struct PickerOwner {
  HWND hwnd = nullptr;
  PickerOwner() {
    static const wchar_t kClass[] = L"MeleeUnlockedPickerOwner";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);   // fails harmlessly when already registered
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    const int x = work.left + (work.right - work.left) / 2, y = work.top + (work.bottom - work.top) / 2;
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClass, product::kNameW, WS_POPUP, x, y, 1, 1,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);
  }
  ~PickerOwner() { if (hwnd) DestroyWindow(hwnd); }
  PickerOwner(const PickerOwner&) = delete;
  PickerOwner& operator=(const PickerOwner&) = delete;
};

// The picker closed with no disc (Escape, Cancel, the X): say what the game needs, then Choose disc
// (the picker again) or Quit. A plain message box with its two buttons renamed as it opens.
inline HHOOK& need_disc_hook() { static thread_local HHOOK hook = nullptr; return hook; }
inline LRESULT CALLBACK need_disc_rename(int code, WPARAM wp, LPARAM lp) {
  if (code == HCBT_ACTIVATE) {
    const HWND box = (HWND)wp;
    SetDlgItemTextW(box, IDOK, L"Choose disc");
    SetDlgItemTextW(box, IDCANCEL, L"Quit");
    UnhookWindowsHookEx(need_disc_hook());
    need_disc_hook() = nullptr;
    return 0;
  }
  return CallNextHookEx(need_disc_hook(), code, wp, lp);
}
inline bool choose_again(HWND owner) {
  need_disc_hook() = SetWindowsHookExW(WH_CBT, need_disc_rename, nullptr, GetCurrentThreadId());
  const int answer = MessageBoxW(owner,
                                 L"" PRODUCT_NAME " needs your own copy of Super Smash Bros. Melee (NTSC 1.02, an .iso file) to start.",
                                 product::kNameW, MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON1);
  if (need_disc_hook()) { UnhookWindowsHookEx(need_disc_hook()); need_disc_hook() = nullptr; }
  return answer == IDOK;
}

inline fs::path pick() {
  const PickerOwner owner;
  for (;;) {
    wchar_t file[32768] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner.hwnd;
    ofn.lpstrFilter = L"Melee disc image (*.iso, *.gcm)\0*.iso;*.gcm\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    ofn.lpstrTitle = L"" PRODUCT_NAME ": choose your Super Smash Bros. Melee NTSC 1.02 disc (ISO)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
      if (CommDlgExtendedError() != 0) return {};   // the picker itself failed: nothing to offer again
      if (choose_again(owner.hwnd)) continue;       // Choose disc: the picker again
      return {};                                    // Quit: start nothing
    }
    const fs::path chosen(file);
    if (is_melee_102(chosen)) return chosen;
    MessageBoxW(owner.hwnd,
                L"That file is not a Super Smash Bros. Melee NTSC 1.02 disc image.\n\n"
                L"" PRODUCT_NAME " needs your own copy of Melee, version 1.02 (game ID GALE01), as an .iso file. "
                L"Choose it again.",
                product::kNameW, MB_OK | MB_ICONWARNING);
  }
}

// One argument quoted the way the C runtime splits it (backslashes before a quote are doubled).
inline std::wstring quote(const std::wstring& arg) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') { ++slashes; continue; }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

// A no-argument start: find the disc, then start this same exe with the .bat's command line and
// return. The game itself runs in that copy, so its own relaunches (settings restart,
// mod looks, all built from GetCommandLineW) keep the data folder and flags. Returns the exit code.
inline int run() {
  const fs::path exe = exe_dir(), data = data_dir();
  if (exe.empty() || data.empty()) {
    MessageBoxW(nullptr, L"" PRODUCT_NAME " could not find its own folder or %LOCALAPPDATA%.", product::kNameW,
                MB_OK | MB_ICONERROR);
    return 1;
  }
  std::error_code ec;
  // A first start: a returning player's settings and remembered disc (from this program's earlier
  // Netlee folder, else Melee Unlocked's), copied once
  // (disc_memory.h copy_forward: that folder is only read).
  disc_memory::copy_forward(data.parent_path());
  for (const wchar_t* sub : {L"User\\Slippi", L"User\\GC\\CardA", L"Replays"}) fs::create_directories(data / sub, ec);
  // The saves of the vanilla-only plain starts before 2026-11-25, into the card a plain start uses now.
  disc_memory::carry_vanilla_saves(data / L"User" / L"GC" / L"CardA");
  const fs::path ini = data / L"standalone.ini";
  fs::path iso = earlier_disc(data.parent_path());   // standalone.ini first, then earlier runs
  if (!is_melee_102(iso)) iso = exe / L"melee.iso";
  if (!is_melee_102(iso)) iso = pick();
  if (iso.empty()) return 0;   // the player closed the picker
  disc_memory::remember(ini, iso);
  std::wstring self(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, self.data(), (DWORD)self.size());
  if (n == 0 || n >= self.size()) return 1;
  self.resize(n);
  // The .bat's command line, no --vanilla-only (disc_memory.h plain_start_args), with the replays where
  // the player's tracker looks: the Slippi Launcher's replay folder, else Documents\Slippi, else ours.
  const disc_memory::ReplayFolder replays = disc_memory::replay_folder(launcher_settings_file(), documents_dir(), data);
  const std::vector<std::wstring> args = disc_memory::plain_start_args(fs::path(self), exe, data, iso, replays);
  std::wstring cmd;
  for (const auto& arg : args) cmd += (cmd.empty() ? L"" : L" ") + quote(arg);
  STARTUPINFOW si{}; si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  // Working folder: the exe's own, as the .bat's cd /d "%~dp0".
  if (!CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.c_str(), &si, &pi)) {
    MessageBoxW(nullptr, L"" PRODUCT_NAME " could not start the game.", product::kNameW, MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  return 0;
}
}  // namespace standalone_launch
#endif
