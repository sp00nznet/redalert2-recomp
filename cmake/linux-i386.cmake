# The native Linux host (src/linux, on pcrecomp's runtime/win32hle): a 32-bit
# program, so the guest's addresses are host addresses. gcc or clang with
# -m32, and SDL2 for i386.
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
#
# Debian/Ubuntu: dpkg --add-architecture i386; apt install gcc-multilib libsdl2-dev:i386
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_FLAGS_INIT "-m32")
# Not position-independent: i386 PIC spends a register on the GOT, and the
# host sits at 0x08048000, clear of the guest image.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -no-pie")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/share/pkgconfig")
