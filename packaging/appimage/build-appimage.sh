#!/bin/sh
# Build a portable AppImage: dist/RedAlert2-Recomp-<ver>-x86_64.AppImage
#
#   ./packaging/appimage/build-appimage.sh            # version from CHANGELOG
#   ./packaging/appimage/build-appimage.sh 0.1.3
#
# Needs appimagetool (and git, for the source snapshot). Like the other
# packages this is installer-style: the game cannot be redistributed, so the
# AppImage carries the source plus the setup driver, and the 30-60 minute
# build happens on first run in ~/.local/share/redalert2-recomp (your own
# game copy is required). It deliberately bundles no toolchain: the host's
# gcc -m32, SDL2 and ffmpeg for i386 are used, which setup.sh checks for.
# Run from the repo root; uncommitted changes are NOT included.
set -e
cd "$(dirname "$0")/../.."

VER=${1:-$(sed -n 's/^## \[\([0-9][^]]*\)\].*/\1/p' CHANGELOG.md | head -1)}
OUT_DIR=${OUT_DIR:-dist}
APPDIR=$(mktemp -d)
trap 'rm -rf "$APPDIR"' EXIT

mkdir -p "$APPDIR/usr/share/redalert2-recomp" "$APPDIR/usr/bin"
git archive HEAD | tar -x -C "$APPDIR/usr/share/redalert2-recomp"
echo "$VER" > "$APPDIR/usr/share/redalert2-recomp/.pkg-version"

# Inside the AppImage the pristine tree lives next to AppRun; the per-user
# logic is the same as /usr/bin/redalert2-recomp-setup, with SRC relocated.
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
SRC=$HERE/usr/share/redalert2-recomp
DST=${XDG_DATA_HOME:-$HOME/.local/share}/redalert2-recomp
VER_FILE=.pkg-version
want=$(cat "$SRC/$VER_FILE" 2>/dev/null || echo unknown)
have=$(cat "$DST/$VER_FILE" 2>/dev/null || echo none)
if [ ! -f "$DST/setup.sh" ] || [ "$want" != "$have" ]; then
    echo "RedAlert2-Recomp: syncing source to $DST"
    mkdir -p "$DST"
    KEEP=$(mktemp -d)
    for p in game work src/recomp/gen src/recomp/gen_game .venv; do
        if [ -e "$DST/$p" ] || [ -L "$DST/$p" ]; then
            mkdir -p "$KEEP/$(dirname "$p")"
            mv "$DST/$p" "$KEEP/$p"
        fi
    done
    find "$DST" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    cp -a "$SRC/." "$DST/"
    for p in game work src/recomp/gen src/recomp/gen_game .venv; do
        if [ -e "$KEEP/$p" ] || [ -L "$KEEP/$p" ]; then
            mkdir -p "$DST/$(dirname "$p")"
            rm -rf "$DST/$p"
            mv "$KEEP/$p" "$DST/$p"
        fi
    done
    rm -rf "$KEEP"
fi
if [ ! -x "$DST/build-linux/ra2" ] && [ ! -x "$DST/build-linux-game/ra2" ]; then
    echo "First run: building needs your own copy of the game (game.exe, gamemd.exe)."
fi
exec "$DST/setup.sh" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

# `redalert2-recomp-play` beside it is shadowed by AppRun; ship a play entry
# point too for desktops that want straight-to-game.
cp packaging/common/redalert2-recomp-play "$APPDIR/usr/bin/"
cp packaging/common/redalert2-recomp.svg "$APPDIR/redalert2-recomp.svg"
cp packaging/common/yuris-revenge-recomp.desktop "$APPDIR/redalert2-recomp.desktop"

mkdir -p "$OUT_DIR"
ARCH=x86_64 appimagetool "$APPDIR" "$OUT_DIR/RedAlert2-Recomp-${VER}-x86_64.AppImage"
echo "wrote $OUT_DIR/RedAlert2-Recomp-${VER}-x86_64.AppImage"
