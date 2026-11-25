// The settings window's Audio: volume, music and custom music, the audio mode and buffers (ASIO), sound
// effects, character voices, menu sounds.
// A page of the settings window: settings_frame (pc_settings.cpp) calls it, and it draws with the
// window's shared helpers (pc_settings_internal.h). The values it edits are the settings store's
// (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// Audio page (tab 1).
void settings_page_audio(SettingsPageContext& page) {
  SettingsState& state = page.state;
  RenderOptions& options = page.options;
  bool& changed = page.changed;
    ImGui::PushItemWidth(330.0f);
    int music = slippi::jukebox::user_volume();
    if (settings_slider("Music", &music, 0, 100, "%d%%")) { slippi::jukebox::set_user_volume(music); changed = true; }
#ifndef MELEE_NO_SLIPPI
    bool use_music_packs = slippi::jukebox::music_packs_enabled();
    if (settings_toggle("Use custom music", &use_music_packs)) {
      slippi::jukebox::set_music_packs_enabled(use_music_packs);
      changed = true;
    }
    ImGui::SameLine();
    if (settings_widgets::Button("Open music packs")) slippi::jukebox::open_music_packs_folder();
    ImGui::TextWrapped("1. Enable Use custom music.\n"
                       "2. Open music packs, then Tracks.txt for the disc track names.\n"
                       "3. For /audio/name.hps, put name.mp3 in MusicPacks/audio/. "
                       "For random songs, put the songs in MusicPacks/audio/name/ (or name+othername/ for both).\n"
                       "Re-enter the stage to play the replacement. Vanilla audio mode uses disc music.");
#endif
    // With a game running the device holds the live value. Without one, which is the settings
    // window the launcher opens, the saved value is all there is: reading back from an audio module
    // that was never opened returned zero every frame and dragged the slider back to it.
    if (host::audio_running()) settings_store::panel_values().volume = host::audio_volume();
    // Moving only this slider did not mark the settings as changed, so the new volume was not saved
    // and the next start came back at the old one.
    if (settings_slider("Volume", &settings_store::panel_values().volume, 0, 100, "%d%%")) { host::audio_set_volume(settings_store::panel_values().volume); changed = true; }
    state.volume = settings_store::panel_values().volume;
    const char* audio_modes[] = {"Auto: Windows shared (recommended)", "Low latency: Windows shared", "WASAPI exclusive", "ASIO\xC2\xAE (audio interface)"};
    static const int mode_at_start = options.audio_mode;
    if (settings_combo("Audio mode", &options.audio_mode, audio_modes, 4)) {
      if (options.audio_mode == 1) options.audio_buffer_ms = 30;
      host::audio_set_buffering(options.audio_mode, options.audio_buffer_ms);
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto and Low latency share the Windows output with other apps.\nAuto adjusts buffering and remembers it per device; Low latency keeps your selected software buffer fixed.\nWASAPI exclusive takes over the Windows output while the game runs.\nASIO uses an installed interface driver and its outputs 1 and 2; if it cannot start, Auto is used.\nThe fastest stable choice depends on the device and driver.");
    if (options.audio_mode != mode_at_start) { ImGui::TextDisabled("Applies after restart."); }
    if (options.audio_mode >= 2)
      ImGui::TextWrapped("Exclusive and ASIO bypass the Windows app volume. Lower the game volume and your amp before switching.");
    // This dialog can enable the manually selected interface mode, so its official logo is
    // shown with the picker even before that mode is selected. Preserve the artwork's alpha
    // and aspect ratio; the same asset is packaged in ui_sources/vendor beside the game.
    if (auto* logo = cosmetic_preview(ui_source_asset_path("vendor", "asio-compatible-logo-white.png"))) {
      const float logo_width = 140.0f;
      ImGui::Image(logo->GetTexRef(), ImVec2(logo_width, logo_width * logo->Height / logo->Width));
    }
    ImGui::TextDisabled("ASIO is a registered trademark of Steinberg Media Technologies GmbH.");
    if (options.audio_mode == 3) {
      // ASIO: the interface's own driver, outputs 1 and 2. If it cannot start, the game uses Auto.
      static std::vector<std::string> drivers;
      static bool asio_listed = false;
      if (!asio_listed) { drivers = host::audio_list_asio_drivers(); asio_listed = true; }
      static const std::string asio_at_start = options.audio_asio_driver;
      static const int asio_buffer_at_start = options.audio_asio_buffer;
      if (options.audio_asio_driver.empty() && !drivers.empty()) { options.audio_asio_driver = drivers.front(); changed = true; }
      if (settings_begin_combo("ASIO driver", options.audio_asio_driver.empty() ? "No ASIO driver installed" : options.audio_asio_driver.c_str())) {
        for (const auto& d : drivers)
          if (settings_widgets::Selectable(d.c_str(), d == options.audio_asio_driver)) { options.audio_asio_driver = d; changed = true; }
        settings_end_combo();
      }
      const char* buffer_names[] = {"Driver default", "64 samples", "96 samples", "128 samples", "192 samples", "256 samples", "384 samples", "512 samples"};
      const int buffer_values[] = {0, 64, 96, 128, 192, 256, 384, 512};
      int buffer_choice = 0;
      for (int i = 0; i < 8; ++i) if (buffer_values[i] == options.audio_asio_buffer) buffer_choice = i;
      if (settings_combo("ASIO buffer", &buffer_choice, buffer_names, 8)) { options.audio_asio_buffer = buffer_values[buffer_choice]; changed = true; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smaller is less delay; too small can crackle. Driver default uses the size set in the interface's control panel.");
      if (options.audio_asio_driver != asio_at_start || options.audio_asio_buffer != asio_buffer_at_start) {
        ImGui::TextDisabled("Applies after restart.");
      }
      ImGui::TextDisabled("ASIO uses driver outputs 1 and 2.");
    }
    if (options.audio_mode == 1) {
      if (settings_slider("Audio buffer", &options.audio_buffer_ms, 5, 120, "%d ms")) {
        host::audio_set_buffering(options.audio_mode, options.audio_buffer_ms);
        changed = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Less buffering reduces delay; too little can cause gaps. This controls the software buffer. Windows and the audio device add their own delay.");
    }
    if (kShowFasterSoundEffects) {
      // Faster sound effects: Source Port only (the Static Recomp's sound code is the console's). The
      // launcher's settings window has no game behind it, so it stays settable there.
      if (settings_toggle("Faster sound effects", &gx::RenderOptions::live_audio_early_keyon())) changed = true;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Starts each new sound effect in the sound frame being mixed now instead of the next one,\n"
                          "up to one sound frame (5 ms) sooner. Music is not affected. Offline only: online matches and replays\n"
                          "always use console timing. Source Port only.");
    }
    {
      // Mute character voices (Source Port: the game library plays the fighters' voices at volume 0).
      if (settings_toggle("Mute character voices", &gx::RenderOptions::live_mute_voices())) changed = true;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The fighters' voices are silent: shouts, grunts, taunts and KO screams. Every other sound\n"
                          "plays as before. Only you hear the difference; online and in replays too. Source Port only.");
    }
    {
      // Output device picker: the Windows default, or one named device (virtual cables, interfaces).
      static std::vector<host::AudioDevice> devices;
      static bool listed = false;
      if (!listed) { devices = host::audio_list_devices(); listed = true; }
      static const std::string device_at_start = options.audio_device;
      std::string current = "Windows default";
      for (const auto& d : devices) if (d.id == options.audio_device) current = d.name;
      ImGui::BeginDisabled(options.audio_mode == 3);
      if (settings_begin_combo("Output device", current.c_str())) {
        if (settings_widgets::Selectable("Windows default", options.audio_device.empty())) { options.audio_device.clear(); changed = true; }
        for (const auto& d : devices)
          if (settings_widgets::Selectable(d.name.c_str(), d.id == options.audio_device)) { options.audio_device = d.id; changed = true; }
        settings_end_combo();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Used by Windows shared and WASAPI exclusive modes, and by Auto if ASIO cannot start.");
      ImGui::EndDisabled();
      if (options.audio_device != device_at_start) { ImGui::TextDisabled("Applies after restart."); }
    }
    host::AsioStatus asio;
    if (host::audio_running() && host::audio_asio_info(&asio)) {
      const double rate = asio.sample_rate ? (double)asio.sample_rate : 48000.0;
      ImGui::TextDisabled("ASIO %s: buffer %ld samples (%.1f ms), %u Hz", asio.driver.c_str(), asio.buffer_frames, asio.buffer_frames * 1000.0 / rate, asio.sample_rate);
      ImGui::TextDisabled("Driver output latency: %ld samples (%.1f ms) | game queue: %u ms", asio.latency_out, asio.latency_out * 1000.0 / rate, host::audio_target_ms());
      uint64_t held_ms = 0;
      const auto gaps = host::audio_underruns(&held_ms);
      ImGui::TextDisabled("Output gaps this session: %llu (%llu ms)", (unsigned long long)gaps, (unsigned long long)held_ms);
    } else if (host::audio_running()) {
      if (options.audio_mode == 3)
        ImGui::TextDisabled("ASIO could not start; using Windows shared Auto.");
      if (host::audio_device_queue_ms() == 0) {
        // Shared mode: one queue from the game's sound to the Windows mixer.
        ImGui::TextDisabled("Output latency: %u ms, game to Windows mixer", host::audio_target_ms());
        // Auto and Low latency ask Windows for the output's shortest period, so this is the driver's own
        // minimum: the part of the delay the game cannot shorten on this output in shared mode.
        const float period_ms = host::audio_engine_period_us() / 1000.0f;
        ImGui::TextDisabled("Windows audio period: %.1f ms, the shortest this output's driver allows", period_ms);
        if (period_ms > 5.0f)
          ImGui::TextDisabled("Many onboard outputs allow 2.7 ms; with an audio interface, Exclusive can be shorter.");
      } else {
        ImGui::TextDisabled("Output latency: %u ms buffer + %u ms device%s",
                            host::audio_target_ms(), host::audio_device_queue_ms(), host::audio_exclusive() ? " (exclusive)" : "");
        ImGui::TextDisabled("Software target: %u ms | device queue: %u ms | period: %.1f ms",
                            host::audio_target_ms(), host::audio_device_queue_ms(), host::audio_engine_period_us() / 1000.0f);
      }
      uint64_t held_ms = 0;
      const auto gaps = host::audio_underruns(&held_ms);
      ImGui::TextDisabled("Output gaps this session: %llu (%llu ms)",
                          (unsigned long long)gaps, (unsigned long long)held_ms);
    }
        ImGui::PopItemWidth();
}

}  // namespace gx
