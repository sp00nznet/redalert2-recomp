# Linux packaging

Native packages for the native Linux pipeline. All four are
**installer-style** on purpose: the games are (c) EA, so no game files and
nothing built from them may be redistributed. Each package ships this
repository's source plus two wrappers; the analyse/lift/compile pipeline
(30-60 minutes, ~6 GB, once) runs on the user's machine, where their own
copy of the game is, in a per-user copy at
`~/.local/share/redalert2-recomp`.

| Format | Definition | Output |
|---|---|---|
| AUR | `aur/PKGBUILD` (`redalert2-recomp-git`) | built by users with `yay`/`paru`/`makepkg` |
| .deb | `deb/build-deb.sh` | `dist/redalert2-recomp_<ver>_amd64.deb` |
| .rpm | `rpm/redalert2-recomp.spec` + `rpm/build-rpm.sh` | `dist/redalert2-recomp-<ver>-1.*.rpm` |
| AppImage | `appimage/build-appimage.sh` | `dist/RedAlert2-Recomp-<ver>-x86_64.AppImage` |
| 1-liner | `install.sh` | user-level install, any distro |

`common/` holds the single source of truth installed by the .deb/.rpm:
`redalert2-recomp-setup` (syncs `/usr/share/redalert2-recomp` to the
per-user copy, keeping the game link, `work/` and the lifted C across
upgrades, then runs `setup.sh`), `redalert2-recomp-play`, two `.desktop`
files, the icon, and man pages. The AUR `PKGBUILD` and the AppImage `AppRun`
embed the same logic inline so they work standalone (the AUR builds from
upstream `main`, which only gains this directory once the packaging PR
merges).

## Building

From the repo root, committed tree only (`git archive HEAD`, so commit first):

```sh
./packaging/deb/build-deb.sh            # dist/redalert2-recomp_0.1.3_amd64.deb
./packaging/rpm/build-rpm.sh            # dist/redalert2-recomp-0.1.3-1.*.rpm
./packaging/appimage/build-appimage.sh  # dist/RedAlert2-Recomp-0.1.3-x86_64.AppImage
```

Check them without installing:

```sh
dpkg-deb -c dist/redalert2-recomp_*_amd64.deb
rpm -qlp dist/redalert2-recomp-*.rpm
./dist/RedAlert2-Recomp-*-x86_64.AppImage --appimage-extract && ls squashfs-root
```

Versioning: the .deb takes it from `CHANGELOG.md`, the .rpm from the
`Version:` in the `.spec`, the AppImage from `CHANGELOG.md`; bump all three
together with the changelog. The AUR `-git` package derives its version from
`git describe` at build time.

## AUR

```sh
cp aur/PKGBUILD /tmp/aur-test/ && cd /tmp/aur-test
makepkg --printsrcinfo > .SRCINFO   # refresh after every PKGBUILD change
makepkg -s                          # full build test (clones upstream main)
```

Publishing (package base `redalert2-recomp-git`, first push creates it):

```sh
git clone ssh://aur@aur.archlinux.org/redalert2-recomp-git.git
cp $OLDPWD/aur/PKGBUILD .
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO && git commit -m "upstream snapshot" && git push
```

NOTE: generate `.SRCINFO` from a pristine copy only. A full `makepkg` run
rewrites `pkgver=`/`pkgrel=` in the build directory's PKGBUILD, so an
`.SRCINFO` made afterwards freezes a stale version — always re-copy the
PKGBUILD first if that directory has ever built.

## The 1-liner

```sh
curl -fsSL https://raw.githubusercontent.com/sp00nznet/redalert2-recomp/main/packaging/install.sh | bash
```

Detects Arch/Debian/Fedora (enables `multilib`/`i386` where needed),
installs the toolchain from `setup.sh`, clones to `~/.local/share`,
installs wrappers to `~/.local/bin` and menu entries to `~/.local/share`,
then runs the setup. Flags pass through: `bash -s -- --yes --game-dir DIR`,
`--no-setup` for deps+tree only. Test locally without the network part:

```sh
bash -n packaging/install.sh && ./packaging/install.sh --no-setup
```
