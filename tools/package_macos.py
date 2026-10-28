#!/usr/bin/env python3
"""Package the native macOS host as a double-clickable .app, with its Homebrew
libraries inside, for people who did not build it.

    .venv/bin/python tools/package_macos.py                      # Yuri's Revenge, from build-macos
    .venv/bin/python tools/package_macos.py --build-dir build-macos-game   # Red Alert 2

Makes dist/<Title>.app and a zip of it beside a README. The app holds no game
data: on first launch it asks for the player's own game folder (the one with
gamemd.exe or game.exe) and remembers it in ~/Library/Application Support.
The host's ra2.ini and mods/ live inside the bundle (Contents/MacOS/ra2.ini,
Contents/mods/), as they live beside the build on a checkout.

Only the Mach-Os are (ad-hoc) signed, as Apple Silicon requires to run them;
the bundle is not sealed, so the settings inside it may change without
macOS calling the app damaged. It is not notarized: Gatekeeper asks
for a right-click > Open (or Privacy & Security > Open Anyway) the first time,
and a freshly downloaded copy must be moved with Finder once before it runs
from a writable place (App Translocation). The Homebrew libraries carry the
macOS version they were built on as their minimum, which becomes the app's
LSMinimumSystemVersion; the app is for this machine's CPU.
"""
import argparse, os, plistlib, re, shutil, stat, struct, subprocess, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BREW = "/opt/homebrew"

# target: title, exe, slug, bundle id, the build the host was lifted from
TITLES = {"gamemd": ("Yuri's Revenge", "gamemd.exe", "yr", "net.sp00nz.redalert2-recomp.yr", "Yuri's Revenge 1.001"),
          "game": ("Red Alert 2", "game.exe", "ra2", "net.sp00nz.redalert2-recomp.ra2", "Red Alert 2 1.006")}


def run(*cmd, check=True, capture=True):
    r = subprocess.run(cmd, capture_output=capture, text=True)
    if check and r.returncode:
        sys.exit(f"{' '.join(cmd)}\n{r.stderr}")
    return r.stdout if capture else ""


def otool_deps(path):
    """Install names this Mach-O loads (its own id excluded)."""
    out = run("otool", "-L", path).splitlines()[1:]
    names = [l.strip().split(" (")[0] for l in out if l.strip()]
    own_id = run("otool", "-D", path).splitlines()[1:]
    return [n for n in names if n not in own_id]


def is_bundled_dep(name):
    """Everything but the OS's own: Homebrew's, tools/deps-macos.sh's, or by rpath."""
    return not (name.startswith("/usr/lib/") or name.startswith("/System/"))


def rpaths(path):
    out = run("otool", "-l", path).splitlines()
    return [out[i + 2].split("path ", 1)[1].split(" (")[0] for i, l in enumerate(out) if l.strip() == "cmd LC_RPATH"]


def resolve(name, referrer, referrer_dir):
    """The file an install name means: absolute as is; @rpath by the referrer's
    rpaths (the build's CMAKE_BUILD_RPATH, or a library's own), then its
    folder; @loader_path by its folder; a bare name by the library folders."""
    if name.startswith("/"):
        return os.path.realpath(name)
    rel = name.split("/", 1)[1] if "/" in name else name
    dirs = (rpaths(referrer) if name.startswith("@rpath/") else []) + [referrer_dir, os.path.join(BREW, "lib")]
    for d in dirs:
        d = d.replace("@loader_path", referrer_dir).replace("@executable_path", referrer_dir)
        p = os.path.realpath(os.path.join(d, rel))
        if os.path.exists(p):
            return p
    sys.exit(f"cannot resolve {name} from {referrer}")


# Libraries a bundled library dlopens by name rather than linking: sdl2-compat
# finds SDL3 as @loader_path/libSDL3.dylib (bundled basename -> [(sibling of
# the source, in its folder or Homebrew's sdl3; bundled name)]).
DLOPENED = {"libSDL2-2.0.0.dylib": [("libSDL3.0.dylib", "libSDL3.dylib")]}


def bundle_libs(exe, frameworks):
    """Copy every Homebrew library the host loads, transitively, into
    Contents/Frameworks and point each load command at the copy, without
    rpaths: the host loads @executable_path/../Frameworks/x, the libraries
    @loader_path/x."""
    os.makedirs(frameworks, exist_ok=True)
    copied = {}                      # bundled basename -> source realpath
    todo = [(exe, True)]
    while todo:
        path, is_exe = todo.pop()
        src_dir = os.path.dirname(os.path.realpath(path)) if is_exe else os.path.dirname(copied[os.path.basename(path)])
        os.chmod(path, os.stat(path).st_mode | stat.S_IWUSR)
        changes = []
        for dep in otool_deps(path):
            if not is_bundled_dep(dep):
                continue
            base = os.path.basename(dep)
            if base not in copied:
                src = resolve(dep, path, src_dir)
                copied[base] = src
                dst = os.path.join(frameworks, base)
                shutil.copy2(src, dst)
                os.chmod(dst, 0o755)
                todo.append((dst, False))
                for extra_src, extra_name in DLOPENED.get(base, []):
                    beside = os.path.join(os.path.dirname(src), extra_src)
                    if not os.path.exists(beside):
                        beside = os.path.join(BREW, "opt/sdl3/lib", extra_src)
                    copied[extra_name] = os.path.realpath(beside)
                    extra_dst = os.path.join(frameworks, extra_name)
                    shutil.copy2(copied[extra_name], extra_dst)
                    os.chmod(extra_dst, 0o755)
                    todo.append((extra_dst, False))
            new = f"@executable_path/../Frameworks/{base}" if is_exe else f"@loader_path/{base}"
            changes += ["-change", dep, new]
        if not is_exe:
            changes += ["-id", f"@loader_path/{os.path.basename(path)}"]
        for rp in rpaths(path):
            changes += ["-delete_rpath", rp]
        if changes:
            run("install_name_tool", *changes, path)
        run("codesign", "--force", "--sign", "-", path)
    return copied


def min_macos(paths):
    """The newest minimum macOS among the Mach-Os: what the app needs."""
    best = (0, 0)
    for p in paths:
        for m in re.finditer(r"minos (\d+)\.(\d+)", run("otool", "-l", p)):
            best = max(best, (int(m.group(1)), int(m.group(2))))
    return "%d.%d" % best


def pe_icon(exe_path):
    """The largest icon in the exe's resources, as 32-bit RGBA rows, or None."""
    try:
        import pefile
    except ImportError:
        return None
    pe = pefile.PE(exe_path)
    icons, groups = {}, {}
    for entry in getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None).entries if hasattr(pe, "DIRECTORY_ENTRY_RESOURCE") else []:
        if entry.id not in (3, 14):
            continue
        for e2 in entry.directory.entries:
            e3 = e2.directory.entries[0]
            (icons if entry.id == 3 else groups)[e2.id] = pe.get_data(e3.data.struct.OffsetToData, e3.data.struct.Size)
    best = None
    for g in groups.values():
        cnt = struct.unpack_from("<HHH", g)[2]
        for i in range(cnt):
            w, h, cc, r, planes, bits, size, iid = struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i)
            key = (w or 256, bits)
            if iid in icons and (best is None or key > best[0]):
                best = (key, icons[iid])
    if not best:
        return None
    return dib_to_rgba(best[1])


def dib_to_rgba(d):
    """A BITMAPINFOHEADER icon image (XOR bitmap over an AND mask) to RGBA."""
    hdr = struct.unpack_from("<IiiHHIIiiII", d)
    w, h2, bits, colors_used = hdr[1], hdr[2], hdr[4], hdr[9]
    h = h2 // 2
    off = hdr[0]
    pal = []
    if bits <= 8:
        n = colors_used or (1 << bits)
        pal = [tuple(d[off + 4 * i:off + 4 * i + 3]) for i in range(n)]
        off += 4 * n
    stride = ((w * bits + 31) // 32) * 4
    mask_off = off + stride * h
    mask_stride = ((w + 31) // 32) * 4
    rows = []
    for y in range(h):
        src = h - 1 - y
        row = bytearray()
        for x in range(w):
            if bits == 32:
                b, g, r, a = d[off + src * stride + 4 * x: off + src * stride + 4 * x + 4]
            else:
                if bits == 8:
                    idx = d[off + src * stride + x]
                elif bits == 4:
                    byte = d[off + src * stride + x // 2]
                    idx = byte >> 4 if x % 2 == 0 else byte & 15
                else:
                    byte = d[off + src * stride + x // 8]
                    idx = (byte >> (7 - x % 8)) & 1
                b, g, r = pal[idx]
                a = 255
            mbyte = d[mask_off + src * mask_stride + x // 8]
            if (mbyte >> (7 - x % 8)) & 1:
                a = 0
            row += bytes((r, g, b, a))
        rows.append(bytes(row))
    return w, h, rows


def write_png(path, w, h, rows):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make_icns(rgba, out_icns, work):
    """An .icns from one small image, scaled up pixel for pixel (nearest
    neighbour): the game's icon is 32x32 and looks right crisp."""
    w, h, rows = rgba
    iconset = os.path.join(work, "app.iconset")
    shutil.rmtree(iconset, ignore_errors=True)
    os.makedirs(iconset)
    for size, name in [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"), (128, "128x128"),
                       (256, "128x128@2x"), (256, "256x256"), (512, "256x256@2x"), (512, "512x512"), (1024, "512x512@2x")]:
        big = []
        for y in range(size):
            src = rows[y * h // size]
            big.append(b"".join(src[4 * (x * w // size): 4 * (x * w // size) + 4] for x in range(size)))
        write_png(os.path.join(iconset, f"icon_{name}.png"), size, size, big)
    run("iconutil", "-c", "icns", iconset, "-o", out_icns)


LAUNCHER = r'''#!/bin/sh
# @TITLE@ (recomp): asks for the game folder once, then runs the host.
HERE=$(cd "$(dirname "$0")" && pwd)
TITLE="@TITLE@"
EXE="@EXE@"
SUPPORT="$HOME/Library/Application Support/Red Alert 2 Recomp"
CONF="$SUPPORT/@SLUG@-game-folder"
LOG="$HOME/Library/Logs/@TITLE@ (recomp).log"
mkdir -p "$SUPPORT" "$HOME/Library/Logs"

dialog() {   # dialog TEXT [stop]
    osascript - "$TITLE" "$1" "$2" >/dev/null 2>&1 <<'APPLESCRIPT'
on run argv
    set t to item 1 of argv
    set msg to item 2 of argv
    if item 3 of argv is "stop" then
        display dialog msg with title t buttons {"OK"} default button 1 with icon stop
    else
        display dialog msg with title t buttons {"OK"} default button 1 with icon note
    end if
end run
APPLESCRIPT
}
has_exe() { [ -n "$1" ] && [ -d "$1" ] && [ -n "$(find "$1" -maxdepth 1 -iname "$EXE" -print -quit 2>/dev/null)" ]; }

case "$HERE" in */AppTranslocation/*)
    dialog "macOS is running this app from a temporary read-only copy, because it was just downloaded. Move the app somewhere with Finder (Applications, or anywhere), then open it from there." stop
    exit 1 ;;
esac

GAME=$(cat "$CONF" 2>/dev/null)
if ! has_exe "$GAME"; then
    dialog "$TITLE needs your own copy of Red Alert 2 (the folder with $EXE, from Steam or the EA App). Choose that folder next; the app will remember it."
    while :; do
        GAME=$(osascript -e 'POSIX path of (choose folder with prompt "Choose the Red Alert 2 folder (the one with '"$EXE"')")' 2>/dev/null)
        [ -z "$GAME" ] && exit 0        # cancelled
        GAME=${GAME%/}
        has_exe "$GAME" && break
        dialog "No $EXE in $GAME. Choose the folder that has it." stop
    done
    printf '%s' "$GAME" > "$CONF"
fi

# Arguments from Finder (none nowadays) are dropped; from a terminal they pass through.
ARGS=""
for a in "$@"; do case "$a" in -psn_*) ;; *) ARGS="$ARGS $a" ;; esac; done
: > "$LOG"
# shellcheck disable=SC2086
"$HERE/ra2" --game "$GAME" --run $ARGS >> "$LOG" 2>&1
STATUS=$?
if [ $STATUS -ne 0 ]; then
    TAIL=$(grep -v '^\[record\]' "$LOG" | tail -n 12)
    case "$TAIL" in
        *"not the build this host was lifted from"*)
            rm -f "$CONF"
            dialog "The $EXE in $GAME is a different build from the one this app was made from. It needs the Steam or EA App release of @BUILD@ (or that same build). The folder choice has been forgotten; launch again to pick another.

$TAIL" stop ;;
        *) dialog "$TITLE stopped (exit $STATUS). The log is at $LOG

$TAIL" stop ;;
    esac
fi
exit $STATUS
'''

README = '''{title} (recomp) for macOS
{underline}

This is the native macOS build of redalert2-recomp, a static recompilation of
Command & Conquer: Red Alert 2 / Yuri's Revenge (https://github.com/sp00nznet/redalert2-recomp).

{game_note}

Needs: an Apple Silicon Mac on macOS {minos} or newer.

First launch: unzip, then move the app somewhere with Finder (Applications,
or any folder) before opening it; a downloaded app opened in place runs from a
read-only copy. The app is not notarized, so macOS blocks it once: either
right-click the app and choose Open, or open it, then go to System Settings >
Privacy & Security and click "Open Anyway" next to the message about it.

Keys: F11 toggles fullscreen, F12 cycles the scaling, F10 HD vehicles,
F9 switches mods (put mods in the app's Contents/mods/{slug}/).

If it stops, the log is in ~/Library/Logs/{title} (recomp).log. To choose a
different game folder, delete
~/Library/Application Support/Red Alert 2 Recomp/{slug}-game-folder.
'''


GAME_NOTE = '''It contains no game data. You need your own copy of the game: the folder with
{exe} from the Steam or EA App release (Command & Conquer: The Ultimate
Collection). The app asks for that folder the first time and remembers it.
Saves, settings and the game's own INI files are written into that folder, so
keep it somewhere you own, like ~/Games.'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default="build-macos", help="a built host (build-macos.sh)")
    ap.add_argument("--game", default="game", help="a game folder, for the icon in its exe")
    ap.add_argument("--out", default="dist")
    a = ap.parse_args()
    os.chdir(ROOT)
    cache = open(os.path.join(a.build_dir, "CMakeCache.txt")).read()
    target = re.search(r"RA2_TARGET:STRING=(\w+)", cache)
    target = target.group(1) if target else "gamemd"
    title, exe_name, slug, ident, build = TITLES[target]
    ra2 = os.path.join(a.build_dir, "ra2")
    if not os.path.exists(ra2):
        sys.exit(f"no {ra2}: build first (build-macos.sh)")
    m = re.search(r"^## \[(\d[^\]]*)\]", open("CHANGELOG.md").read(), re.M)
    version = m.group(1) if m else "0.0.0"

    app = os.path.join(a.out, f"{title}.app")
    contents = os.path.join(app, "Contents")
    shutil.rmtree(app, ignore_errors=True)
    for d in ["MacOS", "Frameworks", "Resources", f"mods/{slug}"]:
        os.makedirs(os.path.join(contents, d))
    shutil.copy2(ra2, os.path.join(contents, "MacOS", "ra2"))
    launcher = os.path.join(contents, "MacOS", "launch")
    with open(launcher, "w") as f:
        f.write(LAUNCHER.replace("@TITLE@", title).replace("@EXE@", exe_name).replace("@SLUG@", slug).replace("@BUILD@", build))
    os.chmod(launcher, 0o755)
    open(os.path.join(contents, "mods", slug, "README.txt"), "w").write(
        f"Mods go here, one folder each (docs in the repository's mods/README.md). F9 at the main menu switches.\n")

    copied = bundle_libs(os.path.join(contents, "MacOS", "ra2"), os.path.join(contents, "Frameworks"))
    print(f"bundled {len(copied)} libraries")
    minos = min_macos([os.path.join(contents, "MacOS", "ra2")] + [os.path.join(contents, "Frameworks", b) for b in copied])

    icon_file = None
    exe_path = os.path.join(a.game, exe_name)
    if os.path.exists(exe_path):
        rgba = pe_icon(exe_path)
        if rgba:
            make_icns(rgba, os.path.join(contents, "Resources", "app.icns"), a.out)
            icon_file = "app"
    plist = {
        "CFBundleName": title, "CFBundleDisplayName": title,
        "CFBundleIdentifier": ident, "CFBundleVersion": version, "CFBundleShortVersionString": version,
        "CFBundleExecutable": "launch", "CFBundlePackageType": "APPL", "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": minos, "LSApplicationCategoryType": "public.app-category.strategy-games",
        "NSHighResolutionCapable": True, "LSArchitecturePriority": [run("uname", "-m").strip()],
        "NSHumanReadableCopyright": "redalert2-recomp; the game is EA's, bring your own copy.",
    }
    if icon_file:
        plist["CFBundleIconFile"] = icon_file
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(plist, f)
    for f in [os.path.join(contents, "MacOS", "ra2")] + [os.path.join(contents, "Frameworks", b) for b in copied]:
        run("codesign", "--verify", "--strict", f)      # each signed in bundle_libs; the bundle itself is not sealed

    readme = os.path.join(a.out, f"README - {title}.txt")
    note = GAME_NOTE.format(exe=exe_name)
    open(readme, "w").write(README.format(title=title, underline="=" * (len(title) + 19), exe=exe_name, slug=slug,
                                          minos=minos, game_note=note))
    zip_path = os.path.join(a.out, f"{title} (recomp) macOS {version}.zip")
    if os.path.exists(zip_path):
        os.remove(zip_path)
    run("ditto", "-c", "-k", "--keepParent", app, zip_path)
    run("zip", "-qj", zip_path, readme)
    print(f"{app}\n{zip_path}\nminimum macOS {minos}, {run('uname', '-m').strip()} only")


if __name__ == "__main__":
    main()
