#!/bin/sh
# Configure and build the native macOS host: the lifted C on pcrecomp's
# runtime/win32hle, a 64-bit Mac program (arm64 or x86-64) with no Wine and no
# CrossOver (src/linux, which is the POSIX host for Linux and macOS alike).
#
#   ./build-macos.sh            # build-macos/ra2: Yuri's Revenge
#   BUILD_DIR=build-macos-game CMAKE_ARGS=-DRA2_TARGET=game ./build-macos.sh   # Red Alert 2
#   build-macos/ra2 --run       # from here, with the game in ./game
#
# Needs a lift (run_lift.py), Xcode's command line tools, and from Homebrew:
# brew install cmake ninja pkgconf sdl2 sdl2_ttf ffmpeg
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-macos}
# The toolkit: PCRECOMP, else ../tools (the Windows layout), else ../pcrecomp.
if [ -z "$PCRECOMP" ]; then
  if [ -d ../tools/runtime/win32hle ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
