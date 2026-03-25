# Emscripten/WASM build for Baudmine
# Usage:
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -B build_wasm -C CMakeLists_wasm.cmake
#   cmake --build build_wasm
#
# This file is loaded via cmake -C (initial cache).

set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(BUILD_WASM ON CACHE BOOL "")
