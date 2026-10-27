Name:           redalert2-recomp
Version:        0.1.3
Release:        1%{?dist}
Summary:        Static recompilation of Red Alert 2 and Yuri's Revenge
License:        MIT
URL:            https://github.com/sp00nznet/redalert2-recomp
# Built from a git checkout: packaging/rpm/build-rpm.sh makes the tarball
# with `git archive`, so uncommitted changes are never packaged.
Source0:        %{name}-%{version}.tar.gz

BuildArch:      x86_64
# Payload is scripts + source: no ELF binaries, so no debuginfo subpackage.
%global debug_package %{nil}
# Native 32-bit pipeline (setup.sh names these): gcc -m32 plus SDL2/ffmpeg
# for i686. Bink movies need the libav* ones; without them build with
# CMAKE_ARGS=-DWITH_FFMPEG=OFF ./build-linux.sh (no intro/cutscene movies).
Requires:       gcc glibc-devel.i686 libgcc.i686 libatomic.i686
Requires:       cmake ninja-build pkgconf python3 git
Requires:       liberation-sans-fonts
Requires:       sdl2-compat-devel.i686 SDL2_ttf-devel.i686
Requires:       libavformat-free-devel.i686 libavcodec-free-devel.i686
Requires:       libswresample-free-devel.i686 libavutil-free-devel.i686
Recommends:     steam

%description
The native Linux pipeline for Command & Conquer: Red Alert 2 and Yuri's
Revenge, statically recompiled to 32-bit Linux through pcrecomp.

This package ships the source and the setup driver; the build (30-60
minutes, ~6 GB, once) runs on your machine because it needs your own copy
of the game (game.exe and gamemd.exe). Run redalert2-recomp-setup after
installing.

%prep
%setup -q

%install
install -d %{buildroot}%{_datadir}/%{name}
# Everything tracked, minus the packaging helpers' build scripts themselves
# (packaging/ stays: it documents how this rpm was made).
tar --exclude=.gitignore -cf - . | tar -xf - -C %{buildroot}%{_datadir}/%{name}
echo "%{version}" > %{buildroot}%{_datadir}/%{name}/.pkg-version

install -d %{buildroot}%{_bindir}
install -m755 packaging/common/redalert2-recomp-setup %{buildroot}%{_bindir}/
install -m755 packaging/common/redalert2-recomp-play %{buildroot}%{_bindir}/

install -d %{buildroot}%{_datadir}/applications
install -m644 packaging/common/yuris-revenge-recomp.desktop \
               packaging/common/red-alert-2-recomp.desktop \
               %{buildroot}%{_datadir}/applications/

install -d %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
install -m644 packaging/common/redalert2-recomp.svg \
               %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/

install -d %{buildroot}%{_mandir}/man1
gzip -9 -n -c packaging/common/redalert2-recomp-setup.1 > %{buildroot}%{_mandir}/man1/redalert2-recomp-setup.1.gz
gzip -9 -n -c packaging/common/redalert2-recomp-play.1 > %{buildroot}%{_mandir}/man1/redalert2-recomp-play.1.gz

install -d %{buildroot}%{_licensedir}/%{name}
install -m644 LICENSE NOTICE.md %{buildroot}%{_licensedir}/%{name}/

%files
%{_datadir}/%{name}/
%{_bindir}/redalert2-recomp-setup
%{_bindir}/redalert2-recomp-play
%{_datadir}/applications/yuris-revenge-recomp.desktop
%{_datadir}/applications/red-alert-2-recomp.desktop
%{_datadir}/icons/hicolor/scalable/apps/redalert2-recomp.svg
%{_mandir}/man1/redalert2-recomp-setup.1.gz
%{_mandir}/man1/redalert2-recomp-play.1.gz
%license LICENSE
%doc NOTICE.md README.md CHANGELOG.md

%post
echo "redalert2-recomp: build the games with (needs your own copy of the game):"
echo "  redalert2-recomp-setup"

%changelog
* Tue Oct 27 2026 9M2PJU - 0.1.3-1
- First packaged release: source + setup/play wrappers, per-user builds.
