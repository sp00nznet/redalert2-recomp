#!/bin/sh
# Configure and build the native Linux host: the lifted C on pcrecomp's
# runtime/win32hle, a 32-bit Linux program with no Wine (src/linux,
# cmake/linux-i386.cmake).
#
#   ./build-linux.sh            # build-linux/ra2: Yuri's Revenge
#   BUILD_DIR=build-linux-game CMAKE_ARGS=-DRA2_TARGET=game ./build-linux.sh   # Red Alert 2
#   build-linux/ra2 --run       # from here, with the game in ./game
#
# Needs a lift (run_lift.py), gcc with -m32 (gcc-multilib), cmake, ninja,
# and SDL2, SDL2_ttf and ffmpeg's libraries for i386; setup.sh lists the
# packages.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-linux}
# The toolkit: PCRECOMP, else ../tools (the Windows layout), else ../pcrecomp.
if [ -z "$PCRECOMP" ]; then
  if [ -d ../tools/runtime/win32hle ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
