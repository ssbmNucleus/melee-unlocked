# .slpz replays (replay_slpz.cpp: Zstandard both ways) for the game's --replay and replay
# compression. Included by the Windows and Linux builds; melee_source links port_slpz.
# The Dolphin RVZ/WIA disc reader that used to be built here as well now lives, unbuilt, in
# port/extras/disc_rvz (see port/extras/README.md).
if(NOT TARGET port_slpz)
  enable_language(C)
  set(SLPZ_APP "${CMAKE_CURRENT_LIST_DIR}")
  set(SLPZ_TP "${CMAKE_CURRENT_LIST_DIR}/../third_party")
  file(GLOB SLPZ_ZSTD "${SLPZ_TP}/zstd/common/*.c" "${SLPZ_TP}/zstd/decompress/*.c" "${SLPZ_TP}/zstd/compress/*.c")
  add_library(port_slpz STATIC "${SLPZ_APP}/replay_slpz.cpp" ${SLPZ_ZSTD})
  target_include_directories(port_slpz PUBLIC "${SLPZ_APP}" PRIVATE "${SLPZ_TP}")
  target_compile_features(port_slpz PRIVATE cxx_std_17)
  # No assembly (zstd's Huffman fast path is a .S file), zstd's xxhash kept private.
  target_compile_definitions(port_slpz PRIVATE ZSTD_DISABLE_ASM=1 XXH_NAMESPACE=ZSTD_ _CRT_SECURE_NO_WARNINGS NOMINMAX)
  if(MSVC)
    target_compile_options(port_slpz PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc> $<$<COMPILE_LANGUAGE:C>:/W1>)
  else()
    set_property(TARGET port_slpz PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_compile_options(port_slpz PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)
  endif()
endif()
