#!/bin/sh
# Configure and build the web host: the lifted C on pcrecomp's runtime/win32hle,
# compiled to WebAssembly with Emscripten; the game runs in a browser tab.
#
#   ./build-web.sh                 # build-web/ra2.js + ra2.wasm: Yuri's Revenge
#   BUILD_DIR=build-web-game CMAKE_ARGS=-DRA2_TARGET=game ./build-web.sh   # Red Alert 2
#   python3 tools/serve_web.py     # then open http://localhost:8000/
#
# Needs a lift (run_lift.py), cmake, ninja, curl and Emscripten (brew install
# emscripten; or an activated emsdk).
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-web}
if [ -z "$PCRECOMP" ]; then
  if [ ! -d ../pcrecomp ] && [ -d ../tools/runtime/win32hle ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
command -v emcmake >/dev/null 2>&1 || { echo "emcmake not found: install Emscripten (brew install emscripten, or source emsdk_env.sh)" >&2; exit 1; }
tools/web_deps.sh                      # the fonts and ffmpeg.wasm's core, beside the page
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  emcmake cmake -S . -B "$BUILD_DIR" -G Ninja -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
