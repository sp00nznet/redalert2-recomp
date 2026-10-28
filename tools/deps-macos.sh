#!/bin/sh
# Builds the native macOS host's libraries from source, for a macOS older than
# the one Homebrew's bottles are built for: Homebrew's SDL, SDL_ttf and ffmpeg
# carry the build machine's macOS as their minimum, so an app packaged with
# them runs nowhere older. This makes the same libraries with a deployment
# target of macOS 11 (the first Apple Silicon release), in deps-macos/:
#
#   tools/deps-macos.sh                       # ~5 minutes; MACOSX_DEPLOYMENT_TARGET=12.0 for another floor
#   BUILD_DIR=build-macos-compat CMAKE_ARGS="-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_BUILD_RPATH=$PWD/deps-macos/lib" \
#     PKG_CONFIG_PATH=$PWD/deps-macos/lib/pkgconfig ./build-macos.sh
#   .venv/bin/python tools/package_macos.py --build-dir build-macos-compat
#
# ffmpeg is cut down to what the host decodes: Bink video and audio (the
# movies) and MP3 (the music, mss.c). Needs cmake, ninja and curl.
set -e
cd "$(dirname "$0")/.."
TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
PREFIX=$(pwd)/deps-macos
SRC=$PREFIX/src
JOBS=$(sysctl -n hw.ncpu)
export MACOSX_DEPLOYMENT_TARGET=$TARGET
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig    # these builds see only each other, not Homebrew
unset PKG_CONFIG_PATH
mkdir -p "$SRC" "$PREFIX/lib/pkgconfig"

fetch() {   # fetch NAME URL SHA256 [FALLBACK-URL FALLBACK-SHA256]: the source tree at $SRC/NAME, checked
  [ -f "$SRC/$1/.fetched" ] && return 0
  rm -rf "$SRC/$1"; mkdir -p "$SRC/$1"
  echo "==> fetching $1"
  if curl -fsSL -o "$SRC/$1.tar" "$2" && [ "$(shasum -a 256 "$SRC/$1.tar" | cut -d' ' -f1)" = "$3" ]; then :
  elif [ -n "$4" ] && curl -fsSL -o "$SRC/$1.tar" "$4" && [ "$(shasum -a 256 "$SRC/$1.tar" | cut -d' ' -f1)" = "$5" ]; then :
  else echo "$1: download failed or its SHA-256 is not the pinned one" >&2; rm -f "$SRC/$1.tar"; exit 1; fi
  tar xf "$SRC/$1.tar" -C "$SRC/$1" --strip-components=1
  rm -f "$SRC/$1.tar"
  touch "$SRC/$1/.fetched"
}
cmake_build() {   # cmake_build NAME [cmake options]
  name=$1; shift
  echo "==> building $name"
  cmake -S "$SRC/$name" -B "$SRC/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" -DCMAKE_INSTALL_RPATH="$PREFIX/lib" "$@" >"$SRC/$name/build.log" 2>&1
  cmake --build "$SRC/$name/build" -j"$JOBS" >>"$SRC/$name/build.log" 2>&1
  cmake --install "$SRC/$name/build" >>"$SRC/$name/build.log" 2>&1
}

fetch SDL3 https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
  9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
cmake_build SDL3 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF

fetch sdl2-compat https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz \
  ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588
cmake_build sdl2-compat -DSDL2COMPAT_TESTS=OFF -DSDL2COMPAT_STATIC=OFF
ln -sf sdl2-compat.pc "$PREFIX/lib/pkgconfig/sdl2.pc"     # the name pkg-config callers ask for (Homebrew links it too)

fetch freetype https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz \
  0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289 \
  https://downloads.sourceforge.net/freetype/freetype-2.13.3.tar.xz \
  0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
cmake_build freetype -DBUILD_SHARED_LIBS=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON

fetch SDL2_ttf https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz \
  0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd
cmake_build SDL2_ttf -DBUILD_SHARED_LIBS=ON -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF

fetch ffmpeg https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
  8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e \
  https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz \
  05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
if [ ! -f "$PREFIX/lib/pkgconfig/libavcodec.pc" ]; then
  echo "==> building ffmpeg (Bink and MP3 decoding only)"
  ( cd "$SRC/ffmpeg" && ./configure --prefix="$PREFIX" --enable-shared --disable-static \
      --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect --disable-x86asm \
      --disable-avdevice --disable-avfilter --disable-swscale \
      --enable-demuxer=bink,mp3 --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,mp3,mp3float \
      --enable-parser=mpegaudio --enable-protocol=file \
      --extra-cflags="-mmacosx-version-min=$TARGET" --extra-ldflags="-mmacosx-version-min=$TARGET" \
      >build.log 2>&1 && make -j"$JOBS" >>build.log 2>&1 && make install >>build.log 2>&1 )
fi

echo "==> done: $PREFIX"
for f in "$PREFIX"/lib/libSDL3.0.dylib "$PREFIX"/lib/libSDL2-2.0.0.dylib "$PREFIX"/lib/libSDL2_ttf-2.0.0.dylib \
         "$PREFIX"/lib/libfreetype.6.dylib "$PREFIX"/lib/libavcodec.*.dylib; do
  printf '  %s: %s\n' "$(basename "$f")" "$(otool -l "$f" | grep -m1 minos | sed 's/ *minos //')"
done
