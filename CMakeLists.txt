cmake_minimum_required(VERSION 3.15)
project(MeleeUnlocked LANGUAGES CXX)
# MSVC reads source without a BOM in the system code page, so a machine set to a double-byte one
# (a Windows test PC) fails on UTF-8 text such as an ellipsis (C2001). Every C and C++ file here is UTF-8;
# set here so it reaches the root's own tests and every subdirectory.
if(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
endif()
# The build that goes to players (port/runtime/host/public_build.h, docs/client-release.md): test-only
# launch options refused, test hooks read nothing from the environment, other players named by a hash
# in the log. Off, everything behaves as it always has. The game library has the same option.
option(NUCLEUS_PUBLIC_BUILD "Build the player release: test hooks and test-only options left out" OFF)
if(NUCLEUS_PUBLIC_BUILD)
  add_compile_definitions(NUCLEUS_PUBLIC_BUILD=1)
endif()
include(port/tests/test_labels.cmake)
include(port/tests/boot_diagnostics.cmake)
include(port/tests/session_diagnostics.cmake)

enable_testing()
find_package(Threads REQUIRED)
add_executable(port_sdl_video_thread_test port/tests/sdl_video_thread_test.cpp)
target_include_directories(port_sdl_video_thread_test PRIVATE port/runtime/host)
target_compile_features(port_sdl_video_thread_test PRIVATE cxx_std_17)
target_link_libraries(port_sdl_video_thread_test PRIVATE Threads::Threads)
add_test(NAME port_sdl_video_thread COMMAND port_sdl_video_thread_test)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME port_linux_test_targets COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/port/tests/linux_test_targets_test.py")
add_test(NAME port_setjmp_done COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/port/tests/setjmp_done_test.py")
add_executable(port_gx_object_layout_test port/tests/gx_object_layout_test.cpp)
target_compile_features(port_gx_object_layout_test PRIVATE cxx_std_17)
add_test(NAME port_gx_object_layout COMMAND port_gx_object_layout_test)
add_test(NAME port_slippi_agent_lock COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/port/tests/slippi_agent_lock_test.py")
# The tools' own unit tests (no game, no disc): digests, replays, crash reports, release signing.
foreach(tool digest_compare fighter_digest replay_validation crash_report_text jev_review windows_signing)
  add_test(NAME tools_${tool} COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_${tool}.py")
endforeach()
add_executable(port_slippi_address_test port/tests/slippi_address_test.cpp)
target_include_directories(port_slippi_address_test PRIVATE port/runtime/hle)
target_compile_features(port_slippi_address_test PRIVATE cxx_std_17)
mu_built_test(port_slippi_address port_slippi_address_test)
add_test(NAME port_slippi_fallback COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/port/tests/slippi_fallback_test.py")

# Release policies run with the legacy default and an actual branch compile definition.
add_executable(port_pacing_diagnostics_test port/tests/pacing_diagnostics_test.cpp)
target_include_directories(port_pacing_diagnostics_test PRIVATE port/runtime/gx)
target_compile_features(port_pacing_diagnostics_test PRIVATE cxx_std_17)
add_test(NAME port_pacing_diagnostics COMMAND port_pacing_diagnostics_test)

find_package(Threads REQUIRED)

# A2: compile presentation/input helpers with the actual A1 model, without HSD.
enable_language(C)
add_executable(port_ranked_setup_widgets_test port/tests/ranked_setup_widgets_test.cpp
  sourceport/game/shim/mu_ranked_widgets.c sourceport/game/shim/mu_ranked_setup_model.c)
target_include_directories(port_ranked_setup_widgets_test PRIVATE sourceport/game/shim sourceport/game/include port/runtime/abi port/runtime/hle)
target_compile_definitions(port_ranked_setup_widgets_test PRIVATE MU_RANKED_WIDGETS_TEST)
target_compile_features(port_ranked_setup_widgets_test PRIVATE cxx_std_17 c_std_11)
foreach(group layout timers inputs characters walk)
  add_test(NAME port_ranked_setup_widgets_${group} COMMAND port_ranked_setup_widgets_test ${group})
endforeach()

# Compiles game shim headers that need GCC/Clang (scalar_storage_order big-endian structs, __builtin_*):
# MSVC cannot build them. The MinGW and Linux builds still run it.
if(NOT MSVC)
add_test(NAME port_ranked_setup_header COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/port/tests/ranked_setup_text_test.py")
add_executable(port_ranked_setup_screen_test port/tests/ranked_setup_screen_test.cpp
  sourceport/game/shim/mu_ranked_widgets.c sourceport/game/shim/mu_ranked_setup_model.c
  sourceport/game/shim/mu_ranked_setup_online.c)
target_include_directories(port_ranked_setup_screen_test PRIVATE sourceport/game/shim sourceport/game/include port/runtime/abi port/runtime/hle)
target_compile_definitions(port_ranked_setup_screen_test PRIVATE MU_RANKED_WIDGETS_TEST MU_RANKED_TEST_CPU)
target_compile_features(port_ranked_setup_screen_test PRIVATE cxx_std_17 c_std_11)
add_test(NAME port_ranked_setup_screen_cpu COMMAND port_ranked_setup_screen_test)
endif()

# A1: compile the allocation-free rules model as C, with C++ host drivers.
enable_language(C)
add_executable(port_slippi_game_prep_test port/tests/slippi_game_prep_test.cpp)
target_include_directories(port_slippi_game_prep_test PRIVATE port/runtime/hle)
target_compile_features(port_slippi_game_prep_test PRIVATE cxx_std_17)
mu_built_test(port_slippi_game_prep port_slippi_game_prep_test)

add_executable(port_ranked_setup_model_test port/tests/ranked_setup_model_test.cpp
  sourceport/game/shim/mu_ranked_setup_model.c)
target_include_directories(port_ranked_setup_model_test PRIVATE sourceport/game/include port/runtime/abi port/runtime/hle)
target_compile_features(port_ranked_setup_model_test PRIVATE cxx_std_17 c_std_11)
foreach(group stages colors timers interleave disconnects tiebreak mirror_idle)
  add_test(NAME port_ranked_setup_${group} COMMAND port_ranked_setup_model_test ${group})
endforeach()


# Compiles game shim C that needs GCC/Clang (scalar_storage_order big-endian structs, __builtin_*,
# C compound literals in C++): MSVC cannot build it. The MinGW and Linux builds still run it.
if(NOT MSVC)
foreach(variant peer cpu)
  add_executable(port_ranked_setup_online_${variant}_test port/tests/ranked_setup_online_test.cpp
    sourceport/game/shim/mu_ranked_setup_model.c sourceport/game/shim/mu_ranked_setup_online.c)
  target_include_directories(port_ranked_setup_online_${variant}_test PRIVATE sourceport/game/include port/runtime/abi port/runtime/hle port/third_party)
  target_compile_features(port_ranked_setup_online_${variant}_test PRIVATE cxx_std_17 c_std_11)
  if(variant STREQUAL "cpu")
    target_compile_definitions(port_ranked_setup_online_${variant}_test PRIVATE MU_RANKED_TEST_CPU)
  endif()
  add_test(NAME port_ranked_setup_online_${variant} COMMAND port_ranked_setup_online_${variant}_test)
endforeach()

endif()



option(MELEE_BUILD_NATIVE_LINUX "Build the native Linux Source Port (ELF host and game)" OFF)
if(MELEE_BUILD_NATIVE_LINUX)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MELEE_BUILD_NATIVE_LINUX requires a 64-bit Linux toolchain")
  endif()
  enable_testing()
  include(port/tests/settings_mouse.cmake)
  include(port/tests/keyboard_navigation.cmake)
  include(port/tests/controller_profiles.cmake)
  add_subdirectory(port/linux)
  return()
endif()
enable_testing()
include(port/tests/settings_mouse.cmake)
include(port/tests/keyboard_navigation.cmake)
include(port/tests/controller_profiles.cmake)
if(WIN32 OR APPLE)
  include(port/tests/production_integrity.cmake)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT WIN32)
  if(APPLE)
    add_executable(port_static_mod_online_test port/tests/static_mod_online_test.cpp
      port/runtime/host/mod_profile.cpp port/tests/identity_stubs/hash_apple.cpp)
    target_include_directories(port_static_mod_online_test PRIVATE port/tests/identity_stubs port/runtime/hle port/runtime/host)
    target_compile_features(port_static_mod_online_test PRIVATE cxx_std_17)
    add_test(NAME port_static_mod_online COMMAND port_static_mod_online_test)
  endif()
  add_executable(port_cosmetic_cli_test port/tests/cosmetic_cli_test.cpp)
  target_include_directories(port_cosmetic_cli_test PRIVATE port/runtime/host)
  target_compile_features(port_cosmetic_cli_test PRIVATE cxx_std_17)
  add_test(NAME port_cosmetic_cli COMMAND port_cosmetic_cli_test)
  add_executable(port_slippi_netplay_receive_test port/tests/slippi_netplay_receive_test.cpp)
  target_include_directories(port_slippi_netplay_receive_test PRIVATE port/runtime/hle port/runtime/host)
  target_compile_features(port_slippi_netplay_receive_test PRIVATE cxx_std_17)
  mu_built_test(port_slippi_netplay_receive port_slippi_netplay_receive_test)
  add_executable(port_slippi_gone_port_test port/tests/slippi_gone_port_test.cpp)
  target_include_directories(port_slippi_gone_port_test PRIVATE port/runtime/hle)
  target_compile_features(port_slippi_gone_port_test PRIVATE cxx_std_17)
  target_compile_definitions(port_slippi_gone_port_test PRIVATE SLIPPI_ONLINE_CPP="${CMAKE_CURRENT_SOURCE_DIR}/port/runtime/hle/slippi_online.cpp" SLIPPI_NET_CPP="${CMAKE_CURRENT_SOURCE_DIR}/port/runtime/hle/slippi_net.cpp")
  mu_built_test(port_slippi_gone_port port_slippi_gone_port_test)
  add_executable(port_slippi_time_sync_test port/tests/slippi_time_sync_test.cpp)
  target_include_directories(port_slippi_time_sync_test PRIVATE port/runtime/hle)
  target_compile_features(port_slippi_time_sync_test PRIVATE cxx_std_17)
  mu_built_test(port_slippi_time_sync port_slippi_time_sync_test)
  add_executable(port_lcancel_policy_test port/tests/lcancel_policy_test.cpp)
  target_include_directories(port_lcancel_policy_test PRIVATE port/runtime/host)
  target_compile_features(port_lcancel_policy_test PRIVATE cxx_std_17)
  add_test(NAME port_lcancel_policy COMMAND port_lcancel_policy_test)
  add_executable(port_gc_content_sha1_test port/tests/gc_content_sha1_test.cpp
    port/runtime/host/sysgen.cpp port/runtime/host/vcdiff.cpp)
  target_include_directories(port_gc_content_sha1_test PRIVATE port/runtime/host port/third_party)
  target_compile_features(port_gc_content_sha1_test PRIVATE cxx_std_17)
  add_test(NAME port_gc_content_sha1 COMMAND port_gc_content_sha1_test)
  add_executable(port_rollback_perf_test port/tests/rollback_perf_test.cpp)
  target_include_directories(port_rollback_perf_test PRIVATE port/runtime/host)
  target_compile_features(port_rollback_perf_test PRIVATE cxx_std_17)
  add_test(NAME port_rollback_perf COMMAND port_rollback_perf_test)
  add_executable(port_sim_work_test port/tests/sim_work_test.cpp)
  target_include_directories(port_sim_work_test PRIVATE port/runtime/host)
  target_compile_features(port_sim_work_test PRIVATE cxx_std_17)
  add_test(NAME port_sim_work COMMAND port_sim_work_test)
  add_executable(port_frame_queue_test port/tests/frame_queue_test.cpp)
  target_include_directories(port_frame_queue_test PRIVATE port/runtime/gx port/runtime/host)
  target_compile_features(port_frame_queue_test PRIVATE cxx_std_17)
  add_test(NAME port_frame_queue COMMAND port_frame_queue_test)
  add_executable(port_prewarm_only_test port/tests/prewarm_only_test.cpp)
  target_include_directories(port_prewarm_only_test PRIVATE port/app)
  target_compile_features(port_prewarm_only_test PRIVATE cxx_std_17)
  add_test(NAME port_prewarm_only COMMAND port_prewarm_only_test)
  add_executable(port_pipeline_cache_store_test port/tests/pipeline_cache_store_test.cpp)
  target_include_directories(port_pipeline_cache_store_test PRIVATE port/runtime/gx)
  target_compile_features(port_pipeline_cache_store_test PRIVATE cxx_std_17)
  find_package(Threads REQUIRED)
  target_link_libraries(port_pipeline_cache_store_test PRIVATE Threads::Threads)
  add_test(NAME port_pipeline_cache_store COMMAND port_pipeline_cache_store_test "${CMAKE_CURRENT_BINARY_DIR}/pipeline_cache_store_test")
  if(APPLE)
    add_test(NAME port_mac_digest COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/port/mac/compare_digest_test.py")
    add_executable(port_mac_ws_frame_test port/linux/compat/tests/ws_frame_test.cpp)
    target_compile_features(port_mac_ws_frame_test PRIVATE cxx_std_17)
    add_test(NAME port_mac_ws_frames COMMAND port_mac_ws_frame_test)
  endif()
  find_package(Threads REQUIRED)
  add_executable(port_native_online_policy_test port/tests/native_online_policy_test.cpp)
  target_include_directories(port_native_online_policy_test PRIVATE port/runtime/hle)
  target_compile_features(port_native_online_policy_test PRIVATE cxx_std_17)
  add_test(NAME port_native_online_policy COMMAND port_native_online_policy_test)
  add_executable(port_source_mod_view_test port/tests/source_mod_view_test.cpp)
  target_include_directories(port_source_mod_view_test PRIVATE port/runtime/hle)
  target_compile_features(port_source_mod_view_test PRIVATE cxx_std_17)
  add_test(NAME port_source_mod_view COMMAND port_source_mod_view_test)
  add_executable(port_stage_read_pin_test port/tests/stage_read_pin_test.cpp)
  target_include_directories(port_stage_read_pin_test PRIVATE port/runtime/host)
  target_compile_features(port_stage_read_pin_test PRIVATE cxx_std_17)
  add_test(NAME port_stage_read_pin COMMAND port_stage_read_pin_test)
  add_executable(port_host_stub_policy_test port/tests/host_stub_policy_test.cpp)
  target_include_directories(port_host_stub_policy_test PRIVATE port/runtime/ppc)
  target_compile_features(port_host_stub_policy_test PRIVATE cxx_std_17)
  add_test(NAME port_host_stub_policy COMMAND port_host_stub_policy_test)
endif()

# The instruction set baseline for every target in this build, including native_animation below.
# Declared here (not in port/CMakeLists.txt) because native_animation is built before add_subdirectory
# (port) runs and links into melee_port_compat.exe same as everything else; without this it kept MSVC's
# default codegen regardless of MELEE_CPU_BASELINE, so a "compat" build could still fault on a
# pre-Haswell/pre-Ryzen CPU with an illegal instruction coming out of the animation/spline math that
# runs every frame, even though port/ itself was genuinely built at SSE2.
set(MELEE_CPU_BASELINE "AVX2" CACHE STRING "Instruction set baseline: AVX2, AVX or SSE2")
set_property(CACHE MELEE_CPU_BASELINE PROPERTY STRINGS AVX2 AVX SSE2)
if(MELEE_CPU_BASELINE STREQUAL "SSE2")
  set(MELEE_ARCH_FLAG "")          # the x64 default; naming it explicitly is an error in MSVC
else()
  set(MELEE_ARCH_FLAG "/arch:${MELEE_CPU_BASELINE}")
endif()

set(HOST_FOBJ "${CMAKE_CURRENT_BINARY_DIR}/generated/FObjHost.cpp")
add_custom_command(OUTPUT "${HOST_FOBJ}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_fobj_host.py" "${HOST_FOBJ}"
  DEPENDS tools/generate_fobj_host.py melee/src/sysdolphin/baselib/fobj.c
          melee/src/sysdolphin/baselib/fobj.h melee/src/sysdolphin/baselib/spline.c)
add_library(native_animation STATIC "${HOST_FOBJ}")
target_include_directories(native_animation PUBLIC native)
target_compile_features(native_animation PUBLIC cxx_std_14)
if(MSVC)
  target_compile_options(native_animation PRIVATE ${MELEE_ARCH_FLAG})
endif()
add_executable(packed_animation_test tests/packed_animation_test.cpp)
add_executable(native_practice_model_test
  tests/native_practice_model_test.cpp
  port/runtime/hle/native_practice_model.cpp)
target_include_directories(native_practice_model_test PRIVATE port/runtime/hle)
target_compile_features(native_practice_model_test PRIVATE cxx_std_17)
add_test(NAME native_practice_model COMMAND native_practice_model_test)
target_link_libraries(packed_animation_test PRIVATE native_animation)
add_test(NAME packed_animation COMMAND packed_animation_test)

# The Windows host in port/: melee_source.exe (the Source Port) and its tests. On by default with MSVC
# x64; the option keeps its old name so existing build folders and scripts still work.
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(melee_windows_host_default ON)
else()
  set(melee_windows_host_default OFF)
endif()
option(MELEE_BUILD_EXPERIMENTAL_PORT "Build the Windows host (port/): melee_source and its tests" ${melee_windows_host_default})
if(MELEE_BUILD_EXPERIMENTAL_PORT)
  if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Windows host (port/) requires Windows x64 and MSVC")
  endif()
  add_subdirectory(port)
endif()
add_executable(native_animation_test tests/native_animation_test.cpp)
target_include_directories(native_animation_test PRIVATE native)
target_compile_features(native_animation_test PRIVATE cxx_std_14)
add_test(NAME native_animation COMMAND native_animation_test)
if(WIN32)
  add_executable(native_viewer native/Viewer.cpp)
  target_link_libraries(native_viewer PRIVATE native_animation)
endif()
