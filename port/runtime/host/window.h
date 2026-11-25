// Host window services.
#pragma once
#include <functional>
#include <cstdint>
#include <string>
#include "product_identity.h"   // the name at the start of every window title

namespace host {
using MessageCallback = std::function<bool(void*, uint32_t, uintptr_t, intptr_t)>;
#ifndef _WIN32
// Linux: the window's messages are SDL events. The callback gets the SDL_Event* with this message number.
constexpr uint32_t kSdlEventMessage = 0x7FFF0001u;
#endif
void window_set_message_callback(MessageCallback cb);
void window_input_capture(bool capture);
bool window_input_captured(); // shared by Windows, macOS and Linux; local console samples only
struct PadState;
bool window_ui_gamecube_pad(PadState& pad);
// Last routed state for every in-game port, captured before UI input suppression. This lets an
// overlay follow the controller port selected in Training instead of assuming port 1.
// Optional ImGui-only snapshot excludes bindings already handled by backend keyboard navigation.
// Both copies are read atomically and share one agent-script acknowledgement.
bool window_ui_pads(PadState pads[4], PadState navigation_pads[4] = nullptr);
using ResizeCallback = std::function<void(int, int)>;
#ifdef _WIN32
void* window_create(int w, int h, const wchar_t* title, bool visible = true, bool flip = false);
#else
void* window_create(int w, int h, const wchar_t* title, bool visible = true);
#endif
// Linux: the next window_create makes a Vulkan window instead of an OpenGL one (SDL allows one
// kind per window). Windows draws either way into the same window, so it ignores this.
void window_prefer_vulkan(bool on);
void window_prefer_background(bool on);   // prewarm: no app activation, before video initialization
void window_set_resize_callback(ResizeCallback cb);
// For a window that scales its own contents with the monitor (the standalone settings window).
// While a callback is set, a move to a monitor with another DPI calls it with the new DPI and then
// resizes the window to the rectangle Windows suggests. Without one the message is left alone, so
// the game window keeps its size in pixels wherever it is dragged.
using DpiCallback = std::function<void(int)>;
void window_set_dpi_callback(DpiCallback cb);
void window_pump();
void window_set_fullscreen(bool enabled);
bool window_is_fullscreen();
// Windows: the game window (an HWND).
void* window_native();
// The game picture's shape in this window (width / height, as the overlay draws it): the room's small
// window goes into the black bar beside a 4:3 picture.
void window_note_picture_aspect(float aspect);
float window_picture_aspect();
#ifndef _WIN32
// SDL window visibility on the video/render thread.
bool window_visible();
#endif
#ifdef __APPLE__
void window_prefer_simple_fullscreen();   // macOS: non-Spaces full screen; before window_create
void window_defer_fullscreen_show();
#endif
#ifndef _WIN32
void window_presented(uint64_t frame);    // SDL hosts (Mac, Linux): after each swap; the window test knobs
#endif
// Resize the client area, as the resolution picker in the PC settings panel does. Ignored while
// the window is fullscreen (there the client area is the monitor) and clamped to the monitor.
void window_set_client_size(int w, int h);
bool window_take_fullscreen_toggle();   // true once per Alt+Enter press in the game window
// True once each time the game window comes back to the front: exclusive fullscreen, which Windows
// leaves on Alt+Tab, is entered again then.
bool window_take_activation();
// Escape in the game window closes the game (the close button's path, Slippi Dolphin's behavior).
// The standalone settings window turns that off and reads each press with window_take_escape.
void window_set_escape_closes(bool closes);
bool window_take_escape();              // true once per Esc press, when Escape does not close the window
bool input_port_uses_keyboard(int port);
int window_take_trace_mark();             // once per mark key press, its kind: 1 plain (F8), 2 looked wrong (F6), 3 input wrong (F7), 4 sounded wrong (F9); else 0
bool window_take_practice_toggle();     // true once per Tab press (auto-repeat ignored)
double window_refresh_rate();
void window_destroy();
void window_set_title(const wchar_t* title);
// Start of every window title: the game, its version and the engine running it ("Source Port" or
// "Static Recomp"), so the title bar and any screenshot of it say which build is playing. main()
// sets it once before the window opens; every title the runtime writes begins with it.
inline std::wstring& window_title_base() { static std::wstring base = product::kWindowTitleW; return base; }
// The version and engine ("SSBM Nucleus Unlocked 0.9.120  |  Source Port"): only in the title while the
// Performance overlay is on, with the display rates after it (threaded_backend.cpp).
inline std::wstring& window_title_debug() { static std::wstring debug = product::kWindowTitleW; return debug; }
// A small progress panel over the game window for startup work that holds the first frame back
// (compiling the pipeline cache on the first launch of a new version). Call from the thread that
// owns the game window; nothing is shown for a hidden window.
void loading_show(const wchar_t* what, size_t done, size_t total);
void loading_close();
// Run renderer startup work on the window thread.
void window_responsive(const std::function<void()>& work);
bool window_closed();
void window_client_size(int* w, int* h);
// Scripted input: text file with lines "FRAME BUTTON+BUTTON [sx=N] [sy=N] [cx=N] [cy=N]"; state holds
// until the next line. Buttons: A B X Y Z L R START DU DD DL DR. A line with only a frame releases all.
bool input_load_script(const char* path);
void input_mark_match_start();   // online match reached frame 1: `@match` script sections begin now
}  // namespace host
