#!/bin/sh
# Build a Debian/Ubuntu package: redalert2-recomp_<ver>_amd64.deb
#
#   ./packaging/deb/build-deb.sh            # version from CHANGELOG
#   ./packaging/deb/build-deb.sh 0.1.3      # explicit version
#   OUT_DIR=dist ./packaging/deb/build-deb.sh
#
# Needs only dpkg-deb and git. The .deb is installer-style: it ships the
# source plus setup/play wrappers, and the 30-60 minute build happens on the
# user's machine (their own game copy is required). Run from the repo root;
# uncommitted changes are NOT included (it packs `git archive HEAD`).
set -e
cd "$(dirname "$0")/../.."

VER=${1:-$(sed -n 's/^## \[\([0-9][^]]*\)\].*/\1/p' CHANGELOG.md | head -1)}
OUT_DIR=${OUT_DIR:-dist}
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

PKG=$STAGE/redalert2-recomp
mkdir -p "$PKG/DEBIAN" \
         "$PKG/usr/share/redalert2-recomp" \
         "$PKG/usr/bin" \
         "$PKG/usr/share/applications" \
         "$PKG/usr/share/icons/hicolor/scalable/apps" \
         "$PKG/usr/share/man/man1" \
         "$PKG/usr/share/doc/redalert2-recomp"

git archive HEAD | tar -x -C "$PKG/usr/share/redalert2-recomp"
echo "$VER" > "$PKG/usr/share/redalert2-recomp/.pkg-version"

install -m755 packaging/common/redalert2-recomp-setup "$PKG/usr/bin/"
install -m755 packaging/common/redalert2-recomp-play "$PKG/usr/bin/"
install -m644 packaging/common/yuris-revenge-recomp.desktop \
               packaging/common/red-alert-2-recomp.desktop \
               "$PKG/usr/share/applications/"
install -m644 packaging/common/redalert2-recomp.svg \
               "$PKG/usr/share/icons/hicolor/scalable/apps/"
gzip -9 -n -c packaging/common/redalert2-recomp-setup.1 > "$PKG/usr/share/man/man1/redalert2-recomp-setup.1.gz"
gzip -9 -n -c packaging/common/redalert2-recomp-play.1 > "$PKG/usr/share/man/man1/redalert2-recomp-play.1.gz"

# Debian copyright file (MIT for our own code; the games stay EA's).
sed "s/@VER@/$VER/" packaging/deb/copyright.in > "$PKG/usr/share/doc/redalert2-recomp/copyright"
cp CHANGELOG.md "$PKG/usr/share/doc/redalert2-recomp/changelog"
gzip -9 -n "$PKG/usr/share/doc/redalert2-recomp/changelog"

cat > "$PKG/DEBIAN/control" <<EOF
Package: redalert2-recomp
Version: $VER
Section: games
Priority: optional
Architecture: amd64
Maintainer: 9M2PJU <https://github.com/9M2PJU>
Depends: gcc, gcc-multilib, cmake, ninja-build, pkg-config, python3, python3-venv, git, fonts-liberation, libsdl2-dev:i386, libsdl2-ttf-dev:i386, libavformat-dev:i386, libavcodec-dev:i386, libswresample-dev:i386, libavutil-dev:i386
Recommends: steam
Description: Static recompilation of Red Alert 2 and Yuri's Revenge
 The native Linux pipeline for Command & Conquer: Red Alert 2 and Yuri's
 Revenge, statically recompiled to 32-bit Linux through pcrecomp.
 .
 This package ships the source and the setup driver; the build (30-60
 minutes, ~6 GB, once) runs on your machine because it needs your own copy
 of the game (game.exe and gamemd.exe). Run redalert2-recomp-setup after
 installing. Enable i386 first: dpkg --add-architecture i386.
EOF

cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
echo "redalert2-recomp: enable 32-bit first if you have not:"
echo "  sudo dpkg --add-architecture i386 && sudo apt update"
echo "then build the games (needs your own copy of the game):"
echo "  redalert2-recomp-setup"
EOF
chmod 755 "$PKG/DEBIAN/postinst"

mkdir -p "$OUT_DIR"
dpkg-deb --build "$PKG" "$OUT_DIR/redalert2-recomp_${VER}_amd64.deb"
echo "wrote $OUT_DIR/redalert2-recomp_${VER}_amd64.deb"
