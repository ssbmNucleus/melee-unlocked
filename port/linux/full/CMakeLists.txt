# The whole Source Port on Linux: the same host sources as the Windows melee_source (Slippi online,
# replays, spectating, mods, cosmetics, the settings panel, Gecko codes, the GameCube adapter), with
# Linux implementations where Windows does the work: the SDL2 window and input, OpenGL rendering,
# SDL2 audio, hidraw controllers, FFmpeg media, and port/linux/compat for the small Win32 API surface
# the portable files use. The game comes from the same melee_game.so the Linux build makes.
cmake_minimum_required(VERSION 3.25)
set(PORT "${PROJECT_SOURCE_DIR}/port")
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)

file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
string(REPLACE "." ";" MELEE_VERSION_PARTS "${MELEE_PORT_VERSION}")
list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
while(MELEE_VERSION_PART_COUNT LESS 4)
  list(APPEND MELEE_VERSION_PARTS "0")
  list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
endwhile()
list(SUBLIST MELEE_VERSION_PARTS 0 4 MELEE_VERSION_PARTS)
list(JOIN MELEE_VERSION_PARTS "," MELEE_VERSION_COMMAS)
# The compatibility build (any other baseline) sits beside the AVX2 one under its own names; whatever
# starts the game picks between them by asking the processor.
if(MELEE_CPU_BASELINE STREQUAL "AVX2")
  set(MELEE_GAME_FILE melee_game.so)
  set(MELEE_SOURCE_NAME melee_source)
else()
  set(MELEE_GAME_FILE melee_game_compat.so)
  set(MELEE_SOURCE_NAME melee_source_compat)
endif()
if(MELEE_CPU_BASELINE STREQUAL "AVX2")
  set(MELEE_HOST_ARCH -mavx2 -mfma)
elseif(MELEE_CPU_BASELINE STREQUAL "AVX")
  set(MELEE_HOST_ARCH -mavx)
else()
  set(MELEE_HOST_ARCH "")
endif()
set(COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/../compat")
set(MELEE_LINUX_COMMON_FLAGS -include "${COMPAT}/windows.h" ${MELEE_HOST_ARCH} -ffp-contract=off -fno-strict-aliasing
    -Wno-unknown-pragmas -Wno-multichar -pthread)

# Generated sources, as the Windows build makes them.
set(FULL_FOBJ "${CMAKE_CURRENT_BINARY_DIR}/generated/FObjHost.cpp")
add_custom_command(OUTPUT "${FULL_FOBJ}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" "${FULL_FOBJ}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" VERBATIM)
add_library(full_native_animation STATIC "${FULL_FOBJ}")
target_include_directories(full_native_animation PUBLIC "${PROJECT_SOURCE_DIR}/native")
target_compile_features(full_native_animation PUBLIC cxx_std_14)
target_compile_options(full_native_animation PRIVATE ${MELEE_HOST_ARCH})
set(NATIVE_RECORDING_CODES "${CMAKE_CURRENT_BINARY_DIR}/native_recording_codes.h")
add_custom_command(OUTPUT "${NATIVE_RECORDING_CODES}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/native_recording_codes.py" "${NATIVE_RECORDING_CODES}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/native_recording_codes.py" "${PROJECT_SOURCE_DIR}/tools/gecko.py"
          "${PORT}/slippi_sys_general/GameSettings/GALE01r2.ini" "${PORT}/slippi_sys/GameSettings/GALE01r2.ini"
  VERBATIM)

enable_language(C)
file(GLOB FULL_ENET_SOURCES "${PORT}/third_party/enet/*.c")
add_library(full_enet STATIC ${FULL_ENET_SOURCES})
target_include_directories(full_enet PUBLIC "${PORT}/third_party/enet/include")
target_compile_definitions(full_enet PRIVATE HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_MSGHDR_FLAGS=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1)
target_link_libraries(full_enet PUBLIC Threads::Threads)
set(FAKE_RING_ENET_SOURCES ${FULL_ENET_SOURCES})
list(REMOVE_ITEM FAKE_RING_ENET_SOURCES "${PORT}/third_party/enet/unix.c" "${PORT}/third_party/enet/win32.c")
add_library(full_monocypher STATIC "${PORT}/third_party/monocypher/monocypher.c" "${PORT}/third_party/monocypher/monocypher-ed25519.c")
target_include_directories(full_monocypher PUBLIC "${PORT}/third_party/monocypher")

add_library(full_imgui STATIC
  "${PORT}/third_party/imgui/imgui.cpp" "${PORT}/third_party/imgui/imgui_draw.cpp"
  "${PORT}/third_party/imgui/imgui_tables.cpp" "${PORT}/third_party/imgui/imgui_widgets.cpp"
  "${PORT}/third_party/imgui/backends/imgui_impl_sdl2.cpp" "${PORT}/third_party/imgui/backends/imgui_impl_opengl3.cpp"
  "${PORT}/third_party/imgui/backends/imgui_impl_vulkan.cpp")
# The Vulkan renderer (runtime/gx/gx_vulkan.cpp) opens libvulkan at run time: headers only.
target_include_directories(full_imgui PUBLIC "${PORT}/third_party/imgui" "${PORT}/third_party/imgui/backends" "${PORT}/third_party/vulkan-headers/include")
target_compile_definitions(full_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_compile_features(full_imgui PUBLIC cxx_std_17)
target_link_libraries(full_imgui PUBLIC SDL2::SDL2 ${CMAKE_DL_LIBS})

# The host runtime: every portable file of runtime/, the Windows renderers and Windows-only GPU
# libraries left out, the Linux implementations added.
file(GLOB FULL_RUNTIME_SOURCES "${PORT}/runtime/ppc/*.cpp" "${PORT}/runtime/hle/*.cpp" "${PORT}/runtime/host/*.cpp" "${PORT}/runtime/gx/*.cpp")
foreach(removed
    runtime/gx/gx_d3d11.cpp runtime/gx/gx_d3d12.cpp
    runtime/gx/gx_streamline.cpp runtime/gx/gx_fsr.cpp runtime/gx/gx_fsr_vk.cpp
    runtime/gx/gx_backend.cpp runtime/gx/pc_settings_dx11.cpp runtime/host/audio.cpp
    runtime/gx/gx_dxc_linux.cpp)
  list(REMOVE_ITEM FULL_RUNTIME_SOURCES "${PORT}/${removed}")
endforeach()
# DXC's loader (gx_dxc_linux.cpp) without the Win32 compatibility preinclude: DXC's own header defines
# those types.
add_library(full_dxc OBJECT "${PORT}/runtime/gx/gx_dxc_linux.cpp")
target_include_directories(full_dxc PRIVATE "${PORT}/third_party")
target_compile_features(full_dxc PRIVATE cxx_std_17)
set_target_properties(full_dxc PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(full_runtime STATIC ${FULL_RUNTIME_SOURCES} $<TARGET_OBJECTS:full_dxc> "${PORT}/third_party/slippilib/SlippiGame.cpp"
  "${COMPAT}/compat.cpp" "${COMPAT}/winhttp_ws.cpp" ../xinput_sdl.cpp ../hid_linux.cpp ../renderer_gl.cpp ../renderer_presenter.cpp
  gx_backend_linux.cpp pc_settings_gl.cpp audio_sdl.cpp linux_media.cpp gpu_features_linux.cpp)
target_compile_definitions(full_runtime PUBLIC MELEE_LINUX GX_PC_SETTINGS
  PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}" MELEE_PLAYBACK_GCT_BASE=0u MELEE_PLAYBACK_GCT_SIZE=0u
  GX_SHADER_CACHE_VERSION="linux-gl-1")
# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
include("${PORT}/cmake/squad.cmake")
mu_squad_target(full_runtime)
target_include_directories(full_runtime PUBLIC "${COMPAT}" "${PORT}/runtime/ppc" "${PORT}/runtime/host" "${PORT}/runtime/hle"
  "${PORT}/runtime/gx" "${PORT}/runtime/mu_net" "${PORT}/runtime/abi" "${PROJECT_SOURCE_DIR}/native"
  "${PORT}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/.." ${LIBUSB_INCLUDE_DIRS})
target_compile_features(full_runtime PUBLIC cxx_std_17)
target_compile_options(full_runtime PRIVATE ${MELEE_LINUX_COMMON_FLAGS})
target_link_libraries(full_runtime PUBLIC full_native_animation full_enet full_imgui PkgConfig::LIBUSB SDL2::SDL2 OpenGL::GL
  GLEW::GLEW CURL::libcurl OpenSSL::Crypto PkgConfig::FFMPEG Threads::Threads ${CMAKE_DL_LIBS} m)

# The Vulkan renderer's DXC path (gx_dxc_linux.cpp) against the shipped libdxcompiler.so, which is not
# in the repository: tools/fetch_graphics_dlls.py downloads it.
if(NOT EXISTS "${PORT}/third_party/dxc/linux/libdxcompiler.so")
  message(FATAL_ERROR "port/third_party/dxc/linux/libdxcompiler.so is missing: run python3 tools/fetch_graphics_dlls.py --dxc-linux")
endif()
add_executable(linux_dxc_spirv_test "${PORT}/tests/linux_dxc_spirv_test.cpp" "${PORT}/runtime/gx/gx_dxc_linux.cpp")
target_include_directories(linux_dxc_spirv_test PRIVATE "${PORT}/third_party")
target_compile_features(linux_dxc_spirv_test PRIVATE cxx_std_17)
target_link_libraries(linux_dxc_spirv_test PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME linux_dxc_spirv COMMAND linux_dxc_spirv_test)
set_tests_properties(linux_dxc_spirv PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${PORT}/third_party/dxc/linux")

add_executable(linux_ws_frame_test "${COMPAT}/tests/ws_frame_test.cpp")
target_compile_features(linux_ws_frame_test PRIVATE cxx_std_17)
target_link_libraries(linux_ws_frame_test PRIVATE OpenSSL::Crypto)
add_test(NAME linux_ws_frames COMMAND linux_ws_frame_test)

add_library(full_mu_net STATIC "${PORT}/runtime/mu_net/session_types.cpp" "${PORT}/runtime/mu_net/peer_identity.cpp"
  "${PORT}/runtime/mu_net/session_security.cpp" "${PORT}/runtime/mu_net/input_transport.cpp" "${PORT}/runtime/mu_net/peer_connector.cpp"
  "${PORT}/runtime/mu_net/rollback_session.cpp" "${PORT}/runtime/mu_net/result_outbox.cpp")
target_include_directories(full_mu_net PUBLIC "${PORT}/runtime/mu_net" "${COMPAT}")
target_compile_features(full_mu_net PUBLIC cxx_std_17)
target_compile_definitions(full_mu_net PRIVATE MELEE_LINUX)
target_compile_options(full_mu_net PRIVATE ${MELEE_LINUX_COMMON_FLAGS})
target_link_libraries(full_mu_net PUBLIC full_enet PRIVATE full_monocypher OpenSSL::Crypto)

file(GLOB MU_SOURCE_HOST_SOURCES "${PORT}/app/source_host*.cpp")   # the host, one file per family (source_host_internal.h)
add_executable(melee_source "${PORT}/app/main.cpp" settings_window_linux.cpp ${MU_SOURCE_HOST_SOURCES}
  "${PORT}/app/source_guest_boundary.cpp" "${PORT}/app/source_p2p.cpp" "${NATIVE_RECORDING_CODES}"
  "${PORT}/app/crash_report.cpp" "${PORT}/app/sim_profiler.cpp" "${PORT}/app/early_start.cpp"
  "${PORT}/app/replay_args.cpp" "${PORT}/app/cosmetics_cli.cpp")
target_include_directories(melee_source PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${PORT}/runtime/abi" "${PORT}/app")
target_compile_definitions(melee_source PRIVATE MELEE_SOURCE_PORT MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
  MELEE_CPU_BASELINE_NAME="${MELEE_CPU_BASELINE}" MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS}
  MELEE_GAME_LIBRARY_NAME="${MELEE_GAME_FILE}")
set_target_properties(melee_source PROPERTIES OUTPUT_NAME ${MELEE_SOURCE_NAME})
# The shared Win32 compatibility preinclude is C++; never force it into the C99 helper.
target_compile_options(melee_source PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${MELEE_LINUX_COMMON_FLAGS}>")
target_compile_features(melee_source PRIVATE c_std_99)
# .slpz replays given to --replay (app/main.cpp playable_replay).
include("${PORT}/app/replay_slpz.cmake")
target_link_libraries(melee_source PRIVATE full_runtime full_mu_net port_slpz)
set_target_properties(melee_source PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The release carries its own copies of the libraries that differ between distributions, in lib/
# beside the program (tools/package_linux_release.py); without lib/ the system's are used.
target_link_options(melee_source PRIVATE -pie -rdynamic "-Wl,-rpath,\$ORIGIN/lib" -Wl,--enable-new-dtags)
# Slippi's three menu files are not in slippi_sys (they are mostly the disc's own assets): the game
# rebuilds them from the player's disc with the deltas, which it reads from deltas/ beside Sys
# (runtime/host/sysgen.cpp), as in a release.
file(GLOB MELEE_SYS_DELTAS "${PORT}/sys_deltas/*.vcdiff")
add_custom_command(TARGET melee_source POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PORT}/slippi_sys" "$<TARGET_FILE_DIR:melee_source>/Sys"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:melee_source>/deltas"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PORT}/sys_deltas/manifest.json" ${MELEE_SYS_DELTAS}
          "$<TARGET_FILE_DIR:melee_source>/deltas"
  VERBATIM)
# The game tier here (port/tests/test_labels.cmake labels them): the Windows build's game tests that do
# not depend on the PE build. They need MELEE_TEST_ISO and a display (the game opens a hidden GL
# window). Left out: source_print_info (its game library fields are Windows only, early_start.cpp) and
# source_plugin_approval (plugin_gate.cpp is not built here).
add_test(NAME source_card_roundtrip COMMAND melee_source --hidden --volume 0
  --log-file "${CMAKE_CURRENT_BINARY_DIR}/source-card-roundtrip.log"
  --card-self-test "${CMAKE_CURRENT_BINARY_DIR}/source-card-roundtrip")
foreach(game_test replay_refusal match_digest p2p_pair)
  add_test(NAME source_${game_test} COMMAND "${Python3_EXECUTABLE}" -B "${PORT}/tests/${game_test}_test.py"
    $<TARGET_FILE:melee_source>)
  set_tests_properties(source_${game_test} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()
mu_label_tests_at_end()

if(TARGET melee_game)
  add_dependencies(melee_source melee_game)
  add_custom_command(TARGET melee_source POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:melee_game>" "$<TARGET_FILE_DIR:melee_source>/${MELEE_GAME_FILE}"
    VERBATIM)
elseif(MELEE_LINUX_GAME_LIBRARY AND EXISTS "${MELEE_LINUX_GAME_LIBRARY}")
  get_filename_component(MELEE_LINUX_GAME_DIR "${MELEE_LINUX_GAME_LIBRARY}" DIRECTORY)
  # Copied on every build, not only when the host relinks: a game library rebuilt with no host change
  # stayed stale beside it.
  add_custom_target(melee_game_copy
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:melee_source>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MELEE_LINUX_GAME_LIBRARY}" "$<TARGET_FILE_DIR:melee_source>/${MELEE_GAME_FILE}"
    VERBATIM)
  add_dependencies(melee_source melee_game_copy)
endif()
