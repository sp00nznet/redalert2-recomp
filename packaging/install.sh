#!/bin/bash
# One-line install of redalert2-recomp (no root-owned source, no release
# hunting): installs the distro's build dependencies, clones the tree to
# ~/.local/share/redalert2-recomp, and drops setup/play wrappers plus menu
# entries into ~/.local. Then runs the setup (needs your own game copy).
#
#   curl -fsSL https://raw.githubusercontent.com/sp00nznet/redalert2-recomp/main/packaging/install.sh | bash
#   curl -fsSL .../install.sh | bash -s -- --yes --game-dir /path/to/game
#   curl -fsSL .../install.sh | bash -s -- --no-setup   # deps + tree only
#
set -e
REPO=https://github.com/sp00nznet/redalert2-recomp.git
DST=${XDG_DATA_HOME:-$HOME/.local/share}/redalert2-recomp
BIN=$HOME/.local/bin
APPLICATIONS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
ICONS=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps
YES=0
SETUP=1
GAME_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --yes) YES=1 ;;
        --no-setup) SETUP=0 ;;
        --game-dir) GAME_ARG=$2; shift ;;
        --game-dir=*) GAME_ARG=${1#--game-dir=} ;;
        *) echo "usage: install.sh [--yes] [--no-setup] [--game-dir DIR]" >&2; exit 2 ;;
    esac
    shift
done
SUDO=""; [ "$(id -u)" != 0 ] && SUDO=sudo

say() { printf '%s\n' "$*"; }
ask() { [ "$YES" = 1 ] && return 0; read -r -p "$1 [Y/n] " r; [[ ! $r =~ ^[nN] ]]; }

. /etc/os-release 2>/dev/null || { ID=unknown; ID_LIKE=""; }
case "$ID $ID_LIKE" in
    *arch*|*cachyos*|*endeavour*|*manjaro*)
        say "==> Arch-based: enabling [multilib] if needed, installing dependencies"
        if ! grep -q '^\[multilib\]' /etc/pacman.conf; then
            say "  [multilib] is commented out in /etc/pacman.conf (needed for lib32 packages)."
            if ask "  Enable it?"; then
                $SUDO sed -i '/^#\[multilib\]/,/^#Include/ s/^#//' /etc/pacman.conf
                $SUDO pacman -Sy
            else
                say "  Without multilib the 32-bit SDL2/ffmpeg cannot install; setup will tell you." >&2
            fi
        fi
        $SUDO pacman -S --needed gcc lib32-gcc-libs lib32-glibc cmake ninja \
            pkgconf python git lib32-sdl2-compat lib32-sdl2_ttf ttf-liberation
        ;;
    *debian*|*ubuntu*|*linuxmint*|*pop*)
        say "==> Debian-based: enabling i386 if needed, installing dependencies"
        if ! dpkg --print-foreign-architectures | grep -q i386; then
            $SUDO dpkg --add-architecture i386 && $SUDO apt update
        fi
        $SUDO apt update
        $SUDO apt install -y gcc-multilib cmake ninja-build pkg-config python3 \
            python3-venv git fonts-liberation libsdl2-dev:i386 libsdl2-ttf-dev:i386 \
            libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386 libavutil-dev:i386
        ;;
    *fedora*|*rhel*|*centos*)
        say "==> Fedora-based: installing dependencies"
        $SUDO dnf install -y gcc glibc-devel.i686 libgcc.i686 libatomic.i686 \
            cmake ninja-build pkgconf python3 git liberation-sans-fonts \
            sdl2-compat-devel.i686 SDL2_ttf-devel.i686 \
            libavformat-free-devel.i686 libavcodec-free-devel.i686 \
            libswresample-free-devel.i686 libavutil-free-devel.i686
        ;;
    *)
        say "==> Unknown distro ($ID): skipping dependency install."
        say "    See setup.sh / docs/building.md for the package list, then rerun."
        ;;
esac

if [ ! -d "$DST/.git" ]; then
    say "==> Cloning into $DST"
    mkdir -p "$(dirname "$DST")"
    git clone --depth 1 "$REPO" "$DST"
else
    say "==> Updating $DST"
    git -C "$DST" pull -q --ff-only || say "    (keeping local changes; continuing)"
fi
echo "0.1.3+git" > "$DST/.pkg-version" 2>/dev/null || true

mkdir -p "$BIN" "$APPLICATIONS" "$ICONS"
# The user-level tree IS the per-user copy: thin wrappers exec it directly.
cat > "$BIN/redalert2-recomp-setup" <<EOF
#!/bin/sh
# User-level install driver (see packaging/install.sh).
exec "$DST/setup.sh" "\$@"
EOF
cat > "$BIN/redalert2-recomp-play" <<EOF
#!/bin/sh
# User-level play driver (see packaging/install.sh).
DST="$DST"
case "\${1:-yr}" in
    ra2) g=ra2; shift ;;
    yr)  shift ;;
    -*)  g=yr ;;
    *) echo "usage: \$(basename "\$0") [yr|ra2] [args...]" >&2; exit 2 ;;
esac
if [ "\$g" = ra2 ]; then exe=build-linux-game/ra2; else exe=build-linux/ra2; fi
if [ ! -x "\$DST/\$exe" ]; then
    echo "redalert2-recomp-play: no built \$g here yet." >&2
    echo "Run: redalert2-recomp-setup \$g   (needs your own copy of the game)" >&2
    exit 1
fi
cd "\$DST" && exec "\$exe" --run "\$@"
EOF
chmod +x "$BIN/redalert2-recomp-setup" "$BIN/redalert2-recomp-play"
sed 's#Exec=redalert2-recomp-play#Exec='"$BIN"'/redalert2-recomp-play#' \
    "$DST/packaging/common/yuris-revenge-recomp.desktop" > "$APPLICATIONS/yuris-revenge-recomp.desktop"
sed 's#Exec=redalert2-recomp-play#Exec='"$BIN"'/redalert2-recomp-play#' \
    "$DST/packaging/common/red-alert-2-recomp.desktop" > "$APPLICATIONS/red-alert-2-recomp.desktop"
cp "$DST/packaging/common/redalert2-recomp.svg" "$ICONS/"
update-desktop-database "$APPLICATIONS" 2>/dev/null || true

say "==> Installed wrappers in $BIN (add it to PATH if needed)."
case ":$PATH:" in *":$BIN:"*) ;; *) say "    export PATH=\"\$HOME/.local/bin:\$PATH\""; ;; esac

if [ "$SETUP" = 1 ]; then
    args=""; [ "$YES" = 1 ] && args="$args --yes"
    [ -n "$GAME_ARG" ] && args="$args --game-dir $GAME_ARG"
    # shellcheck disable=SC2086
    exec "$DST/setup.sh" $args
fi
say "Done. Run: redalert2-recomp-setup   (needs your own copy of the game)"
