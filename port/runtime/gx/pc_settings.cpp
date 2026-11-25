// The settings window's frame: its ImGui context and renderer binding, the category list and the page
// that slides in over it, and settings_frame, which runs every frame in the settings window and in the
// game window (overlays, the Practice menu, saves the game asks for). The pages are
// settings_page_<page>.cpp; the helpers they share are declared in pc_settings_internal.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"
#define STB_IMAGE_STATIC
// Every picture decoded here is a PNG: the menu art under ui_sources and the cosmetic companions,
// which an import keeps only when they are PNG files (cosmetic_vault.cpp png_companion). The other
// decoders are left out, and no side may be larger than a companion's.
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#ifdef _WIN32
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"
#endif
#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#endif
namespace gx {

std::unordered_map<std::string, std::unique_ptr<ImTextureData>> g_cosmetic_previews;

std::string ui_source_asset_path_uncached(const char* source, const char* filename);
// Resolved once per asset. The GD Melee page asks for its font atlas and artwork dozens of times a
// frame; resolving each call meant two filesystem checks and a module-path lookup every time,
// which held that page near 80 fps with spikes past 200 ms.
std::string ui_source_asset_path(const char* source, const char* filename) {
  static std::unordered_map<std::string, std::string> resolved;
  std::string key(source);
  key += '/';
  key += filename;
  auto found = resolved.find(key);
  if (found != resolved.end()) return found->second;
  return resolved.emplace(std::move(key), ui_source_asset_path_uncached(source, filename)).first->second;
}

std::string ui_source_asset_path_uncached(const char* source, const char* filename) {
  const std::filesystem::path source_asset =
      std::filesystem::path("port/runtime/gx/ui_sources") / source / "assets" / filename;
  if (std::filesystem::exists(source_asset)) return source_asset.string();
  wchar_t module[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    const std::filesystem::path packaged =
#ifdef __APPLE__
        mac_host::resources() / "ui_sources" / source / filename;
#else
        std::filesystem::path(module).parent_path() / "ui_sources" / source / filename;
#endif
    if (std::filesystem::exists(packaged)) return packaged.string();
  }
  return source_asset.string();
}

ImTextureData* cosmetic_preview(const std::string& path) {
  if (path.empty()) return nullptr;
  auto cached = g_cosmetic_previews.find(path);
  if (cached != g_cosmetic_previews.end()) return cached->second.get();
  std::unique_ptr<ImTextureData> texture;
  std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
  if (file && file.tellg() > 0 && file.tellg() <= 16 * 1024 * 1024) {
    const size_t size = (size_t)file.tellg();
    std::vector<unsigned char> encoded(size);
    file.seekg(0);
    if (file.read((char*)encoded.data(), size)) {
      int width = 0, height = 0, channels = 0;
      if (stbi_info_from_memory(encoded.data(), (int)size, &width, &height, &channels) &&
          width > 0 && height > 0 && (int64_t)width * height <= 4'000'000) {
        unsigned char* pixels = stbi_load_from_memory(encoded.data(), (int)size,
                                                       &width, &height, &channels, 4);
        if (pixels) {
          texture = std::make_unique<ImTextureData>();
          texture->Create(ImTextureFormat_RGBA32, width, height);
          std::memcpy(texture->Pixels, pixels, (size_t)width * height * 4);
          texture->UseColors = true;
          ImGui::RegisterUserTexture(texture.get());
          stbi_image_free(pixels);
        }
      }
    }
  }
  auto* result = texture.get();
  g_cosmetic_previews.emplace(path, std::move(texture));
  return result;
}

// The ImGui context and the Win32 platform backend are the same for every renderer backend.
std::atomic<bool> g_textures_dirty{false};
// "texpackoff <name>" per pack the player switched off, written with the rest of the settings.

const bool g_ui_diag = std::getenv("MELEE_UI_DIAG") != nullptr;   // logs settings panel state transitions
ImGuiStyle g_input_overlay_style;   // 0.6.61 look for the controller overlay, set up at context creation
std::atomic<bool> g_fill_window{false};
std::atomic<bool> g_close_requested{false};
bool settings_close_requested() { return g_close_requested.exchange(false, std::memory_order_relaxed); }
void settings_fill_window(bool on) { g_fill_window.store(on, std::memory_order_relaxed); }
std::atomic<int> g_initial_tab{-1};
void settings_set_initial_tab(int tab) { g_initial_tab.store(tab, std::memory_order_relaxed); }
bool settings_fills_window() { return g_fill_window.load(std::memory_order_relaxed); }
// One scale for everything ImGui draws in a frame. The panels are full of pixel literals (item
// widths, column offsets, button sizes, font sizes), so rather than scale each one, the whole frame
// is laid out in smaller units and ImGui's framebuffer scale draws it larger: settings_frame divides
// the display size by the scale, the pointer is divided to match, and the fonts are rasterised at
// the final pixel size, so nothing is stretched.
// g_window_scale is the standalone settings window's, from its monitor's DPI. The game window works
// its own out from its height (game_ui_scale). g_frame_ui_scale is whichever the last frame used.
std::atomic<float> g_window_scale{1.0f};
std::atomic<float> g_frame_ui_scale{1.0f};
ImVec2 g_frame_pixel_size(0, 0);   // the window's client area in pixels, before the division
void settings_set_window_scale(float scale) {
  g_window_scale.store(std::clamp(scale, 1.0f, 4.0f), std::memory_order_relaxed);
}
// The game window: laid out as if it were 1080 pixels high once it is taller than that, so a 1440p
// window draws the panels 1.33 times larger and a 4K one twice. At 1080 and below the scale is
// exactly 1 and nothing changes. MELEE_TEST_UI_SCALE=<scale> (tests) replaces the result, so a
// small hidden run can show the scaled layout.
static float game_ui_scale(float client_height) {
  static const float forced = [] {
    const char* v = host::test_env::getenv("MELEE_TEST_UI_SCALE");
    return v ? std::clamp((float)std::atof(v), 1.0f, 3.0f) : 0.0f;
  }();
  if (forced > 0.0f) return forced;
  return client_height > 1080.0f ? std::min(client_height / 1080.0f, 3.0f) : 1.0f;
}
bool settings_textures_dirty() { return g_textures_dirty.exchange(false, std::memory_order_relaxed); }

// The category icons of the settings window's list, by page.
static void draw_settings_icon(int index, ImVec2 p, ImU32 color) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float x = p.x, y = p.y;
  switch (index) {
    case 0: // display
      draw->AddRect(ImVec2(x - 11, y - 8), ImVec2(x + 11, y + 6), color, 2.0f, 0, 2.0f);
      draw->AddLine(ImVec2(x, y + 6), ImVec2(x, y + 11), color, 2);
      draw->AddLine(ImVec2(x - 6, y + 11), ImVec2(x + 6, y + 11), color, 2);
      break;
    case 1: // speaker
      draw->AddRectFilled(ImVec2(x - 11, y - 4), ImVec2(x - 5, y + 4), color);
      draw->AddTriangleFilled(ImVec2(x - 5, y - 4), ImVec2(x + 3, y - 9), ImVec2(x + 3, y + 9), color);
      draw->AddLine(ImVec2(x + 7, y - 7), ImVec2(x + 11, y - 3), color, 2);
      draw->AddLine(ImVec2(x + 11, y - 3), ImVec2(x + 11, y + 3), color, 2);
      draw->AddLine(ImVec2(x + 11, y + 3), ImVec2(x + 7, y + 7), color, 2);
      break;
    case 2: // gamepad
      draw->AddRect(ImVec2(x - 13, y - 6), ImVec2(x + 13, y + 8), color, 7.0f, 0, 2.0f);
      draw->AddLine(ImVec2(x - 8, y + 1), ImVec2(x - 2, y + 1), color, 2);
      draw->AddLine(ImVec2(x - 5, y - 2), ImVec2(x - 5, y + 4), color, 2);
      draw->AddCircleFilled(ImVec2(x + 6, y), 2, color);
      draw->AddCircleFilled(ImVec2(x + 10, y + 3), 2, color);
      break;
    case 3: // controls: directional cross
      draw->AddRect(ImVec2(x - 10, y - 10), ImVec2(x + 10, y + 10), color, 4.0f, 0, 2.0f);
      draw->AddLine(ImVec2(x - 6, y), ImVec2(x + 6, y), color, 2);
      draw->AddLine(ImVec2(x, y - 6), ImVec2(x, y + 6), color, 2);
      break;
    case 4: // layers
      draw->AddRect(ImVec2(x - 12, y - 9), ImVec2(x + 5, y + 5), color, 2.0f, 0, 2.0f);
      draw->AddRect(ImVec2(x - 5, y - 3), ImVec2(x + 12, y + 10), color, 2.0f, 0, 2.0f);
      break;
    default: // code brackets
      draw->AddLine(ImVec2(x - 3, y - 8), ImVec2(x - 10, y), color, 2);
      draw->AddLine(ImVec2(x - 10, y), ImVec2(x - 3, y + 8), color, 2);
      draw->AddLine(ImVec2(x + 3, y - 8), ImVec2(x + 10, y), color, 2);
      draw->AddLine(ImVec2(x + 10, y), ImVec2(x + 3, y + 8), color, 2);
      break;
  }
}

// The pages, by the number the window, --settings-tab and MELEE_TEST_SETTINGS_TAB use. 5 and 7 were
// Customize and Mods, both removed; the others keep their numbers.
static constexpr int kSettingsPages[] = {0, 1, 2, 3, 4, 6};
static constexpr int kSettingsPageCount = (int)(sizeof kSettingsPages / sizeof kSettingsPages[0]);

// The window's frame: its title shard, top left, and the chip plates at the foot of the panel (two on
// the category list, A SELECT and B BACK; one on a page, B BACK).
static void settings_clean_chrome(const char* heading, int chips) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  const ImVec2 end(p.x + s.x, p.y + s.y);
  // The title is anchored to the game's top-left, independently of the category stack
  // on the right. Only these shards and the cards paint over the live game.
  const ImVec2 display=ImGui::GetIO().DisplaySize;
  const float game_left=std::max(0.0f,(display.x-display.y*(73.0f/60.0f))*.5f);
  const float hx=game_left+18.0f, hy=18.0f;
  const float hw=std::clamp(display.x*.39f,300.0f,420.0f);
  ImDrawList* front=ImGui::GetForegroundDrawList();
  const ImVec2 header[]={ImVec2(hx+24,hy),ImVec2(hx+hw,hy),
      ImVec2(hx+hw-75,hy+94),ImVec2(hx-10,hy+94)};
  front->AddConvexPolyFilled(header,4,IM_COL32(226,20,126,245));
  const ImVec2 slash[]={ImVec2(hx+hw*.40f,hy),ImVec2(hx+hw*.55f,hy),
      ImVec2(hx+hw*.55f-75,hy+94),ImVec2(hx+hw*.40f-75,hy+94)};
  front->AddConvexPolyFilled(slash,4,IM_COL32(11,13,29,250));
  front->AddLine(ImVec2(hx+4,hy+94),ImVec2(hx+hw-78,hy+94),
                 IM_COL32(255,134,197,240),2.0f);
  front->AddText(ImGui::GetFont(),12.0f,ImVec2(hx+24,hy+12),
                 IM_COL32(255,235,248,255),PRODUCT_NAME_UPPER);
  const float heading_size=std::strlen(heading)>9?34.0f:42.0f;
  settings_slanted_text(front,settings_heading_font(),heading_size,
      ImVec2(hx+23,hy+46),IM_COL32_WHITE,heading,.08f);
  const float chip_w=(s.x-78.0f)*.5f;
  for(int chip=0;chip<chips;++chip) {
    const float x=p.x+11.0f+chip*(chip_w+5.0f);
    const ImVec2 shadow[]={ImVec2(x+11,end.y-24),ImVec2(x+chip_w+2,end.y-36),
                           ImVec2(x+chip_w-7,end.y-8),ImVec2(x,end.y+2)};
    draw->AddConvexPolyFilled(shadow,4,IM_COL32(0,0,0,110));
    const ImVec2 card[]={ImVec2(x+12,end.y-29),ImVec2(x+chip_w,end.y-41),
                         ImVec2(x+chip_w-10,end.y-11),ImVec2(x,end.y-1)};
    draw->AddConvexPolyFilled(card,4,chip==0?
        IM_COL32(145,14,89,248):IM_COL32(28,17,35,245));
    draw->AddLine(card[0],card[1],IM_COL32(255,135,204,220),1.8f);
  }
}

static void settings_clean_home(SettingsState& state) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  const ImVec2 saved = ImGui::GetCursorPos();
  state.clean_home_frames = std::min(24.0f,
      state.clean_home_frames + ImGui::GetIO().DeltaTime * 60.0f);
  const float row_h = std::min(61.0f, (s.y - 210.0f) / (float)kSettingsPageCount);
  const float step = row_h - 1.0f;
  // The rows overlap, so the focus frame is drawn once they are all down or the next row hides it.
  bool nav_frame_row = false;
  ImVec2 nav_frame_outline[7];
  int nav_frame_points = 0;
  for (int i = 0; i < kSettingsPageCount; ++i) {
    const int page = kSettingsPages[i];
    const float t = std::clamp((state.clean_home_frames - i * 1.25f) / 14.0f, 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);
    const float entry_x=p.x+19.0f+(1.0f-eased)*90.0f;
    const float y = p.y + 161.0f + i * step;
    const float width = s.x - 39.0f;
    ImGui::SetCursorScreenPos(ImVec2(entry_x, y-width*0.05f));
    ImGui::PushID(i);
    const bool clicked = settings_shaped_hit_button("##clean_category", ImVec2(width, row_h));
    const bool nav_frame = settings_nav_frame_on_item();
    if (page == state.active_tab) ImGui::SetItemDefaultFocus();
    const bool nav_focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (nav_focused && state.open && settings_nav_input_pressed()) state.active_tab = page;
    if (hovered && state.open) state.active_tab = page;
    const bool selected = state.active_tab == page;
    const float target=selected?1.0f:0.0f;
    state.clean_hover[i]+=(target-state.clean_hover[i])*
        std::min(1.0f,ImGui::GetIO().DeltaTime*13.0f);
    const float x=entry_x-state.clean_hover[i]*10.0f;
    // Only the active category reads as selected. The old
    // alternating accent rows made Gecko Codes look active even when untouched.
    const ImU32 face = selected ? IM_COL32(247, 31, 143, 250) :
                                  IM_COL32(12, 15, 28, 232);
    const float tilt = width * 0.10f;
    const ImVec2 depth[] = {ImVec2(x+19,y+8),ImVec2(x+width+3,y-tilt+8),
                            ImVec2(x+width-14,y+row_h-tilt+10),ImVec2(x-3,y+row_h+10)};
    const ImVec2 shadow[] = {ImVec2(x+17,y+12),ImVec2(x+width+6,y-tilt+12),
                             ImVec2(x+width-12,y+row_h-tilt+14),ImVec2(x-5,y+row_h+14)};
    draw->AddConvexPolyFilled(shadow,4,IM_COL32(0,0,0,70));
    draw->AddConvexPolyFilled(depth,4,IM_COL32(55,10,47,235));
    const ImVec2 left_fold[]={ImVec2(x+18,y),ImVec2(x+2,y+row_h*.38f),
                              ImVec2(x-8,y+row_h+7),ImVec2(x+3,y+row_h)};
    draw->AddConvexPolyFilled(left_fold,4,
        selected?IM_COL32(102,13,73,250):IM_COL32(18,19,37,245));
    const ImVec2 poly[] = {ImVec2(x + 18.0f, y), ImVec2(x + width, y-tilt),
                           ImVec2(x + width - 18.0f, y + row_h-tilt), ImVec2(x, y + row_h)};
    draw->AddConvexPolyFilled(poly, 4, face);
    if (nav_frame) {
      // Frame the button as players see it (report d6ad1693): face and left fold, plus the
      // slab on the last row; the next row covers the slab of every other row.
      nav_frame_row = true;
      const ImVec2 slab[] = {poly[0], poly[1], depth[1], depth[2], depth[3],
                             left_fold[2], left_fold[1]};
      const ImVec2 covered[] = {poly[0], poly[1], poly[2], poly[3], left_fold[2], left_fold[1]};
      const bool last = i == kSettingsPageCount - 1;
      nav_frame_points = last ? 7 : 6;
      std::copy_n(last ? slab : covered, nav_frame_points, nav_frame_outline);
    }
    draw->AddLine(ImVec2(x + 18.0f, y + 1.0f), ImVec2(x + width - 1.0f, y + 1.0f-tilt),
                  selected ? IM_COL32(255, 223, 245, 255) :
                             IM_COL32(220,72,142,145), 1.4f);
    draw->AddLine(poly[3],poly[2],selected?
        IM_COL32(104,10,70,220):IM_COL32(56,35,62,215),1.4f);
    draw_settings_icon(page, ImVec2(x + 38.0f, y + row_h * 0.5f-4.0f),
                       IM_COL32(255, 255, 255, 255));
    settings_slanted_text(draw, settings_heading_font(), 23.0f,
                  ImVec2(x + 69.0f, y + (row_h - 23.0f) * 0.5f-7.0f),
                  IM_COL32(255, 255, 255, 255), settings_page_title(page), 0.10f);
    if (clicked && state.open) {
      state.active_tab = page;
      state.clean_detail_open = true;
    }
  }
  if (nav_frame_row) settings_nav_frame_outline(draw, nav_frame_outline, nav_frame_points);
  // The A SELECT / B BACK chips drawn by settings_clean_chrome are real buttons: Select opens
  // the highlighted category, Back closes the window. Hover lifts and lightens the chip.
  {
    const float chip_w = (s.x - 78.0f) * .5f;
    const float bottom = p.y + s.y;
    for (int chip = 0; chip < 2; ++chip) {
      const float x = p.x + 11.0f + chip * (chip_w + 5.0f);
      ImGui::SetCursorScreenPos(ImVec2(x, bottom - 41.0f));
      ImGui::PushID(chip);
      const bool pressed = settings_shaped_hit_button("##clean_home_chip", ImVec2(chip_w, 40.0f));
      const bool nav_frame = settings_nav_frame_on_item();
      const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
      ImGui::PopID();
      state.footer_hover[chip] += ((hot ? 1.0f : 0.0f) - state.footer_hover[chip]) *
                                  std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
      const float glow = state.footer_hover[chip];
      const float lift = glow * 4.0f;
      const ImVec2 card[] = {ImVec2(x + 12, bottom - 29 - lift), ImVec2(x + chip_w, bottom - 41 - lift),
                             ImVec2(x + chip_w - 10, bottom - 11 - lift), ImVec2(x, bottom - 1 - lift)};
      if (nav_frame) settings_nav_frame(draw, card);
      if (glow > 0.01f) {
        draw->AddConvexPolyFilled(card, 4, chip == 0 ?
            IM_COL32(247, 31, 143, (int)(250 * glow)) :
            IM_COL32(70, 44, 88, (int)(245 * glow)));
        draw->AddLine(card[0], card[1], IM_COL32(255, 224, 246, (int)(255 * glow)), 2.0f);
      }
      if (pressed && state.open) {
        if (chip == 0) state.clean_detail_open = true;
        else state.open = false;
      }
    }
  }
  settings_slanted_text(draw,settings_heading_font(),13.0f,
      ImVec2(p.x+34.0f,p.y+s.y-27.0f),IM_COL32(255,239,249,255),
      "A  SELECT",.08f);
  settings_slanted_text(draw,settings_heading_font(),13.0f,
      ImVec2(p.x+39.0f+(s.x-78.0f)*.5f,p.y+s.y-27.0f),
      IM_COL32(255,239,249,255),"B  BACK",.08f);
  ImGui::SetCursorScreenPos(ImVec2(p.x + s.x - 37.0f, p.y + 16.0f));
  if (settings_close_hit("##close_clean", ImVec2(28.0f, 28.0f))) state.open = false;
  draw->AddText(ImVec2(p.x + s.x - 30.0f, p.y + 18.0f),
                IM_COL32(255, 255, 255, 255), "X");
  ImGui::SetCursorPos(saved);
}

void settings_context_create(void* window) {
  IMGUI_CHECKVERSION(); ImGui::CreateContext();
  auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
  // The duplicate-ID tooltip offers a debugger break; without a debugger that click crashed the game.
  io.ConfigDebugHighlightIdConflicts = false; io.ConfigDebugHighlightIdConflictsShowItemPicker = false;
  g_settings_old_font = io.Fonts->AddFontDefault();
#ifdef _WIN32
  if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
    g_settings_body_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
  if (g_settings_body_font) io.FontDefault = g_settings_body_font;
#else
  // Linux: the body font shipped beside the game (Source Sans 3), else the desktop's usual sans and
  // monospace fonts, else ImGui's own.
  {
    const char* bodies[] = {"Fonts/SourceSans3-Regular.otf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                            "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char* path : bodies)
      if (std::filesystem::exists(path)) { g_settings_body_font = io.Fonts->AddFontFromFileTTF(path, 16.0f); break; }
    if (g_settings_body_font) io.FontDefault = g_settings_body_font;
  }
#endif
  const std::string heading_font_path = ui_source_asset_path("gd_melee", "kit/SourceSans3-Black.otf");
  if (std::filesystem::exists(heading_font_path))
    g_settings_heading_font = io.Fonts->AddFontFromFileTTF(heading_font_path.c_str(), 28.0f);
  // ImGui's outline version of the old font, for scaled frames (see settings_old_font). Added last,
  // so the fonts above keep their places, and nothing is rasterised from it until it is used.
  g_settings_old_vector_font = io.Fonts->AddFontDefaultVector();
  settings_input::configure(io);
  ImGui::StyleColorsDark();
  auto& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.14f, 0.97f);
  style.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.08f, 0.19f, 0.60f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.30f, 1);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.20f, 0.42f, 1);
  style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.18f, 0.38f, 1);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.39f, 0.25f, 0.59f, 1);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.85f, 0.57f, 0.98f, 1);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.72f, 0.42f, 0.91f, 1);
  style.WindowRounding = 10.0f; style.ChildRounding = 8.0f;
  style.FrameRounding = 6.0f; style.FramePadding = ImVec2(12, 10);
  style.ItemSpacing = ImVec2(12, 14);
  style.ScaleAllSizes(1.15f);
  // The controller overlay keeps the exact 0.6.61 look (dark style scaled 1.25x, ImGui's own font).
  // The menu redesign above is for the settings panel only; players tune their overlay placement
  // and reading around the old one.
  {
    ImGuiStyle legacy;
    ImGui::StyleColorsDark(&legacy);
    legacy.ScaleAllSizes(1.25f);
    g_input_overlay_style = legacy;
  }
#ifndef _WIN32
  if (SDL_GetWindowFlags(static_cast<SDL_Window*>(window)) & SDL_WINDOW_VULKAN) ImGui_ImplSDL2_InitForVulkan(static_cast<SDL_Window*>(window));
  else ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window*>(window), SDL_GL_GetCurrentContext());
  ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
  // The window code decides when the pointer shows (window_sdl.inl show_cursor). Without this the
  // SDL backend turned it back on every frame, so it never hid on Linux.
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  host::window_set_message_callback([](void* w, uint32_t m, uintptr_t, intptr_t) {
    if (m != host::kSdlEventMessage) return false;
    SDL_Event e = *static_cast<SDL_Event*>(w);
    // The pointer arrives in pixels and a scaled frame is laid out in smaller units.
    if (const float scale = g_frame_ui_scale.load(std::memory_order_relaxed); scale != 1.0f) {
      if (e.type == SDL_MOUSEMOTION) { e.motion.x = (int)std::lround(e.motion.x / scale); e.motion.y = (int)std::lround(e.motion.y / scale); }
      if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { e.button.x = (int)std::lround(e.button.x / scale); e.button.y = (int)std::lround(e.button.y / scale); }
    }
    ImGui_ImplSDL2_ProcessEvent(&e);
    return false;   // as on Windows: the window still sees every event
  });
}
#else
  ImGui_ImplWin32_Init(window);
  host::window_set_message_callback([](void* w, uint32_t m, uintptr_t a, intptr_t b) {
    // The pointer arrives in pixels and a scaled frame is laid out in smaller units (see
    // g_frame_ui_scale), so its position is converted before ImGui reads it. Only this copy changes:
    // the window procedure still has the original message.
    if (m == WM_MOUSEMOVE) {
      if (const float scale = g_frame_ui_scale.load(std::memory_order_relaxed); scale != 1.0f) {
        const long x = std::lround((short)LOWORD(b) / scale), y = std::lround((short)HIWORD(b) / scale);
        b = (intptr_t)MAKELPARAM(x, y);
      }
    }
    return ImGui_ImplWin32_WndProcHandler((HWND)w, m, a, b) != 0;
  });
}
#endif

void settings_context_destroy() {
  for (auto& preview : g_cosmetic_previews)
    if (preview.second) ImGui::UnregisterUserTexture(preview.second.get());
  g_cosmetic_previews.clear();
  host::window_set_message_callback({}); host::window_input_capture(false);
#ifdef _WIN32
  ImGui_ImplWin32_Shutdown();
#else
  ImGui_ImplSDL2_Shutdown();
#endif
  ImGui::DestroyContext();
}

// HDR10 paper white of the settings panel (0 on an SDR screen); set by the Direct3D 12 presenter.
namespace { float g_settings_hdr_nits = 0.0f; }
#ifdef _WIN32
struct PcSettingsUI::Impl {
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  // One slot per ImGui texture: the font atlas and every skin preview and thumbnail the Mods and
  // Skins lists show (cached for the session). 64 ran out on a large skin library ("descriptor heap
  // exhausted"); 4096 costs about 128 KB.
  std::array<bool, 4096> used{};
  UINT stride = 0;
  SettingsState state;
};

void PcSettingsUI::set_hdr_paper_white(float nits) { g_settings_hdr_nits = nits; }

PcSettingsUI::PcSettingsUI(void* window, ID3D12Device* device, ID3D12CommandQueue* queue, const RenderOptions& options,
                           int rtv_format)
    : impl_(std::make_unique<Impl>()) {
  auto& state = *impl_;
  (void)options;
  state.state.open = settings_fills_window();   // the panel shows only in the settings window
  settings_context_create(window);
  D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = (UINT)state.used.size(); desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&state.heap)))) host::die("PC settings descriptor heap creation failed");
  state.stride = device->GetDescriptorHandleIncrementSize(desc.Type);
  ImGui_ImplDX12_InitInfo info{}; info.Device = device; info.CommandQueue = queue; info.NumFramesInFlight = 9;   // FSR frame generation draws the overlay twice per frame (two presents), and the Coming soon stickers under a shader preset once more
  info.RTVFormat = (DXGI_FORMAT)rtv_format; info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  info.SrvDescriptorHeap = state.heap.Get(); info.UserData = &state;
  info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto& s = *(Impl*)i->UserData;
    for (size_t slot = 0; slot < s.used.size(); ++slot) if (!s.used[slot]) {
      s.used[slot] = true; *cpu = s.heap->GetCPUDescriptorHandleForHeapStart(); *gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
      cpu->ptr += slot*s.stride; gpu->ptr += slot*s.stride; return;
    }
    host::die("PC settings descriptor heap exhausted");
  };
  info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto& s = *(Impl*)i->UserData;
    size_t slot = (cpu.ptr-s.heap->GetCPUDescriptorHandleForHeapStart().ptr)/s.stride;
    if (slot < s.used.size()) s.used[slot] = false;
  };
  if (!ImGui_ImplDX12_Init(&info)) host::die("PC settings renderer initialization failed");
}

PcSettingsUI::~PcSettingsUI() {
  ImGui_ImplDX12_Shutdown();
  settings_context_destroy();
}

bool PcSettingsUI::begin(RenderOptions& options) {
  ImGui_ImplDX12_NewFrame();
  return settings_frame(impl_->state, options);
}
#endif

// The settings file is the settings store's (settings_store.h, settings_file.cpp). The panel
// saves through it.
static bool write_settings_file(const RenderOptions& options) {
  return settings_store::write(options);
}

bool settings_frame(SettingsState& state, RenderOptions& options) {
  state.fill_window = g_fill_window.load(std::memory_order_relaxed);
  state.page_navigation.active = false;
  settings_page::observer = nullptr;
  slippi::native_practice::Snapshot practice = slippi::native_practice::snapshot();
  const bool practice_nav = state.practice_open;
  // Platform gamepad polling is disabled (Win32 at build time, SDL in settings_context_create).
  // ImGui 1.92.8 polls even with NavEnableGamepad off: clearing that flag alone did not stop
  // XInput's X/menu key or its competing events. Keep the window-switcher fix and practice's
  // selected port by feeding only the routed GameCube-format pad below. Clear stale pad state
  // through HasGamepad when navigation is closed, disconnected or waiting for neutral.
  {
    auto& io = ImGui::GetIO();
    settings_input::navigation(io, state.open || practice_nav || state.fill_window);
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  }
#ifdef _WIN32
  ImGui_ImplWin32_NewFrame();
#else
  ImGui_ImplSDL2_NewFrame();
#endif
  // The backend has just set the display size in pixels. On a scaled frame (the standalone settings
  // window on a scaled display, the game window above 1080 pixels high) lay it out in smaller units
  // instead and let the framebuffer scale draw it at full size (see g_frame_ui_scale). Rounded up so
  // a panel that fills the window still covers the last pixel row. At a scale of 1 the display size
  // is left alone.
  {
    auto& io = ImGui::GetIO();
    g_frame_pixel_size = io.DisplaySize;
    const float ui_scale = g_fill_window.load(std::memory_order_relaxed) ?
        g_window_scale.load(std::memory_order_relaxed) : game_ui_scale(io.DisplaySize.y);
    // The small allowance keeps float rounding from turning an exact 1080 into 1081.
    if (ui_scale != 1.0f)
      io.DisplaySize = ImVec2(std::ceil(io.DisplaySize.x / ui_scale - 0.001f),
                              std::ceil(io.DisplaySize.y / ui_scale - 0.001f));
#ifdef __APPLE__
    // SDL reports window points and the Retina drawable scale. Preserve both axes
    // so the overlay fills the same framebuffer as the game.
    io.DisplayFramebufferScale.x *= ui_scale;
    io.DisplayFramebufferScale.y *= ui_scale;
#else
    io.DisplayFramebufferScale = ImVec2(ui_scale, ui_scale);
#endif
    g_frame_ui_scale.store(ui_scale, std::memory_order_relaxed);
  }
  // UI navigation is armed only after the selected pad has returned to neutral. A held A that
  // opened no UI cannot click Start, Cancel, or acknowledge a later disconnect.
  host::PadState pads[4]{};
  host::PadState navigation_pads[4]{};
  const bool have_any_pad = host::window_ui_pads(pads, navigation_pads);
  if (!have_any_pad) for (host::PadState& p : pads) p.err = -1;
  host::settings_nav::Pad nav_pads[4];
  for (int i = 0; i < 4; ++i)
    nav_pads[i] = {pads[i].err == 0, pads[i].button, pads[i].stick_x, pads[i].stick_y};
  if (state.practice_release_capture) {
    // The pad that drove the menu back at rest and every button up, or one second at most: a pad
    // that never rests must not keep the game from its controllers (settings_nav.h).
    state.practice_release_seconds += ImGui::GetIO().DeltaTime;
    if (!host::settings_nav::hold_release(nav_pads, std::clamp(state.settings_controller_port, 0, 3),
                                          state.practice_release_seconds)) {
      if (state.practice_release_seconds >= host::settings_nav::kReleaseTimeout)
        for (int i = 0; i < 4; ++i)
          if (host::settings_nav::active(nav_pads[i]))
            host::log("controls: port %d was still held %.0f s after the menu closed (buttons %04X, stick %d,%d); the game has its controllers back",
                      i + 1, host::settings_nav::kReleaseTimeout, pads[i].button, pads[i].stick_x, pads[i].stick_y);
      state.practice_release_capture = false;
    }
  }
  if (!state.practice_release_capture) state.practice_release_seconds = 0.0f;
  const bool nav_active = state.open || practice_nav || state.fill_window;
  // The Practice menu starts on port 1's controller; then it, like the panel, follows the pad that
  // was last picked up, not the first one that reads as active. It used to read one port only, so a
  // second pad in someone's hands could not move it (0.9.97 report).
  if (practice_nav && !state.practice_nav_was) state.settings_controller_port = 0;
  state.practice_nav_was = practice_nav;
  const int followed = host::settings_nav::follow(nav_pads, state.settings_pad_was_at_rest.data(),
                                                  state.settings_controller_port, nav_active);
  state.settings_controller_port = followed;
  int nav_port = followed;
  if (pads[nav_port].err != 0)
    for (int i = 0; i < 4; ++i) if (pads[i].err == 0) { nav_port = i; break; }
  const host::PadState& pad = pads[nav_port];
  const bool have_pad = pad.err == 0;
  state.settings_stick_x = have_pad ? pad.stick_x : 0;
  state.settings_stick_y = have_pad ? pad.stick_y : 0;
  if (nav_active && !state.practice_nav_active) {
    ImGui::GetIO().ClearEventsQueue();
    ImGui::GetIO().ClearInputKeys();
    state.practice_pad_armed = false;
  }
  state.practice_nav_active = nav_active;
  if (!nav_active) state.practice_pad_armed = false;
  else if (!state.practice_pad_armed && have_pad)
    // Only what the menus read must be at rest (A, B, Start, the D-pad and the stick): a held X, Y,
    // L, R or Z, or a pad that reports one, kept the menu from ever listening (0.9.97 report).
    state.practice_pad_armed = (pad.button & 0x130F) == 0 && std::abs(pad.stick_x) < 30 && std::abs(pad.stick_y) < 30;
  if (g_ui_diag && nav_active) {
    static uint32_t last_sig = 0xFFFFFFFFu;
    const uint32_t sig = have_pad ? ((uint32_t)nav_port << 28) ^ pad.button ^
        ((uint32_t)(uint8_t)(pad.stick_x / 20) << 16) ^ ((uint32_t)(uint8_t)(pad.stick_y / 20) << 20) : 0xFFFFFFFEu;
    if (sig != last_sig) {
      last_sig = sig;
      host::log("ui diag: nav pad port %d present %d armed %d buttons %04X stick %d,%d c %d,%d",
                nav_port, (int)have_pad, (int)state.practice_pad_armed, have_pad ? pad.button : 0,
                have_pad ? pad.stick_x : 0, have_pad ? pad.stick_y : 0, have_pad ? pad.sub_x : 0, have_pad ? pad.sub_y : 0);
    }
  }
  // Test runs (MELEE_TEST_PHOB): the PhobGCC menu opens on adapter socket 1 with the panel
  // ("panel": only the Controls tab on that socket, with its Controller test and PhobGCC entries).
  {
    static bool phob_test_opened = false;
    const char* phob_test = host::test_env::getenv("MELEE_TEST_PHOB");
    if (!phob_test_opened && state.open && phob_test && std::strcmp(phob_test, "panel") != 0) { phob_test_opened = true; phob_native_request(0); }  }
  if (!state.open && phob_native_open()) phob_native_close();
  // The PhobGCC menu reads the controller itself: the panel gets none of it.
  if (have_pad && nav_active && state.practice_pad_armed && phob_native_open()) {
    auto& io = ImGui::GetIO();
    for (ImGuiKey k : {ImGuiKey_GamepadStart, ImGuiKey_GamepadBack, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                       ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
      io.AddKeyEvent(k, false);
  } else if (have_pad && nav_active && state.practice_pad_armed) {
    auto& io = ImGui::GetIO();
    // Backend navigation keys (including Enter/KeypadEnter) never also become pad keys.
    // Raw `pad` above still owns port following, neutral arming and release capture.
    settings_input::gamepad(io, navigation_pads[nav_port]);
  }
  // Test runs (MELEE_TEST_PANEL_NAV, hidden only): a D-pad press per 8 frames from the string
  // (U D L R, A for the A button, B for B), each logged with where the focus went.
  static int test_nav_step = -1;
  {
    static const char* test_nav = host::test_env::getenv("MELEE_TEST_PANEL_NAV");
    static int test_nav_frame = 0;
    if (test_nav && state.open && test_nav_frame++ >= 120) {
      const int step = (test_nav_frame - 120) / 8, phase = (test_nav_frame - 120) % 8;
      auto& io = ImGui::GetIO();
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
      io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
      const char c = step < (int)std::strlen(test_nav) ? test_nav[step] : 0;
      const bool down = phase == 0;
      io.AddKeyEvent(ImGuiKey_GamepadDpadUp, down && c == 'U');
      io.AddKeyEvent(ImGuiKey_GamepadDpadDown, down && c == 'D');
      io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, down && c == 'L');
      io.AddKeyEvent(ImGuiKey_GamepadDpadRight, down && c == 'R');
      io.AddKeyEvent(ImGuiKey_GamepadFaceDown, down && c == 'A');
      io.AddKeyEvent(ImGuiKey_GamepadFaceRight, down && c == 'B');
      test_nav_step = c && phase == 6 ? step : -1;
    }
  }
  ImGui::NewFrame();
  set_hud_scales(options.stock_hud_scale, options.damage_hud_scale, gecko::option_pal_stock_icons);
  set_low_poly_fighters(options.low_poly_fighters != 0);
  set_lightweight_level(options.lightweight);
  set_fod_reflections(options.fod_reflections);
  set_css_background_free(options.css_background_free);
  set_player_tags_always(options.player_tags_always != 0);
  set_arcade_display(ArcadeSource::Settings, arcade_settings_bits(options)); set_shield_colors(options.shield_color, options.shield_color_online);
  static bool test_tab_set = false;
  if (!test_tab_set) {
    test_tab_set = true;
    // --settings-tab (a desktop app button that opens one page), or the test hook: that page itself,
    // not the category list.
    const int initial = g_initial_tab.load(std::memory_order_relaxed);
    if (initial >= 0) { state.active_tab = settings_page_id(initial); state.clean_detail_open = true; }
    else if (const char* tab = host::test_env::getenv("MELEE_TEST_SETTINGS_TAB")) {
      state.active_tab = settings_page_id(std::atoi(tab));
      state.clean_detail_open = true;
    }
  }
  bool tab_pressed = host::window_take_practice_toggle();
  // Screenshot tooling (MELEE_TEST_PRACTICE_PAGE=<page>,<frame>): the Practice panel opens on that
  // page at that frame, wherever the game is.
  {
    static int test_page = -1, test_frame = 0;
    static bool parsed = false;
    if (!parsed) {
      parsed = true;
      if (const char* v = host::test_env::getenv("MELEE_TEST_PRACTICE_PAGE")) std::sscanf(v, "%d,%d", &test_page, &test_frame);
    }
    if (test_page >= 0 && host::retrace_count() >= (uint32_t)test_frame) {
      practice_ui::g_tab_request = test_page;
      test_page = -1;
      state.practice_open = true;
    }
    // Video tooling (MELEE_TEST_PRACTICE_TOUR=<frame>,<frames per page>): the panel opens at that
    // frame, shows every page in turn and closes again.
    static int tour_start = -1, tour_step = 0, tour_page = -1;
    static bool tour_parsed = false;
    if (!tour_parsed) {
      tour_parsed = true;
      if (const char* v = host::test_env::getenv("MELEE_TEST_PRACTICE_TOUR")) std::sscanf(v, "%d,%d", &tour_start, &tour_step);
    }
    if (tour_start >= 0 && tour_step > 0 && host::retrace_count() >= (uint32_t)tour_start) {
      const int page = (int)((host::retrace_count() - (uint32_t)tour_start) / (uint32_t)tour_step);
      if (page != tour_page) {
        tour_page = page;
        if (page < practice_ui::kTabCount) {
          practice_ui::g_tab_request = page;
          state.practice_open = true;
        } else {
          state.practice_open = false;
          state.practice_release_capture = true;
          tour_start = -1;
        }
      }
    }
  }
  if (tab_pressed && !state.open && !state.fill_window && practice.tab_available &&
      !replay_bar::active()) {   // while a replay plays, Tab is its fast forward
    const bool was_open = state.practice_open;
    state.practice_open = !state.practice_open;
    if (!state.practice_open && was_open) state.practice_release_capture = true;
  }
  // Settings the game or a scan changed by itself, saved at once (settings_file.cpp).
  settings_store::save_game_changes(options, state.mod_choices_seen, state.saved);
  // Ctrl+F2: write the next ~90 presented frames into capture\. For defects that only show in a real
  // session, where scripted runs reproduce nothing: press it while the problem is happening and
  // the frames themselves can be read afterwards.
  // Ctrl+F2 (F2 alone was hit by accident next to F1, and writing 90 frames to disk drops the game
  // to about 20 frames a second while it runs).
  if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && ImGui::GetIO().KeyCtrl) { request_frame_capture(90); host::log("capture: Ctrl+F2, writing the next 90 presented frames into capture\\"); }
  settings_store::follow_display_refresh(options, state.saved);
  {   // the picture's shape as drawn (4:3 with bars, or wider)
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x > 0 && screen.y > 0) host::window_note_picture_aspect(presented_aspect(options, (int)screen.x, (int)screen.y, true));
  }
  // 20XX TE's Input display is the same switch as the Controller overlay (te_pairs_linked), and the
  // overlay already shows the controllers: with it on, TE's own display is not drawn as well.
  const bool te_input_row = training_overlay::input_display() && !options.input_overlay;
  const bool clean_replay = gx::clean_replay_active();   // "Clean replays": nothing of ours over a replay
  training_overlay::draw(!clean_replay && options.native_source &&
                             ((RenderOptions::live_te_options() & 0x48000u) == 0x48000u || te_input_row) &&
                             !state.fill_window,
                         ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
  if (!clean_replay) net_overlay::draw(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
  replay_bar::draw(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y, state.open || state.practice_open || state.fill_window);
  // Start on the controller closes the settings window from any page.
  if (state.open && !phob_native_open() && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && state.rebind_action < 0)
    state.open = false;
  // B goes back from a page to the category list, and from the list closes the window.
  if (state.open && !phob_native_open() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && state.rebind_action < 0)
    settings_back_page(state);
  // Escape reaches here only in the settings window: in the game window it closes the game
  // (window.cpp). It leaves the PhobGCC menu for the Controls page; while a key is being captured it
  // only cancels the capture (input_poll_capture); otherwise it closes the window.
  if (phob_native_open() && host::window_take_escape()) {
    phob_native_close();
  } else if (host::window_take_escape() && state.rebind_action < 0) {
    state.open = false;
  }
  // The window closes through its own loop (settings_window.cpp): the panel itself stays open.
  if (state.fill_window && !state.open) {
    g_close_requested.store(true, std::memory_order_relaxed);
    state.open = true;
  }
  if (state.open) state.practice_open = false;
  if (practice.returning) state.practice_open = false;   // on the way back to Training after an online session
  // The clean side appearance keeps one physical panel and slides its content between pages.
  // The exit track remains visible until it finishes, including when Back is pressed mid motion.
  const float clean_step = ImGui::GetIO().DeltaTime * 60.0f;
  state.clean_detail_frames = std::clamp(state.clean_detail_frames +
      (state.clean_detail_open ? clean_step : -clean_step), 0.0f, 14.0f);
  // The PhobGCC menu (phob_menu.cpp) draws instead of the panel; the panel stays open underneath,
  // so the game keeps its controllers away exactly as with the panel.
  if (state.open && phob_native_open()) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    PhobNativeIn pin;
    const float aspect = presented_aspect(options, (int)screen.x, (int)screen.y, true);
    float gw = screen.x, gh = gw / std::max(aspect, 0.1f);
    if (gh > screen.y) { gh = screen.y; gw = gh * aspect; }
    pin.x0 = (screen.x - gw) * 0.5f; pin.y0 = (screen.y - gh) * 0.5f; pin.x1 = pin.x0 + gw; pin.y1 = pin.y0 + gh;
    if (have_pad) { pin.buttons = pad.button; pin.stick_x = pad.stick_x; pin.stick_y = pad.stick_y; }
    pin.armed = state.practice_pad_armed;
    pin.time = ImGui::GetTime();
    const PhobNativeOut pout = phob_native_frame(pin);
    if (pout.back_to_panel) { phob_native_close(); state.practice_pad_armed = false; }
    // The Start that closed it must not reach the game (it would pause): hold the controllers until let go.
    if (pout.close_all) { phob_native_close(); state.open = false; state.practice_release_capture = true; }
  }
  const bool panel_visible = state.open && state.fill_window && !phob_native_open();
  const bool practice_capture = state.practice_open || state.practice_release_capture;
  host::window_input_capture(state.open || practice_capture || replay_bar::wants_cursor());
  state.intervals[state.cursor++ % state.intervals.size()] = ImGui::GetIO().DeltaTime*1000.f;
  if (streamline::reflex_available() || streamline::reflex_latency_ms() > 0.0f)   // Vulkan on NVIDIA too
    state.latencies[state.latency_cursor++ % state.latencies.size()] = streamline::reflex_latency_ms();
  bool changed = false;
  // Hidden test hook: MELEE_TEST_LOOK_SEQUENCE="<frame>:<look>,..." picks those looks (by their number
  // in look_presets) at those panel frames, exactly as the Look list does, so going from one look to
  // another in one run can be captured (a look that leaves something of itself behind).
  if (const char* seq = host::test_env::getenv("MELEE_TEST_LOOK_SEQUENCE")) {
    static size_t next = 0;
    const char* p = seq;
    for (size_t i = 0; i < next && p && *p; ++i) { p = std::strchr(p, ','); if (p) ++p; }
    int at = 0, look = 0;
    if (p && *p && std::sscanf(p, "%d:%d", &at, &look) == 2 && ImGui::GetFrameCount() >= at) {
      apply_look_preset(options, look, d3d11_available());
      set_lightweight_level(options.lightweight);
      host::log("look test: frame %d, look %d: frame cap %.0f, sub-frame %d, lightweight %d, full models %d, cap lifted %d",
                ImGui::GetFrameCount(), look, options.fps_cap, (int)options.subframe, options.lightweight,
                options.look_full_models ? 1 : 0, options.look_cap_lifted ? 1 : 0);
      ++next;
      changed = true;
    }
  }
  if (panel_visible) {
    // The standalone settings window (--settings-window), the one place settings are changed, in its
    // one style, Clean side: the category list, and a page that slides in over it. Nothing of this
    // is drawn in the game window.
    const SettingsAppearanceScope appearance_scope;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float panel_w = std::max(300.0f, std::min(std::min(560.0f, std::max(390.0f, display.x * 0.38f)),
                                                    display.x - 12.0f));
    ImGui::SetNextWindowPos(ImVec2(display.x - panel_w, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, display.y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("PC settings", &state.open, ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoBackground);
    static bool screenshot_tab_set = false;
    if (!screenshot_tab_set) {
      screenshot_tab_set = true;
      // MELEE_SETTINGS_TAB=<page name> (screenshots): the window opens on that page.
      if (const char* requested = host::test_env::getenv("MELEE_SETTINGS_TAB"))
        for (int page : kSettingsPages)
          if (std::strcmp(requested, settings_page_name(page)) == 0) {
            state.active_tab = page;
            state.clean_detail_open = true;
          }
    }
    // Clean side draws its title in the foreground so it can extend beyond the narrow panel.
    settings_clean_chrome(state.clean_detail_open ? settings_page_title(state.active_tab) : "SETTINGS",
                          state.clean_detail_open ? 1 : 2);
    if (!state.clean_detail_open && state.clean_detail_frames <= 0.0f) {
      settings_clean_home(state);
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.06f, 0.19f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.91f, 0.12f, 0.48f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.24f, 0.58f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.07f, 0.14f, 1.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
      const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
      ImDrawList* draw = ImGui::GetWindowDrawList();
      ImGui::SetCursorScreenPos(ImVec2(p.x + s.x - 37.0f, p.y + 15.0f));
      if (settings_close_hit("##close_clean_detail", ImVec2(28.0f, 28.0f))) state.open = false;
      draw->AddText(ImVec2(p.x + s.x - 30.0f, p.y + 18.0f),
                    IM_COL32(255, 255, 255, 255), "X");
      ImGui::SetCursorScreenPos(ImVec2(p.x + 20.0f, p.y + 115.0f));
      const bool categories_clicked=settings_shaped_hit_button("##clean_categories",ImVec2(s.x-40.0f,34.0f));
      const bool categories_nav_frame=settings_nav_frame_on_item();
      const bool categories_hovered=ImGui::IsItemHovered()||ImGui::IsItemFocused();
      const ImVec2 ca=ImGui::GetItemRectMin(), cb=ImGui::GetItemRectMax();
      const ImVec2 category_shadow[]={ImVec2(ca.x+15,ca.y+5),ImVec2(cb.x,ca.y-2),
                                      ImVec2(cb.x-9,cb.y+5),ImVec2(ca.x+2,cb.y+5)};
      draw->AddConvexPolyFilled(category_shadow,4,IM_COL32(0,0,0,105));
      const ImVec2 category_card[]={ImVec2(ca.x+13,ca.y),ImVec2(cb.x-2,ca.y-7),
                                    ImVec2(cb.x-13,cb.y-7),ImVec2(ca.x,cb.y)};
      draw->AddConvexPolyFilled(category_card,4,categories_hovered?
          settings_accent():IM_COL32(26,17,35,235));
      draw->AddLine(category_card[0],category_card[1],IM_COL32(255,108,189,245),2.0f);
      if (categories_nav_frame) settings_nav_frame(draw,category_card);
      settings_slanted_text(draw,settings_heading_font(),16.0f,
          ImVec2(ca.x+23,ca.y+7),IM_COL32_WHITE,"<  CATEGORIES",.08f);
      if (categories_clicked) {
        state.clean_detail_open = false;
        state.clean_home_frames = 0.0f;
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x + 18.0f, p.y + 153.0f));
      ImGui::BeginDisabled(!state.open);
      // The page slides in from the right as it opens, and back out after Back.
      const float detail_t = std::clamp(state.clean_detail_frames / 14.0f, 0.0f, 1.0f);
      const float slide = detail_t * detail_t * (3.0f - 2.0f * detail_t);
      const ImVec2 content_origin = ImGui::GetCursorPos();
      ImGui::SetCursorPos(ImVec2(content_origin.x + ImGui::GetWindowWidth() * 0.65f * (1.0f - slide),
                                 content_origin.y));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f + 0.65f * detail_t);
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.025f, 0.055f, 0.78f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 20.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
      // The page ends 84 pixels above the bottom: room for a save error over the Back chip.
      ImGui::BeginChild("##settings_content", ImVec2(0, -84.0f), ImGuiChildFlags_NavFlattened,
                        ImGuiWindowFlags_NoScrollWithMouse);
      {
        // Smooth wheel scrolling at the presented frame rate. ImGui's own wheel scroll jumps a whole
        // notch in one frame, which reads as 60 Hz stepping on a high-refresh display. The target
        // follows any other scroll change (scrollbar drag, keyboard or controller navigation).
        ImGuiWindow* content = ImGui::GetCurrentWindow();
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID target_id = ImGui::GetID("##smooth_scroll_target");
        const ImGuiID applied_id = ImGui::GetID("##smooth_scroll_applied");
        const float current = content->Scroll.y;
        float target = storage->GetFloat(target_id, current);
        const float applied = storage->GetFloat(applied_id, current);
        if (std::fabs(current - applied) > 0.5f) target = current;
        const ImGuiIO& scroll_io = ImGui::GetIO();
        if (GImGui->HoveredWindow == content && scroll_io.MouseWheel != 0.0f)
          target -= scroll_io.MouseWheel * std::max(60.0f, ImGui::GetTextLineHeightWithSpacing() * 4.0f);
        target = std::clamp(target, 0.0f, content->ScrollMax.y);
        float next = current + (target - current) * std::min(1.0f, scroll_io.DeltaTime * 16.0f);
        if (std::fabs(target - next) < 0.5f) next = target;
        if (next != current) ImGui::SetScrollY(next);
        storage->SetFloat(target_id, target);
        storage->SetFloat(applied_id, next);
      }
      ImGui::PushTextWrapPos(0.0f);
      // Search every page at once (settings_search_box, pc_settings_search.cpp). "Go to" on a result
      // switches to its page first.
      if (g_goto_page >= 0 && !g_goto_label.empty() && state.active_tab != g_goto_page) state.active_tab = g_goto_page;
      settings_search_box();
      if (state.open && state.clean_detail_open) {
        state.page_navigation.begin(state.active_tab, 0);
        settings_page::observer = &state.page_navigation;
      }
      SettingsPageContext page_context{state, options, changed};
      if (settings_page_shown(state.active_tab, 0)) settings_page_video(page_context);
      if (settings_page_shown(state.active_tab, 1)) settings_page_audio(page_context);
      if (settings_page_shown(state.active_tab, 2)) settings_page_game(page_context);
      if (settings_page_shown(state.active_tab, 3)) settings_page_controls(page_context);
      if (settings_page_shown(state.active_tab, 4)) settings_page_overlays(page_context);
      if (settings_page_shown(state.active_tab, 6)) settings_page_gecko(page_context);
      settings_pages_end();   // the last section closes; a search sizes the list to its results
      settings_page::observer = nullptr;
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ImGui::PopStyleVar(3);
      ImGui::PopStyleColor();
      const ImVec2 content_size = ImGui::GetItemRectSize();
      // Keep the footer anchored while the page itself slides in. BeginChild advances the parent
      // cursor by the displaced child position, so restore its undisturbed layout position here.
      ImGui::SetCursorPos(ImVec2(content_origin.x, content_origin.y + content_size.y));
      ImGui::PopStyleVar();
      settings_page_footer(state);
      ImGui::EndDisabled();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(4);
    }
    // Every change is saved on its own, once the control that changed it is released (a slider
    // being dragged writes once, at the end, not sixty times a second).
    if (changed) state.dirty = true;
    if (state.dirty && !ImGui::IsAnyItemActive()) {
      state.dirty = false;
      state.saved = write_settings_file(options);
    }
    if (!settings_store::save_error().empty()) {
      const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
      ImGui::SetCursorScreenPos(ImVec2(p.x + 20.0f, p.y + s.y - 80.0f));
      ImGui::PushTextWrapPos(s.x - 20.0f);
      ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.36f, 1.0f), "%s", settings_store::save_error().c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }
  const bool practice_was_open = state.practice_open;
  if (!state.fill_window)
    draw_native_practice(state, options, practice, pad, have_pad);
  if (!state.fill_window && replay_control::active() && !gx::clean_replay_active())
    draw_replay_practice(options, pad, have_pad);
  // A watcher's window that opened after the match began: while it catches up (not drawn), a plain
  // card; the match appears as it is now, never as a fast-forward.
  if (replay_control::catching_up()) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(8, 10, 16, 255));
    const char* line = "Joining the match live...";
    const ImVec2 size = ImGui::CalcTextSize(line);
    dl->AddText(ImVec2((screen.x - size.x) * 0.5f, (screen.y - size.y) * 0.5f), IM_COL32(230, 232, 240, 255), line);
  }
  if (!state.fill_window && !replay_control::active() && walter_egg::active()) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float aspect = presented_aspect(options, (int)screen.x, (int)screen.y, true);
    float gw = screen.x, gh = gw / std::max(aspect, 0.1f);
    if (gh > screen.y) { gh = screen.y; gw = gh * aspect; }
    walter_egg::Frame ef;
    ef.game_x0 = (screen.x - gw) * 0.5f; ef.game_y0 = (screen.y - gh) * 0.5f;
    ef.game_x1 = ef.game_x0 + gw; ef.game_y1 = ef.game_y0 + gh;
    ef.time = ImGui::GetTime();
    walter_egg::draw(ef);
  }
  if (practice_was_open && !state.practice_open) state.practice_release_capture = true;
  // Practice menu changes (the CPU, your setups, skipped codes, the hint count) are saved as they happen.
  // The panel's own autosave runs only while the panel shows, so these were lost at exit before.
  if (state.dirty && !panel_visible && !ImGui::IsAnyItemActive()) {
    state.dirty = false;
    state.saved = write_settings_file(options);
  }
  // Test hook for screenshots: MELEE_TEST_OVERLAY=1 shows the controller overlay with its values.
  static const bool test_overlay = host::test_env::getenv("MELEE_TEST_OVERLAY") != nullptr;
  if (test_overlay) { options.input_overlay = true; options.input_overlay_values = true; if (const char* k = host::test_env::getenv("MELEE_TEST_KNOB")) options.input_overlay_stick = std::atoi(k); }
  if (options.input_overlay && !gx::clean_replay_active()) {
    const int mask = options.input_overlay_ports ? options.input_overlay_ports : 1;
    const bool lone = (mask & (mask - 1)) == 0;   // exactly one port selected
    int row = 0;
    // Draw with the 0.6.61 style and font; restore the menu style afterwards.
    ImGuiStyle& live_style = ImGui::GetStyle();
    const ImGuiStyle menu_style = live_style;
    live_style = g_input_overlay_style;
    if (g_settings_old_font) ImGui::PushFont(settings_old_font());
    bool overlay_moved = false;
    for (int i = 0; i < 4; ++i)
      if (mask & (1 << i)) draw_input_overlay(i, row++, lone, state.open, options.input_overlay_hide_border, options.input_overlay_values,
                                              options.input_overlay_stick, options.input_overlay_rect, &overlay_moved);
    if (overlay_moved && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.dirty = true;   // saved once the drag ends
    if (g_settings_old_font) ImGui::PopFont();
    live_style = menu_style;
  }
  // Below here is what only makes sense with a match behind it: the frame time graph, which in the
  // standalone settings window would report that window's own frame rate. The notices the game
  // window used to show over the game go to the log instead (log_game_notices).
  // The controller display above is deliberately left on: it shows live pad input, which is exactly
  // what someone checking their bindings in this window wants to see.
  // Skipped as a block, never by returning: ImGui::Render() is below, and a frame that leaves this
  // function without it hands the renderer draw data that was never built, which is an access
  // violation on the next present. 0.3.2 shipped exactly that as an early return here and crashed
  // the launcher's settings window.
  if (!state.fill_window) {
    const bool clean_replay = gx::clean_replay_active();
    log_game_notices();
    // The plain readouts: frame rate and, while online, the ping. Small, top left, no window
    // chrome, the way a Dolphin OSD line looks, and separate from the performance graph.
    // Reflex flash indicator: a white square on the frame A goes down on port 1, and the matching
    // marker, which a latency analyzer monitor or LDAT times against the photons.
    gx::set_latency_test(options.latency_test);
    if (const int fill = gx::presented_test_fill(); fill >= 0) {
      const ImVec2 size = ImGui::GetIO().DisplaySize;
      const ImU32 color = fill ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
      ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), size, color);
    }
    if (options.reflex_flash && options.reflex_mode > 0) {
      static bool a_was_down = false;
      host::PadState pads[4]{};
      host::input_last_pads(pads);
      const bool a_down = (pads[0].button & 0x0100) != 0;
      if (a_down && !a_was_down) {
        const OverlayBounds bounds = overlay_bounds();
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(bounds.left, bounds.top), ImVec2(bounds.left + 64, bounds.top + 64), IM_COL32(255, 255, 255, 255));
        streamline::pcl_marker(7);   // eTriggerFlash
      }
      a_was_down = a_down;
    }
    // Drawn as lines of text like Dolphin's: the frame rate, and the ping on the line under it.
    // The performance graph opens below them; if it is dragged over them, they move below it.
    static ImVec2 perf_min(0, 0), perf_max(0, 0);   // the graph's rectangle last frame, if shown
    // Test hook for screenshots: MELEE_TEST_READOUT=1 shows both lines and the graph.
    static const bool test_readout = host::test_env::getenv("MELEE_TEST_READOUT") != nullptr;
    if (test_readout) { options.show_fps = true; options.show_ping = true; options.performance_overlay = true; }
    const bool ping_line = options.show_ping && (slippi::online::is_online_match() || test_readout);
    if (!ping_line || clean_replay) state.readout_ping_at = -1.0;
    const float line_h = ImGui::GetTextLineHeight() + 2.0f;
    const float readout_h = 8.0f + line_h * 4;       // room for every line, so the graph never moves
    const bool latency_line = options.reflex_stats && streamline::reflex_latency_ms() > 0.0f;
    float vram_used = 0, vram_total = 0;
    const bool vram_line = options.show_vram && vram_usage(&vram_used, &vram_total);
    if ((options.show_fps || ping_line || latency_line || vram_line) && !clean_replay) {
      char lines[4][40];
      int n = 0;
      if (options.show_fps) {
        // Frames shown, and the game's own rate when it falls short of 60 (the frames shown can be many
        // more than the game steps: a high count over a slow game looked fine while it played badly).
        const double game_hz = host::game_rate();
        if (game_hz > 20.0 && game_hz < 57.0)
          std::snprintf(lines[n++], sizeof lines[0], "FPS: %.0f (game %.0f Hz)", ImGui::GetIO().Framerate, game_hz);
        else
          std::snprintf(lines[n++], sizeof lines[0], "FPS: %.0f", ImGui::GetIO().Framerate);
      }
      if (ping_line) {
        // Hold Teams' per-port readings and the singles reading together.
        const double now = ImGui::GetTime();
        const size_t remote_count = slippi::online::remote_ping_count();
        if (options.live_ping || state.readout_ping_at < 0.0 || now - state.readout_ping_at >= 1.0 ||
            now < state.readout_ping_at || state.readout_remote_pings.size() != remote_count) {
          state.readout_remote_pings = slippi::online::remote_pings_ms();
          state.readout_ping = slippi::online::ping_ms();
          state.readout_ping_at = now;
        }
        const auto& pings = state.readout_remote_pings;
        if (pings.size() > 1) {
          int used = std::snprintf(lines[n], sizeof lines[0], "Ping:");
          for (size_t i = 0; i < pings.size() && used > 0 && used < (int)sizeof lines[0]; ++i)
            used += std::snprintf(lines[n] + used, sizeof lines[0] - used, i ? " | %d" : " %d", pings[i]);
          if (used > 0 && used < (int)sizeof lines[0]) std::snprintf(lines[n] + used, sizeof lines[0] - used, " ms");
          ++n;
        } else {
          std::snprintf(lines[n++], sizeof lines[0], "Ping: %d ms", state.readout_ping);
        }
      }
      if (options.reflex_stats && streamline::reflex_latency_ms() > 0.0f && n < 3)
        std::snprintf(lines[n++], sizeof lines[0], "Render latency: %.1f ms", streamline::reflex_latency_ms());
      if (vram_line) std::snprintf(lines[n++], sizeof lines[0], "VRAM: %.1f / %.1f GB", vram_used, vram_total);
      const OverlayBounds bounds = overlay_bounds();
      ImVec2 at(bounds.left + 10, bounds.top + 8);
      float wide = 0;
      for (int i = 0; i < n; ++i) wide = std::max(wide, ImGui::CalcTextSize(lines[i]).x);
      const bool graph_there = options.performance_overlay && perf_max.x > perf_min.x &&
                               at.x < perf_max.x && at.x + wide > perf_min.x && at.y < perf_max.y && at.y + line_h * n > perf_min.y;
      if (graph_there) at.y = perf_max.y + 6;
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      for (int i = 0; i < n; ++i) {
        const ImVec2 p(at.x, at.y + line_h * i);
        fg->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), lines[i]);
        fg->AddText(p, IM_COL32(0, 255, 255, 255), lines[i]);
      }
    }
    if (options.performance_overlay && !clean_replay) {
      // Draggable, and resizable by its bottom-right corner (drag it smaller if it is in the way);
      // NoTitleBar keeps it out of the way otherwise. Opens under the FPS and ping lines.
      const OverlayBounds bounds = overlay_bounds();
      ImGui::SetNextWindowPos(ImVec2(bounds.left + 12, bounds.top + ((options.show_fps || options.show_ping) ? 8.0f + readout_h : 12.0f)), ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSize(ImVec2(266, 0), ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSizeConstraints(ImVec2(120, 40), ImVec2(FLT_MAX, FLT_MAX));
      ImGui::SetNextWindowBgAlpha(0.75f);
      ImGui::Begin("Performance", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
      clamp_overlay_window();
      const float plot_w = ImGui::GetContentRegionAvail().x;
      ImGui::Text("%.0f presentations/s | %.2f ms", ImGui::GetIO().Framerate, 1000.f/std::max(1.f, ImGui::GetIO().Framerate));
      ImGui::PlotLines("##frametimes", state.intervals.data(), (int)state.intervals.size(), state.cursor % state.intervals.size(), nullptr, 0, 33.4f, ImVec2(plot_w, 60));
      // Render latency: works whether or not Reflex's low-latency mode is on, so Native has a number
      // to show too (and something to compare "On" against). The breakdown adds up to roughly the
      // total; osRenderQueue and driver overlap the other stages a little, per NVIDIA's own report.
      // Vulkan on NVIDIA publishes the same report (VK_NV_low_latency2) without Streamline.
      const bool reflex_here = streamline::reflex_available();
      if (reflex_here || streamline::reflex_latency_ms() > 0.0f) {
        const float lat = streamline::reflex_latency_ms();
        ImGui::Text("Render latency: %.2f ms%s", lat, reflex_here && options.reflex_mode == 0 ? " (Reflex off)" : "");
        ImGui::PlotLines("##latency", state.latencies.data(), (int)state.latencies.size(),
                         state.latency_cursor % state.latencies.size(), nullptr, 0, std::max(8.0f, lat * 2.0f), ImVec2(plot_w, 60));
        const streamline::ReflexBreakdown b = streamline::reflex_breakdown();
        ImGui::TextWrapped("Sim %.2f | Submit %.2f | Driver %.2f | OS queue %.2f | GPU %.2f ms",
                           b.sim, b.render_submit, b.driver, b.os_queue, b.gpu_render);
      }
      perf_min = ImGui::GetWindowPos();
      perf_max = ImVec2(perf_min.x + ImGui::GetWindowSize().x, perf_min.y + ImGui::GetWindowSize().y);
      ImGui::End();
    } else {
      perf_min = perf_max = ImVec2(0, 0);
    }
  }
  // Same rule as at the top of the frame.
  host::window_input_capture(state.open || practice_capture || state.practice_release_capture ||
                             replay_bar::wants_cursor());
  // Open/close actions above also apply to this frame's resulting state. The
  // neutral-release capture still prevents a closing A/B/Start reaching Melee.
  settings_input::navigation(ImGui::GetIO(), state.open || state.practice_open || state.fill_window);
  if (!state.fill_window && options.show_player_nicknames)
    draw_player_nicknames();
  if (!state.fill_window) draw_combo_counter();
  if (!state.fill_window) draw_community_code_overlays();
  {
    static bool logged_open = false;
    static int logged_page = -1;
    if (state.open && !logged_open)
      host::log("settings: window opened (port %d, page %s)", state.settings_controller_port + 1,
                settings_page_name(state.active_tab));
    if (state.open && state.active_tab != logged_page)
      host::log("settings: page %s", settings_page_name(state.active_tab));
    if (state.open && state.page_navigation.active)
      if (const char* label = state.page_navigation.changed_focus())
        host::log("settings: focus %s", label);
    if (!state.open && logged_open) host::log("settings: window closed");
    logged_open = state.open;
    logged_page = state.open ? state.active_tab : -1;
    if (!state.open || !state.page_navigation.active) state.page_navigation.leave();
  }
  if (g_ui_diag) {
    // One line per change, so a report of "the menu jumped" can be read back from the log.
    static int last_open = -1, last_tab = -1, last_detail = -1;
    const int detail = state.clean_detail_open ? 1 : 0;
    if (last_open != (int)state.open || last_tab != state.active_tab || last_detail != detail) {
      host::log("ui diag: open %d tab %d detail %d", (int)state.open, state.active_tab, detail);
      last_open = state.open; last_tab = state.active_tab; last_detail = detail;
    }
  }
  const ImGuiIO& diag_io = ImGui::GetIO();
  if (g_ui_diag && (diag_io.MouseClicked[0] || diag_io.MouseReleased[0]))
      host::log("ui diag: mouse %s at %.0f,%.0f hovered id %08X active id %08X nav id %08X",
                diag_io.MouseClicked[0] ? "down" : "up", diag_io.MousePos.x, diag_io.MousePos.y,
                ImGui::GetHoveredID(), ImGui::GetActiveID(), ImGui::GetFocusID());
  if (test_nav_step >= 0) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* w = g.NavWindow;
    host::log("panel nav: step %d nav id %08X active %08X window %s rect y %.0f..%.0f scroll %.0f/%.0f", test_nav_step,
              g.NavId, g.ActiveId, w ? w->Name : "-", w ? w->NavRectRel[0].Min.y : 0.f, w ? w->NavRectRel[0].Max.y : 0.f,
              w ? w->Scroll.y : 0.f, w ? w->ScrollMax.y : 0.f);
  }
  ImGui::Render();
  // HDR10 output: the panel's sRGB colours as PQ at paper white (BT.2020), so it reads the same
  // as on an SDR screen.
  if (g_settings_hdr_nits > 0.0f) {
    auto pq = [](float nits) {
      const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), 0.1593017578125f);
      return std::pow((0.8359375f + 18.8515625f * y) / (1.0f + 18.6875f * y), 78.84375f);
    };
    auto decode = [](uint32_t v) {
      const float c = v / 255.0f;
      return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    ImDrawData* data = ImGui::GetDrawData();
    for (int list_index = 0; data && list_index < data->CmdListsCount; ++list_index) {
      for (ImDrawVert& vertex : data->CmdLists[list_index]->VtxBuffer) {
        const float r = decode((vertex.col >> IM_COL32_R_SHIFT) & 0xff), g = decode((vertex.col >> IM_COL32_G_SHIFT) & 0xff),
                    b = decode((vertex.col >> IM_COL32_B_SHIFT) & 0xff);
        const float r2 = 0.6274f * r + 0.3293f * g + 0.0433f * b, g2 = 0.0691f * r + 0.9195f * g + 0.0114f * b,
                    b2 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
        auto q = [&](float lin) { return (uint32_t)std::lround(pq(lin * g_settings_hdr_nits) * 255.0f); };
        vertex.col = (vertex.col & IM_COL32_A_MASK) | (q(r2) << IM_COL32_R_SHIFT) | (q(g2) << IM_COL32_G_SHIFT) |
                     (q(b2) << IM_COL32_B_SHIFT);
      }
    }
  }
  return changed;
}

#ifdef _WIN32
void PcSettingsUI::draw(ID3D12GraphicsCommandList* list) {
  // A panel rebuilt this frame (HDR switched on or off) has not run a frame yet: nothing to draw.
  ImDrawData* data = ImGui::GetCurrentContext() ? ImGui::GetDrawData() : nullptr;
  if (!data) return;
  ID3D12DescriptorHeap* heap = impl_->heap.Get(); list->SetDescriptorHeaps(1, &heap);
  ImGui_ImplDX12_RenderDrawData(data, list);
}

#endif
#ifndef _WIN32
// No Direct3D 12 here: no HDR swap chain.
bool hdr_display_available() { return false; }
#endif
}
