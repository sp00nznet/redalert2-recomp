#!/bin/sh
# What the web page needs beside it and does not keep in the repository:
# the Liberation fonts (SIL Open Font License; they stand in for Arial, Times
# and Courier) into web/fonts, and ffmpeg.wasm's core (@ffmpeg/core 0.10.0,
# the FFmpeg build with the Bink decoders that transcodes the movies) into
# web/ffmpeg. build-web.sh runs this; a run with everything there does nothing.
set -e
cd "$(dirname "$0")/.."
FONTS=https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz
CORE=https://unpkg.com/@ffmpeg/core@0.10.0/dist
if [ ! -f web/fonts/LiberationSans-Regular.ttf ]; then
  echo "fetching the Liberation fonts"
  mkdir -p web/fonts
  curl -sSL "$FONTS" | tar xz -C web/fonts --strip-components=1 \
    --include='*/LiberationSans-*.ttf' --include='*/LiberationSerif-*.ttf' --include='*/LiberationMono-*.ttf' --include='*/LICENSE' 2>/dev/null \
  || curl -sSL "$FONTS" | tar xz -C web/fonts --strip-components=1 --wildcards '*/Liberation*.ttf' '*/LICENSE'
fi
for f in ffmpeg-core.js ffmpeg-core.wasm ffmpeg-core.worker.js; do
  if [ ! -f "web/ffmpeg/$f" ]; then
    echo "fetching $f"
    mkdir -p web/ffmpeg
    curl -sSL -o "web/ffmpeg/$f" "$CORE/$f"
  fi
done
