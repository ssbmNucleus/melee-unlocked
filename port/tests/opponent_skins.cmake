# Opponents' skins online (host/costume_served_form.h, host/skin_fetch.h, host/opponent_skins.h,
# hle/mu_skin_wire.h): unit tests, and the corpus tool (not a test: it needs the retail disc and a
# folder of skins, tools/skin_corpus.py drives it).
# Included from port/CMakeLists.txt; paths are relative to port/.

add_executable(port_costume_served_form_test tests/costume_served_form_test.cpp runtime/host/costume_served_form.cpp
  ${MU_COSMETIC_SOURCES} runtime/host/archive_unpack.cpp runtime/host/http_client.cpp)
target_include_directories(port_costume_served_form_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_costume_served_form_test PRIVATE cxx_std_17)
target_compile_options(port_costume_served_form_test PRIVATE /EHsc)
target_compile_definitions(port_costume_served_form_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_costume_served_form_test PRIVATE bcrypt comdlg32 user32)
add_test(NAME port_costume_served_form COMMAND port_costume_served_form_test)

add_executable(port_skin_corpus tests/skin_corpus.cpp runtime/host/costume_served_form.cpp
  ${MU_COSMETIC_SOURCES} runtime/host/archive_unpack.cpp runtime/host/http_client.cpp)
target_include_directories(port_skin_corpus PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_skin_corpus PRIVATE cxx_std_17)
target_compile_options(port_skin_corpus PRIVATE /EHsc)
target_compile_definitions(port_skin_corpus PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_skin_corpus PRIVATE bcrypt comdlg32 user32)

# Fetch by id and hash through the mod browser's real GET (http_client.h), against a stand-in API the test
# serves itself on 127.0.0.1 (ports 42050-42089).
add_executable(port_skin_fetch_test tests/skin_fetch_test.cpp runtime/host/skin_fetch.cpp runtime/host/mod_browser.cpp
  runtime/host/roster.cpp ${MU_COSMETIC_SOURCES} runtime/host/archive_unpack.cpp runtime/host/http_client.cpp)
target_include_directories(port_skin_fetch_test PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx third_party)
target_compile_features(port_skin_fetch_test PRIVATE cxx_std_17)
target_compile_options(port_skin_fetch_test PRIVATE /EHsc)
target_compile_definitions(port_skin_fetch_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(port_skin_fetch_test PRIVATE winhttp ws2_32 bcrypt comdlg32 user32)
add_test(NAME port_skin_fetch COMMAND port_skin_fetch_test)
set_tests_properties(port_skin_fetch PROPERTIES SKIP_RETURN_CODE 77)   # a public build: no stand-in
# The view marker on the card's own download route, the 400 retry and installs: a process of its own,
# so its API requests do not pass the mod browser's 30 a minute with the cases above.
add_test(NAME port_skin_fetch_view COMMAND port_skin_fetch_test --view)
set_tests_properties(port_skin_fetch_view PROPERTIES SKIP_RETURN_CODE 77)   # a public build: no stand-in

# The receiving machine's Teams color (host/opponent_skins.h costume_of_slot, recolor_form): an
# opponent's skin fetched for the color picked, served in the color played. Synthetic files, no disc.
add_executable(port_opponent_skins_recolor_test tests/opponent_skins_recolor_test.cpp app/source_guest_boundary.cpp)
target_link_libraries(port_opponent_skins_recolor_test PRIVATE runtime)
target_include_directories(port_opponent_skins_recolor_test PRIVATE runtime/host runtime/hle)
target_compile_options(port_opponent_skins_recolor_test PRIVATE /EHsc /utf-8)
target_compile_definitions(port_opponent_skins_recolor_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
mu_built_test(port_opponent_skins_recolor port_opponent_skins_recolor_test)

# The wire message (header-only).
add_executable(port_mu_skin_wire_test tests/mu_skin_wire_test.cpp)
target_include_directories(port_mu_skin_wire_test PRIVATE runtime/hle)
target_compile_features(port_mu_skin_wire_test PRIVATE cxx_std_17)
target_compile_options(port_mu_skin_wire_test PRIVATE /EHsc)
add_test(NAME port_mu_skin_wire COMMAND port_mu_skin_wire_test)
