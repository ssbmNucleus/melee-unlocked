# These window-free diagnostics must run on Linux as well as macOS and Windows.
enable_testing()
foreach(case IN ITEMS session_logs net_diagnostics)
  add_executable(port_${case}_test "${CMAKE_CURRENT_LIST_DIR}/${case}_test.cpp")
  target_include_directories(port_${case}_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/host"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/include")
  target_compile_features(port_${case}_test PRIVATE cxx_std_17)
  add_test(NAME port_${case} COMMAND port_${case}_test)
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME port_online_menu COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/online_menu_test.py")
add_test(NAME port_results_sjis_labels COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/results_sjis_labels_test.py")
add_test(NAME port_slippi_title COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/slippi_title_test.py")
# X / Y on the online Teams select reaches the installed skins (mn/mncharsel.c mu_css_costume_press).
add_test(NAME port_css_teams_skins COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/css_teams_skins_test.py")
# Test switches read through host::test_env / mu_test_getenv only (a public build reads none of them).
add_test(NAME port_raw_env_reads COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/raw_env_reads_test.py")

enable_language(C)
add_executable(port_css_state_test "${CMAKE_CURRENT_LIST_DIR}/css_state_test.c"
  "${PROJECT_SOURCE_DIR}/sourceport/game/shim/mu_css_state.c")
target_include_directories(port_css_state_test PRIVATE "${PROJECT_SOURCE_DIR}/sourceport/game/include"
  "${PROJECT_SOURCE_DIR}/port/runtime/abi")
target_compile_definitions(port_css_state_test PRIVATE MU_NATIVE)
if(MSVC)
  # The shim names host CRT types with GCC builtins; MSVC's own spellings of the same types.
  target_compile_definitions(port_css_state_test PRIVATE "__SIZE_TYPE__=size_t" "__builtin_va_list=char*")
  target_link_libraries(port_css_state_test PRIVATE legacy_stdio_definitions)
endif()
target_compile_features(port_css_state_test PRIVATE c_std_11)
add_test(NAME port_css_state COMMAND port_css_state_test)
