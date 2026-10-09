#!/bin/bash
# macOS and Linux setup: the same pipeline as Setup.cmd. On Linux each game is
# built as a native program (build-linux.sh: no Wine); on a Mac, or on Linux
# with --wine, the Windows exe is cross-compiled here and played under Wine
# (CrossOver on a Mac). On a Mac Red Alert 2 has to be installed in a
# CrossOver bottle (Steam for Windows in a bottle, then the game from your
# library); on Linux, from Steam (it runs it with Proton) or anywhere else.
#
#   ./setup.sh                 # both games
#   ./setup.sh yr              # only Yuri's Revenge (or: ra2)
#   ./setup.sh --wine          # on Linux: the Windows exes, under Wine
#   ./setup.sh --force         # redo every step
#
# It links game/ to the install (nothing is copied), builds the function
# catalog, lifts and builds each game, and leaves a "<game> (recomp)" launcher
# here (.command on a Mac, .sh on Linux). A rerun skips finished steps.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
FORCE=0
NATIVE=1
GAMES="yr ra2"
for a in "$@"; do
  case "$a" in
    --force) FORCE=1 ;;
    --wine) NATIVE=0 ;;
    yr|ra2) GAMES=$a ;;
    *) echo "usage: ./setup.sh [yr|ra2] [--force] [--wine]" >&2; exit 2 ;;
  esac
done
MAC=0; [ "$(uname)" = Darwin ] && MAC=1 && NATIVE=0

say()  { printf '%s\n' "$*"; }
step() { printf '\n\033[36m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; exit 1; }
ask()  { read -r -p "$1 [Y/n] " r; [[ ! $r =~ ^[nN] ]]; }
done_already() { [ "$FORCE" = 0 ] && [ -e "$1" ]; }
# A program on PATH or, as Debian and Ubuntu keep clang-cl, in /usr/lib/llvm-*/bin.
have() { command -v "$1" >/dev/null || ls /usr/lib/llvm-*/bin/"$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------- tools
step "Checking the tools"
if [ "$MAC" = 1 ]; then
  command -v brew >/dev/null || fail "Homebrew is needed: https://brew.sh"
  need=()
  for f in llvm lld cmake ninja xwin uv; do brew list --formula "$f" >/dev/null 2>&1 || need+=("$f"); done
  if [ ${#need[@]} -gt 0 ]; then
    ask "  Install ${need[*]} with Homebrew?" || fail "${need[*]} are required."
    brew install "${need[@]}"
  fi
elif [ "$NATIVE" = 1 ]; then
  # gcc that builds 32-bit; SDL2, SDL2_ttf and ffmpeg (the Bink movies) for i386
  need=()
  for t in gcc cmake ninja python3 pkg-config; do have "$t" || need+=("$t"); done
  printf 'int main(void){return 0;}' > /tmp/m32.c
  gcc -m32 /tmp/m32.c -o /tmp/m32 2>/dev/null || need+=("gcc -m32")
  # the 32-bit .pc files: Debian and Ubuntu, Arch (lib32), Fedora (/usr/lib where
  # /usr/lib64 is a directory of its own; on Arch it is /usr/lib, 64-bit)
  pcdirs="/usr/lib/i386-linux-gnu/pkgconfig /usr/lib32/pkgconfig"
  [ -d /usr/lib64 ] && [ ! -L /usr/lib64 ] && pcdirs="$pcdirs /usr/lib/pkgconfig"
  for pc in sdl2 SDL2_ttf libavformat libavcodec libswresample libavutil; do
    # where pkg-config finds it (by name or by a package's Provides, as Fedora's
    # sdl2-compat provides sdl2), and only a 32-bit directory counts
    at=$(PKG_CONFIG_PATH=${pcdirs// /:} pkg-config --path "$pc" 2>/dev/null)
    found=0; for d in $pcdirs; do [ "${at%/*}" = "$d" ] && found=1; done
    [ $found = 1 ] || { need+=("SDL2, SDL2_ttf and ffmpeg's libraries for i386"); break; }
  done
  if [ ${#need[@]} -gt 0 ]; then
    say "  Missing: ${need[*]}. From your package manager, for example:"
    say "    Debian, Ubuntu: sudo dpkg --add-architecture i386 && sudo apt update"
    say "                    sudo apt install gcc-multilib cmake ninja-build pkg-config python3-venv fonts-liberation \\"
    say "                                     libsdl2-dev:i386 libsdl2-ttf-dev:i386 libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386 libavutil-dev:i386"
    say "    Fedora:         sudo dnf install gcc glibc-devel.i686 libgcc.i686 libatomic.i686 cmake ninja-build pkgconf python3 liberation-sans-fonts \\"
    say "                                     sdl2-compat-devel.i686 SDL2_ttf-devel.i686 \\"
    say "                                     libavformat-free-devel.i686 libavcodec-free-devel.i686 libswresample-free-devel.i686 libavutil-free-devel.i686"
    say "    Arch:           sudo pacman -S gcc cmake ninja python lib32-sdl2 lib32-sdl2_ttf lib32-ffmpeg ttf-liberation (multilib)"
    say "  Or ./setup.sh --wine to play the Windows builds under Wine instead."
    fail "install them, then run ./setup.sh again."
  fi
else
  need=()
  for t in clang-cl lld-link llvm-lib cmake ninja wine python3; do have "$t" || need+=("$t"); done
  if [ ${#need[@]} -gt 0 ]; then
    say "  Missing: ${need[*]}. From your package manager, for example:"
    say "    Debian, Ubuntu: sudo apt install clang clang-tools lld llvm cmake ninja-build wine python3-venv"
    say "                    (and for 32-bit Wine: sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine32:i386)"
    say "    Fedora:         sudo dnf install clang lld llvm cmake ninja-build wine python3"
    say "    Arch:           sudo pacman -S clang lld llvm cmake ninja wine python"
    fail "install them, then run ./setup.sh again."
  fi
  if ! command -v xwin >/dev/null && [ ! -x "$HOME/.local/bin/xwin" ]; then
    say "  xwin (https://github.com/Jake-Shadle/xwin) fetches the MSVC C runtime and Windows SDK."
    ask "  Download its release binary into ~/.local/bin?" || fail "xwin is required (or: cargo install xwin)."
    mkdir -p "$HOME/.local/bin"
    v=0.6.6
    curl -sSL "https://github.com/Jake-Shadle/xwin/releases/download/$v/xwin-$v-x86_64-unknown-linux-musl.tar.gz" |
      tar -xz -C "$HOME/.local/bin" --strip-components=1 "xwin-$v-x86_64-unknown-linux-musl/xwin"
  fi
  PATH=$PATH:$HOME/.local/bin
fi

XWIN_DIR=${XWIN_DIR:-$HOME/.xwin}
if [ "$NATIVE" = 0 ] && [ ! -d "$XWIN_DIR/crt/lib/x86" ]; then
  say "  The x86 MSVC C runtime and Windows SDK are needed to build a Windows exe (about 1 GB)."
  say "  xwin downloads them from Microsoft, under Microsoft's licence:"
  say "  https://go.microsoft.com/fwlink/?LinkId=2086102"
  ask "  Accept that licence and download them into $XWIN_DIR?" || fail "the CRT and SDK are required."
  # The cache beside the output: --temp unpacks in /tmp, and moving out of
  # it fails where /tmp is another filesystem (tmpfs on most Linux systems).
  xwin --accept-license --cache-dir "$XWIN_DIR.cache" --arch x86 splat --output "$XWIN_DIR"
  rm -rf "$XWIN_DIR.cache"
fi
export XWIN_DIR

if python3 -c 'import pefile, capstone' 2>/dev/null; then
  PY=$(command -v python3)
else
  if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c 'import pefile, capstone' 2>/dev/null; then
    if command -v uv >/dev/null; then
      uv venv .venv && uv pip install --python .venv/bin/python pefile capstone
    else
      python3 -m venv .venv && .venv/bin/pip install -q pefile capstone
    fi
  fi
  PY=$ROOT/.venv/bin/python
fi

# The toolkit: PCRECOMP, else ../tools (the Windows layout), else ../pcrecomp.
if [ -z "$PCRECOMP" ]; then
  if [ -d ../tools/runtime/native32 ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -d "$PCRECOMP/runtime/native32" ]; then
  ask "  Clone the pcrecomp toolkit into $PCRECOMP?" || fail "pcrecomp is required."
  git clone https://github.com/sp00nznet/pcrecomp "$PCRECOMP"
fi
# Under Wine, callbacks into lifted code need DEP turned on and a fetch that
# Wine reports as a read accepted (runtime/native32/native32.c). The native
# host is the lifted game on win32hle's DirectDraw, DirectSound, Bink and windows.
# Until pcrecomp #62 is merged its main branch has neither: a clone of it is
# offered the pull request's branch.
WIN32HLE_BRANCH=feat/win32hle-ts
if [ "$NATIVE" = 1 ]; then
  if [ ! -f "$PCRECOMP/runtime/win32hle/bink.c" ] && [ -d "$PCRECOMP/.git" ] &&
     git -C "$PCRECOMP" ls-remote --exit-code origin "$WIN32HLE_BRANCH" >/dev/null 2>&1; then
    say "  The native build needs win32hle's DirectDraw and Bink, which are on pcrecomp's"
    say "  $WIN32HLE_BRANCH branch (pull request #62) and not yet on its main branch."
    if ask "  Check that branch out in $PCRECOMP?"; then
      git -C "$PCRECOMP" fetch -q origin "$WIN32HLE_BRANCH" &&
        git -C "$PCRECOMP" checkout -q FETCH_HEAD ||
        fail "could not check out $WIN32HLE_BRANCH in $PCRECOMP (local changes?)."
    fi
  fi
  [ -f "$PCRECOMP/runtime/win32hle/bink.c" ] ||
    fail "$PCRECOMP predates win32hle's DirectDraw and Bink (pcrecomp #62): update it, or ./setup.sh --wine."
else
  grep -q SetProcessDEPPolicy "$PCRECOMP/runtime/native32/native32.c" ||
    fail "$PCRECOMP predates native32's Wine support (pcrecomp #55): update it."
fi
export PCRECOMP
say "  pcrecomp: $PCRECOMP"

if [ "$MAC" = 1 ]; then
  WINE=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine
  [ -x "$WINE" ] || fail "CrossOver is needed: https://www.codeweavers.com/crossover"
fi

# ---------------------------------------------------------------- the game
GAME_SUB="steamapps/common/Command & Conquer Red Alert II"
if [ ! -e game ]; then
  found=""
  if [ "$MAC" = 1 ]; then
    step "Finding Red Alert 2 in your CrossOver bottles"
    candidates=("$HOME/Library/Application Support/CrossOver/Bottles"/*/drive_c/Program\ Files*/Steam/"$GAME_SUB")
  else
    step "Finding Red Alert 2 in your Steam libraries"
    candidates=()
    for s in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
      [ -d "$s" ] || continue
      candidates+=("$s/$GAME_SUB")
      # every library Steam knows, from libraryfolders.vdf's "path" lines
      vdf="$s/steamapps/libraryfolders.vdf"
      [ -f "$vdf" ] && while IFS= read -r lib; do candidates+=("$lib/$GAME_SUB"); done \
        < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"$/\1/p' "$vdf")
    done
  fi
  for d in "${candidates[@]}"; do
    [ -f "$d/gamemd.exe" ] && [ -f "$d/game.exe" ] && { found=$d; break; }
  done
  while [ -z "$found" ]; do
    read -r -p "  The folder holding gamemd.exe and game.exe: " found
    [ -f "$found/gamemd.exe" ] && [ -f "$found/game.exe" ] || { say "  Not there."; found=""; }
  done
  ln -s "$found" game
fi
GAME_DIR=$(cd game && pwd -P)
say "  game/ -> $GAME_DIR"
if [ "$MAC" = 1 ]; then
  case "$GAME_DIR" in
    */CrossOver/Bottles/*) BOTTLE=${GAME_DIR#*/CrossOver/Bottles/}; BOTTLE=${BOTTLE%%/*} ;;
    *) BOTTLE=${CX_BOTTLE:-Steam} ;;
  esac
  say "  bottle: $BOTTLE"
fi

# ---------------------------------------------------------------- each game
T=$PCRECOMP/tools
for g in $GAMES; do
  if [ "$g" = yr ]; then
    name="Yuri's Revenge"; exe=gamemd.exe; target=gamemd; work=work; gen=src/recomp/gen; build=build
  else
    name="Red Alert 2"; exe=game.exe; target=game; work=work/game; gen=src/recomp/gen_game; build=build-game
  fi
  mkdir -p "$work"
  step "$name: analysing $exe"
  if done_already "$work/rtti_seeds.json"; then say "  done (skipping)"; else
    "$PY" "$T/pe/pe_analyze.py" "game/$exe" --json "$work/pe_analysis.json" >/dev/null
    "$PY" "$T/cpp/rtti.py" "game/$exe" -o "$work/rtti.json" --seeds "$work/rtti_seeds.json" | tail -3
  fi
  step "$name: finding every function (about 15 minutes, once)"
  if done_already "$work/functions.json"; then say "  done (skipping)"; else
    "$PY" "$T/disasm/disasm32.py" "game/$exe" -o "$work/functions.json" \
          --seed-functions "$work/rtti_seeds.json" > "$work/disasm.log" 2>&1 || fail "see $work/disasm.log"
    grep -E "Functions:|coverage" "$work/disasm.log"
  fi
  step "$name: lifting to C (a minute or two)"
  if done_already "$gen/recomp_dispatch.c"; then say "  done (skipping)"; else
    "$PY" run_lift.py --all --target "$target" > "$work/lift.log" 2>&1 || fail "see $work/lift.log"
    grep "lifted" "$work/lift.log" | tail -1
  fi
  if [ "$NATIVE" = 1 ]; then
    lbuild=${build/build/build-linux}
    step "$name: building $lbuild/ra2 (10 to 30 minutes)"
    if done_already "$lbuild/ra2"; then say "  done (skipping)"; else
      [ "$FORCE" = 1 ] && rm -rf "$lbuild"
      BUILD_DIR=$lbuild CMAKE_ARGS=-DRA2_TARGET=$target ./build-linux.sh > "$work/build-linux.log" 2>&1 ||
        fail "the build failed: see $work/build-linux.log"
    fi
  else
    step "$name: building $build/ra2.exe (10 to 20 minutes)"
    if done_already "$build/ra2.exe"; then say "  done (skipping)"; else
      [ "$FORCE" = 1 ] && rm -rf "$build"
      BUILD_DIR=$build CMAKE_ARGS=-DRA2_TARGET=$target ./build.sh > "$work/build.log" 2>&1 ||
        fail "the build failed: see $work/build.log"
    fi
  fi
  if [ "$NATIVE" = 1 ]; then
    launcher="$name (recomp).sh"
    cat > "$launcher" <<EOF
#!/bin/sh
# Plays the recompiled game, natively: scaling F12, fullscreen F11.
cd "\$(dirname "\$0")" && exec $lbuild/ra2 --run "\$@"
EOF
  elif [ "$MAC" = 1 ]; then
    launcher="$name (recomp).command"
    cat > "$launcher" <<EOF
#!/bin/sh
# Plays the recompiled game under CrossOver: settings F10, scaling F12, fullscreen F11.
cd "\$(dirname "\$0")" && CX_BOTTLE="$BOTTLE" exec ./play.sh $g --run
EOF
  else
    launcher="$name (recomp).sh"
    cat > "$launcher" <<EOF
#!/bin/sh
# Plays the recompiled game under Wine: settings F10, scaling F12, fullscreen F11.
cd "\$(dirname "\$0")" && exec ./play.sh $g --run
EOF
  fi
  chmod +x "$launcher"
  say "  $launcher"
done

printf '\n\033[32mDone.\033[0m Run a "(recomp)" launcher here to play; %s.\n' \
  "$([ "$NATIVE" = 1 ] && echo "F12 changes the scaling, F11 is fullscreen" || echo "F10 opens the settings")"
