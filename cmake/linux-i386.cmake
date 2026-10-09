# The native Linux host (src/linux, on pcrecomp's runtime/win32hle): a 32-bit
# program, so the guest's addresses are host addresses. gcc or clang with
# -m32, and SDL2 for i386.
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
#
# The packages for Debian, Ubuntu, Fedora and Arch: setup.sh lists them.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_FLAGS_INIT "-m32")
# Not position-independent: i386 PIC spends a register on the GOT, and the
# host sits at 0x08048000, clear of the guest image.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -no-pie")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
# The 32-bit .pc files first: Debian and Ubuntu, Arch (lib32), Fedora (/usr/lib;
# its 64-bit ones are in /usr/lib64). Searched before the system's, not instead:
# Fedora's i686 packages lean on the 64-bit .pc files of their dependencies
# (harfbuzz, zlib...), whose include paths serve both.
set(ENV{PKG_CONFIG_PATH} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/pkgconfig")
