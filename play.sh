#!/bin/sh
# Play a cross-built ra2.exe under CrossOver: the host runs in a bottle and
# finds the game through game/ (a link to that bottle's install, or a copy).
#
#   ./play.sh                       # Yuri's Revenge, build/ra2.exe --run
#   ./play.sh ra2                   # Red Alert 2, build-game/ra2.exe --run
#   ./play.sh yr --classic --debuglog
#
# CX_BOTTLE picks the bottle (default Steam); WINE another wine launcher.
cd "$(dirname "$0")"
case "$1" in
  ra2) exe=build-game/ra2.exe; shift ;;
  yr)  exe=build/ra2.exe; shift ;;
  *)   exe=build/ra2.exe ;;
esac
WINE=${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}
[ -f "$exe" ] || { echo "no $exe: run ./build.sh first" >&2; exit 1; }
[ $# -gt 0 ] || set -- --run
exec "$WINE" --bottle "${CX_BOTTLE:-Steam}" --workdir "$PWD" "$PWD/$exe" "$@"
