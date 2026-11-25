# The Windows host: melee_source.exe (the Source Port, with melee_game.dll from sourceport/game) and
# its runtime.
cmake_minimum_required(VERSION 3.15)
include(cmake/squad.cmake)
mu_label_tests_at_end()   # port/tests/test_labels.cmake
# The cosmetic catalog (host::cosmetics, cosmetic_mods.h), one file per part: for the tests that build it
# alone. melee_source takes runtime/host/*.cpp.
set(MU_COSMETIC_SOURCES runtime/host/cosmetic_mods.cpp runtime/host/cosmetic_catalog.cpp
  runtime/host/cosmetic_checks.cpp runtime/host/cosmetic_disc_scan.cpp runtime/host/cosmetic_effects.cpp
  runtime/host/cosmetic_import.cpp runtime/host/cosmetic_live.cpp runtime/host/cosmetic_looks.cpp
  runtime/host/cosmetic_portraits.cpp runtime/host/cosmetic_profile.cpp runtime/host/cosmetic_publish.cpp
  runtime/host/cosmetic_serve.cpp runtime/host/cosmetic_sounds.cpp runtime/host/cosmetic_stage_pools.cpp
  runtime/host/cosmetic_stage_skins.cpp runtime/host/cosmetic_vault.cpp runtime/host/effect_slots.cpp
  runtime/host/cosmetic_effect_slots.cpp runtime/host/cosmetic_effect_check.cpp
  runtime/host/release_scope.cpp)   # publishing reads the Particles release switch
# The Source Port host (port/app/source_host.h), one file per family of commands and services
# (source_host_internal.h lists what they share): melee_source in both builds below.
set(MU_SOURCE_HOST_SOURCES app/source_host.cpp app/source_host_boot.cpp app/source_host_card.cpp
  app/source_host_console.cpp app/source_host_cosmetics.cpp app/source_host_disc.cpp app/source_host_lobbies.cpp
  app/source_host_menu_mods.cpp app/source_host_mod_overlay.cpp app/source_host_mods_menu.cpp
  app/source_host_new_stages.cpp app/source_host_online.cpp app/source_host_options.cpp app/source_host_practice.cpp
  app/source_host_replays.cpp app/source_host_roster.cpp app/source_host_savestates.cpp)


# A Source Port with no Slippi code compiled or linked: -DMELEE_NO_SLIPPI=ON in a build directory of
# its own. Only melee_source is defined there, from the same runtime sources minus the Slippi layer
# (runtime_ns), so the tests that need that layer are simply not part of that build directory.
# Everything after this block is the normal build; the option being OFF changes nothing in it.
# When a setting of the runtime or of melee_source changes below, change it here as well.
# .slpz replays (port/app/replay_slpz.cpp) and the Zstandard library they use.
include("${CMAKE_CURRENT_SOURCE_DIR}/app/replay_slpz.cmake")
option(MELEE_NO_SLIPPI "Build only melee_source, without the Slippi layer" OFF)
if(MELEE_NO_SLIPPI)
  message(STATUS "melee: CPU baseline ${MELEE_CPU_BASELINE}, Source Port without the Slippi layer")
  file(GLOB RUNTIME_NS_SOURCES runtime/ppc/*.cpp runtime/hle/*.cpp runtime/host/*.cpp runtime/gx/*.cpp
       runtime/mu_net/*.cpp)   # mu_net: the project's own peer-to-peer session, when present
  foreach(ns_removed
      runtime/hle/exi_slippi.cpp runtime/hle/slippi_net.cpp runtime/hle/slippi_mm_service.cpp runtime/hle/slippi_online.cpp runtime/hle/slippi_spectate.cpp
      runtime/hle/slippi_report.cpp runtime/hle/slippi_playback.cpp runtime/hle/native_practice.cpp
      runtime/hle/native_practice_model.cpp runtime/hle/native_replay_stream.cpp runtime/hle/jukebox.cpp
      runtime/host/hackpack_ai.cpp)   # the old engine's loader for the pack's AI block (source_noslippi.cpp)
    list(REMOVE_ITEM RUNTIME_NS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${ns_removed}")
  endforeach()
  enable_language(C)
  file(GLOB ENET_SOURCES third_party/enet/*.c)
  add_library(port_enet STATIC ${ENET_SOURCES})
  target_include_directories(port_enet PUBLIC third_party/enet/include)
  target_compile_definitions(port_enet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
  target_link_libraries(port_enet PUBLIC ws2_32 winmm)
  add_library(port_libusb STATIC
    third_party/libusb/libusb/core.c
    third_party/libusb/libusb/descriptor.c
    third_party/libusb/libusb/hotplug.c
    third_party/libusb/libusb/io.c
    third_party/libusb/libusb/strerror.c
    third_party/libusb/libusb/sync.c
    third_party/libusb/libusb/os/events_windows.c
    third_party/libusb/libusb/os/threads_windows.c
    third_party/libusb/libusb/os/windows_common.c
    third_party/libusb/libusb/os/windows_usbdk.c
    third_party/libusb/libusb/os/windows_winusb.c)
  target_include_directories(port_libusb PUBLIC third_party/libusb/libusb PRIVATE third_party/libusb/msvc)
  target_compile_definitions(port_libusb PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(port_libusb PUBLIC advapi32 ole32 setupapi)
  file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")
  string(REPLACE "." ";" MELEE_VERSION_PARTS "${MELEE_PORT_VERSION}")
  list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
  while(MELEE_VERSION_PART_COUNT LESS 4)
    list(APPEND MELEE_VERSION_PARTS "0")
    list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
  endwhile()
  list(SUBLIST MELEE_VERSION_PARTS 0 4 MELEE_VERSION_PARTS)
  list(JOIN MELEE_VERSION_PARTS "," MELEE_VERSION_COMMAS)

  add_library(runtime_ns STATIC ${RUNTIME_NS_SOURCES} app/source_noslippi.cpp
    third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c)   # mu_net: keys, signatures, packet sealing
  mu_squad_target(runtime_ns)
  target_include_directories(runtime_ns PUBLIC third_party/monocypher)
  target_compile_definitions(runtime_ns PRIVATE MELEE_NO_SLIPPI MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
    MELEE_PLAYBACK_GCT_BASE=0u MELEE_PLAYBACK_GCT_SIZE=0u)
  if(WIN32)
    set(MELEE_ASIO_SOURCES third_party/asio/asio.cpp third_party/asio/asiodrivers.cpp third_party/asio/asiolist.cpp)
    target_sources(runtime_ns PRIVATE ${MELEE_ASIO_SOURCES})
    set_source_files_properties(${MELEE_ASIO_SOURCES} PROPERTIES COMPILE_OPTIONS "/UUNICODE;/U_UNICODE;/W0")
    target_link_libraries(runtime_ns PUBLIC advapi32)
  endif()
  target_include_directories(runtime_ns PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx runtime/mu_net
    "${PROJECT_SOURCE_DIR}/native" third_party)
  target_compile_features(runtime_ns PUBLIC cxx_std_17)
  target_compile_options(runtime_ns PRIVATE /EHsc ${MELEE_ARCH_FLAG} /fp:precise /W3)
  target_compile_definitions(runtime_ns PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
  target_link_libraries(runtime_ns PUBLIC port_slpz native_animation port_enet port_libusb bcrypt comdlg32 d3d12 d3d11 dxgi d3dcompiler xinput9_1_0 winmm user32 gdi32 setupapi winusb hid mfplat mfreadwrite mfuuid ole32 shell32 advapi32 wintrust crypt32)

  add_executable(melee_source WIN32 app/main.cpp app/settings_window.cpp ${MU_SOURCE_HOST_SOURCES} app/source_guest_boundary.cpp
    app/source_p2p.cpp   # the --p2p-* command line: one mu_net session on the game's command channel
    app/melee_unlocked.rc app/utf8.manifest)
  target_include_directories(melee_source PRIVATE runtime/abi app)
target_sources(melee_source PRIVATE app/crash_report.cpp app/sim_profiler.cpp app/early_start.cpp app/replay_args.cpp app/cosmetics_cli.cpp)   # the pieces split out of main.cpp
  target_link_libraries(melee_source PRIVATE runtime_ns dbghelp)
  target_sources(melee_source PRIVATE app/plugin_gate.cpp app/plugin_keys.cpp)   # fighter plugin approval (Monocypher is in runtime_ns)
  target_link_options(melee_source PRIVATE /MAP:$<TARGET_FILE_DIR:melee_source>/melee_source.map)
  target_compile_options(melee_source PRIVATE /EHsc)
  target_compile_definitions(melee_source PRIVATE MELEE_SOURCE_PORT MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELEE_PORT_VERSION="${MELEE_PORT_VERSION}" MELEE_CPU_BASELINE_NAME="${MELEE_CPU_BASELINE}"
    MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS})
  add_test(NAME source_card_roundtrip COMMAND melee_source --hidden --volume 0
    --log-file "${CMAKE_CURRENT_BINARY_DIR}/source-card-roundtrip.log"
    --card-self-test "${CMAKE_CURRENT_BINARY_DIR}/source-card-roundtrip")

  # Cache identity follows shader generation, constant layout and pipeline setup.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    runtime/gx/gx_shader.cpp runtime/gx/gx_shader.h runtime/gx/gx_d3d12.cpp)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.cpp" GX_SHADER_HASH)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.h" GX_LAYOUT_HASH)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_d3d12.cpp" GX_PIPELINE_HASH)
  string(SHA256 GX_CACHE_HASH "${GX_SHADER_HASH}${GX_LAYOUT_HASH}${GX_PIPELINE_HASH}")
  target_compile_definitions(runtime_ns PRIVATE GX_SHADER_CACHE_VERSION="${GX_CACHE_HASH}")

  add_library(port_imgui STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx12.cpp third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
  # The Vulkan renderer (gx_vulkan.cpp) opens the loader at run time: headers only, no import library.
  target_include_directories(port_imgui PUBLIC third_party/imgui third_party/vulkan-headers/include)
  target_compile_definitions(port_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
  target_compile_features(port_imgui PUBLIC cxx_std_17)
  target_compile_definitions(port_imgui PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
  target_compile_options(port_imgui PRIVATE ${MELEE_ARCH_FLAG})
  target_link_libraries(port_imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)
  target_compile_definitions(runtime_ns PRIVATE GX_PC_SETTINGS)
  target_link_libraries(runtime_ns PUBLIC port_imgui)

  # NVIDIA Streamline: the same optional piece as the normal build, enabled only when its
  # headers, import library and runtime DLLs are present.
  set(MELEE_STREAMLINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline" CACHE PATH
      "NVIDIA Streamline SDK root (headers, import library, and x64 runtime plugins)")
  if(EXISTS "${MELEE_STREAMLINE_ROOT}/include/sl.h" AND
     EXISTS "${MELEE_STREAMLINE_ROOT}/lib/x64/sl.interposer.lib" AND
     EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/sl.interposer.dll" AND
     EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/sl.common.dll")
    target_compile_definitions(runtime_ns PRIVATE GX_STREAMLINE)
    target_include_directories(runtime_ns PRIVATE "${MELEE_STREAMLINE_ROOT}/include")
    target_link_libraries(runtime_ns PUBLIC "${MELEE_STREAMLINE_ROOT}/lib/x64/sl.interposer.lib" wintrust crypt32 delayimp)
    target_link_options(melee_source PRIVATE /DELAYLOAD:sl.interposer.dll)
    foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_g.dll nvngx_dlssg.dll sl.reflex.dll sl.pcl.dll)
      if(EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/${sl_dll}")
        add_custom_command(TARGET melee_source POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MELEE_STREAMLINE_ROOT}/bin/x64/${sl_dll}" "$<TARGET_FILE_DIR:melee_source>/${sl_dll}")
      endif()
    endforeach()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librashader/bin/librashader.dll")
      add_custom_command(TARGET melee_source POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librashader/bin/librashader.dll" "$<TARGET_FILE_DIR:melee_source>/librashader.dll")
      foreach(dxc_dll dxcompiler.dll dxil.dll)
        add_custom_command(TARGET melee_source POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc/bin/${dxc_dll}" "$<TARGET_FILE_DIR:melee_source>/${dxc_dll}")
      endforeach()
    endif()
  elseif(EXISTS "${MELEE_STREAMLINE_ROOT}/include/sl.h")
    message(STATUS "NVIDIA Streamline runtime is incomplete; building without Streamline/DLSS")
  endif()

  # Shared regression checks run against the standalone build as well.
  add_executable(port_offline_input_delay_ns_test tests/offline_input_delay_test.cpp)
  target_include_directories(port_offline_input_delay_ns_test PRIVATE runtime/host runtime/ppc third_party)
  target_compile_features(port_offline_input_delay_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_offline_input_delay_ns_test PRIVATE /EHsc)
  target_compile_definitions(port_offline_input_delay_ns_test PRIVATE MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_test(NAME port_offline_input_delay_ns COMMAND port_offline_input_delay_ns_test)
  add_executable(port_early_input_ns_test tests/early_input_test.cpp)
  target_include_directories(port_early_input_ns_test PRIVATE runtime/host)
  target_compile_features(port_early_input_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_early_input_ns_test PRIVATE /EHsc)
  add_test(NAME port_early_input_ns COMMAND port_early_input_ns_test)
  add_executable(port_stage_dat_safety_ns_test tests/stage_dat_safety_test.cpp)
  target_include_directories(port_stage_dat_safety_ns_test PRIVATE runtime/host runtime/ppc third_party)
  target_compile_features(port_stage_dat_safety_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_stage_dat_safety_ns_test PRIVATE /EHsc)
  target_compile_definitions(port_stage_dat_safety_ns_test PRIVATE MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_test(NAME port_stage_dat_safety_ns COMMAND port_stage_dat_safety_ns_test)
  add_executable(port_user_gecko_ns_test tests/user_gecko_test.cpp)
  target_include_directories(port_user_gecko_ns_test PRIVATE runtime/host runtime/ppc third_party)
  target_compile_features(port_user_gecko_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_user_gecko_ns_test PRIVATE /EHsc)
  target_compile_definitions(port_user_gecko_ns_test PRIVATE MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_test(NAME port_user_gecko_ns COMMAND port_user_gecko_ns_test)
  add_executable(port_disc_memory_ns_test tests/disc_memory_test.cpp)
  target_compile_features(port_disc_memory_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_disc_memory_ns_test PRIVATE /EHsc)
  add_test(NAME port_disc_memory_ns COMMAND port_disc_memory_ns_test)
  add_executable(port_disc_archive_ns_test tests/disc_archive_test.cpp)
  target_include_directories(port_disc_archive_ns_test PRIVATE runtime/host runtime/ppc third_party)
  target_compile_features(port_disc_archive_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_disc_archive_ns_test PRIVATE /EHsc)
  target_compile_definitions(port_disc_archive_ns_test PRIVATE MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_test(NAME port_disc_archive_ns COMMAND port_disc_archive_ns_test)
  add_executable(port_disc_fst_paths_ns_test tests/disc_fst_paths_test.cpp runtime/host/disc_fst_paths.cpp)
  target_include_directories(port_disc_fst_paths_ns_test PRIVATE runtime/host runtime/ppc third_party)
  target_compile_features(port_disc_fst_paths_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_disc_fst_paths_ns_test PRIVATE /EHsc)
  target_compile_definitions(port_disc_fst_paths_ns_test PRIVATE MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_test(NAME port_disc_fst_paths_ns COMMAND port_disc_fst_paths_ns_test)
  add_executable(port_cosmetic_mods_ns_test tests/cosmetic_mods_test.cpp ${MU_COSMETIC_SOURCES}
    runtime/host/archive_unpack.cpp runtime/host/http_client.cpp)
  target_include_directories(port_cosmetic_mods_ns_test PRIVATE runtime/host runtime/ppc third_party)
  target_compile_features(port_cosmetic_mods_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_cosmetic_mods_ns_test PRIVATE /EHsc)
  target_compile_definitions(port_cosmetic_mods_ns_test PRIVATE MELEE_NO_SLIPPI NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_test(NAME port_cosmetic_mods_ns COMMAND port_cosmetic_mods_ns_test)
  target_link_libraries(port_cosmetic_mods_ns_test PRIVATE bcrypt comdlg32 user32)
  add_executable(port_mu_net_ns_test tests/mu_net_test.cpp)
  target_include_directories(port_mu_net_ns_test PRIVATE runtime/mu_net)
  target_compile_features(port_mu_net_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_mu_net_ns_test PRIVATE /EHsc /utf-8)
  target_compile_definitions(port_mu_net_ns_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(port_mu_net_ns_test PRIVATE runtime_ns)
  add_test(NAME port_mu_net_ns COMMAND port_mu_net_ns_test)
  # The Mods menu's host half fed what the site might send (nulls, missing keys, the wrong types,
  # huge strings, odd Unicode) for both lists: every reply keeps its layout and drawable words.
  add_executable(port_mods_menu_fuzz_ns_test tests/mods_menu_fuzz_test.cpp)
  target_include_directories(port_mods_menu_fuzz_ns_test PRIVATE runtime/host)
  target_compile_features(port_mods_menu_fuzz_ns_test PRIVATE cxx_std_17)
  target_compile_options(port_mods_menu_fuzz_ns_test PRIVATE /EHsc /utf-8)
  target_compile_definitions(port_mods_menu_fuzz_ns_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(port_mods_menu_fuzz_ns_test PRIVATE runtime_ns)
  add_test(NAME port_mods_menu_fuzz_ns COMMAND port_mods_menu_fuzz_ns_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/nucleus_cards_16421.json ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/nucleus_mods_costume_10.json)
  set_tests_properties(port_mods_menu_fuzz_ns PROPERTIES TIMEOUT 120)
  return()
endif()

add_executable(port_native_replay_stream_test tests/native_replay_stream_test.cpp runtime/hle/native_replay_stream.cpp)
target_include_directories(port_native_replay_stream_test PRIVATE runtime/hle)
target_compile_features(port_native_replay_stream_test PRIVATE cxx_std_17)
add_test(NAME port_native_replay_stream COMMAND port_native_replay_stream_test)
add_executable(port_source_mod_overlay_test tests/source_mod_overlay_test.cpp runtime/host/source_mod_overlay.cpp)
target_include_directories(port_source_mod_overlay_test PRIVATE runtime/host)
target_compile_features(port_source_mod_overlay_test PRIVATE cxx_std_17)
target_compile_options(port_source_mod_overlay_test PRIVATE /EHsc)
add_test(NAME port_source_mod_overlay COMMAND port_source_mod_overlay_test)
add_executable(port_disc_archive_test tests/disc_archive_test.cpp)
target_include_directories(port_disc_archive_test PRIVATE runtime/host)
target_compile_features(port_disc_archive_test PRIVATE cxx_std_17)
add_test(NAME port_disc_archive COMMAND port_disc_archive_test)
# An effect file's slots (Falcon Punch, ...): bytes by slot, a mod's changes, files made slot by slot.
# port_effect_slots_real reads the disc (MELEE_TEST_ISO) and the effects study's files (MU_FX_SLOTS_DIR).
add_executable(port_effect_slots_test tests/effect_slots_test.cpp runtime/host/effect_slots.cpp)
target_include_directories(port_effect_slots_test PRIVATE runtime/host)
target_compile_features(port_effect_slots_test PRIVATE cxx_std_17)
target_compile_options(port_effect_slots_test PRIVATE /EHsc)
target_compile_definitions(port_effect_slots_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_effect_slots COMMAND port_effect_slots_test)
add_test(NAME port_effect_slots_real COMMAND port_effect_slots_test --real)
set_tests_properties(port_effect_slots_real PROPERTIES SKIP_RETURN_CODE 77 LABELS game)
add_executable(port_mod_profile_test tests/mod_profile_test.cpp runtime/host/mod_profile.cpp)
target_include_directories(port_mod_profile_test PRIVATE runtime/host)
target_compile_features(port_mod_profile_test PRIVATE cxx_std_17)
target_compile_options(port_mod_profile_test PRIVATE /EHsc)
add_test(NAME port_mod_profile COMMAND port_mod_profile_test)
add_executable(port_mod_scan_test tests/mod_scan_test.cpp runtime/host/mod_scan.cpp runtime/host/mod_profile.cpp)
target_include_directories(port_mod_scan_test PRIVATE runtime/host)
target_compile_features(port_mod_scan_test PRIVATE cxx_std_17)
target_compile_options(port_mod_scan_test PRIVATE /EHsc /utf-8)
target_compile_definitions(port_mod_scan_test PRIVATE MU_REPO_ROOT="${CMAKE_SOURCE_DIR}" NOMINMAX)
target_link_libraries(port_mod_scan_test PRIVATE bcrypt shell32)
add_test(NAME port_mod_scan COMMAND port_mod_scan_test)
add_executable(port_replay_slpz_test tests/replay_slpz_test.cpp)
target_compile_features(port_replay_slpz_test PRIVATE cxx_std_17)
target_compile_options(port_replay_slpz_test PRIVATE /EHsc /utf-8)
target_link_libraries(port_replay_slpz_test PRIVATE port_slpz)
add_test(NAME port_replay_slpz COMMAND port_replay_slpz_test)
add_executable(port_lcancel_policy_test tests/lcancel_policy_test.cpp)
target_include_directories(port_lcancel_policy_test PRIVATE runtime/host)
target_compile_features(port_lcancel_policy_test PRIVATE cxx_std_17)
add_test(NAME port_lcancel_policy COMMAND port_lcancel_policy_test)
add_executable(port_lcancel_flash_policy_test tests/lcancel_flash_policy_test.cpp)
target_include_directories(port_lcancel_flash_policy_test PRIVATE runtime/abi)
target_compile_features(port_lcancel_flash_policy_test PRIVATE cxx_std_17)
add_test(NAME port_lcancel_flash_policy COMMAND port_lcancel_flash_policy_test)

add_executable(port_stage_read_pin_test tests/stage_read_pin_test.cpp)
target_include_directories(port_stage_read_pin_test PRIVATE runtime/host)
target_compile_features(port_stage_read_pin_test PRIVATE cxx_std_17)
add_test(NAME port_stage_read_pin COMMAND port_stage_read_pin_test)


add_executable(port_host_stub_policy_test tests/host_stub_policy_test.cpp)
target_include_directories(port_host_stub_policy_test PRIVATE runtime/ppc)
target_compile_features(port_host_stub_policy_test PRIVATE cxx_std_17)
add_test(NAME port_host_stub_policy COMMAND port_host_stub_policy_test)
# The online menu's split rows on a synthetic menu archive (no game data).

add_executable(port_native_online_policy_test tests/native_online_policy_test.cpp)
target_include_directories(port_native_online_policy_test PRIVATE runtime/hle)
target_compile_features(port_native_online_policy_test PRIVATE cxx_std_17)
add_test(NAME port_native_online_policy COMMAND port_native_online_policy_test)
add_executable(port_native_slippi_bridge_test tests/native_slippi_bridge_test.cpp)
target_include_directories(port_native_slippi_bridge_test PRIVATE runtime/hle)
target_compile_features(port_native_slippi_bridge_test PRIVATE cxx_std_17)
add_test(NAME port_native_slippi_bridge COMMAND port_native_slippi_bridge_test)
add_executable(port_native_slippi_menu_payload_test tests/native_slippi_menu_payload_test.cpp)
target_include_directories(port_native_slippi_menu_payload_test PRIVATE runtime/hle)
target_compile_features(port_native_slippi_menu_payload_test PRIVATE cxx_std_17)
add_test(NAME port_native_slippi_menu_payload COMMAND port_native_slippi_menu_payload_test)
add_executable(port_native_state_layout_test tests/native_state_layout_test.cpp)
target_include_directories(port_native_state_layout_test PRIVATE runtime/hle runtime/abi)
target_compile_features(port_native_state_layout_test PRIVATE cxx_std_17)
add_test(NAME port_native_state_layout COMMAND port_native_state_layout_test)
add_executable(port_native_savestate_test tests/native_savestate_test.cpp runtime/hle/native_savestate.cpp)
target_include_directories(port_native_savestate_test PRIVATE runtime/hle runtime/abi)
target_compile_features(port_native_savestate_test PRIVATE cxx_std_17)
add_test(NAME port_native_savestate COMMAND port_native_savestate_test)
add_test(NAME port_slp_diff COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_slp_diff.py")
add_executable(port_frame_queue_test tests/frame_queue_test.cpp)
target_include_directories(port_frame_queue_test PRIVATE runtime/gx)
target_compile_features(port_frame_queue_test PRIVATE cxx_std_17)
add_test(NAME port_frame_queue COMMAND port_frame_queue_test)
add_executable(port_prewarm_only_test tests/prewarm_only_test.cpp)
target_include_directories(port_prewarm_only_test PRIVATE app)
target_compile_features(port_prewarm_only_test PRIVATE cxx_std_17)
add_test(NAME port_prewarm_only COMMAND port_prewarm_only_test)
add_executable(port_pipeline_cache_store_test tests/pipeline_cache_store_test.cpp)
target_include_directories(port_pipeline_cache_store_test PRIVATE runtime/gx)
target_compile_features(port_pipeline_cache_store_test PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(port_pipeline_cache_store_test PRIVATE Threads::Threads)
add_test(NAME port_pipeline_cache_store COMMAND port_pipeline_cache_store_test "${CMAKE_CURRENT_BINARY_DIR}/pipeline_cache_store_test")
add_executable(port_replay_coach_test tests/replay_coach_test.cpp runtime/host/replay_coach.cpp)
target_include_directories(port_replay_coach_test PRIVATE runtime/host)
target_compile_features(port_replay_coach_test PRIVATE cxx_std_17)
add_test(NAME port_replay_coach COMMAND port_replay_coach_test)
add_executable(port_practice_cpu_model_test tests/practice_cpu_model_test.cpp)
target_compile_features(port_practice_cpu_model_test PRIVATE cxx_std_17)
add_test(NAME port_practice_cpu_model COMMAND port_practice_cpu_model_test)
set_tests_properties(port_frame_queue PROPERTIES TIMEOUT 10)
add_executable(port_video_background_learning_test tests/video_background_learning_test.cpp)
target_include_directories(port_video_background_learning_test PRIVATE runtime/gx)
target_compile_features(port_video_background_learning_test PRIVATE cxx_std_17)
add_test(NAME port_video_background_learning COMMAND port_video_background_learning_test)

add_executable(port_settings_chord_test tests/settings_chord_test.cpp)
target_include_directories(port_settings_chord_test PRIVATE runtime/host)
target_compile_features(port_settings_chord_test PRIVATE cxx_std_17)
add_test(NAME port_settings_chord COMMAND port_settings_chord_test)

# port/tests/settings_mouse.cmake (included from the top level on every platform) builds this test too.
if(NOT TARGET port_settings_nav_test)
  add_executable(port_settings_nav_test tests/settings_nav_test.cpp)
  target_include_directories(port_settings_nav_test PRIVATE runtime/host)
  target_compile_features(port_settings_nav_test PRIVATE cxx_std_17)
  add_test(NAME port_settings_nav COMMAND port_settings_nav_test)
endif()

add_executable(port_net_trace_test tests/net_trace_test.cpp runtime/host/net_trace_file.cpp runtime/host/net_trace_read.cpp)
target_include_directories(port_net_trace_test PRIVATE runtime/host)
target_compile_features(port_net_trace_test PRIVATE cxx_std_17)
target_compile_options(port_net_trace_test PRIVATE /EHsc)
add_test(NAME port_net_trace COMMAND port_net_trace_test)
add_executable(port_replay_revisions_test tests/replay_revisions_test.cpp runtime/host/replay_revisions.cpp)
target_include_directories(port_replay_revisions_test PRIVATE runtime/host)
target_compile_features(port_replay_revisions_test PRIVATE cxx_std_17)
target_compile_options(port_replay_revisions_test PRIVATE /EHsc)
add_test(NAME port_replay_revisions COMMAND port_replay_revisions_test)
add_executable(port_user_gecko_test tests/user_gecko_test.cpp)
target_include_directories(port_user_gecko_test PRIVATE runtime/host runtime/ppc)
target_compile_features(port_user_gecko_test PRIVATE cxx_std_17)
target_compile_options(port_user_gecko_test PRIVATE /EHsc)
target_compile_definitions(port_user_gecko_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_user_gecko COMMAND port_user_gecko_test)
add_executable(port_gecko_function_patch_test tests/gecko_function_patch_test.cpp)
target_include_directories(port_gecko_function_patch_test PRIVATE runtime/host)
target_compile_features(port_gecko_function_patch_test PRIVATE cxx_std_17)
target_compile_options(port_gecko_function_patch_test PRIVATE /EHsc)
add_test(NAME port_gecko_function_patch COMMAND port_gecko_function_patch_test)
add_executable(port_unlock_code_policy_test tests/unlock_code_policy_test.cpp)
target_include_directories(port_unlock_code_policy_test PRIVATE runtime/hle)
target_compile_features(port_unlock_code_policy_test PRIVATE cxx_std_17)
add_test(NAME port_unlock_code_policy COMMAND port_unlock_code_policy_test)
add_executable(port_lcancel_indicator_test tests/lcancel_indicator_test.cpp runtime/host/lcancel.cpp)
target_include_directories(port_lcancel_indicator_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx runtime/abi)
target_compile_features(port_lcancel_indicator_test PRIVATE cxx_std_17)
target_compile_options(port_lcancel_indicator_test PRIVATE /EHsc)
target_compile_definitions(port_lcancel_indicator_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_lcancel_indicator COMMAND port_lcancel_indicator_test)
add_executable(port_walter_egg_test tests/walter_egg_test.cpp)
target_include_directories(port_walter_egg_test PRIVATE runtime/gx)
target_compile_features(port_walter_egg_test PRIVATE cxx_std_17)
target_compile_options(port_walter_egg_test PRIVATE /EHsc)
target_compile_definitions(port_walter_egg_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_walter_egg COMMAND port_walter_egg_test)
add_executable(port_gecko_online_test tests/gecko_online_test.cpp runtime/host/gecko_online.cpp)
target_include_directories(port_gecko_online_test PRIVATE runtime/host)
target_compile_features(port_gecko_online_test PRIVATE cxx_std_17)
target_compile_options(port_gecko_online_test PRIVATE /EHsc)
target_compile_definitions(port_gecko_online_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_gecko_online COMMAND port_gecko_online_test)
add_executable(port_settings_search_test tests/settings_search_test.cpp)
target_include_directories(port_settings_search_test PRIVATE runtime/gx)
target_compile_features(port_settings_search_test PRIVATE cxx_std_17)
target_compile_options(port_settings_search_test PRIVATE /EHsc)
target_compile_definitions(port_settings_search_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_settings_search COMMAND port_settings_search_test)
# Test program: Slippi's matchmaking server stood in on loopback (MELEE_TEST_MM_HOST), for room tests.
add_executable(port_mm_standin tests/mm_standin.cpp)
target_include_directories(port_mm_standin PRIVATE third_party)
target_compile_features(port_mm_standin PRIVATE cxx_std_17)
target_compile_options(port_mm_standin PRIVATE /EHsc)
target_compile_definitions(port_mm_standin PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_link_libraries(port_mm_standin PRIVATE port_enet)
add_executable(port_offline_input_delay_test tests/offline_input_delay_test.cpp)
target_include_directories(port_offline_input_delay_test PRIVATE runtime/host runtime/ppc)
target_compile_features(port_offline_input_delay_test PRIVATE cxx_std_17)
add_test(NAME port_offline_input_delay COMMAND port_offline_input_delay_test)
add_executable(port_fullscreen_key_test tests/fullscreen_key_test.cpp)
target_include_directories(port_fullscreen_key_test PRIVATE runtime/host)
target_compile_features(port_fullscreen_key_test PRIVATE cxx_std_17)
add_test(NAME port_fullscreen_key COMMAND port_fullscreen_key_test)
add_executable(port_early_input_test tests/early_input_test.cpp)
target_include_directories(port_early_input_test PRIVATE runtime/host)
target_compile_features(port_early_input_test PRIVATE cxx_std_17)
target_compile_options(port_early_input_test PRIVATE /EHsc)
add_test(NAME port_early_input COMMAND port_early_input_test)
add_executable(port_slippi_input_suggestions_test tests/slippi_input_suggestions_test.cpp)
target_include_directories(port_slippi_input_suggestions_test PRIVATE runtime/hle)
target_compile_features(port_slippi_input_suggestions_test PRIVATE cxx_std_17)
target_compile_options(port_slippi_input_suggestions_test PRIVATE /EHsc)
mu_built_test(port_slippi_input_suggestions port_slippi_input_suggestions_test)
# Slippi 3.7.0 game-file asset links (hle/slippi_hsd_links.cpp): Slippi's own tests on synthetic archives.
# By hand, --local <GameFiles dir> <extracted disc dir or disc image> [--ref <dir>] resolves the real files.
add_executable(port_slippi_hsd_links_test tests/slippi_hsd_links_test.cpp runtime/hle/slippi_hsd_links.cpp)
target_include_directories(port_slippi_hsd_links_test PRIVATE runtime/hle)
target_compile_features(port_slippi_hsd_links_test PRIVATE cxx_std_17)
target_compile_options(port_slippi_hsd_links_test PRIVATE /EHsc)
mu_built_test(port_slippi_hsd_links port_slippi_hsd_links_test)

add_executable(port_pack_skin_rule_test tests/pack_skin_rule_test.cpp runtime/host/pack_skin_rule.cpp)
target_include_directories(port_pack_skin_rule_test PRIVATE runtime/host)
target_compile_features(port_pack_skin_rule_test PRIVATE cxx_std_17)
target_compile_options(port_pack_skin_rule_test PRIVATE /EHsc)
add_test(NAME port_pack_skin_rule COMMAND port_pack_skin_rule_test)

add_executable(port_hackpack_ai_test tests/hackpack_ai_test.cpp)
target_include_directories(port_hackpack_ai_test PRIVATE runtime/host)
target_compile_features(port_hackpack_ai_test PRIVATE cxx_std_17)
target_compile_options(port_hackpack_ai_test PRIVATE /EHsc)
add_test(NAME port_hackpack_ai COMMAND port_hackpack_ai_test)

add_executable(port_gd_melee_ui_motion_test tests/gd_melee_ui_motion_test.cpp)
target_include_directories(port_gd_melee_ui_motion_test PRIVATE runtime/gx)
target_compile_features(port_gd_melee_ui_motion_test PRIVATE cxx_std_17)
add_test(NAME port_gd_melee_ui_motion COMMAND port_gd_melee_ui_motion_test)
add_executable(port_radial_navigation_test tests/radial_navigation_test.cpp)
target_include_directories(port_radial_navigation_test PRIVATE runtime/gx)
target_compile_features(port_radial_navigation_test PRIVATE cxx_std_17)
add_test(NAME port_radial_navigation COMMAND port_radial_navigation_test)
add_executable(port_memory_range_test tests/memory_range_test.cpp)
target_include_directories(port_memory_range_test PRIVATE runtime/host)
target_compile_features(port_memory_range_test PRIVATE cxx_std_17)
add_test(NAME port_memory_ranges COMMAND port_memory_range_test)
add_test(NAME port_gecko_layout COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/gecko_layout_test.py")
add_test(NAME port_cosmetic_import_diagnostic COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cosmetic_import_diagnostic_test.py")
add_test(NAME port_nucleus_vault_diagnostic COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nucleus_vault_diagnostic_test.py")
add_test(NAME port_stage_cosmetic_diagnostic COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stage_cosmetic_diagnostic_test.py")
add_test(NAME port_effect_cosmetic_diagnostic COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/effect_cosmetic_diagnostic_test.py")
add_test(NAME port_companion_texture_diagnostic COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/companion_texture_diagnostic_test.py")
add_executable(port_texture_test tests/texture_snapshot_test.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_texture_test PRIVATE runtime/gx)
target_compile_features(port_texture_test PRIVATE cxx_std_17)
add_test(NAME port_texture_snapshots COMMAND port_texture_test)
add_executable(port_ram_watch_test tests/ram_watch_test.cpp)
target_include_directories(port_ram_watch_test PRIVATE runtime/ppc)
target_compile_features(port_ram_watch_test PRIVATE cxx_std_17)
add_test(NAME port_ram_watch COMMAND port_ram_watch_test)
add_executable(port_draw_batch_test tests/draw_batch_test.cpp)
target_include_directories(port_draw_batch_test PRIVATE runtime/gx)
target_compile_features(port_draw_batch_test PRIVATE cxx_std_17)
add_test(NAME port_draw_batch COMMAND port_draw_batch_test)
add_executable(port_lightweight_test tests/lightweight_test.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_lightweight_test PRIVATE runtime/gx runtime runtime/host)
target_compile_features(port_lightweight_test PRIVATE cxx_std_17)
add_test(NAME port_lightweight COMMAND port_lightweight_test)
add_executable(port_rtx_semantics_test tests/rtx_semantics_test.cpp runtime/gx/rtx_semantics.cpp)
target_include_directories(port_rtx_semantics_test PRIVATE runtime/gx)
target_compile_features(port_rtx_semantics_test PRIVATE cxx_std_17)
add_test(NAME port_rtx_semantics COMMAND port_rtx_semantics_test)
add_executable(port_ax_ucode_test tests/ax_ucode_test.cpp runtime/hle/ax_ucode.cpp)
target_include_directories(port_ax_ucode_test PRIVATE runtime/hle)
target_compile_features(port_ax_ucode_test PRIVATE cxx_std_17)
add_test(NAME port_ax_ucode COMMAND port_ax_ucode_test)
add_executable(port_pad_report_test tests/pad_report_test.cpp runtime/host/playstation_pad.cpp runtime/host/hid_pad.cpp)
target_include_directories(port_pad_report_test PRIVATE runtime/host runtime/ppc)
target_compile_features(port_pad_report_test PRIVATE cxx_std_17)
target_compile_definitions(port_pad_report_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_pad_report_test PRIVATE hid user32)
add_test(NAME port_pad_reports COMMAND port_pad_report_test)
add_executable(port_replay_tips_test tests/replay_tips_test.cpp)
target_include_directories(port_replay_tips_test PRIVATE runtime/gx)
target_compile_features(port_replay_tips_test PRIVATE cxx_std_17)
target_compile_options(port_replay_tips_test PRIVATE /EHsc)
add_test(NAME port_replay_tips COMMAND port_replay_tips_test)
add_executable(port_vrr_detect_test tests/vrr_detect_test.cpp)
target_include_directories(port_vrr_detect_test PRIVATE runtime/gx)
target_compile_features(port_vrr_detect_test PRIVATE cxx_std_17)
add_test(NAME port_vrr_detect COMMAND port_vrr_detect_test)
add_executable(port_temporal_history_test tests/temporal_history_test.cpp)
target_include_directories(port_temporal_history_test PRIVATE runtime/gx)
target_compile_features(port_temporal_history_test PRIVATE cxx_std_17)
add_test(NAME port_temporal_history COMMAND port_temporal_history_test)
add_executable(port_vblank_model_test tests/vblank_model_test.cpp)
target_include_directories(port_vblank_model_test PRIVATE runtime/gx)
target_compile_features(port_vblank_model_test PRIVATE cxx_std_17)
add_test(NAME port_vblank_model COMMAND port_vblank_model_test)
add_executable(port_vulkan_recipes_test tests/vulkan_recipes_test.cpp)
target_include_directories(port_vulkan_recipes_test PRIVATE runtime/gx)
target_compile_features(port_vulkan_recipes_test PRIVATE cxx_std_17)
add_test(NAME port_vulkan_recipes COMMAND port_vulkan_recipes_test)
add_executable(port_box_melee_test tests/box_melee_test.cpp)
target_include_directories(port_box_melee_test PRIVATE runtime/host runtime/ppc)
target_compile_features(port_box_melee_test PRIVATE cxx_std_17)
target_compile_definitions(port_box_melee_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_box_melee COMMAND port_box_melee_test)
add_executable(port_gc_adapter_reset_test tests/gc_adapter_reset_test.cpp runtime/host/phob.cpp)
target_include_directories(port_gc_adapter_reset_test PRIVATE runtime/host runtime/ppc runtime/hle third_party/libusb/libusb)
target_compile_features(port_gc_adapter_reset_test PRIVATE cxx_std_17)
target_compile_options(port_gc_adapter_reset_test PRIVATE /EHsc)
target_compile_definitions(port_gc_adapter_reset_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_gc_adapter_reset_test PRIVATE port_libusb setupapi hid winmm)
add_test(NAME port_gc_adapter_reset COMMAND port_gc_adapter_reset_test)
set_tests_properties(port_gc_adapter_reset PROPERTIES TIMEOUT 10)
add_executable(port_cosmetic_mods_test tests/cosmetic_mods_test.cpp ${MU_COSMETIC_SOURCES} runtime/host/archive_unpack.cpp
  runtime/host/http_client.cpp)
target_include_directories(port_cosmetic_mods_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_cosmetic_mods_test PRIVATE cxx_std_17)
target_compile_options(port_cosmetic_mods_test PRIVATE /EHsc)
target_compile_definitions(port_cosmetic_mods_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_cosmetic_mods_test PRIVATE bcrypt comdlg32 user32)
add_test(NAME port_cosmetic_mods COMMAND port_cosmetic_mods_test)
# The effect checker (cosmetic_effect_check.cpp, --check-effect): every verdict from files made here.
# port_effect_check_real reads the effects study's real files (MU_FX_SLOTS_DIR).
add_executable(port_effect_check_test tests/effect_check_test.cpp ${MU_COSMETIC_SOURCES} runtime/host/archive_unpack.cpp
  runtime/host/http_client.cpp)
target_include_directories(port_effect_check_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_effect_check_test PRIVATE cxx_std_17)
target_compile_options(port_effect_check_test PRIVATE /EHsc)
target_compile_definitions(port_effect_check_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_effect_check_test PRIVATE bcrypt comdlg32 user32)
add_test(NAME port_effect_check COMMAND port_effect_check_test)
add_test(NAME port_effect_check_real COMMAND port_effect_check_test --real)
set_tests_properties(port_effect_check_real PROPERTIES SKIP_RETURN_CODE 77 LABELS game)
include(tests/opponent_skins.cmake)   # opponents' skins online: served form, fetch, wire message
include(tests/online_content.cmake)   # custom fighters online: wire message
# Host<->game payload codecs in runtime/abi (pick previews, the Squad Strike lineup): round trips.
add_executable(port_command_payload_test tests/command_payload_test.cpp)
target_include_directories(port_command_payload_test PRIVATE runtime/abi)
target_compile_features(port_command_payload_test PRIVATE cxx_std_17)
add_test(NAME port_command_payload COMMAND port_command_payload_test)
# A no-argument start (app/disc_memory.h): the remembered disc, the profile copied forward, its command
# line (no --vanilla-only), the earlier vanilla-only saves carried into its card, and its replay folder
# (the Slippi Launcher's, else Documents\Slippi, else its own). No window.
add_executable(port_disc_memory_test tests/disc_memory_test.cpp)
target_compile_features(port_disc_memory_test PRIVATE cxx_std_17)
add_test(NAME port_disc_memory COMMAND port_disc_memory_test)
add_executable(port_stage_dat_safety_test tests/stage_dat_safety_test.cpp)
target_include_directories(port_stage_dat_safety_test PRIVATE runtime/host)
target_compile_features(port_stage_dat_safety_test PRIVATE cxx_std_17)
target_compile_definitions(port_stage_dat_safety_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_stage_dat_safety COMMAND port_stage_dat_safety_test)
add_executable(port_mod_data_safety_test tests/mod_data_safety_test.cpp)
target_include_directories(port_mod_data_safety_test PRIVATE runtime/host)
target_compile_features(port_mod_data_safety_test PRIVATE cxx_std_17)
add_test(NAME port_mod_data_safety COMMAND port_mod_data_safety_test)
add_executable(port_vcdiff_test tests/vcdiff_test.cpp runtime/host/vcdiff.cpp)
target_include_directories(port_vcdiff_test PRIVATE runtime/host)
target_compile_features(port_vcdiff_test PRIVATE cxx_std_17)
add_test(NAME port_vcdiff COMMAND port_vcdiff_test)
add_executable(port_subframe_test tests/subframe_test.cpp runtime/gx/subframe.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_shader.cpp runtime/gx/authored_pose.cpp)
target_include_directories(port_subframe_test PRIVATE runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_link_libraries(port_subframe_test PRIVATE native_animation)
target_compile_features(port_subframe_test PRIVATE cxx_std_17)
target_compile_definitions(port_subframe_test PRIVATE NOMINMAX)
add_test(NAME port_subframe COMMAND port_subframe_test)

# The instruction set the game library and the runtime are built for. AVX2 is the default and
# what every release so far has used; it needs Intel Haswell (2013) or AMD Ryzen. "AVX" reaches
# back to 2011, and "SSE2" is every x64 processor ever made. A player on a processor without AVX2
# cannot start the game at all, so this exists to build them something that runs.
# MELEE_CPU_BASELINE and MELEE_ARCH_FLAG are declared at the root CMakeLists.txt, before this
# subdirectory is added, so native_animation (built there) also gets the right /arch flag.
message(STATUS "melee: CPU baseline ${MELEE_CPU_BASELINE}")

# Host runtime: CPU helpers, memory, HLE, host services.
file(GLOB RUNTIME_SOURCES runtime/ppc/*.cpp runtime/hle/*.cpp runtime/host/*.cpp runtime/gx/*.cpp)
enable_language(C)
file(GLOB ENET_SOURCES third_party/enet/*.c)
add_library(port_enet STATIC ${ENET_SOURCES})
target_include_directories(port_enet PUBLIC third_party/enet/include)
target_compile_definitions(port_enet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_link_libraries(port_enet PUBLIC ws2_32 winmm)
# libusb, as Dolphin uses for the GameCube adapter. Talking to WinUSB directly meant an adapter on
# libusbK or libusb-win32 was invisible to us while Dolphin reported it detected, and players were
# being asked to swap drivers for hardware that already worked. libusb's Windows backend covers all
# three, so an adapter that works in Slippi works here.
add_library(port_libusb STATIC
  third_party/libusb/libusb/core.c
  third_party/libusb/libusb/descriptor.c
  third_party/libusb/libusb/hotplug.c
  third_party/libusb/libusb/io.c
  third_party/libusb/libusb/strerror.c
  third_party/libusb/libusb/sync.c
  third_party/libusb/libusb/os/events_windows.c
  third_party/libusb/libusb/os/threads_windows.c
  third_party/libusb/libusb/os/windows_common.c
  third_party/libusb/libusb/os/windows_usbdk.c
  third_party/libusb/libusb/os/windows_winusb.c)
target_include_directories(port_libusb PUBLIC third_party/libusb/libusb PRIVATE third_party/libusb/msvc)
target_compile_definitions(port_libusb PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_libusb PUBLIC advapi32 ole32 setupapi)
file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")
# VERSIONINFO wants the version as four comma separated numbers, not the "0.6.2" string. An
# executable with no version resource at all has no company, product or description for Windows to
# show, which is part of what makes an unsigned binary look like a dropper to SmartScreen and
# Defender: players have had the game quarantined on download. This does not replace signing,
# but it is free and it is a prerequisite for it.
string(REPLACE "." ";" MELEE_VERSION_PARTS "${MELEE_PORT_VERSION}")
list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
while(MELEE_VERSION_PART_COUNT LESS 4)
  list(APPEND MELEE_VERSION_PARTS "0")
  list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
endwhile()
list(SUBLIST MELEE_VERSION_PARTS 0 4 MELEE_VERSION_PARTS)
list(JOIN MELEE_VERSION_PARTS "," MELEE_VERSION_COMMAS)
add_library(runtime STATIC ${RUNTIME_SOURCES} third_party/slippilib/SlippiGame.cpp)
mu_squad_target(runtime)
target_compile_definitions(runtime PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
)
# ASIO output (audio mode 3): the Steinberg ASIO SDK host files only, under the SDK's GPL Version 3
# option (asio.cpp, asiodrivers.cpp, asiolist.cpp; see third_party/asio/README.txt). Windows only. The
# SDK's registry helpers use the ANSI API, so these three build without UNICODE.
if(WIN32)
  set(MELEE_ASIO_SOURCES third_party/asio/asio.cpp third_party/asio/asiodrivers.cpp third_party/asio/asiolist.cpp)
  target_sources(runtime PRIVATE ${MELEE_ASIO_SOURCES})
  set_source_files_properties(${MELEE_ASIO_SOURCES} PROPERTIES COMPILE_OPTIONS "/UUNICODE;/U_UNICODE;/W0")
  target_link_libraries(runtime PUBLIC advapi32)
endif()
target_include_directories(runtime PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx "${PROJECT_SOURCE_DIR}/native" third_party)
target_compile_features(runtime PUBLIC cxx_std_17)
target_compile_options(runtime PRIVATE /EHsc ${MELEE_ARCH_FLAG} /fp:precise /W3)
target_compile_definitions(runtime PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
target_link_libraries(runtime PUBLIC port_slpz native_animation port_enet port_libusb bcrypt comdlg32 d3d12 d3d11 dxgi d3dcompiler xinput9_1_0 winmm user32 gdi32 setupapi winusb hid mfplat mfreadwrite mfuuid ole32 shell32 advapi32 wintrust crypt32)

# mu_net: the project's own peer-to-peer session (no Slippi code). Keys, signatures and packet
# sealing come from Monocypher, the transport is ENet. Built where its sources are present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/mu_net/session_types.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher/monocypher.c")
add_library(port_monocypher STATIC third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c)
target_include_directories(port_monocypher PUBLIC third_party/monocypher)
add_library(mu_net STATIC runtime/mu_net/session_types.cpp runtime/mu_net/peer_identity.cpp
  runtime/mu_net/session_security.cpp runtime/mu_net/input_transport.cpp runtime/mu_net/peer_connector.cpp
  runtime/mu_net/rollback_session.cpp runtime/mu_net/result_outbox.cpp)
target_include_directories(mu_net PUBLIC runtime/mu_net)
target_compile_features(mu_net PUBLIC cxx_std_17)
target_compile_definitions(mu_net PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(mu_net PRIVATE /EHsc /utf-8)
target_link_libraries(mu_net PUBLIC port_enet PRIVATE port_monocypher bcrypt)
add_executable(port_mu_net_test tests/mu_net_test.cpp)
target_compile_definitions(port_mu_net_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(port_mu_net_test PRIVATE /EHsc /utf-8)
target_link_libraries(port_mu_net_test PRIVATE mu_net)
add_test(NAME port_mu_net COMMAND port_mu_net_test)
endif()

# WIN32: no console window when the game is double-clicked or started by another program. Command-line
# use still works, because main() reattaches to a parent console when there is one (see app/main.cpp).
# melee_host_exes: the programs that get the shaders and graphics DLLs beside them.
set(melee_host_exes melee_source)
# The source port: the whole application (renderers, DLSS, sub-frame, input, panel) with the game
# coming from melee_game.dll, built from the decompiled sources by sourceport/game with GCC.
# melee_source_probe is the bare first stage (no window, no runtime), for chasing boot crashes.
set(NATIVE_RECORDING_CODES "${CMAKE_CURRENT_BINARY_DIR}/native_recording_codes.h")
add_custom_command(OUTPUT "${NATIVE_RECORDING_CODES}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/native_recording_codes.py" "${NATIVE_RECORDING_CODES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/native_recording_codes.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gecko.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/slippi_sys_general/GameSettings/GALE01r2.ini"
          "${CMAKE_CURRENT_SOURCE_DIR}/slippi_sys/GameSettings/GALE01r2.ini"
  VERBATIM)
add_executable(melee_source WIN32 app/main.cpp app/settings_window.cpp ${MU_SOURCE_HOST_SOURCES} app/source_guest_boundary.cpp app/melee_unlocked.rc app/utf8.manifest)
target_sources(melee_source PRIVATE "${NATIVE_RECORDING_CODES}")
target_include_directories(melee_source PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(melee_source PRIVATE runtime/abi app)
target_sources(melee_source PRIVATE app/crash_report.cpp app/sim_profiler.cpp app/early_start.cpp app/replay_args.cpp app/cosmetics_cli.cpp)   # the pieces split out of main.cpp
target_link_libraries(melee_source PRIVATE runtime dbghelp)
# The --p2p-* command line: one mu_net session on the game's command channel, chosen at run time when
# those options are given. Without them nothing of it runs and Slippi play is as before.
target_sources(melee_source PRIVATE app/source_p2p.cpp)
target_link_libraries(melee_source PRIVATE mu_net)
# Fighter plugin approval (app/plugin_gate.h, docs/fighter-plugins.md): only plugins Nucleus signed or
# the player approved reach the game library. MELEE_PLUGIN_TEST_KEY trusts the TEST-ONLY key
# (tests/plugin_test_key) for scripted runs; --print-info reports it and windows-build/client_release.py
# refuses to package such a build.
option(MELEE_PLUGIN_TEST_KEY "Trust the TEST-ONLY fighter plugin signing key (scripted tests only, never a release)" OFF)
target_sources(melee_source PRIVATE app/plugin_gate.cpp app/plugin_keys.cpp)
if(TARGET port_monocypher)
  target_link_libraries(melee_source PRIVATE port_monocypher)
endif()
if(MELEE_PLUGIN_TEST_KEY)
  set_source_files_properties(app/plugin_keys.cpp PROPERTIES COMPILE_DEFINITIONS MELEE_PLUGIN_TEST_KEY)
endif()
if(TARGET port_monocypher AND MSVC)
  add_executable(port_plugin_gate_test tests/plugin_gate_test.cpp app/plugin_gate.cpp app/plugin_keys.cpp)
  target_include_directories(port_plugin_gate_test PRIVATE app third_party)
  target_link_libraries(port_plugin_gate_test PRIVATE port_monocypher shell32)
  target_compile_features(port_plugin_gate_test PRIVATE cxx_std_17)
  target_compile_options(port_plugin_gate_test PRIVATE /EHsc)
  target_compile_definitions(port_plugin_gate_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELEE_PLUGIN_TEST_SEED="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_test_key/TEST_ONLY_ed25519_seed.txt")
  add_test(NAME port_plugin_gate COMMAND port_plugin_gate_test)
endif()
# The command line and --print-info of the gate, through the built program (no disc, no game library).
add_test(NAME source_plugin_approval COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_approval_test.py"
  $<TARGET_FILE:melee_source>)
set_tests_properties(source_plugin_approval PROPERTIES TIMEOUT 300)
# The Source Port client release: its manifest, zip and --verify (windows-build/client_release.py).
add_test(NAME client_release COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../windows-build/client_release_test.py")
set_tests_properties(client_release PROPERTIES TIMEOUT 300)
# The link map: which runtime objects the program links, and its symbols (tools/crash_triage.py).
target_link_options(melee_source PRIVATE /MAP:$<TARGET_FILE_DIR:melee_source>/melee_source.map)
target_compile_options(melee_source PRIVATE /EHsc)
target_compile_definitions(melee_source PRIVATE MELEE_SOURCE_PORT NOMINMAX _CRT_SECURE_NO_WARNINGS
  MELEE_PORT_VERSION="${MELEE_PORT_VERSION}" MELEE_CPU_BASELINE_NAME="${MELEE_CPU_BASELINE}"
  MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS})
# Off by default; never on for a build that goes to a production listing (tests/agent_test_hook_absent.sh checks it).
add_test(NAME source_card_roundtrip COMMAND melee_source --hidden --volume 0
  --log-file "${CMAKE_CURRENT_BINARY_DIR}/source-card-roundtrip.log"
  --card-self-test "${CMAKE_CURRENT_BINARY_DIR}/source-card-roundtrip")
# --print-info through a pipe (one JSON object, exit 0, no disc: app/main.cpp print_info), and every
# one-shot mode with stdin closed: each must end within 20 s without a window or a crash report.
add_test(NAME source_print_info COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/print_info_test.py"
  $<TARGET_FILE:melee_source>)
set_tests_properties(source_print_info PROPERTIES TIMEOUT 300)
add_test(NAME source_cli_options COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/print_info_test.py"
  $<TARGET_FILE:melee_source> --cli $<BOOL:${MELEE_SQUAD_STRIKE}>)
set_tests_properties(source_cli_options PROPERTIES TIMEOUT 180)
# A replay naming a fighter, stage or costume this game does not have, or recorded with installed
# fighters, is refused before its match (exit 5, one log line, no recording); a retail one still plays.
# Needs MELEE_TEST_ISO and the game library (beside the program or MELEE_GAME_DLL); skipped without them.
add_test(NAME source_replay_refusal COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_refusal_test.py"
  $<TARGET_FILE:melee_source>)
set_tests_properties(source_replay_refusal PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
add_test(NAME port_coin_flags COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/coin_flags_test.py")
# One scripted offline match played twice with a fixed clock and seed: both state digests must be the
# same, and the same as the one recorded in the test (a change to gameplay updates it on purpose).
add_test(NAME source_match_digest COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/match_digest_test.py"
  $<TARGET_FILE:melee_source>)
set_tests_properties(source_match_digest PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
# Two instances play one peer-to-peer rollback match on loopback (tools/p2p_pair.py): 0 mismatched.
# Ports: MU_TEST_BASE_PORT (tools/run_tests.py sets it from the worktree's own range).
add_test(NAME source_p2p_pair COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p_pair_test.py"
  $<TARGET_FILE:melee_source>)
set_tests_properties(source_p2p_pair PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
add_executable(melee_source_probe app/main_source.cpp)
target_include_directories(melee_source_probe PRIVATE runtime/abi)
# Slippi's matchmaking service client (hle/slippi_mm_service.cpp, in runtime through the glob above):
# the join message, STUN, the NAT type, and whole sessions against a fake service on loopback.
add_executable(port_slippi_mm_service_test tests/slippi_mm_service_test.cpp runtime/hle/slippi_mm_service.cpp)
target_include_directories(port_slippi_mm_service_test PRIVATE runtime/hle runtime/host third_party)
target_compile_features(port_slippi_mm_service_test PRIVATE cxx_std_17)
target_compile_options(port_slippi_mm_service_test PRIVATE /EHsc)
target_compile_definitions(port_slippi_mm_service_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_slippi_mm_service_test PRIVATE ws2_32 secur32)
mu_built_test(port_slippi_mm_service port_slippi_mm_service_test)
set_tests_properties(port_slippi_mm_service PROPERTIES TIMEOUT 90)
# The Mods menu's host half fed what the site might send (nulls, missing keys, the wrong types, huge
# strings, odd Unicode) for both lists: every reply keeps its layout and drawable words. Offline.
# It and port_settings_load link the runtime the way melee_source does: the empty guest registries of
# app/source_guest_boundary.cpp, no translated game.
add_executable(port_mods_menu_fuzz_test tests/mods_menu_fuzz_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_mods_menu_fuzz_test PRIVATE runtime)
target_include_directories(port_mods_menu_fuzz_test PRIVATE runtime/host)
target_compile_options(port_mods_menu_fuzz_test PRIVATE /EHsc /utf-8)
target_compile_definitions(port_mods_menu_fuzz_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
mu_built_test(port_mods_menu_fuzz port_mods_menu_fuzz_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/nucleus_cards_16421.json ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/nucleus_mods_costume_10.json)
set_tests_properties(port_mods_menu_fuzz PROPERTIES TIMEOUT 120)
add_executable(port_fighter_package_test tests/fighter_package_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_fighter_package_test PRIVATE runtime)
target_include_directories(port_fighter_package_test PRIVATE runtime/host)
target_compile_options(port_fighter_package_test PRIVATE /EHsc /utf-8)
target_compile_definitions(port_fighter_package_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
mu_built_test(port_fighter_package port_fighter_package_test)
# Menu mods (host/menu_mods.h): a menu file's checks, the DAT Texture Wizard layout made readable, and
# the store's install, replace, serve and remove. Offline, no disc.
add_executable(port_menu_mods_test tests/menu_mods_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_menu_mods_test PRIVATE runtime)
target_include_directories(port_menu_mods_test PRIVATE runtime/host)
target_compile_options(port_menu_mods_test PRIVATE /EHsc /utf-8)
target_compile_definitions(port_menu_mods_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
mu_built_test(port_menu_mods port_menu_mods_test)
set_tests_properties(port_menu_mods PROPERTIES TIMEOUT 60)
# New stages (host/new_stages.h): the base a file plays over, the checks (groups, lines, starts, camera,
# blast zones) and the store's install, serve, icon, random setting and remove. Offline, no disc.
add_executable(port_new_stages_test tests/new_stages_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_new_stages_test PRIVATE runtime)
target_include_directories(port_new_stages_test PRIVATE runtime/host)
target_compile_options(port_new_stages_test PRIVATE /EHsc /utf-8)
target_compile_definitions(port_new_stages_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
mu_built_test(port_new_stages port_new_stages_test)
set_tests_properties(port_new_stages PROPERTIES TIMEOUT 60)
add_executable(port_settings_load_test tests/settings_load_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_settings_load_test PRIVATE runtime)
target_include_directories(port_settings_load_test PRIVATE runtime)
target_compile_options(port_settings_load_test PRIVATE /EHsc)
target_compile_definitions(port_settings_load_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_settings_load COMMAND port_settings_load_test)
# Items and Squad Strike online: never offered in a Vanilla run, Off in effect there whatever is saved.
add_executable(port_online_options_test tests/online_options_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_online_options_test PRIVATE runtime)
target_include_directories(port_online_options_test PRIVATE runtime)
target_compile_options(port_online_options_test PRIVATE /EHsc)
target_compile_definitions(port_online_options_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_online_options COMMAND port_online_options_test)
# The Mods categories' release switches (host/release_scope.h): the table's defaults, the settings
# lines, an automated run's MELEE_TEST_MODS_SCOPE, and Fighters off advertising nothing online.
add_executable(port_release_scope_test tests/release_scope_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_release_scope_test PRIVATE runtime)
target_include_directories(port_release_scope_test PRIVATE runtime)
target_compile_options(port_release_scope_test PRIVATE /EHsc)
target_compile_definitions(port_release_scope_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_test(NAME port_release_scope COMMAND port_release_scope_test)

add_executable(port_hps_bound_test tests/hps_bound_test.cpp)
target_include_directories(port_hps_bound_test PRIVATE runtime/hle runtime/host runtime/ppc)
target_compile_features(port_hps_bound_test PRIVATE cxx_std_17)
target_compile_options(port_hps_bound_test PRIVATE /EHsc)
target_compile_definitions(port_hps_bound_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_hps_bound_test PRIVATE mfplat mfreadwrite mfuuid ole32 shell32)
add_test(NAME port_hps_bound COMMAND port_hps_bound_test)

add_executable(port_audio_quality_test tests/audio_quality_test.cpp)
target_include_directories(port_audio_quality_test PRIVATE runtime/hle runtime/host runtime/ppc)
target_compile_features(port_audio_quality_test PRIVATE cxx_std_17)
target_compile_options(port_audio_quality_test PRIVATE /EHsc)
target_compile_definitions(port_audio_quality_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_audio_quality_test PRIVATE mfplat mfreadwrite mfuuid ole32 shell32)
add_test(NAME port_audio_quality COMMAND port_audio_quality_test)
add_executable(port_disc_fst_paths_test tests/disc_fst_paths_test.cpp runtime/host/disc_fst_paths.cpp)
target_include_directories(port_disc_fst_paths_test PRIVATE runtime/host)
target_compile_features(port_disc_fst_paths_test PRIVATE cxx_std_17)
target_compile_options(port_disc_fst_paths_test PRIVATE /EHsc)
add_test(NAME port_disc_fst_paths COMMAND port_disc_fst_paths_test)
add_executable(port_jukebox_custom_music_test tests/jukebox_custom_music_test.cpp runtime/hle/jukebox.cpp)
target_include_directories(port_jukebox_custom_music_test PRIVATE runtime/hle runtime/host runtime/ppc)
target_compile_features(port_jukebox_custom_music_test PRIVATE cxx_std_17)
target_compile_options(port_jukebox_custom_music_test PRIVATE /EHsc)
target_compile_definitions(port_jukebox_custom_music_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_jukebox_custom_music_test PRIVATE mfplat mfreadwrite mfuuid ole32 shell32)
add_test(NAME port_jukebox_custom_music COMMAND port_jukebox_custom_music_test)
add_executable(port_slippilib_bounds_test tests/slippilib_bounds_test.cpp third_party/slippilib/SlippiGame.cpp)
target_include_directories(port_slippilib_bounds_test PRIVATE third_party)
target_compile_features(port_slippilib_bounds_test PRIVATE cxx_std_17)
target_compile_options(port_slippilib_bounds_test PRIVATE /EHsc)
target_compile_definitions(port_slippilib_bounds_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
add_test(NAME port_slippilib_bounds COMMAND port_slippilib_bounds_test)
# Downloads unpacked in process; `port_archive_unpack_test --download <part> <game folder>` also runs
# one optional download for real (network, not part of ctest).
add_executable(port_archive_unpack_test tests/archive_unpack_test.cpp runtime/host/archive_unpack.cpp runtime/host/optional_downloads.cpp
  runtime/host/http_client.cpp)
target_include_directories(port_archive_unpack_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_archive_unpack_test PRIVATE cxx_std_17)
target_compile_options(port_archive_unpack_test PRIVATE /EHsc)
target_compile_definitions(port_archive_unpack_test PRIVATE NOMINMAX UNICODE _UNICODE)
add_test(NAME port_archive_unpack COMMAND port_archive_unpack_test)
# The host's one HTTP client against a small server the test runs on 127.0.0.1 (the first free port of
# 43800-43899): size cap, redirect limit, SHA-256, time limits, refused addresses.
add_executable(port_http_client_test tests/http_client_test.cpp runtime/host/http_client.cpp)
target_include_directories(port_http_client_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_http_client_test PRIVATE cxx_std_17)
target_compile_options(port_http_client_test PRIVATE /EHsc)
target_compile_definitions(port_http_client_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_http_client_test PRIVATE winhttp ws2_32 bcrypt)
add_test(NAME port_http_client COMMAND port_http_client_test)

add_executable(port_gpu_resource_test tests/gpu_resource_test.cpp runtime/gx/gx_d3d12.cpp runtime/gx/gx_shader.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_streamline.cpp runtime/gx/gx_fsr.cpp runtime/gx/flicker_scan.cpp runtime/gx/texture_pack.cpp
  runtime/gx/rtx_semantics.cpp)
target_include_directories(port_gpu_resource_test PRIVATE runtime/gx runtime/host runtime/ppc runtime/hle third_party)
target_compile_features(port_gpu_resource_test PRIVATE cxx_std_17)
target_compile_definitions(port_gpu_resource_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS GX_SRV_HEAP_SIZE=32 GX_SAMPLER_HEAP_SIZE=16 GX_CONSTANT_PAGE_SIZE=4096)
target_link_libraries(port_gpu_resource_test PRIVATE d3d12 dxgi d3dcompiler user32)
add_test(NAME port_gpu_resources COMMAND port_gpu_resource_test)
set_tests_properties(port_gpu_resources PROPERTIES TIMEOUT 60)

add_executable(port_render_observer_test tests/render_observer_test.cpp runtime/gx/render_observer.cpp runtime/gx/gx_texture.cpp runtime/gx/authored_pose.cpp runtime/gx/subframe.cpp runtime/gx/gx_shader.cpp
  runtime/gx/rtx_semantics.cpp)
target_include_directories(port_render_observer_test PRIVATE runtime/ppc runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_compile_features(port_render_observer_test PRIVATE cxx_std_17)
target_compile_definitions(port_render_observer_test PRIVATE NOMINMAX)
target_link_libraries(port_render_observer_test PRIVATE native_animation)
add_executable(port_projection_2d_test tests/projection_2d_test.cpp runtime/gx/gx_texture.cpp runtime/gx/authored_pose.cpp runtime/gx/subframe.cpp runtime/gx/gx_shader.cpp)
target_include_directories(port_projection_2d_test PRIVATE runtime/ppc runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_compile_features(port_projection_2d_test PRIVATE cxx_std_17)
target_compile_definitions(port_projection_2d_test PRIVATE NOMINMAX)
target_link_libraries(port_projection_2d_test PRIVATE native_animation)
add_test(NAME port_projection_2d COMMAND port_projection_2d_test)
add_test(NAME port_render_observer COMMAND port_render_observer_test)

add_executable(port_source_boundary_test tests/source_boundary_test.cpp app/source_guest_boundary.cpp)
target_include_directories(port_source_boundary_test PRIVATE runtime/ppc runtime/hle runtime/host)
target_compile_features(port_source_boundary_test PRIVATE cxx_std_17)
target_compile_options(port_source_boundary_test PRIVATE /EHsc)
add_test(NAME port_source_boundary COMMAND port_source_boundary_test)

add_executable(port_backend_dispatch_test tests/backend_dispatch_test.cpp runtime/gx/gx_backend_dispatch.cpp)
target_include_directories(port_backend_dispatch_test PRIVATE runtime/gx)
target_compile_features(port_backend_dispatch_test PRIVATE cxx_std_17)
add_test(NAME port_backend_dispatch COMMAND port_backend_dispatch_test)

add_executable(port_native_draw_audit_test tests/native_draw_audit_test.cpp runtime/gx/native_draw_audit.cpp)
target_include_directories(port_native_draw_audit_test PRIVATE runtime/gx)
target_compile_features(port_native_draw_audit_test PRIVATE cxx_std_17)
add_test(NAME port_native_draw_audit COMMAND port_native_draw_audit_test)

add_executable(port_native_pose_bridge_test tests/native_pose_bridge_test.cpp runtime/gx/native_pose_bridge.cpp)
target_include_directories(port_native_pose_bridge_test PRIVATE runtime/gx runtime/abi "${PROJECT_SOURCE_DIR}/native")
target_compile_features(port_native_pose_bridge_test PRIVATE cxx_std_17)
add_test(NAME port_native_pose_bridge COMMAND port_native_pose_bridge_test)
add_executable(port_native_pose_bridge_equiv_test tests/native_pose_bridge_equiv_test.cpp runtime/gx/native_pose_bridge.cpp)
target_include_directories(port_native_pose_bridge_equiv_test PRIVATE runtime/gx runtime/abi "${PROJECT_SOURCE_DIR}/native")
target_compile_features(port_native_pose_bridge_equiv_test PRIVATE cxx_std_17)
add_test(NAME port_native_pose_bridge_equiv COMMAND port_native_pose_bridge_equiv_test)

# Cache identity follows shader generation, constant layout and pipeline setup.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  runtime/gx/gx_shader.cpp runtime/gx/gx_shader.h runtime/gx/gx_d3d12.cpp)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.cpp" GX_SHADER_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.h" GX_LAYOUT_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_d3d12.cpp" GX_PIPELINE_HASH)
string(SHA256 GX_CACHE_HASH "${GX_SHADER_HASH}${GX_LAYOUT_HASH}${GX_PIPELINE_HASH}")
foreach(cache_target runtime port_gpu_resource_test)
  target_compile_definitions(${cache_target} PRIVATE GX_SHADER_CACHE_VERSION="${GX_CACHE_HASH}")
endforeach()

add_library(port_imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx12.cpp third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_vulkan.cpp)
# The Vulkan renderer (gx_vulkan.cpp) opens the loader at run time: headers only, no import library.
target_include_directories(port_imgui PUBLIC third_party/imgui third_party/vulkan-headers/include)
target_compile_definitions(port_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_compile_features(port_imgui PUBLIC cxx_std_17)
target_compile_definitions(port_imgui PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_compile_options(port_imgui PRIVATE ${MELEE_ARCH_FLAG})
target_link_libraries(port_imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)
target_compile_definitions(runtime PRIVATE GX_PC_SETTINGS)
target_link_libraries(runtime PUBLIC port_imgui)
# NVIDIA Streamline (DLSS): optional, enabled only when the headers, import library, and core
# redistributable DLLs are all present. A source archive may intentionally omit proprietary runtime
# binaries; that must produce a normal non-Streamline build rather than a post-link copy failure.
set(MELEE_STREAMLINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline" CACHE PATH
    "NVIDIA Streamline SDK root (headers, import library, and x64 runtime plugins)")
option(MELEE_REQUIRE_STREAMLINE "Fail configuration if the advertised NVIDIA features cannot be compiled" OFF)
if(MELEE_REQUIRE_STREAMLINE AND NOT (EXISTS "${MELEE_STREAMLINE_ROOT}/include/sl.h" AND
   EXISTS "${MELEE_STREAMLINE_ROOT}/lib/x64/sl.interposer.lib" AND
   EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/sl.interposer.dll" AND
   EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/sl.common.dll"))
  message(FATAL_ERROR "MELEE_REQUIRE_STREAMLINE requires a complete MELEE_STREAMLINE_ROOT; runtime DLL copying alone does not enable NVIDIA features")
endif()
if(EXISTS "${MELEE_STREAMLINE_ROOT}/include/sl.h" AND
   EXISTS "${MELEE_STREAMLINE_ROOT}/lib/x64/sl.interposer.lib" AND
   EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/sl.interposer.dll" AND
   EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/sl.common.dll")
  target_compile_definitions(runtime PRIVATE GX_STREAMLINE)
  target_include_directories(runtime PRIVATE "${MELEE_STREAMLINE_ROOT}/include")
  target_link_libraries(runtime PUBLIC "${MELEE_STREAMLINE_ROOT}/lib/x64/sl.interposer.lib" wintrust crypt32 delayimp)
  # librashader (player-chosen shader presets; loaded at run time; missing file = the setting is off)
  # and DXC below: not in the repository, tools/fetch_graphics_dlls.py downloads them.
  foreach(fetched_dll librashader/bin/librashader.dll dxc/bin/dxcompiler.dll dxc/bin/dxil.dll)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${fetched_dll}")
      message(FATAL_ERROR "port/third_party/${fetched_dll} is missing: run python tools/fetch_graphics_dlls.py")
    endif()
  endforeach()
  foreach(host_exe IN LISTS melee_host_exes)   # same folder, same DLLs
    target_link_options(${host_exe} PRIVATE /DELAYLOAD:sl.interposer.dll)
    foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_g.dll nvngx_dlssg.dll sl.reflex.dll sl.pcl.dll)
      if(EXISTS "${MELEE_STREAMLINE_ROOT}/bin/x64/${sl_dll}")
        add_custom_command(TARGET ${host_exe} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MELEE_STREAMLINE_ROOT}/bin/x64/${sl_dll}" "$<TARGET_FILE_DIR:${host_exe}>/${sl_dll}")
      endif()
    endforeach()
    add_custom_command(TARGET ${host_exe} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librashader/bin/librashader.dll" "$<TARGET_FILE_DIR:${host_exe}>/librashader.dll")
    foreach(dxc_dll dxcompiler.dll dxil.dll)   # the compiler librashader uses on Direct3D 12
      add_custom_command(TARGET ${host_exe} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc/bin/${dxc_dll}" "$<TARGET_FILE_DIR:${host_exe}>/${dxc_dll}")
    endforeach()
  endforeach()
elseif(EXISTS "${MELEE_STREAMLINE_ROOT}/include/sl.h")
  message(STATUS "NVIDIA Streamline runtime is incomplete; building without Streamline/DLSS")
endif()

add_executable(port_phob_test tests/phob_test.cpp runtime/host/phob.cpp runtime/host/phobscope.cpp)
target_include_directories(port_phob_test PRIVATE runtime/host)
target_compile_features(port_phob_test PRIVATE cxx_std_17)
add_test(NAME port_phob COMMAND port_phob_test)
