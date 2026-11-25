// The settings window's Video: quality presets, display and window, renderer and upscalers, frame rate,
// looks, texture packs, shaders, HDR, and the Advanced group.
// A page of the settings window: settings_frame (pc_settings.cpp) calls it, and it draws with the
// window's shared helpers (pc_settings_internal.h). The values it edits are the settings store's
// (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// Video page (tab 0).
void settings_page_video(SettingsPageContext& page) {
  SettingsState& state = page.state;
  RenderOptions& options = page.options;
  bool& changed = page.changed;
    // One width for every combo and slider on this tab, so their labels all begin at the same x.
    // Left to itself ImGui sizes each control from the space its own label needs, which staggered
    // the labels down the column.
    ImGui::PushItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 135.0f, 145.0f, 330.0f));
    // ---- Quality presets: one click for people who do not want to learn the settings below ----
    // Each sets the image settings and the frame rate; the controls below still show and change
    // every value, and the row says "Custom" once any of them differs from all four.
    {
      struct Preset { const char* name; const char* tip; int efb; int ssaa; int aniso; int dlss; double fps; SubFrameMode sub; };
      const bool dlaa_ok = streamline::available() && !state.running_d3d11;
      const Preset presets[] = {
        {"Low", "For laptops and integrated graphics: native resolution, no anti-aliasing,\n60 frames per second, no sub-frame animation.", 1, 1, 1, 0, 60, SubFrameMode::Off},
        {"Medium", "For most older PCs: 2x resolution, 4x filtering,\nyour monitor's refresh rate with smooth in-between frames.", 2, 1, 4, 0, -1, SubFrameMode::Authored},
        // 4x filtering since 0.9.0: a player found 16x, the old default, made play less smooth.
        {"High", "Recommended, and the default: resolution matched to your window, 4x filtering,\nyour monitor's refresh rate with smooth in-between frames.", 0, 1, 4, 0, -1, SubFrameMode::Authored},
        // DLAA already smooths every edge, so Ultra uses it alone where it exists. Stacked on 4x
        // supersampling it ran DLAA over an image several times 4K: a large drop in frame rate, and
        // out of video memory on 8 GB cards once a recorder was running too.
        {"Ultra", dlaa_ok ? "High with 16x filtering, plus NVIDIA DLAA for the smoothest edges."
                          : "High with 16x filtering, plus 4x supersampling for the smoothest edges.",
         0, dlaa_ok ? 1 : 2, 16, dlaa_ok ? 1 : 0, -1, SubFrameMode::Authored},
      };
      constexpr int kPresetCount = sizeof(presets) / sizeof(presets[0]);
      auto matches = [&](const Preset& pr) {
        return options.efb_scale == pr.efb && options.ssaa == pr.ssaa && options.anisotropy == pr.aniso &&
               options.dlss_mode == pr.dlss && options.fps_cap == pr.fps && options.subframe == pr.sub;
      };
      int current = -1;
      for (int i = kPresetCount - 1; i >= 0 && current < 0; --i) if (matches(presets[i])) current = i;
      // Settings that match no preset are the player's own: keep them as Custom.
      if (current < 0)
        settings_store::panel_values().custom_preset = {true, options.efb_scale, options.ssaa, options.anisotropy, options.dlss_mode, options.fps_cap, (int)options.subframe};
      const char* preset_labels[kPresetCount + 1];
      for (int i=0;i<kPresetCount;++i) preset_labels[i]=presets[i].name;
      preset_labels[kPresetCount]="Custom";
      int picked=current<0?kPresetCount:current;
      bool preset_changed=false;
      preset_changed=settings_combo("Quality",&picked,preset_labels,kPresetCount+1);
      if (preset_changed) {
        if (picked<kPresetCount) {
          const Preset& pr=presets[picked];
          options.efb_scale=pr.efb; options.ssaa=pr.ssaa; options.anisotropy=pr.aniso;
          options.dlss_mode=pr.dlss; options.fps_cap=pr.fps; options.subframe=pr.sub;
          options.low_spec=false; options.lightweight=0; changed=true;
        } else if (settings_store::panel_values().custom_preset.set) {
          const settings_store::CustomPreset& c=settings_store::panel_values().custom_preset;
          options.efb_scale=c.efb;options.ssaa=c.ssaa;options.anisotropy=c.aniso;
          options.dlss_mode=c.dlss;options.fps_cap=c.fps;options.subframe=(SubFrameMode)c.sub;
          options.low_spec=false;options.lightweight=0;changed=true;
        }
      }
      if (ImGui::IsItemHovered() && picked<kPresetCount)
        ImGui::SetTooltip("%s",presets[picked].tip);
    }
    // Frame rate first: it is the setting this port exists for, and the one people look for.
    const double rates[] = {-1, 0, 60, 120, 144, 165, 200, 240, 360, 480};
    const char* names[] = {"Match monitor", "Unlocked", "60", "120", "144", "165", "200", "240", "360", "480"};
    int selected = -1; for (int i = 0; i < 10; ++i) if (options.fps_cap == rates[i]) selected = i;
    if (settings_combo("Frame rate", &selected, names, 10)) { options.fps_cap = rates[selected]; changed = true; }
#ifdef _WIN32
    const bool diagnostic_vsync_before = options.vsync;
#endif
    if (settings_toggle("VSync (prevents tearing)", &options.vsync)) { changed = true; options.vsync_auto = false; }
#ifdef _WIN32
    if (options.vsync != diagnostic_vsync_before)
      host::log("present: vsync %s (settings)", options.vsync ? "on" : "off");
#endif
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On prevents tearing but can add display latency. Off favors lower latency.\n"
                        "G-Sync/FreeSync must be enabled in your monitor and driver; the game keeps\n"
                        "the frame rate below refresh when it detects VRR. Fixed-refresh displays\n"
                        "can tear with Off. Driver VSync settings may override this switch.");
    // Display: one choice. Borderless and exclusive fullscreen were two switches that turned each
    // other off; the same two settings are written, so the renderers see no difference.
    {
      const char* displays[] = {"Window", "Borderless fullscreen", "Exclusive fullscreen (experimental)"};
      int display = options.exclusive_fullscreen ? 2 : options.fullscreen ? 1 : 0;
      if (settings_combo("Display", &display, displays, 3)) {
          options.fullscreen = display == 1;
          options.exclusive_fullscreen = display == 2;
          changed = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Borderless fullscreen: the game's window covers the whole screen.\n"
                               "Exclusive fullscreen: the game takes the display over, or falls back to\n"
                               "borderless when Windows does not allow it.\n"
                               "Press F11 or Alt+Enter in the game window to toggle fullscreen.\n%s", kExperimentalNote);
      }
    }
    // Widescreen: True widescreen only. The Slippi code was offered here too until 0.9.63 (a saved
    // choice of it fills the screen, load_pc_settings); --widescreen still runs it for test runs.
    // True widescreen widens the frustum here in the renderer instead of running the Gecko code.
    // The one thing the game does differently is draw a fighter who is in the added sides (the Source Port
    // puts the fighter's state back afterwards, the Static Recomp uses the Slippi code's own
    // one-instruction change). Experimental because the game still lays out and culls scenery for
    // 73:60: geometry can be missing or pop in at the new edges.
    {
      const char* widescreen_modes[] = {"Off", "Fill my screen (experimental)", "16:9 (experimental)",
                                        "16:10 (experimental)", "Custom (experimental)"};
      // UI order is independent of the saved TrueAspect enum values.
      constexpr TrueAspect true_aspects[] = {TrueAspect::FitWindow, TrueAspect::Wide16_9,
                                            TrueAspect::Wide16_10, TrueAspect::Custom};
      int widescreen_mode = 0;
      if (options.true_widescreen) {
        switch (options.true_aspect) {
          case TrueAspect::FitWindow: widescreen_mode = 1; break;
          case TrueAspect::Wide16_9: widescreen_mode = 2; break;
          case TrueAspect::Wide16_10: widescreen_mode = 3; break;
          case TrueAspect::Custom: widescreen_mode = 4; break;
        }
      }
      if (settings_combo("Widescreen", &widescreen_mode, widescreen_modes, 5)) {
        options.widescreen = false;
        options.true_widescreen = widescreen_mode >= 1;
        if (widescreen_mode >= 1) options.true_aspect = true_aspects[widescreen_mode - 1];
        changed = true;
      }
      if (options.true_widescreen && !options.widescreen && options.true_aspect == TrueAspect::Custom) {
        int wh[2] = {options.true_custom_w, options.true_custom_h};
        ImGui::SetNextItemWidth(160.0f);
        if (settings_widgets::InputInt2("Custom shape (W:H)", wh)) {
          options.true_custom_w = std::clamp(wh[0], 1, 100);
          options.true_custom_h = std::clamp(wh[1], 1, 100);
          changed = true;
        }
        const float asked = (float)options.true_custom_w / (float)options.true_custom_h;
        if (asked > kTrueAspectMax + 1e-4f)
          settings_hint("Wider than the widest shape True widescreen draws: it is drawn at that width with side bars.");
        else if (asked < kTrueAspectMin - 1e-4f)
          settings_hint("Narrower than 4:3: it is drawn at 4:3.");
      }
      if (options.true_widescreen && !options.widescreen && options.true_aspect == TrueAspect::FitWindow)
        settings_hint("Widens to the window's shape; a screen wider than 64:27 still gets side bars.");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Widens the camera in the renderer and fills a wide screen. Online safe: it changes only\n"
                          "what you see, and applies at once. Fighters in the added sides are drawn.\n"
                          "Watch the edges for missing or popping scenery: the game still culls for 73:60.\n"
                          "Menus, character and stage select, matches, training and replays all fill the\n"
                          "screen. Only the opening movie keeps its side bars.\n%s", kExperimentalNote);
    }
    // In pixels: on a scaled frame the display size is in layout units, not the window's size.
    float win_w = g_frame_pixel_size.x, win_h = g_frame_pixel_size.y;

    // Aspect ratio and window size are presentation only: they change nothing the game computes,
    // so they cannot desync and the two players in a match may each pick their own.
    {
      const char* aspects[] = {"Auto (Melee's own: 73:60, or 16:9 with widescreen on)",
                               "73:60 (Melee's native)", "4:3", "16:9", "Stretch to window (no black bars)"};
      int index = std::clamp((int)options.aspect, 0, 4);
      if (settings_combo("Aspect ratio", &index, aspects, 5)) { options.aspect = (AspectMode)index; changed = true; }
      if (options.aspect == AspectMode::Stretch)
        settings_hint("Fills the whole window or screen, so the picture is stretched. Pick a 4:3 window\n"
                            "size below and a wider screen to get the stretched resolution players use.");
      else
        settings_hint("Melee's camera asks for 73:60, not 4:3; Widescreen above widens it.\n"
                            "Auto follows Widescreen.");

      // Window size, the way Dolphin lets a player choose one. 4:3 sizes first: those are what
      // Melee players run (1440x1080 is the common one), then the 16:9 sizes.
      struct Size { int w, h; const char* name; };
      static const Size kSizes[] = {
        {0, 0, nullptr},                     // slot 0 is the "follow window" label, built below
        {640, 480, "640x480 (4:3)"},         {960, 720, "960x720 (4:3)"},
        {1280, 960, "1280x960 (4:3)"},       {1440, 1080, "1440x1080 (4:3)"},
        {1600, 1200, "1600x1200 (4:3)"},     {1920, 1440, "1920x1440 (4:3)"},
        {1280, 720, "1280x720 (16:9)"},      {1920, 1080, "1920x1080 (16:9)"},
        {2560, 1440, "2560x1440 (16:9)"},
      };
      const int kPresets = (int)(sizeof kSizes / sizeof kSizes[0]);
      char follow[80], custom[80];
      std::snprintf(follow, sizeof follow, "Follow window (now %dx%d)", (int)win_w, (int)win_h);
      std::snprintf(custom, sizeof custom, "Custom (%dx%d)", options.window_w, options.window_h);
      const char* items[kPresets + 1];
      items[0] = follow;
      for (int i = 1; i < kPresets; ++i) items[i] = kSizes[i].name;
      int count = kPresets, size_index = 0;
      if (options.window_pinned) {
        size_index = -1;
        for (int i = 1; i < kPresets; ++i)
          if (kSizes[i].w == options.window_w && kSizes[i].h == options.window_h) size_index = i;
        if (size_index < 0) { items[kPresets] = custom; count = kPresets + 1; size_index = kPresets; }
      }
      const bool full = options.fullscreen || options.exclusive_fullscreen || host::window_is_fullscreen();
      if (full) ImGui::BeginDisabled();
      if (settings_combo("Window size", &size_index, items, count)) {
        if (size_index == 0) options.window_pinned = false;
        else if (size_index < kPresets) {
          options.window_pinned = true;
          options.window_w = kSizes[size_index].w; options.window_h = kSizes[size_index].h;
          host::window_set_client_size(options.window_w, options.window_h);
        }
        changed = true;
      }
      if (full) { ImGui::EndDisabled(); settings_hint("Fullscreen uses the whole screen. Stretch above fills it; the other aspects add bars."); }
      // A window bigger than the desktop cannot be shown with its title bar on screen, so Windows
      // (and the clamp in window_set_client_size) gives back a smaller one. The player can also just
      // have dragged the edge since. Either way, say what the window actually is.
      else if (options.window_pinned && ((int)win_w != options.window_w || (int)win_h != options.window_h))
        settings_hint("Picked %dx%d, window is %dx%d (dragged, or capped to your desktop).\nFullscreen is never capped.",
                            options.window_w, options.window_h, (int)win_w, (int)win_h);
      settings_hint("Aspect ratio and window size only change how the picture is fitted to your screen.\n"
                          "They cannot desync, and your opponent can be on different ones.");
    }

    // Same numbers Dolphin shows (EFB 640x528 per multiplier). Auto = the smallest multiplier
    // whose 640x480 image covers the window, like Dolphin's "Auto (Window Size)".
    float aspect = presented_aspect(options, (int)win_w, (int)win_h);
    float vw = win_w, vh = win_w / aspect; if (vh > win_h) { vh = win_h; vw = win_h * aspect; }
    int auto_scale = std::clamp(std::max((int)std::ceil(vw / (480.0f * aspect)), (int)std::ceil(vh / 480.0f)), 1, 8);
    char auto_label[64]; std::snprintf(auto_label, sizeof auto_label, "Auto (%dx = %dx%d for this window)", auto_scale, 640 * auto_scale, 528 * auto_scale);
    const char* scales[] = {auto_label, "Native (640x528)", "2x (1280x1056) for 720p", "3x (1920x1584) for 1080p", "4x (2560x2112) for 1440p",
                            "5x (3200x2640)", "6x (3840x3168) for 4K", "7x (4480x3696)", "8x (5120x4224)"};
    // DLSS chooses its own render size, so the internal resolution and supersampling settings do
    // nothing while it is on and are shown greyed out. DLAA renders at the internal resolution the
    // player picked and only anti-aliases it, so it leaves both of them working.
    const bool dlss_picks_resolution = options.dlss_mode >= 2 && options.dlss_mode != 11;   // not DLAA / FSR Native AA
    if (dlss_picks_resolution) ImGui::BeginDisabled();
    changed |= settings_combo("Internal resolution", &options.efb_scale, scales, 9);
    // DLSS (DLAA included) does its own anti-aliasing, so supersampling on top would render larger
    // for a second pass over the same edges: grey it out rather than let the two stack.
    // MSAA smooths polygon edges only and costs little; SSAA renders every pixel 4, 9 or 16 times
    // (factor 2, 3, 4 per side) and also smooths textures and effects. One choice, never both.
    const char* aa[] = {"None", "2x MSAA (light)", "4x MSAA", "8x MSAA", "4x SSAA (supersampling)", "9x SSAA (heavier)", "16x SSAA (heaviest)"};
    int aa_index = options.ssaa > 1 ? 2 + std::clamp(options.ssaa, 2, 4) : options.msaa >= 8 ? 3 : options.msaa >= 4 ? 2 : options.msaa >= 2 ? 1 : 0;
    if (options.dlss_mode && !dlss_picks_resolution) ImGui::BeginDisabled();
    if (settings_combo("Anti-aliasing", &aa_index, aa, 7)) {
      options.msaa = aa_index >= 1 && aa_index <= 3 ? 1 << aa_index : 1;
      options.ssaa = aa_index >= 4 ? aa_index - 2 : 1;
      changed = true;
    }
    if (options.dlss_mode && !dlss_picks_resolution) ImGui::EndDisabled();
    if (dlss_picks_resolution) ImGui::EndDisabled();
    const char* anis[] = {"1x", "2x", "4x", "8x", "16x"};
    int an_index = options.anisotropy >= 16 ? 4 : options.anisotropy >= 8 ? 3 : options.anisotropy >= 4 ? 2 : options.anisotropy >= 2 ? 1 : 0;
    if (settings_combo("Anisotropic filtering", &an_index, anis, 5)) { options.anisotropy = 1 << an_index; changed = true; }
    // Live, right where the settings that move it are, rather than only in an on-screen overlay
    // during play: raise Internal resolution, Anti-aliasing or a DLSS mode and watch this move.
    {
      float used = 0, total = 0;
      if (vram_usage(&used, &total)) settings_hint("Video memory in use: %.1f / %.1f GB", used, total);
    }
    // ---- Get mod looks ----
    // One click each: the authors' own download, checked against its published file and built from the
    // player's own disc (host::cosmetics::install_official_look). Open until all three are installed; once
    // an Animelee is built the Mods folder is scanned again and it is switched on.
    {
      namespace mods = source_port::mods;
      const std::string mods_dir = mods::mods_folder().u8string();
      {
        // Screenshot tooling (MELEE_SETTINGS_SCROLL_TO=modlooks): bring the cards into view once.
        static int scroll_frames = 0;
        if (scroll_frames < 30) {
          const char* target = host::test_env::getenv("MELEE_SETTINGS_SCROLL_TO");
          if (target && std::strcmp(target, "modlooks") == 0) ImGui::SetScrollHereY(0.1f);
          ++scroll_frames;
        }
      }
      const host::cosmetics::OfficialLookState looks[3] = {host::cosmetics::official_look_state(0, mods_dir),
                                                           host::cosmetics::official_look_state(1, mods_dir),
                                                           host::cosmetics::official_look_state(2, mods_dir)};
      static bool was_busy[3] = {false, false, false};
      static int enable_animelee = 0;                             // 1 or 2: just built, switched on after the rescan
      static bool animelee_new[3] = {false, false, false};       // installed in this session: not running yet
      for (int which = 1; which < 3; ++which)
        if (was_busy[which] && !looks[which].busy && looks[which].installed) { mods::rescan_async(); enable_animelee = which; animelee_new[which] = true; }
      for (int which = 0; which < 3; ++which) was_busy[which] = looks[which].busy;
      if (enable_animelee) {
        const auto view = mods::panel_view();
        if (!view.scanning) {
          // The two Animelees replace the same files: the one just installed goes on, the other off.
          const auto on = std::filesystem::u8path(looks[enable_animelee].disc).filename();
          const auto off = std::filesystem::u8path(looks[3 - enable_animelee].disc).filename();
          for (const auto& d : view.result.items) {
            if (d.can_enable && !d.enabled && d.path.filename() == on) mods::choose(d.key, true);
            if (d.enabled && d.path.filename() == off) mods::choose(d.key, false);
          }
          enable_animelee = 0;
        }
      }
      if (settings_widgets::CollapsingHeader("Get mod looks##official", looks[0].installed && looks[1].installed && looks[2].installed ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        struct Card { const char* name; const char* credit; const char* page; const char* size; };
        static const Card cards[3] = {
            {"Diet Melee (Classic, 64 and Crystal)", "By the Diet Melee team", "https://diet.melee.tv", "about 200 MB"},
            {"Animelee (Complete Edition)", "By the Animelee team", "https://www.animelee.xyz", "about 30 MB"},
            {"Animelee x DBZ (Dragon Ball Z edition)", "By the Animelee team", "https://www.animelee.xyz", "about 30 MB"},
        };
        for (int which = 0; which < 3; ++which) {
          const auto& s = looks[which];
          const Card& card = cards[which];
          ImGui::PushID(which);
          ImGui::TextUnformatted(card.name);
          ImGui::SameLine();
          ImGui::TextDisabled("%s", card.credit);
          if (s.busy) {
            ImGui::ProgressBar(s.progress, ImVec2(-1.0f, 0.0f));
            if (!s.message.empty()) settings_hint("%s", s.message.c_str());
          } else if (s.installed) {
            ImGui::TextColored(ImVec4(.37f, .84f, .53f, 1), "Installed");
            // The Static Recomp plays a disc mod started as a mod disc, not from the MODS tab.
            settings_hint("%s", which == 0 ? "Pick Look > Diet Classic, Diet 64 or Crystal under Low-Detail Modes, then restart."
                                : options.native_source ? "On in Game mods (MODS tab), one Animelee at a time. Restart to play with it."
                                                        : "Start it as a mod disc to play it.");
          } else {
            ImGui::BeginDisabled(s.any_busy);
            if (settings_widgets::Button("Install")) {
              const std::string disc = host::options.mod_base_iso.empty() ? host::options.iso : host::options.mod_base_iso;
              std::string tool;
#ifdef _WIN32
              wchar_t exe[MAX_PATH] = {};
              if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
                tool = (std::filesystem::path(exe).parent_path() / L"tools" / L"xdelta3.exe").u8string();
#endif
              host::cosmetics::install_official_look(which, disc, tool, mods_dir);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip("Downloads it from its authors' own page (%s), checks it is the file they published,\n"
                                "and builds it from your Melee disc on this PC. Nothing else is installed.", card.size);
            ImGui::SameLine();
            ImGui::TextDisabled("%s download", card.size);
            if (!s.message.empty()) ImGui::TextWrapped("%s", s.message.c_str());
          }
#ifdef _WIN32
          if (settings_widgets::SmallButton("Authors' page")) ShellExecuteA(nullptr, "open", card.page, nullptr, nullptr, SW_SHOWNORMAL);
#endif
          ImGui::PopID();
          if (which < 2) ImGui::Separator();
        }
        ImGui::Unindent(8.0f);
      }
    }
    // ---- Low poly and lightweight ----
    // Every setting that trades looks for frame rate on slow PCs, in one sub-section: Look, Visual effects and
    // the Lightweight ladder up front, the exact looks and the fine controls folded under them. Open by default.
    if (settings_widgets::CollapsingHeader("Low-Detail Modes##lightweight", ImGuiTreeNodeFlags_DefaultOpen)) {
    // Hidden capture hook: scroll to this section and optionally open its list for review.
    if (const char* shot = host::test_env::getenv("MELEE_TEST_LOOK_MENU")) {
      ImGui::SetScrollHereY(0.05f);
      if (std::strcmp(shot, "open") == 0 && ImGui::GetFrameCount() == 120)
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID("Look")), ImGuiPopupFlags_None);
    }
    ImGui::Indent(8.0f);
    // Looks by name. Picking one sets the Lightweight level and every look control below; changing any of them
    // afterwards makes this read Custom.
    {
      int count = 0; const LookPreset* presets = look_presets(&count);
      const int current = look_preset_of(options);
      const auto packs = host::cosmetics::look_packs();
      auto ready = [&](const char* name) {
        for (const auto& pack : packs) if (pack.name == name && pack.built) return true;
        return false;
      };
      // Relative gains observed on one uncapped Static Recomp / Final Destination test.
      static const char* const gain[] = {"", "High", "High", "High", "High", "Modest", "Modest",
                                         "High", "High", "High", "High", "High"};
      const char* preview = current < 0 ? "Custom" : presets[current].name;
      if (settings_begin_combo("Look", preview, ImGuiComboFlags_HeightLarge)) {
        auto group = [&](const char* title, std::initializer_list<int> entries) {
          ImGui::Separator(); ImGui::TextDisabled("%s", title);
          for (int i : entries) {
            const char* pack = i == 3 ? "Diet Classic" : i == 4 ? "Diet 64" : (i == 5 || i == 6) ? "Crystal" : "";
            const bool available = !*pack || ready(pack);
            std::string label = presets[i].name;
            if (*gain[i]) { label += "  ("; label += gain[i]; label += " boost)"; }
            if (!available) label += " (setup required)";
            ImGui::BeginDisabled(!available);
            if (settings_widgets::Selectable(label.c_str(), current == i || (i == 5 && current == 6))) {
              apply_look_preset(options, i, d3d11_available());
              set_lightweight_level(options.lightweight);
              changed = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", presets[i].tip);
          }
        };
        group("Faster", {0, 1, 2, 7, 8});
        group("Built-in styles", {11, 9, 10});
        group("Mod artwork (setup required)", {3, 4, 5});
        settings_end_combo();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nPerformance varies by computer, stage and settings.",
                          current >= 0 ? presets[current].tip : "Your own mix of the controls below.");
      const std::string chosen = exact_look_of(options);
      const std::string active = host::cosmetics::active_look_pack();
      if (!chosen.empty() && !ready(chosen.c_str()))
        settings_hint("%s needs setup below. Its mod artwork is not active.", chosen.c_str());
      else if (chosen != active) {
        settings_hint("Restart the game to apply the selected artwork.");
      }
      if (current == 5 || current == 6) {
        const char* colors[] = {"Original mod colors", "Player colors (red, blue, yellow, green)"};
        int selected = current == 6 ? 1 : 0;
        if (settings_combo("Crystal fighter colors", &selected, colors, 2)) {
          apply_look_preset(options, selected ? 6 : 5, d3d11_available());
          set_lightweight_level(options.lightweight);
          changed = true;
        }
      }
      if (current > 0) ImGui::TextDisabled("FPS boost in our test: %s", gain[current]);
      ImGui::TextWrapped("Boost labels are based on one PC using Static Recomp, with no FPS cap on Final Destination. Results vary by PC and settings. Start with Light or Lighter.");
    }
    {
      const char* levels[] = {"Full", "Reduced (no sparks or glow)", "Minimal (no screen overlays)", "No decorative particles"};
      int shown = lightweight_effects_level(options.effects_level, options.lightweight);
      if (settings_combo("Visual effects", &shown, levels, 4)) { options.effects_level = shown; changed = true; }
      if (options.lightweight >= 3) ImGui::TextDisabled("This look uses at least Minimal effects.");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Skips decorative effects during matches to help slower PCs: glow, sparks and\n"
                          "flashes (Reduced), plus full-screen overlays (Minimal). Menus, fighters, the\n"
                          "stage and the HUD always draw. No decorative particles also removes\n"
                          "particle smoke and dust. Shields, reflectors and projectiles stay visible\n"
                          "at every level. Display only: safe online.");
    }
    // "Lightweight": one ladder from the full game down to wireframe, each step keeping the ones
    // below it. 1 to 3 are display only and change live; 4 and 5 also hold the image settings at
    // their cheapest (the Low spec pair: what the player had is kept and given back). The hint
    // under it is the last match frame's draw count, so a player sees what each step removes.
    if (settings_widgets::TreeNodeEx("Advanced lightweight levels##lightweightlevels", 0)) {
      const char* names[kLightweightLevels] = {
          "Off", "1  Low poly fighters", "2  + No stage backgrounds", "3  + Flat shading",
          "4  + Lowest image", "5  Wireframe"};
      static const char* const tips[kLightweightLevels] = {
          "The game as it is.",
          "Fighters use the low-polygon models the game itself draws in reflections, shadows and the\n"
          "magnifier bubble. A fighter without one keeps its full model.",
          "Level 1, plus the stage's scenery behind the fighting area is not drawn: sky, crowds, far\n"
          "buildings. Platforms, ledges, items and the HUD stay.",
          "Level 2, plus no textures on the stage or the fighters: colors and lighting only. Visual\n"
          "effects are held at Minimal.",
          "Level 3, plus native resolution, no anti-aliasing or filtering, and no DLSS.\n"
          "Lowering it restores your previous image settings.",
          "Everything drawn as lines on a plain background, with the frame cap lifted: for the\n"
          "weakest machines, or as many frames as yours can make."};
      int level = std::clamp(options.lightweight, 0, kLightweightLevels - 1);
      if (settings_combo("Lightweight", &level, names, kLightweightLevels)) {
        apply_lightweight(options, level, d3d11_available());
        set_lightweight_level(options.lightweight);
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nDisplay only: safe online and in replays; the other player sees their own choice.", tips[level]);
      if (options.lightweight > 0) {
        const LightweightCounts c = lightweight_last_counts();
        if (c.drawn + c.skipped > 0)
          settings_hint("Last match frame: %u draws, %u skipped (%d%%)", c.drawn, c.skipped,
                        (int)(100.0 * c.skipped / (double)(c.drawn + c.skipped) + 0.5));
      }
      ImGui::TreePop();
    }
    // The older "Retro (64 style)" switch is the 64 Style look now; a saved "retro 1" still loads.
    // Exact looks: Diet Classic, Diet 64 and Crystal exactly as made, built once on this PC from the player's own
    // game disc and the published patch files (nothing of them ships with the game). The build runs on a worker
    // thread; the line under the button says how it went. Folded away: it is needed once.
    if (settings_widgets::TreeNodeEx("Set up mod artwork (Diet and Crystal)##exactlooks", 0)) {
      static std::mutex build_mutex;
      static std::string build_message;
      static std::atomic<bool> building{false};
      static char patcher_folder[512] = "";
      const auto packs = host::cosmetics::look_packs();
      std::string have;
      for (const auto& pack : packs) {
        if (!have.empty()) have += ", ";
        have += pack.name + (pack.built ? " ready" : " not built");
      }
      ImGui::TextWrapped("Use your Melee disc and the Diet patch files to prepare the optional mod artwork once. Restart after choosing a mod look.");
      ImGui::TextWrapped("Mod artwork: %s", have.c_str());
      const std::string active = host::cosmetics::active_look_pack();
      const std::string chosen = exact_look_of(options);
      if (!chosen.empty() && chosen != active) {
        settings_hint("%s applies after restart once its artwork is prepared.", chosen.c_str());
      }
      settings_widgets::InputText("Patch folder##exactlooks", patcher_folder, sizeof patcher_folder);
#ifdef _WIN32
      ImGui::SameLine();
      if (settings_widgets::Button("Browse...##lookpatch")) {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof dialog;
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFilter = L"Diet patch files (*.xdelta)\0*.xdelta\0\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = (DWORD)std::size(path);
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
          const std::string folder = std::filesystem::path(path).parent_path().u8string();
          std::snprintf(patcher_folder, sizeof patcher_folder, "%s", folder.c_str());
        }
      }
#endif
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The folder of the looks' patcher download (it holds the three .xdelta files).\n"
                          "Your own game disc is patched on this PC; nothing is downloaded or shipped.");
      ImGui::BeginDisabled(building.load() || !patcher_folder[0]);
      if (settings_widgets::Button(building.load() ? "Preparing..." : "Prepare mod artwork")) {
        building = true;
        const std::string folder = patcher_folder;
        const std::string disc = host::options.mod_base_iso.empty() ? host::options.iso : host::options.mod_base_iso;
        std::string tool;
#ifdef _WIN32
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
          tool = (std::filesystem::path(exe).parent_path() / L"tools" / L"xdelta3.exe").u8string();
#endif
        std::thread([folder, disc, tool] {
          std::string message;
          host::cosmetics::build_look_packs(folder, disc, tool, &message);
          { std::lock_guard<std::mutex> lock(build_mutex); build_message = message; }
          building = false;
        }).detach();
      }
      ImGui::EndDisabled();
      {
        std::lock_guard<std::mutex> lock(build_mutex);
        if (!build_message.empty()) ImGui::TextWrapped("%s", build_message.c_str());
      }
      ImGui::TreePop();
    }
    // The look controls: what the presets set, one by one. Folded away; changing one makes Look read Custom.
    // Hidden capture hook (MELEE_TEST_LOOK_MENU=custom): this list open and in view.
    if (const char* shot = host::test_env::getenv("MELEE_TEST_LOOK_MENU"); shot && std::strcmp(shot, "custom") == 0) {
      ImGui::SetNextItemOpen(true);
      ImGui::SetScrollHereY(0.0f);
    }
    if (settings_widgets::TreeNodeEx("Customize the look##lookcontrols", 0)) {
      static const char* surfaces[] = {"Textured", "Retro 64 (64 x 64)", "Flat colors", "White", "Posterized", "Wireframe"};
      if (settings_combo("Fighters", &options.look_fighter, surfaces, kSurfaceCount)) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the fighters are textured in a match. Flat colors: one color per texture.");
      if (settings_combo("Stage", &options.look_stage, surfaces, kSurfaceCount)) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the stage is textured in a match.");
      if (settings_combo("Menus and HUD", &options.look_menus, surfaces, kSurfaceCount - 1)) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Character and stage select, HUD and pause screen pictures.");
      if (settings_toggle("Unfiltered textures", &options.look_unfiltered)) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sharp texels on the treated textures, the Nintendo 64 look.");
      static const char* colours[] = {"Original", "Port colors"};
      if (settings_combo("Fighter colors", &options.look_colours, colours, 2)) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Port colors: P1 red, P2 blue, P3 yellow, P4 green, on every fighter's model.");
      static const char* backgrounds[] = {"The stage's own", "Black", "Plain color"};
      if (settings_combo("Background", &options.look_background, backgrounds, 3)) changed = true;
      if (options.look_background == 2) {
        float rgb[3] = {((options.look_background_rgb >> 16) & 0xFF) / 255.0f, ((options.look_background_rgb >> 8) & 0xFF) / 255.0f,
                        (options.look_background_rgb & 0xFF) / 255.0f};
        if (settings_color_edit3("Background color", rgb, ImGuiColorEditFlags_NoInputs)) {
          options.look_background_rgb = ((uint32_t)(rgb[0] * 255 + 0.5f) << 16) | ((uint32_t)(rgb[1] * 255 + 0.5f) << 8) | (uint32_t)(rgb[2] * 255 + 0.5f);
          changed = true;
        }
      }
      ImGui::TreePop();
    }
    ImGui::Unindent(8.0f);
    }
    // ---- Arcade look ----
    // Picture-only switches for a 2D-fighter look. The hitboxes widen the game's own debug draw tests
    // (MU_DISPLAY_OPTION_HITBOXES and the next two); the game ignores them in a network match.
    if (settings_widgets::CollapsingHeader("Arcade look##arcade")) {
    if (host::test_env::getenv("MELEE_TEST_ARCADE_MENU")) ImGui::SetScrollHereY(0.05f);   // hidden capture hook
    ImGui::Indent(8.0f);
    changed |= settings_toggle("Show attack hitboxes", &options.arcade_hitboxes);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Attack hitboxes show on every fighter in offline matches and replays.\n"
                        "Hurtboxes stay off; the Practice menu has those. Never shown online.");
    changed |= settings_toggle("Projectile hitboxes too", &options.arcade_projectile_hitboxes);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Lasers, bombs, items and other projectiles show their hitboxes as well.");
    changed |= settings_toggle("Draw hitboxes behind the fighter", &options.arcade_hitboxes_behind);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Hitboxes show around the fighter instead of covering it, so the\n"
                        "character stays easy to read. Also applies to the Practice menu's hitboxes.");
    changed |= settings_toggle("Combo counter", &options.arcade_combo_counter);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Counts the hits of a combo over the attacker, from the second hit until the other\n"
                        "player can act again. Offline matches and replays only.");
    changed |= settings_toggle("2D camera", &options.arcade_camera_2d);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The match camera loses its perspective, like a 2D fighter. Offline on the Source Port it\n"
                        "is the 2D-style build's own camera: flat, straight in front of the action, with its\n"
                        "tracking and zoom. Online, and on the Static Recomp, only the perspective goes.\n"
                        "Display only: safe online.");
    {
      const char* backdrops[] = {"Stage", "Plain color", "Grid"};
      if (settings_combo("Background##arcade", &options.arcade_backdrop, backdrops, 3)) changed = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Plain color hides the stage's scenery behind the fighting area (sky, crowds,\n"
                          "far buildings) and shows one color instead. Platforms, ledges and items stay.\n"
                          "Grid adds lines every 5 units behind the stage (Source Port; the Static Recomp shows the plain color).\n"
                          "Display only: safe online.");
      if (options.arcade_backdrop >= 1) {
        const uint32_t c = options.arcade_background_rgb;
        float rgb[3] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
        if (settings_color_edit3("Background color##arcade", rgb, ImGuiColorEditFlags_DisplayRGB)) {
          options.arcade_background_rgb = ((uint32_t)(rgb[0] * 255 + 0.5f) << 16) | ((uint32_t)(rgb[1] * 255 + 0.5f) << 8) |
                                          (uint32_t)(rgb[2] * 255 + 0.5f);
          changed = true;
        }
        ImGui::SameLine();
        if (settings_widgets::Button("Reset color##arcade")) { options.arcade_background_rgb = RenderOptions{}.arcade_background_rgb; changed = true; }
        if (options.arcade_backdrop == 2) {
          const uint32_t g = options.arcade_grid_rgb;
          float grid[3] = {((g >> 16) & 0xFF) / 255.0f, ((g >> 8) & 0xFF) / 255.0f, (g & 0xFF) / 255.0f};
          if (settings_color_edit3("Grid color##arcade", grid, ImGuiColorEditFlags_DisplayRGB)) {
            options.arcade_grid_rgb = ((uint32_t)(grid[0] * 255 + 0.5f) << 16) | ((uint32_t)(grid[1] * 255 + 0.5f) << 8) |
                                      (uint32_t)(grid[2] * 255 + 0.5f);
            changed = true;
          }
          ImGui::SameLine();
          if (settings_widgets::Button("Reset##arcadegrid")) { options.arcade_grid_rgb = RenderOptions{}.arcade_grid_rgb; changed = true; }
        }
      }
    }
    auto set_all = [&](bool on) {
      options.arcade_hitboxes = options.arcade_projectile_hitboxes = options.arcade_hitboxes_behind = on;
      options.arcade_camera_2d = options.arcade_combo_counter = on;
      options.arcade_backdrop = on ? 2 : 0;
      changed = true;
    };
    if (settings_widgets::Button("Arcade preset##arcade")) set_all(true);
    ImGui::SameLine();
    if (settings_widgets::Button("Reset##arcade")) set_all(false);
    if (gx::arcade_display() & ~arcade_settings_bits(options))
      settings_hint("Some of these are on for now from the Practice menu or the active mod.");
    ImGui::Unindent(8.0f);
    }
    // Creating a device and a swapchain on another API means restarting; the choice is saved and
    // read again at the next launch (see load_pc_settings and --backend).
#ifndef _WIN32
    // Linux: OpenGL, or Vulkan where a Vulkan 1.3 GPU is present. Direct3D and DLSS are Windows parts.
    {
      const char* linux_backends[] = {"OpenGL (default)", "Vulkan (experimental)"};
      int linux_index = options.api == RenderApi::Vulkan ? 1 : 0;
      if (vulkan_available()) {
        if (settings_combo("Graphics backend", &linux_index, linux_backends, 2)) {
          options.api = linux_index ? RenderApi::Vulkan : RenderApi::D3D12; changed = true;
          if (linux_index) vulkan_forget_failed_starts();
        }
        settings_hint("Takes effect at the next launch: save settings, then restart.");
      } else ImGui::TextUnformatted("Graphics backend: OpenGL");
    }
    const bool d3d11 = true;   // the upscaler row below stays disabled
    const bool vk_fsr = false;   // AMD FSR's Vulkan build is Windows only
#else
    {
    // Vulkan is offered only where a Vulkan 1.3 GPU is present; it draws without DLSS (FSR upscaling runs on it).
    const char* backends[] = {"Direct3D 12 (default)", "Direct3D 11 (older GPUs and drivers)", "Vulkan (experimental)"};
    int api_index = options.api == RenderApi::D3D11 ? 1 : options.api == RenderApi::Vulkan ? 2 : 0;
    if (settings_combo("Graphics backend", &api_index, backends, vulkan_available() ? 3 : 2)) {
      options.api = api_index == 1 ? RenderApi::D3D11 : api_index == 2 ? RenderApi::Vulkan : RenderApi::D3D12; changed = true;
      if (api_index == 2) vulkan_forget_failed_starts();
    }
    settings_hint("Takes effect at the next launch: save settings, then restart.");
    }
    const bool d3d11 = options.api == RenderApi::D3D11 || options.api == RenderApi::Vulkan;   // no DLSS on either
    const bool vk_fsr = options.api == RenderApi::Vulkan;   // but FSR runs on Vulkan
#endif
    // Menu entries 0-5 are dlss_mode 0-5 (DLSS) and 6-10 are dlss_mode 11-15 (FSR): the values 6-10
    // were Intel XeSS's, removed (settings_file.cpp loads them as Native).
    const char* upscalers[] = {"Native", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance",
                               "FSR Native AA", "FSR Quality", "FSR Balanced", "FSR Performance", "FSR Ultra Performance"};
    if (d3d11 && !vk_fsr) ImGui::BeginDisabled();
    int upscaler_index = options.dlss_mode >= 11 ? options.dlss_mode - 5 : options.dlss_mode <= 5 ? options.dlss_mode : 0;
    if (settings_combo("Upscaling (DLSS / FSR)", &upscaler_index, upscalers, 11)) {
      options.dlss_mode = upscaler_index >= 6 ? upscaler_index + 5 : upscaler_index; changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("DLSS: NVIDIA RTX cards.\n"
                        "FSR: AMD's upscaler, works on AMD, NVIDIA and Intel cards (FSR 4 on Radeon RX 6000 and newer).\n"
                        "AA modes (DLAA, FSR Native AA) keep the full resolution and only smooth edges.\n"
                        "DLSS Ultra Performance would render below the GameCube's own resolution on\n"
                        "anything short of an 8K display, so there it switches to Performance.");
    if (options.api == RenderApi::Vulkan)
      settings_hint(vk_fsr ? "On Vulkan the FSR modes run; DLSS needs Direct3D 12 (it draws at native resolution here)."
                           : "On Vulkan the game draws at the Internal resolution above: DLSS needs Direct3D 12.");
#ifdef _WIN32
    {
      std::string game_dir;
      {
        wchar_t module[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
        if (length > 0 && length < MAX_PATH) game_dir = std::filesystem::path(module).parent_path().u8string();
      }
      using host::downloads::ModelState;
      // AMD FSR is its own download: AMD's own signed files, fetched and checked one by one (host/optional_downloads.cpp).
      const ModelState fs = game_dir.empty() ? ModelState::Present : host::downloads::fsr_state(game_dir);
      if (fs == ModelState::Downloading) {
        settings_hint("%s", host::downloads::fsr_message().c_str());
      } else if (fs == ModelState::Present && !fsr::available() && !host::downloads::fsr_message().empty()) {
        settings_hint("AMD FSR is installed. Restart the game to use it.");
      } else if (fs != ModelState::Present) {
        char fsr_label[64];
        std::snprintf(fsr_label, sizeof fsr_label, "Download AMD FSR (%s)", host::downloads::kFsrSize);
        if (ImGui::Button(fsr_label, ImVec2(0, 32))) host::downloads::fsr_download(game_dir);
        if (fs == ModelState::Failed) settings_hint("%s", host::downloads::fsr_message().c_str());
        else settings_hint("%s", fsr::is_fsr_mode(options.dlss_mode)
                                     ? "FSR is a separate download: until it is installed, the game draws at native resolution."
                                     : "AMD's FSR upscaler is a separate download, AMD's own signed files. It goes beside the game and stays there through updates.");
      } else if (fsr::available() && fsr::is_fsr_mode(options.dlss_mode) && !fsr::provider().empty()) {
        settings_hint("AMD's library runs %s on this graphics card.", fsr::provider().c_str());
      }
    }
#endif
    // What each of these is actually costing, right where the settings that cost it are, instead of
    // only in the separate performance graph. Render latency is Reflex's measured total (works at
    // Native too, see gx_streamline.h); the "adds" number is the GPU-timestamp measured cost of
    // the pass (see gx_d3d12.cpp read_gpu_timers), and "Native (estimated)" is the total with it
    // subtracted out -- an estimate, since it assumes the cost is simply additive to the total,
    // which is usually close but not exact (queueing and driver overhead do not scale perfectly linearly).
    if (streamline::reflex_available()) {
      float dlaa_ms = 0;
      gpu_pass_cost(&dlaa_ms);
      const float total = streamline::reflex_latency_ms();
      const float native_est = std::max(0.0f, total - dlaa_ms);
      char line[192];
      int n = std::snprintf(line, sizeof line, "Render latency: %.1f ms", total);
      if (dlaa_ms > 0.0f) {
        n += std::snprintf(line + n, sizeof(line) - n, "  (Native est. %.1f ms", native_est);
        n += std::snprintf(line + n, sizeof(line) - n, ", DLAA/DLSS +%.1f ms", dlaa_ms);
        std::snprintf(line + n, sizeof(line) - n, ")");
      }
      settings_hint("%s", line);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An average of the last measured frames, so it lags a change by a second or\n"
                          "two, not instant. \"Native (estimated)\" is the total with the measured\n"
                          "GPU cost subtracted out -- an estimate, not a separate measurement.");
    }
    bool vk_rows_disabled = false;   // Vulkan: DLSS frame generation and Reflex stay Direct3D 12 features
#ifdef _WIN32
    // With an FSR mode, frame generation is AMD's (the FSR download): 2x, on any graphics card,
    // Direct3D 12 or Vulkan. It reuses FSR's depth and motion vectors as DLSS's reuses DLSS's.
    if (fsr::is_fsr_mode(options.dlss_mode) && (!d3d11 || vk_fsr)) {
      const bool fsr_loaded = vk_fsr ? fsr::vk_available() : fsr::available();
      const bool fg_ok = vk_fsr ? fsr::vk_fg_available() : fsr::fg_available();
      const std::string fg_name = vk_fsr ? fsr::vk_fg_provider() : fsr::fg_provider();
      const char* fsr_fg_modes[] = {"Off", "2x (AMD FSR)"};
      int fsr_fg = options.frame_generation_mode > 0 ? 1 : 0;
      // Installed is enough to choose it (the launcher's settings window never loads AMD's library);
      // only a game that loaded it and found frame generation unsupported keeps the row closed.
      bool fsr_installed = fsr_loaded;
      if (!fsr_installed) {
        wchar_t module[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
        if (length > 0 && length < MAX_PATH) fsr_installed = host::downloads::fsr_present(std::filesystem::path(module).parent_path().u8string());
      }
      const bool fg_unsupported = fsr_loaded && !fg_ok;
      ImGui::BeginDisabled(!fsr_installed || fg_unsupported);
      ImGui::SetNextItemWidth(160.0f);
      if (settings_combo("Frame generation", &fsr_fg, fsr_fg_modes, 2)) { options.frame_generation_mode = fsr_fg; changed = true; }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("AMD FSR Frame Generation: a generated frame between each pair of rendered frames,\n"
                          "in matches. It adds input delay, so use it for solo play rather than competitive\n"
                          "online matches. Needs an FSR upscaling mode; menus are shown as rendered.");
      if (!fsr_installed) settings_hint("%s", "Frame generation with FSR needs the AMD FSR download above.");
      else if (fg_unsupported) settings_hint("AMD's frame generation did not start on this graphics card.");
      else if (!fsr_loaded && options.frame_generation_mode > 0) settings_hint("Frame generation starts with the game.");
      else if (options.frame_generation_mode > 0 && !fg_name.empty())
        settings_hint("AMD's library runs %s on this graphics card.", fg_name.c_str());
    } else
#endif
    {
    if (d3d11 && vk_fsr) { ImGui::BeginDisabled(); vk_rows_disabled = true; }
    // Frame generation reuses DLSS's depth and motion vectors, so it needs a DLSS mode. Build the
    // list from Streamline's present-thread capability query: RTX 40 usually reports 2x, while newer
    // RTX 50 drivers can expose fixed multipliers through 6x and Dynamic.
    // The plugin is loaded only when frame generation was on at startup (a loaded but unused plugin
    // stalled the first match). Without it, offer what the driver reported last time it was loaded
    // (2x when never known); the choice applies at the next start.
    const bool fg_deferred = streamline::frame_generation_deferred();
    const bool fg_queried = streamline::frame_generation_capabilities_queried();
    if (fg_queried) {
      const int live_max = (int)std::min<uint32_t>(5, streamline::frame_generation_max_multiplier());
      const bool live_dynamic = streamline::frame_generation_dynamic_supported();
      if (live_max != options.fg_cached_max || live_dynamic != options.fg_cached_dynamic) {
        options.fg_cached_max = live_max; options.fg_cached_dynamic = live_dynamic; changed = true;
      }
    }
    // A process without the plugin (not loaded this session, or the launcher's settings window) uses
    // the cached report when there is one.
    const bool fg_from_cache = !fg_queried && (fg_deferred || options.fg_cached_max > 0);
    const bool fg_capabilities_known = fg_queried || fg_from_cache;
    ImGui::BeginDisabled(options.dlss_mode == 0 || options.dlss_mode >= 6 || !fg_capabilities_known);
    {
      const uint32_t max_mult = fg_queried ? std::min<uint32_t>(5, streamline::frame_generation_max_multiplier())
                                           : (uint32_t)std::max(1, options.fg_cached_max);
      const bool dynamic_ok = fg_queried ? streamline::frame_generation_dynamic_supported() : options.fg_cached_dynamic;
      static const char* labels[] = {"Off", "2x", "3x", "4x", "5x", "6x", "Dynamic"};
      const int values[] = {0, 1, 2, 3, 5, 6, 4};
      const char* modes[7]; int mode_values[7]; int n = 0;
      for (int i = 0; i <= (int)max_mult; ++i) {
        modes[n] = labels[i];
        mode_values[n++] = values[i];
      }
      if (dynamic_ok) { modes[n] = labels[6]; mode_values[n++] = values[6]; }
      int selected = 0; bool found_mode = false;
      for (int i = 0; i < n; ++i) {
        if (mode_values[i] == options.frame_generation_mode) { selected = i; found_mode = true; break; }
      }
      if (!found_mode) selected = (int)max_mult;  // safest supported fixed multiplier
      if (fg_queried && mode_values[selected] != options.frame_generation_mode) {
        options.frame_generation_mode = mode_values[selected];
      }
      ImGui::SetNextItemWidth(160.0f);
      if (settings_combo("Frame generation", &selected, modes, n)) {
        options.frame_generation_mode = mode_values[selected];
        changed = true;
      }
      if (!fg_capabilities_known) settings_hint("Checking supported modes…");
      else if (fg_from_cache && options.frame_generation_mode != 0) {
        settings_hint("Frame generation starts the next time the game starts.");
      } else if (!fg_deferred && fg_queried && options.frame_generation_mode == 0) {
        settings_hint("Restart the game to fully unload frame generation.");
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("NVIDIA DLSS Frame Generation (RTX 40 series and newer): generated frames between\n"
                        "rendered frames. RTX 50 Multi Frame Generation can offer up to 6x when the\n"
                        "driver reports support. It adds input delay, so use it for solo play rather\n"
                        "than competitive online matches. Needs DLSS upscaling; Reflex turns on with it.\n"
                        "With an FSR mode, AMD's frame generation is offered here instead (any card).");
    }
    if (d3d11 && vk_fsr && !vk_rows_disabled) { ImGui::BeginDisabled(); vk_rows_disabled = true; }
    // NVIDIA Reflex, laid out the way games offer it.
    {
      const char* modes[] = {"Off", "On", "On + Boost"};
      ImGui::SetNextItemWidth(160.0f);
      if (settings_combo("NVIDIA Reflex Low Latency", &options.reflex_mode, modes, 3)) changed = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the GPU stops queueing frames ahead of the game, so what you press shows sooner.\n"
                          "On + Boost: also keeps the GPU clocks up, trading power for a little more.\n"
                          "Frame generation always runs with Reflex at least On. NVIDIA cards only.");
      // The flash indicator for latency analyzers is under Advanced, at the bottom of this tab.
    }
    if (d3d11) { ImGui::EndDisabled(); settings_hint("DLSS needs Direct3D 12 and an NVIDIA GPU."); }
    settings_section("HDR");
    ImGui::BeginDisabled(d3d11 || !hdr_display_available());
    if (settings_toggle("HDR output", &options.hdr_output)) changed = true;
    ImGui::EndDisabled();
    if (d3d11) settings_hint("Needs Direct3D 12.");
    else if (!hdr_display_available()) settings_hint("Turn on HDR in Windows display settings for this screen.");
    if (options.hdr_output && hdr_display_available()) {
      if (settings_slider("Paper white", &options.hdr_paper_white, 80, 500, "%d nits")) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("How bright the game's white (and the menus) is in HDR.");
      if (settings_slider("Peak brightness", &options.hdr_peak, 0, 4000, options.hdr_peak ? "%d nits" : "From Windows")) changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("The brightest highlights. From Windows uses the display's own value.");
    }
    if (options.dlss_mode == 1 || options.dlss_mode == 11) {
      ImGui::TextWrapped("DLAA anti-aliases the game at the Internal resolution above without changing it, then the picture is fitted to the window as usual. Internal resolution and Anti-aliasing keep working, so DLAA stacks with 4x SSAA if you want both.");
    } else if (dlss_picks_resolution) {
      static const char* ratios[] = {"", "", "67% (Quality)", "58% (Balanced)", "50% (Performance)", "33% (Ultra Performance)",
                                     "", "", "", "", "",   // 6-10: Intel XeSS's values, removed
                                     "", "67% (Quality)", "59% (Balanced)", "50% (Performance)", "33% (Ultra Performance)"};
      ImGui::TextWrapped("DLSS renders the game at %s of the window size (at 1080p about 1280x960) and upscales it. That is what DLSS is for in heavy games; Melee is cheap to render, so here it is a downgrade in sharpness, and Internal resolution and Anti-aliasing above are ignored while it is on. For the sharpest image choose Native, set Internal resolution to 3x or higher and Anti-aliasing to 4x SSAA (the Dolphin look), or choose DLAA (full resolution, DLSS used only as anti-aliasing).", ratios[options.dlss_mode]);
    }
    // Border art: the files in the Borders folder, read when the list is opened or the page appears.
    {
      static std::vector<std::string> art_files;
      static double art_listed = -10.0;
      if (ImGui::GetTime() - art_listed > 2.0) { art_files = video_bg::border_files(); art_listed = ImGui::GetTime(); }
      std::vector<const char*> art_names{"None (black bars)"};
      int art_index = 0;
      for (size_t i = 0; i < art_files.size(); ++i) {
        art_names.push_back(art_files[i].c_str());
        if (art_files[i] == options.border_art) art_index = (int)i + 1;
      }
      if (settings_combo("Border art", &art_index, art_names.data(), (int)art_names.size())) {
        options.border_art = art_index > 0 ? art_files[(size_t)art_index - 1] : std::string();
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A picture or looping video shown in the bars beside the game picture.\n"
                          "Put PNG, JPG, BMP or MP4 files in the Borders folder and pick one here.\n"
                          "It is scaled to cover the window. Display only: it cannot affect online play.");
      ImGui::SameLine();
      if (settings_widgets::SmallButton("+ Open Borders folder")) video_bg::open_border_folder();
      if (!options.border_art.empty()) {
        const char* edges[] = {"Sharp", "Soft shadow"};
        int edge = options.border_edge ? 1 : 0;
        if (settings_combo("Border edge", &edge, edges, 2)) { options.border_edge = edge; changed = true; }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Soft shadow: the game picture casts a gradient shadow on the art beside it.\nSharp: the art is cut off at the picture's edge.");
      }
    }
    // Two different things draw a CRT here: the game's own tube model (crt_shader.h) and a RetroArch
    // preset of the player's choice. Each has its own heading so nobody mistakes one for the other.
    settings_section("Built-in CRT");
    settings_hint(PRODUCT_NAME "'s own tube model, made for Melee. Light on frame time.");
    ImGui::BeginDisabled(!options.shader_preset.empty());
    static const char* crt_looks[] = {"Off", "Studio monitor", "Home television"};
    if (settings_combo("Built-in CRT look", &options.crt_filter, crt_looks, 3)) changed = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Shows the picture the way a tube display draws it: scanline beams that widen with brightness,\n"
                        "a phosphor mask sized to this screen's pixels, and a little glow in the glass.\n"
                        "Studio monitor is flat and sharp. Home television is softer, with curved glass.\n"
                        "Scanlines are clearest at 1440p and above; at 1080p they are kept faint so they do not shimmer.\n"
                        "Sharpening is not applied while this is on.");
    if (options.crt_filter == 2) {
      int curve = (int)std::lround(options.crt_curve * 100.0f);
      if (settings_slider("Screen curve", &curve, 0, 300, "%d%%")) { options.crt_curve = curve / 100.0f; changed = true; }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How curved the home television's glass is. 0%% is flat, 100%% is the default.");
    }
    ImGui::EndDisabled();
    if (!options.shader_preset.empty() && options.crt_filter)
      settings_hint("Off while a RetroArch shader preset is applied (set the preset to None to use it).");
    settings_section("RetroArch shader presets");
    settings_hint("Community shaders, grouped by the people who made them. Applying one turns the built-in CRT off.");
    // A shader preset of the player's choice (RetroArch slang presets in the Shaders folder).
    {
      ImGui::TextUnformatted("Shader preset");
      ImGui::SameLine();
      if (settings_widgets::SmallButton("+ Open Shaders folder")) user_shader::open_folder();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs a RetroArch shader preset (.slangp) over the game picture, CRT-Royale for example.\n"
                          "Put the whole slang shader pack in the Shaders folder with its subfolders, then pick a preset below.\n"
                          "A preset only lists shader files, so a .slangp copied on its own cannot run.\n"
                          "A single .slang shader placed directly in the Shaders folder can be picked too.\n"
                          "A preset takes the place of the built-in CRT. Heavy presets cost frame time.");
      if (!user_shader::available()) {
        ImGui::TextDisabled("Not available: librashader.dll is missing from the game folder.");
      } else {
        struct ShaderEntry { std::string path, label, search, heading; int group; };
        static std::vector<ShaderEntry> entries;
        static bool listed = false;
        static bool recommended_present = false;
        static bool pending_initialized = false;
        static std::string pending_shader, observed_shader;
        constexpr const char* recommended = "slang-shaders/crt/crt-yo6-flat-trinitron-tv.slangp";
        if (observed_shader != options.shader_preset) {
          pending_shader = observed_shader = options.shader_preset;
        }
        auto friendly = [](const std::string& path) {
          std::string label = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
          const size_t dot = label.rfind('.');
          if (dot != std::string::npos) label.resize(dot);
          if (label.rfind("crt-", 0) == 0) label.erase(0, 4);
          for (char& ch : label) if (ch == '-' || ch == '_') ch = ' ';
          if (!label.empty()) label[0] = (char)std::toupper((unsigned char)label[0]);
          return label;
        };
        if (!listed) {
          entries.clear();
          recommended_present = false;
          // Grouped by shader family, with the people the shader files themselves credit (their
          // headers); a family whose files name nobody is listed without a name.
          struct Family { const char* prefix; const char* heading; };
          static const Family families[] = {
              {"crt-maximus-royale", "CRT-Maximus-Royale (DigiDwrf)"}, {"crt-royale", "CRT-Royale (TroggleMonkey)"},
              {"crt-guest-advanced", "Guest Advanced (guest.r)"}, {"crt-gdv-mini", "GDV Mini (DariusG, after guest.r)"},
              {"crt-geom", "CRT-Geom (cgwg, Themaister and DOLLS)"}, {"crt-interlaced-halation", "CRT-Geom (cgwg, Themaister and DOLLS)"},
              {"crt-cgwg", "CRT-Geom (cgwg, Themaister and DOLLS)"}, {"fake-crt-geom", "Fake CRT-Geom (hunterk, DariusG)"},
              {"crt-hyllian", "Hyllian (Hyllian)"}, {"crt-nobody", "Hyllian (Hyllian)"}, {"crt-super-xbr", "Hyllian (Hyllian)"},
              {"crt-yah", "CRT-Yah (Jezze, after Hyllian)"}, {"crt-caligari", "Caligari (caligari)"},
              {"crt-lottes", "Lottes (Timothy Lottes)"}, {"lottesRVM", "Lottes (Timothy Lottes)"}, {"fakelottes", "Fake Lottes (hunterk)"},
              {"crt-easymode", "EasyMode (EasyMode)"}, {"crt-aperture", "EasyMode (EasyMode)"},
              {"crt-1tap", "1-tap and 2-tap (fishku)"}, {"crt-2tap", "1-tap and 2-tap (fishku)"},
              {"zfast-crt", "zfast (Greg Hogan)"}, {"newpixie", "newpixie (Mattias Gustavsson)"}, {"crt-mattias", "Mattias (Mattias)"},
              {"crt-potato", "Potato (Brad Parker, DariusG)"}, {"crt-simple", "CRT Simple (DariusG)"},
              {"gizmo", "Gizmo (gizmo98)"}, {"crt-CreativeForce", "CreativeForce (djayjp)"}, {"crt-blurPi", "blurPi (Oriol Ferrer Mesia)"},
              {"crt-pi", "crt-pi (davej)"}, {"crt-torridgristle", "torridgristle (torridgristle)"}, {"GritsScanlines", "torridgristle (torridgristle)"},
              {"gtu", "GTU (aliaspider)"}, {"tvout-tweaks", "GTU (aliaspider)"}, {"vt220", "VT220 (sprash3)"}, {"monoCRT", "monoCRT (hunterk)"},
              {"crt-yo6", "crt-yo6"}, {"crt-beans", "crt-beans"}, {"crt-consumer", "Consumer"},
          };
          for (const std::string& path : user_shader::presets()) {
            const std::string file = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
            const bool single = path.size() >= 6 && path.substr(path.size() - 6) == ".slang";
            const size_t lola = path.find("/lola-retro/");
            int group = 4;
            std::string heading = "Other presets", label = friendly(path);
            if (path == recommended) { group = 0; heading = "Recommended"; label = "Flat Trinitron TV (crt-yo6)"; }
            else if (lola != std::string::npos) {
              // Lola Retro (Zoki, MIT): its Melee presets first, then each display flat and curved.
              const std::string rest = path.substr(lola + 12);
              std::string folder = rest.substr(0, rest.find('/'));
              if (!folder.empty()) folder[0] = (char)std::toupper((unsigned char)folder[0]);
              group = 1; heading = "Lola Retro (Zoki)"; label = folder + ": " + label;
            } else if (single) { group = 5; heading = "Single shaders"; }
            else if (path.find("/crt/") != std::string::npos) {
              group = 3; heading = "Other CRT presets";
              size_t best = 0;
              for (const Family& f : families) {
                const size_t n = std::strlen(f.prefix);
                if (n > best && file.compare(0, n, f.prefix) == 0) { best = n; group = 2; heading = f.heading; }
              }
            }
            std::string search = label + " " + heading + " " + path;
            for (char& ch : search) ch = (char)std::tolower((unsigned char)ch);
            entries.push_back({path, std::move(label), std::move(search), std::move(heading), group});
            if (path == recommended) recommended_present = true;
          }
          std::stable_sort(entries.begin(), entries.end(), [](const ShaderEntry& a, const ShaderEntry& b) {
            if (a.group != b.group) return a.group < b.group;
            if (a.heading != b.heading) return a.heading < b.heading;
            const bool am = a.label.compare(0, 6, "Melee:") == 0, bm = b.label.compare(0, 6, "Melee:") == 0;
            if (am != bm) return am;   // Lola Retro's Melee presets first
            return a.label < b.label;
          });
          listed = true;
        }
        if (!pending_initialized) {
          pending_shader = options.shader_preset.empty() && recommended_present ? recommended : options.shader_preset;
          observed_shader = options.shader_preset;
          pending_initialized = true;
        }
        if (settings_widgets::SmallButton("Refresh presets")) { user_shader::refresh_presets(); listed = false; }
        if (settings_widgets::CollapsingHeader("Browse shader presets")) {
          static char shader_filter[64] = "";
          ImGui::SetNextItemWidth(260.0f);
          settings_widgets::InputTextWithHint("##shaderfilter", "type to search presets", shader_filter, sizeof shader_filter);
          if (entries.empty()) ImGui::TextDisabled("Nothing in the Shaders folder yet. Put a shader pack folder in it.");
          if (ImGui::BeginListBox("##shaderlist", ImVec2(-1.0f, entries.empty() ? 30.0f : 150.0f))) {
            if (settings_widgets::Selectable("None (unfiltered)", pending_shader.empty())) pending_shader.clear();
            std::string needle = shader_filter;
            for (char& ch : needle) ch = (char)std::tolower((unsigned char)ch);
            int shown = 0;
            const std::string* last_heading = nullptr;
            for (const ShaderEntry& entry : entries) {
              if (!needle.empty() && entry.search.find(needle) == std::string::npos) continue;
              if (++shown > 400) { ImGui::TextDisabled("more: narrow the search"); break; }
              if (!last_heading || *last_heading != entry.heading) {
                ImGui::SeparatorText(entry.heading.c_str());
                last_heading = &entry.heading;
              }
              ImGui::PushID(entry.path.c_str());
              if (settings_widgets::Selectable(entry.label.c_str(), entry.path == pending_shader)) pending_shader = entry.path;
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.path.c_str());
              ImGui::PopID();
            }
            ImGui::EndListBox();
          }
        }
        if (recommended_present && settings_widgets::SmallButton("Select recommended Trinitron")) pending_shader = recommended;
        const std::string applied = options.shader_preset.empty() ? "None" : friendly(options.shader_preset);
        const std::string selected = pending_shader.empty() ? "None" : friendly(pending_shader);
        ImGui::TextWrapped("Applied: %s", applied.c_str());
        if (pending_shader != options.shader_preset) {
          ImGui::TextWrapped("Selected: %s", selected.c_str());
          if (settings_widgets::Button("Apply shader preset")) {
            options.shader_preset = observed_shader = pending_shader;
            changed = true;
          }
          ImGui::SameLine();
          if (settings_widgets::SmallButton("Cancel selection")) pending_shader = options.shader_preset;
          ImGui::TextDisabled("Applying a preset may pause briefly while its shaders compile.");
        }
        const std::string why = user_shader::status();
        if (!options.shader_preset.empty() && !why.empty()) ImGui::TextWrapped("This preset is not running: %s", why.c_str());
      }
    }
    int sharp = (int)std::lround(options.sharpness * 100.0f);
    if (settings_slider("Sharpening", &sharp, 0, 100, "%d%%")) { options.sharpness = sharp / 100.0f; changed = true; }
    int ao = (int)std::lround(options.screen_space_ao * 100.0f);
    if (settings_slider("Screen-space ambient occlusion", &ao, 0, 100, "%d%%")) { options.screen_space_ao = ao / 100.0f; changed = true; }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Depth-based contact shading. It uses the game's depth buffer and works on D3D11 and\n"
                        "D3D12; it is not RTX ray tracing.\n%s", kExperimentalNote);
    // Display adjustment over the finished picture, HUD included; 100% is neutral on all three.
    int bright = (int)std::lround(options.brightness * 100.0f);
    if (settings_slider("Brightness", &bright, 50, 150, "%d%%")) { options.brightness = bright / 100.0f; changed = true; }
    int contrast = (int)std::lround(options.contrast * 100.0f);
    if (settings_slider("Contrast", &contrast, 50, 150, "%d%%")) { options.contrast = contrast / 100.0f; changed = true; }
    int vibrance = (int)std::lround(options.vibrance * 100.0f);
    if (settings_slider("Vibrance", &vibrance, 0, 200, "%d%%")) { options.vibrance = vibrance / 100.0f; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0%% is grayscale, 100%% is native, above that is more saturated.");
    const char* subframe_modes[] = {"Off (60 Hz poses only)", "Predict ahead (no delay, can overshoot on speed changes)", "Interpolate (exact, one frame of delay)"};
    int sf = options.subframe == SubFrameMode::Off ? 0 : options.subframe == SubFrameMode::AuthoredInterpolate ? 2 : 1;
    if (settings_combo("Sub-frame animation", &sf, subframe_modes, 3)) { options.subframe = sf == 0 ? SubFrameMode::Off : sf == 2 ? SubFrameMode::AuthoredInterpolate : SubFrameMode::Authored; changed = true; }
    if (sf == 1) ImGui::TextWrapped("Samples supported animation beyond the latest pose. Sudden stops can require correction.");
    if (sf == 2) ImGui::TextWrapped("Samples between completed poses. This adds up to one simulation tick of visual delay; unsupported motion may hold.");
    // Say it rather than quietly ignoring the setting: a player who picked a mode and sees no
    // difference should be told why, and this pairing is what several stage glitch reports were.
    // Judged on what is actually being presented: "follow the monitor" on a 60 Hz display is the
    // same one-frame-per-tick case as an explicit cap of 60.
    const double shown_rate = options.fps_cap > 0 ? options.fps_cap
                            : options.fps_cap < 0 ? (double)host::window_refresh_rate() : 0.0;
    // Warnings wrap like every other explanation here. TextColored does not wrap, so both of these
    // ran off the right edge of the panel and were cut mid-word, which is how a warning ends up
    // reading as a rendering fault.
    auto warning = [](const char* fmt, ...) {
      va_list args; va_start(args, fmt);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.25f, 1.0f));
      ImGui::TextWrappedV(fmt, args);
      ImGui::PopStyleColor();
      va_end(args);
    };
    if (sf != 0 && shown_rate > 0.0 && shown_rate <= 60.5)
      warning("Not in use: only %.0f frames a second are being shown. There is one frame "
              "per tick either way, so this would cost delay and accuracy and buy no "
              "smoothness. Raise the frame rate above 60, or leave this off.", shown_rate);
    if (options.fps_cap == 0)
      warning("Uncapped draws frames no display can show, and on light scenes it takes "
              "enough of the machine to slow the game down.");
    // The "Visual effects" control was removed: the filter it drove deleted the stage select
    // pointer and menu text, and nothing in a draw separates a hit spark from a cursor.

    // ---- Texture packs ----
    // The list is always here, whether or not replacement is switched on: someone who has already
    // put a pack in the folder should see it without having to find a checkbox first. Scanning is
    // filenames only, so showing it costs nothing; decoding the PNGs is the expensive part and
    // still happens only when packs are on.
    ImGui::Separator();
    texpack::refresh_packs();
    const auto installed = texpack::packs();
    ImGui::TextUnformatted("HUD and texture packs");
    ImGui::SameLine();
    if (settings_widgets::SmallButton("+ Add")) texpack::open_packs_folder();
    wrapped_tooltip("Opens TexturePacks. Drop a folder of replacement PNGs there; HUD, menus, and stages are supported.");
    ImGui::TextWrapped("Texture packs can replace the HUD, menu art, and stage textures. Put a pack folder in TexturePacks, then turn it on here.");
    if (installed.empty()) {
      ImGui::TextWrapped("None installed. Press + Add and drop a pack folder in.");
    } else {
      changed |= settings_toggle("Use texture packs", &options.custom_textures);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        texpack::configure(options.custom_textures, options.dump_textures);
        g_textures_dirty.store(true, std::memory_order_relaxed);
      }
      ImGui::SameLine();
      changed |= settings_toggle("Load them at startup", &options.prefetch_textures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Decodes every replacement once when the game starts instead of the first time each\n"
                          "texture appears. One wait up front rather than stutters through the first minutes.");
      // A box per pack: green while it is on, red while it is off, so the state reads at a glance.
      for (const auto& pack : installed) {
        const ImVec4 on(0.25f, 0.62f, 0.35f, 1.0f), off(0.62f, 0.24f, 0.22f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, pack.enabled ? on : off);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pack.enabled ? on : off);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, pack.enabled ? on : off);
        ImGui::PushID(pack.name.c_str());
        if (settings_widgets::Button(pack.enabled ? "ON" : "OFF", ImVec2(44, 0))) {
          texpack::set_pack_enabled(pack.name, !pack.enabled);
          g_textures_dirty.store(true, std::memory_order_relaxed);
          changed = true;
        }
        ImGui::PopID();
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::Text("%s", pack.name.c_str());
        ImGui::SameLine();
        settings_hint("(%llu textures)", (unsigned long long)pack.files);
      }
      if (texpack::prefetching()) {
        uint64_t done = 0, total = 0;
        texpack::prefetch_progress(&done, &total);
        ImGui::Text("Loading textures %llu / %llu", (unsigned long long)done, (unsigned long long)total);
      }
      // Dump textures, for making a pack, is under Advanced at the bottom of this tab.
    }

    // ---- Looping menu backgrounds ----
    // Fixed filenames make the first test hard to misconfigure: replace css.mp4 or sss.mp4 in
    // place, and one checkbox restores the unmodified game backdrop.
    ImGui::Separator();
    ImGui::TextUnformatted("Animated menu backgrounds");
    ImGui::SameLine();
    if (settings_widgets::SmallButton("+ Open folder")) video_bg::open_folder();
    bool video_on = options.video_backgrounds;
    if (settings_toggle("Use looping MP4 backgrounds", &video_on)) {
      options.video_backgrounds = video_on;
      video_bg::set_enabled(video_on);
      changed = true;
    }
    ImGui::TextWrapped("Drop css.mp4 and/or sss.mp4 into VideoBackgrounds. Audio is ignored. "
                       "The first visit learns the vanilla backdrop without changing the ISO.");
    const std::string css_status = video_bg::status(0);
    const std::string sss_status = video_bg::status(1);
    auto target_picker = [](int slot, const char* label) {
      const auto observed = video_bg::observed_textures(slot);
      if (observed.empty()) return;
      const std::string current = video_bg::target(slot);
      const std::string preview = current.empty() ? "Choose observed texture..." : current;
      ImGui::PushID(slot == 0 ? "css-video-target" : "sss-video-target");
      ImGui::SetNextItemWidth(-1.0f);
      if (settings_begin_combo(label, preview.c_str())) {
        for (const auto& texture : observed) {
          const std::string item = texture.name + ": " + std::to_string(texture.width) + "x" +
              std::to_string(texture.height) + ", " +
              std::to_string(texture.distinct_frames) + " frames / " +
              std::to_string(texture.observations) + " observations";
          if (settings_widgets::Selectable(item.c_str(), texture.name == current))
            video_bg::choose_target(slot, texture.name);
          if (texture.name == current) ImGui::SetItemDefaultFocus();
        }
        settings_end_combo();
      }
      ImGui::PopID();
    };
    ImGui::Text("Character select: %s", css_status.c_str());
    ImGui::SameLine();
    if (settings_widgets::SmallButton("Relearn CSS")) video_bg::relearn(0);
    target_picker(0, "CSS texture target");
    ImGui::Text("Stage select: %s", sss_status.c_str());
    ImGui::SameLine();
    if (settings_widgets::SmallButton("Relearn SSS")) video_bg::relearn(1);
    target_picker(1, "SSS texture target");

    // ---- Graphics backend change ----
    // Lightweight 4 and up may pick Direct3D 11 (apply_low_spec), which needs a new device and so a
    // new launch: say so here.
#ifdef _WIN32
    {
      // The backend is the one thing here that cannot change while the game is running.
      const RenderApi running = state.running_d3d11 ? RenderApi::D3D11 : state.running_vulkan ? RenderApi::Vulkan : RenderApi::D3D12;
      if (options.api != running) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Graphics backend: %s at the next launch. Save settings, then restart.",
                           options.api == RenderApi::D3D11 ? "Direct3D 11" : options.api == RenderApi::Vulkan ? "Vulkan" : "Direct3D 12");
      }
    }
#endif

    // ---- Advanced ----
    // Diagnostics and texture dumping for pack makers: nothing a player needs
    // while setting up the picture, so they sit here, closed until opened. Nothing was removed.
    if (settings_advanced_group(0, &changed)) {
      // For LDAT and monitors with the NVIDIA Reflex Latency Analyzer, not for playing.
      ImGui::BeginDisabled(d3d11);
      if (settings_toggle("Reflex flash indicator", &options.reflex_flash)) changed = true;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Flashes a white square in the top left corner when A is pressed on port 1,\n"
                          "for monitors with the NVIDIA Reflex Latency Analyzer and for LDAT.\n"
                          "Needs Direct3D 12 and NVIDIA Reflex Low Latency On.");
      if (settings_toggle("Display latency test", &options.latency_test)) changed = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For photodiode testers that press X + D-pad Down (LDAT, pico lag testers).\n"
                          "X + D-pad Down turns the picture black, then switches it between black and white\n"
                          "with each press, on the frame the press is read; X + D-pad Up gives the picture back.\n"
                          "Only the picture changes: the game is the same, online too.");
      if (settings_toggle("Lossless adapter lag test (next launch only)", &options.lossless_lag_test)) changed = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets the next launch up the way the Lossless Adapter Manager sets up Slippi for its lag test.\n"
                          "Turn it on, close the game and start it again with the adapter in its Slippi lag test\n"
                          "mode: the game boots to character select and rumbles, the adapter plays its inputs into\n"
                          "a Dream Land match and holds X (white) and A (black) while its sensor times the change.\n"
                          "Uses a memory card of its own; your saves are untouched. The launch after it is normal again.");
      if (std::getenv("MELEE_LAG_TEST"))
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "This launch is the lag test. Restart the game to play normally.");
      // For pack authors, not for playing: writes what the game drew, named the way a pack must.
      // Offered with no pack installed too: dumping is how a new pack starts.
      changed |= settings_toggle("Dump textures (for making a pack)", &options.dump_textures);
      if (ImGui::IsItemDeactivatedAfterEdit()) texpack::configure(options.custom_textures, options.dump_textures);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Saves every texture the game draws into Dump\\Textures\\GALE01 with the exact\n"
                          "filenames a replacement has to use. Only useful if you are making a pack.");
      ImGui::SameLine();
      if (settings_widgets::SmallButton("Open texture dump")) texpack::open_dump_folder();
    }
        ImGui::PopItemWidth();
}

}  // namespace gx
