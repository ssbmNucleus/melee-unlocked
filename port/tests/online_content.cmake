# Custom fighters online (docs/online-custom-content.md): the wire message (header-only).
# Included from port/CMakeLists.txt; paths are relative to port/.

add_executable(port_mu_content_wire_test tests/mu_content_wire_test.cpp)
target_include_directories(port_mu_content_wire_test PRIVATE runtime/hle)
target_compile_features(port_mu_content_wire_test PRIVATE cxx_std_17)
target_compile_options(port_mu_content_wire_test PRIVATE /EHsc)
add_test(NAME port_mu_content_wire COMMAND port_mu_content_wire_test)
