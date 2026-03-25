#!/bin/bash
# Build Baudline for WebAssembly
# Prerequisites: Emscripten SDK installed at ~/emsdk
set -e

EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
source "$EMSDK_DIR/emsdk_env.sh" 2>/dev/null

echo "=== Configuring WASM build ==="
emcmake cmake -B build_wasm -C CMakeLists_wasm.cmake -Wno-dev

echo "=== Building ==="
cmake --build build_wasm -j$(nproc)

echo "=== Done ==="
echo "Output: build_wasm/web/baudline.html"
echo "To test: cd build_wasm/web && python3 -m http.server 8080"
echo "Then open http://localhost:8080/baudline.html"
