
# Test labels and skips for every directory that registers tests (included once, from the top level).
#
#   quick  needs only the test build: no game run, no disc, each under about 10 s.
#   game   starts the built melee_source.exe or reads a real disc (MELEE_TEST_ISO).
#   slow   the rest: long programs, and programs built only on request (port_slippi_*).
# python tools/run_tests.py quick|game|slow|all runs one label in both build folders
# (docs/build-source-port.md, "Running the tests").
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(MU_GAME_TESTS source_card_roundtrip source_plugin_approval source_print_info source_replay_refusal
  source_match_digest source_p2p_pair)
# port_real_disc_vanilla benchmarks the mod scanner over every .iso in MU_REAL_DISC_DIR (skips without it).
set(MU_SLOW_TESTS port_native_savestate client_release port_cosmetic_mods port_gpu_resources
  port_real_disc_vanilla)
# These compile a fixture with GCC or Clang (MSVC cannot build the game headers they use). With no such
# compiler they skip and say so.
set(MU_FIXTURE_COMPILER_TESTS port_online_menu port_slippi_title port_css_teams_skins port_results_sjis_labels port_slippi_agent_lock
  port_slippi_fallback)
find_program(MU_FIXTURE_CXX NAMES g++ c++ clang++ HINTS "${MELEE_MINGW_ROOT}/bin" "C:/toolchains/mingw64/bin")
find_program(MU_FIXTURE_CC NAMES gcc cc clang HINTS "${MELEE_MINGW_ROOT}/bin" "C:/toolchains/mingw64/bin")

# A test program that is built only on request: a missing program is a skip, not a failure. The
# port_slippi_* tests are slow, never quick: a quick run has no skip that hides a missing program.
function(mu_built_test name target)
  add_test(NAME ${name} COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/port/tests/run_if_built.py"
    "$<TARGET_FILE:${target}>" ${target} ${ARGN})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  if(name MATCHES "^port_slippi_")
    set_property(TEST ${name} APPEND PROPERTY LABELS slow)
  endif()
endfunction()

function(mu_label_tests)
  # A static MinGW fixture runs without the toolchain's DLLs on PATH.
  set(static "")
  if(WIN32)
    set(static " -static")
  endif()
  get_property(tests DIRECTORY PROPERTY TESTS)
  foreach(test IN LISTS tests)
    get_property(labels TEST ${test} PROPERTY LABELS)
    if(test IN_LIST MU_GAME_TESTS)
      set_property(TEST ${test} APPEND PROPERTY LABELS game)
    elseif(test IN_LIST MU_SLOW_TESTS)
      set_property(TEST ${test} APPEND PROPERTY LABELS slow)
    elseif(NOT labels)
      set_property(TEST ${test} APPEND PROPERTY LABELS quick)
    endif()
    if(test IN_LIST MU_FIXTURE_COMPILER_TESTS)
      set_property(TEST ${test} PROPERTY SKIP_RETURN_CODE 77)
      if(MU_FIXTURE_CXX)
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "CXX=\"${MU_FIXTURE_CXX}\"${static}")
      endif()
      if(MU_FIXTURE_CC)
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "CC=\"${MU_FIXTURE_CC}\"${static}")
      endif()
    endif()
  endforeach()
endfunction()

# At the end of this directory and of port/ (after any early return()).
macro(mu_label_tests_at_end)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
    cmake_language(DEFER CALL mu_label_tests)
  endif()
endmacro()
mu_label_tests_at_end()
