#!/bin/sh
# Configure and build the 32-bit Windows host on macOS (or Linux): clang-cl and
# lld-link against xwin's MSVC CRT and Windows SDK (cmake/clang-cl-x86.cmake).
# The same ra2.exe build.cmd makes, to run under CrossOver or Wine (play.sh).
#
#   ./build.sh                                          # Yuri's Revenge -> build/ra2.exe
#   BUILD_DIR=build-game CMAKE_ARGS=-DRA2_TARGET=game ./build.sh   # Red Alert 2
#
# Needs: brew install llvm lld cmake ninja xwin, and once,
#        xwin --accept-license --arch x86 splat --output ~/.xwin
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
# The toolkit: PCRECOMP, else ../tools (the Windows layout), else ../pcrecomp.
if [ -z "$PCRECOMP" ]; then
  if [ -d ../tools/runtime/native32 ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
