// Linux build of window.cpp's window half: an SDL2 window with an OpenGL 4.5 context for the
// renderer, the keyboard in Windows virtual-key codes (the bindings and the settings panel store
// those), the same shortcut keys, fullscreen, the client-size picker, the refresh rate and the
// loading panel. Included from window.cpp inside namespace host, where the shared state lives.
// SPDX-License-Identifier: GPL-2.0-or-later

namespace {
SdlVideoThread g_sdl_video_thread;
bool video_thread(SdlVideoThread::Call call) {
  return g_sdl_video_thread.allow(call, [](const char* message) { host::log("%s", message); });
}
SDL_Window* sdl_window() { return static_cast<SDL_Window*>(g_hwnd); }

// SDL scancodes to the Windows virtual-key codes the key bindings are stored in, so a
// port-settings.ini written on Windows means the same keys here.
int vk_from_scancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
  switch (sc) {
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
    case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;
    case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
    case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
    case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
    case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_LALT: return VK_LMENU;
    case SDL_SCANCODE_RALT: return VK_RMENU;
    case SDL_SCANCODE_LGUI: return VK_LWIN;
    case SDL_SCANCODE_RGUI: return VK_RWIN;
    default: return 0;
  }
}

// The generic modifier codes Windows also reports beside the left/right ones.
int vk_generic(int vk) {
  switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return 0;
  }
}

void set_key(int vk, bool down) {
  if (vk <= 0 || vk >= 256) return;
  g_keys[vk] = down;
  linux_compat::set_key_state(vk, down);
  if (int generic = vk_generic(vk)) {
    const bool any = vk_generic(vk) == VK_SHIFT ? (g_keys[VK_LSHIFT] || g_keys[VK_RSHIFT])
                   : vk_generic(vk) == VK_CONTROL ? (g_keys[VK_LCONTROL] || g_keys[VK_RCONTROL])
                   : (g_keys[VK_LMENU] || g_keys[VK_RMENU]);
    g_keys[generic] = any;
    linux_compat::set_key_state(generic, any);
  }
}

#ifdef __APPLE__
void* ns_window() {
  SDL_SysWMinfo info{}; SDL_VERSION(&info.version);
  return g_hwnd && SDL_GetWindowWMInfo(sdl_window(), &info) ? (void*)info.info.cocoa.window : nullptr;
}
bool g_escape_left_fullscreen = false;
bool g_deferred_fullscreen_show = false;
#endif
bool g_cursor_visible = true;
// Wayland compositors set their own pointer again when it re-enters the window or the window gets
// the focus back, so the state is applied again then even if it did not change (two players on
// Wayland kept seeing the pointer over the game).
bool g_cursor_reapply = false;
void show_cursor(bool visible) {
  // Also compare with SDL's own state: anything else that shows the pointer is undone here.
  if (visible == g_cursor_visible && !g_cursor_reapply &&
      (SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE) == visible) return;
  g_cursor_visible = visible;
  g_cursor_reapply = false;
  SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

// One SDL event, with the same rules as wnd_proc on Windows.
void handle_event(const SDL_Event& e) {
  if (!video_thread(SdlVideoThread::Call::Event)) return;
  if (e.type == SDL_MOUSEMOTION) {
    g_pointer_moved = GetTickCount64();
    g_pointer_rested = false;
#ifndef __APPLE__
    show_cursor(true);
#endif
  }
  // Tab is reserved for the practice overlay before the settings panel or the bindings see it.
  if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && e.key.keysym.scancode == SDL_SCANCODE_TAB) {
    if (e.type == SDL_KEYDOWN && !e.key.repeat) g_practice_toggle.store(true);
    std::lock_guard<std::mutex> lock(g_keys_mutex); set_key(VK_TAB, false);
    return;
  }
#ifdef __APPLE__
  // Escape must reach fullscreen handling before ImGui/menu input can consume it.
  if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
    if (e.type == SDL_KEYUP && g_escape_left_fullscreen) { g_escape_left_fullscreen = false; return; }
    if (e.type == SDL_KEYDOWN && (g_escape_left_fullscreen || window_is_fullscreen())) {
      if (!g_escape_left_fullscreen && !e.key.repeat) {
        window_set_fullscreen(false);
        host::log("Mac video: Escape: leaving full screen (menu untouched)");
      }
      g_escape_left_fullscreen = true;
      return;
    }
  }
#endif
  if (g_on_message && g_on_message(const_cast<SDL_Event*>(&e), kSdlEventMessage, 0, 0)) return;
  switch (e.type) {
    case SDL_QUIT: host::log("window: closing: the system asked the game to quit"); g_closed = true; request_user_exit(); return;
    case SDL_WINDOWEVENT:
      switch (e.window.event) {
        case SDL_WINDOWEVENT_CLOSE: host::log("window: closing: the window was closed"); g_closed = true; request_user_exit(); return;
        case SDL_WINDOWEVENT_SIZE_CHANGED: {
          int w = 0, h = 0;
          SDL_GL_GetDrawableSize(sdl_window(), &w, &h);
          if (w > 0 && h > 0) {
            g_client_w = w; g_client_h = h;
            if (g_on_resize) g_on_resize(w, h);
          }
          return;
        }
        case SDL_WINDOWEVENT_MINIMIZED: host::log("window: minimized"); return;
        case SDL_WINDOWEVENT_FOCUS_LOST: {
          { std::lock_guard<std::mutex> lock(g_keys_mutex);
            for (int vk = 0; vk < 256; ++vk) if (g_keys[vk]) set_key(vk, false); }
          linux_compat::set_window_focused(false);
          show_cursor(true);
          host::log("window: another program took the focus");
          return;
        }
        case SDL_WINDOWEVENT_ENTER:
          g_cursor_reapply = true;
          return;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
#ifdef __APPLE__
          g_activated.store(true);
#endif
          g_cursor_reapply = true;
#ifndef __APPLE__
          g_pointer_moved = GetTickCount64();
#endif
          linux_compat::set_window_focused(true);
          host::log("window: the game is in front again");
          return;
        default: return;
      }
    case SDL_KEYDOWN: {
      const bool first = !e.key.repeat;
      const SDL_Keymod mod = (SDL_Keymod)e.key.keysym.mod;
      if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
          (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (mod & KMOD_ALT))) {
        if (first) g_fullscreen_toggle.store(true);
        return;
      }
      if (first) {
        switch (e.key.keysym.scancode) {
          case SDL_SCANCODE_F8: g_trace_mark.store(1); break;
          case SDL_SCANCODE_F6: g_trace_mark.store(2); break;
          case SDL_SCANCODE_F7: g_trace_mark.store(3); break;
          case SDL_SCANCODE_F9: g_trace_mark.store(4); break;
          case SDL_SCANCODE_ESCAPE:
            // Escape closes the game, as the window's close button does (window.cpp says why); the
            // standalone settings window keeps it for itself (window_set_escape_closes).
            if (g_escape_closes.load()) {
              if (!g_closed) host::log("window: Escape pressed, closing the game");
              g_closed = true;
              request_user_exit();
              return;
            }
            g_escape_press.store(true);
            break;
          default: break;
        }
      }
      std::lock_guard<std::mutex> lock(g_keys_mutex); set_key(vk_from_scancode(e.key.keysym.scancode), true);
      return;
    }
    case SDL_KEYUP: { std::lock_guard<std::mutex> lock(g_keys_mutex); set_key(vk_from_scancode(e.key.keysym.scancode), false); return; }
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED: case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED:
      return;   // xinput_sdl.cpp rescans on its own schedule
    default: return;
  }
}

std::atomic<uint64_t> g_pending_client_size{0};
bool g_fullscreen = false;
std::string utf8_title(const wchar_t* title) {
  std::string out;
  if (!title) return out;
  const int n = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
  out.resize(n > 0 ? (size_t)n : 1);
  WideCharToMultiByte(CP_UTF8, 0, title, -1, out.data(), n, nullptr, nullptr);
  if (!out.empty() && out.back() == 0) out.pop_back();
  return out;
}

void apply_client_size(int w, int h) {
#ifdef __APPLE__
  if (!g_hwnd || window_is_fullscreen() || w < 320 || h < 240) return;
#else
  if (!g_hwnd || g_fullscreen || w < 320 || h < 240) return;
#endif
  SDL_Rect usable{};
  if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(sdl_window()), &usable) == 0) {
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(sdl_window(), &top, &left, &bottom, &right);
    w = std::min(w, usable.w - left - right);
    h = std::min(h, usable.h - top - bottom);
  }
  if (w == g_client_w && h == g_client_h) return;
  SDL_SetWindowSize(sdl_window(), w, h);
}

// The thread that made the window. It alone reads the window's events and makes SDL video calls.
// With the render thread (threaded_backend.cpp) that is not the game's thread, which still shows
// start-up progress (loading_show while the disc is read): polling events there as well fed the
// overlay's input queue from two threads at once while the render thread drew the overlay (a crash
// report: abort in free under AddMousePosEvent when the window got the focus back during that read).
std::atomic<std::thread::id> g_window_thread{};
bool on_window_thread() { return g_window_thread.load(std::memory_order_acquire) == std::this_thread::get_id(); }
// A title another thread asked for (UTF-8), shown by the window's thread at its next pump.
std::mutex g_title_mutex;
std::string g_title_for_window;
bool g_title_for_window_pending = false;
std::atomic<bool> g_loading_from_other_thread{false};
void title_from_another_thread(std::string title) {
  std::lock_guard<std::mutex> lock(g_title_mutex);
  g_title_for_window = std::move(title);
  g_title_for_window_pending = true;
}
}  // namespace

void window_prefer_background(bool on) {
#ifdef __APPLE__
  SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP, on ? "1" : "0", SDL_HINT_OVERRIDE);
#endif
}
bool g_vulkan_window = false;
void window_prefer_vulkan(bool on) { g_vulkan_window = on; }

void* window_create(int w, int h, const wchar_t* title, bool visible) {
  // Claim before SDL initialization and the Mac/X11 adapters, including their startup pumps.
  g_sdl_video_thread.claim();
  g_window_thread.store(std::this_thread::get_id(), std::memory_order_release);
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "0");
  if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
    die("cannot start SDL video: %s", SDL_GetError());
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
#ifdef __APPLE__
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
#endif
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 0);
  Uint32 flags = (g_vulkan_window ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  flags |= visible ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN;
  SDL_Window* window = SDL_CreateWindow(utf8_title(title).c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
  if (!window) die("cannot create the game window: %s", SDL_GetError());
  g_hwnd = window;
  linux_compat::set_game_window(g_hwnd);
  ds4_init_defaults();
  hidraw_start();
  g_closed = false;
  int dw = w, dh = h;
  SDL_GL_GetDrawableSize(window, &dw, &dh);
  g_client_w = dw > 0 ? dw : w; g_client_h = dh > 0 ? dh : h;
  if (visible) { SDL_RaiseWindow(window); linux_compat::set_window_focused(true); }
  return g_hwnd;
}

void window_set_fullscreen(bool enabled) {
#ifdef __APPLE__
  if (!g_hwnd || enabled == window_is_fullscreen()) return;
  if (!enabled && !(SDL_GetWindowFlags(sdl_window()) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
    if (void* w = ns_window(); w && mac_window_is_fullscreen(w)) mac_toggle_fullscreen(w);
    return;
  }
#else
  if (!g_hwnd || enabled == g_fullscreen) return;
#endif
  // Borderless at the desktop's own mode, as on Windows: no display mode change.
#ifdef __APPLE__
  if (SDL_SetWindowFullscreen(sdl_window(), enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
    g_fullscreen = enabled;
  else host::log("window: fullscreen failed: %s", SDL_GetError());
#else
  SDL_SetWindowFullscreen(sdl_window(), enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  g_fullscreen = enabled;
#endif
}
#ifdef __APPLE__
bool window_is_fullscreen() {
  if (!g_hwnd) return false;
  if (SDL_GetWindowFlags(sdl_window()) & SDL_WINDOW_FULLSCREEN_DESKTOP) return true;
  void* w = ns_window();
  return w && mac_window_is_fullscreen(w);
}
// SDL reads the hint when the window is created, so call this before window_create.
void window_prefer_simple_fullscreen() { SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0"); }
void window_defer_fullscreen_show() {
  g_deferred_fullscreen_show = true;
}
#else
bool window_is_fullscreen() { return g_fullscreen; }
#endif
// Called after each swap by the renderer's after_swap hook (the Mac's first complete game picture also starts its
// deferred full-screen show). Hidden checks never arm it.
void window_presented(uint64_t frame) {
#ifdef __APPLE__
  if (!g_hwnd) return;
  if (g_deferred_fullscreen_show) {
    g_deferred_fullscreen_show = false;
    window_set_fullscreen(true);
    SDL_ShowWindow(sdl_window()); SDL_RaiseWindow(sdl_window());
    host::log("Mac video: first frame %llu, showing the window in full screen", (unsigned long long)frame);
  }
  const char* const tag = "Mac test";
#else
  if (!g_hwnd) return;
  const char* const tag = "Linux test";
#endif
  // OpenGL and Metal present here. Vulkan consumes the shortcut in its own frame setup.
  if (window_take_fullscreen_toggle()) window_set_fullscreen(!window_is_fullscreen());
  // Same opt-in evidence controls as M2; counts here are presented frames, not simulation retraces.
  // RESIZE and FULLSCREEN work on both; TOGGLE (AppKit's green button) and ESCAPE (the Mac menu check) are Mac-only.
  static const auto number = [](const char* name) -> uint64_t {
    const char* value = host::test_env::getenv(name); return value ? std::strtoull(value, nullptr, 10) : 0;
  };
  static const uint64_t fullscreen = number("MELEE_TEST_FULLSCREEN");
#ifdef __APPLE__
  static const uint64_t toggle = number("MELEE_TEST_TOGGLE"), escape = number("MELEE_TEST_ESCAPE");
#else
  constexpr uint64_t toggle = 0;
#endif
  static int resize_w = 0, resize_h = 0; static unsigned long long resize_at = 0;
  static const bool parsed = [] {
    if (const char* v = host::test_env::getenv("MELEE_TEST_RESIZE")) std::sscanf(v, "%dx%d@%llu", &resize_w, &resize_h, &resize_at);
    return true;
  }();
  (void)parsed;
  if (resize_w > 0 && resize_h > 0 && frame == resize_at) {
    SDL_SetWindowSize(sdl_window(), resize_w, resize_h);
    host::log("%s: frame %llu resized to %dx%d", tag, (unsigned long long)frame, resize_w, resize_h);
  }
  const uint64_t at = toggle ? toggle : fullscreen;
  if (at && (frame == at || frame == at + 240)) {
    if (toggle) {
#ifdef __APPLE__
      if (void* w = ns_window(); w && (frame == at || mac_window_is_fullscreen(w))) mac_toggle_fullscreen(w);
#endif
    } else window_set_fullscreen(frame == at);
  }
#ifdef __APPLE__
  if (escape && frame == escape) {
    SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
    handle_event(e); e.type = SDL_KEYUP; handle_event(e);
    host::log("Mac test: Escape at frame %llu; fullscreen=%d, menu_press=%d, closed=%d",
              (unsigned long long)frame, window_is_fullscreen(), g_escape_press.load(), g_closed);
  }
#endif
  if (at && frame >= at && frame <= at + 300 && (frame - at) % 30 == 0) {
    int w = 0, h = 0; SDL_GL_GetDrawableSize(sdl_window(), &w, &h);
    host::log("%s: frame %llu fullscreen=%d drawable=%dx%d", tag, (unsigned long long)frame, window_is_fullscreen(), w, h);
  }
}

bool window_visible() {
  if (!g_hwnd || g_closed) return false;
  return (SDL_GetWindowFlags(sdl_window()) & SDL_WINDOW_SHOWN) != 0;
}

void window_set_client_size(int w, int h) {
  if (w < 320 || h < 240) return;
  g_pending_client_size.store(((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
}

double window_refresh_rate() {
  if (!g_hwnd) return 60.0;
  SDL_DisplayMode mode{};
  if (SDL_GetWindowDisplayMode(sdl_window(), &mode) == 0 && mode.refresh_rate > 1) return mode.refresh_rate;
  if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(sdl_window()), &mode) == 0 && mode.refresh_rate > 1) return mode.refresh_rate;
  return 60.0;
}

void window_set_message_callback(MessageCallback cb) { g_on_message = std::move(cb); }
void window_input_capture(bool capture) {
  // The pointer is hidden over the game and shown while a menu has input; applied by window_pump.
  g_ui_capture.store(capture);
}
bool window_ui_gamecube_pad(PadState& pad) { std::lock_guard<std::mutex> lock(g_ui_pad_mutex); pad = g_ui_pad; return g_ui_gamecube; }
bool window_ui_pads(PadState pads[4], PadState navigation_pads[4]) {
  std::lock_guard<std::mutex> lock(g_ui_pad_mutex);
  for (int i = 0; i < 4; ++i) {
    pads[i] = g_ui_pads[i];
    if (navigation_pads) navigation_pads[i] = g_ui_navigation_pads[i];
  }
  return g_ui_has_pad;
}
void window_set_resize_callback(ResizeCallback cb) { g_on_resize = std::move(cb); }
void window_set_dpi_callback(DpiCallback cb) { g_on_dpi = std::move(cb); }
bool window_take_fullscreen_toggle() { return g_fullscreen_toggle.exchange(false); }
#ifdef __APPLE__
bool window_take_activation() { return g_activated.exchange(false); }
#else
// Linux has no exclusive fullscreen to enter again when the window comes back.
bool window_take_activation() { return false; }
#endif
int window_take_trace_mark() { return g_trace_mark.exchange(0); }
bool window_take_practice_toggle() { return g_practice_toggle.exchange(false); }
bool window_take_escape() { return g_escape_press.exchange(false); }
void window_set_escape_closes(bool closes) { g_escape_closes.store(closes); }

void window_destroy() {
  if (!video_thread(SdlVideoThread::Call::Destroy)) return;
#ifdef __APPLE__
  g_deferred_fullscreen_show = false;
  g_escape_left_fullscreen = false;
  g_fullscreen = false;
#endif
  hidraw_stop();
  if (g_hwnd) { linux_compat::set_game_window(nullptr); SDL_DestroyWindow(sdl_window()); g_hwnd = nullptr; }
  g_sdl_video_thread.release();
}

// Do not even read window/embed state on another thread. exit_requested() is atomic in
// host.cpp; console waiters read it after this returns. The render owner keeps pumping
// even without frames (present_loop's empty-queue wait is bounded to 2 ms).
void window_pump() {
  if (!video_thread(SdlVideoThread::Call::Pump)) return;
  if (uint64_t want = g_pending_client_size.exchange(0))
    apply_client_size((int)(want >> 32), (int)(want & 0xFFFFFFFFu));
  {
    std::lock_guard<std::mutex> lock(g_title_mutex);
    if (g_title_for_window_pending && g_hwnd) SDL_SetWindowTitle(sdl_window(), g_title_for_window.c_str());
    g_title_for_window_pending = false;
  }
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    handle_event(e);
    if (g_closed) return;
  }
  if (!g_hwnd) return;
  const bool focused = (SDL_GetWindowFlags(sdl_window()) & SDL_WINDOW_INPUT_FOCUS) != 0;
#ifdef __APPLE__
  // No pointer over the game while it has focus and no menu has input; and none after it has rested
  // for a while even then.
  bool visible = !focused || g_ui_capture.load();
  if (visible && !g_pointer_rested && g_pointer_moved && GetTickCount64() - g_pointer_moved >= kPointerRestMs) g_pointer_rested = true;
  if (g_pointer_rested) visible = false;
#else
  const bool visible = cursor_idle::visible(GetTickCount64(), g_pointer_moved, focused, g_ui_capture.load());
#endif
  show_cursor(visible);
}

void window_set_title(const wchar_t* title) {
  if (!video_thread(SdlVideoThread::Call::Title)) return;
  if (g_hwnd) SDL_SetWindowTitle(sdl_window(), utf8_title(title).c_str());
}

// The first-launch pipeline compile: progress goes to the window title, which the window manager
// shows while the first frame is held back (no second window to steal focus).
namespace { std::wstring g_title_before_loading; bool g_loading_shown = false; }
void loading_show(const wchar_t* what, size_t done, size_t total) {
  if (!on_window_thread()) {
    // Another thread (the game's, at start, while the render thread runs the window): no events and
    // no SDL calls here; the window's thread shows the line.
    if (!g_hwnd) return;
    wchar_t line[256];
    swprintf(line, 256, L"%ls  |  %ls  %zu / %zu", window_title_base().c_str(), what, done, total);
    title_from_another_thread(utf8_title(line));
    g_loading_from_other_thread.store(true);
    return;
  }
  if (!video_thread(SdlVideoThread::Call::LoadingShow)) return;
  if (!g_hwnd || !(SDL_GetWindowFlags(sdl_window()) & SDL_WINDOW_SHOWN)) return;
  if (!g_loading_shown) { const char* t = SDL_GetWindowTitle(sdl_window()); g_title_before_loading = t ? std::wstring(t, t + std::strlen(t)) : L""; g_loading_shown = true; }
  wchar_t line[256];
  swprintf(line, 256, L"%ls  |  %ls  %zu / %zu", window_title_base().c_str(), what, done, total);
  SDL_SetWindowTitle(sdl_window(), utf8_title(line).c_str());
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    handle_event(e);
    if (g_closed) return;
  }
}
void loading_close() {
  if (g_loading_from_other_thread.exchange(false)) title_from_another_thread(utf8_title(window_title_base().c_str()));
  if (!on_window_thread()) return;
  if (!video_thread(SdlVideoThread::Call::LoadingClose)) return;
  if (!g_loading_shown) return;
  g_loading_shown = false;
  if (g_hwnd) SDL_SetWindowTitle(sdl_window(), utf8_title(g_title_before_loading.c_str()).c_str());
}
bool window_closed() { return g_closed; }
void window_client_size(int* w, int* h) { *w = g_client_w; *h = g_client_h; }
