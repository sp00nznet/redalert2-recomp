#!/bin/sh
# Build an RPM: dist/redalert2-recomp-0.1.3-1.*.rpm
#
#   ./packaging/rpm/build-rpm.sh            # version/release from the .spec
#
# Needs rpmbuild and git. Run from the repo root; packs `git archive HEAD`,
# so uncommitted changes are NOT included. Drops the tarball and the result
# in OUT_DIR (default dist/).
set -e
cd "$(dirname "$0")/../.."

OUT_DIR=${OUT_DIR:-dist}
SPEC=packaging/rpm/redalert2-recomp.spec
VER=$(sed -n 's/^Version:[[:space:]]*//p' "$SPEC" | head -1)
REL=$(sed -n 's/^Release:[[:space:]]*//p' "$SPEC" | head -1 | sed 's/%{?dist}//')

TOP=$(mktemp -d)
trap 'rm -rf "$TOP"' EXIT
mkdir -p "$TOP/SOURCES" "$TOP/SPECS" "$TOP/BUILD" "$TOP/RPMS" "$TOP/SRPMS"
cp "$SPEC" "$TOP/SPECS/"

# What `git archive <tag>` would hold once this is tagged v$VER; until then
# the current HEAD (which is what you are packaging).
git archive --prefix="redalert2-recomp-$VER/" HEAD | gzip -9 -n > "$TOP/SOURCES/redalert2-recomp-$VER.tar.gz"

rpmbuild -bb "$TOP/SPECS/$(basename "$SPEC")" \
    --define "_topdir $TOP" \
    --define "_sourcedir $TOP/SOURCES"

mkdir -p "$OUT_DIR"
cp "$TOP"/RPMS/*/*.rpm "$OUT_DIR/"
echo "wrote:"
ls "$OUT_DIR"/redalert2-recomp-"$VER"-"$REL"*.rpm
