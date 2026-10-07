#!/bin/sh
# Play a cross-built ra2.exe under Wine: CrossOver on a Mac, wine on Linux.
# The host finds the game through game/ (a link to the install, or a copy).
#
#   ./play.sh                       # Yuri's Revenge, build/ra2.exe --run
#   ./play.sh ra2                   # Red Alert 2, build-game/ra2.exe --run
#   ./play.sh yr --classic --debuglog
#
# On a Mac CX_BOTTLE picks the bottle (default Steam); WINE another wine
# launcher, anywhere.
cd "$(dirname "$0")"
case "$1" in
  ra2) exe=build-game/ra2.exe; shift ;;
  yr)  exe=build/ra2.exe; shift ;;
  *)   exe=build/ra2.exe ;;
esac
[ -f "$exe" ] || { echo "no $exe: run ./build.sh first" >&2; exit 1; }
[ $# -gt 0 ] || set -- --run
# The game folder's wsock32.dll is IPXEmu, the network games' IPX; Wine's own has none.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-wsock32=n,b}"
if [ "$(uname)" = Darwin ]; then
  WINE=${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}
  exec "$WINE" --bottle "${CX_BOTTLE:-Steam}" --workdir "$PWD" "$PWD/$exe" "$@"
fi
exec "${WINE:-wine}" "$PWD/$exe" "$@"
