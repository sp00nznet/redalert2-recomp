# Linux packaging

The games are (c) EA, so no game files and nothing built from them may be
redistributed. Packaging is therefore **installer-style**: it ships this
repository's source plus two wrappers, and the analyse/lift/compile
pipeline (30-60 minutes, ~6 GB, once) runs on the user's machine, where
their own copy of the game is, in a per-user copy at
`~/.local/share/redalert2-recomp`.

| Format | Definition | Output |
|---|---|---|
| AUR | `aur/PKGBUILD` (`redalert2-recomp-git`) | built by users with `yay`/`paru`/`makepkg` |
| 1-liner | `install.sh` | user-level install, any distro |

`common/` holds the desktop files and the icon used by `install.sh`. The
AUR `PKGBUILD` embeds its wrappers and man pages inline so it is
standalone (it builds from upstream `main`, where `packaging/` does not
exist yet).

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

`pkgver()` follows the upstream tags (`git describe`, e.g. `v0.1.4-3-gabcdef0`
becomes `0.1.4.r3.gabcdef0`), so tagging a release needs no PKGBUILD edit;
only the `pkgver=` starting value and the `.SRCINFO` snapshot age while the
package is not rebuilt.

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
