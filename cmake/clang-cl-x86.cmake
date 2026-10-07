# Cross-compile the 32-bit Windows host on macOS (or Linux): clang-cl and
# lld-link against the MSVC CRT and Windows SDK that xwin downloads.
#
#   xwin --accept-license --arch x86 splat --output ~/.xwin
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake
#
# build.sh does both. The ra2.exe it makes is the same Windows program
# build.cmd makes, to run under CrossOver or Wine.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR X86)

set(XWIN_DIR "$ENV{XWIN_DIR}")
if(NOT XWIN_DIR)
  set(XWIN_DIR "$ENV{HOME}/.xwin")
endif()
if(NOT EXISTS "${XWIN_DIR}/crt/lib/x86")
  message(FATAL_ERROR "no x86 CRT in ${XWIN_DIR}: run xwin --accept-license --arch x86 splat --output ${XWIN_DIR}")
endif()

# Homebrew keeps LLVM and lld keg-only, and Debian and Ubuntu keep clang-cl and
# llvm-lib in a versioned folder, so look there as well as on PATH.
file(GLOB _llvm_versioned LIST_DIRECTORIES true /usr/lib/llvm-*/bin)
list(SORT _llvm_versioned COMPARE NATURAL ORDER DESCENDING)
set(_llvm_hints /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/lld/bin
                /usr/local/opt/llvm/bin /usr/local/opt/lld/bin ${_llvm_versioned})
find_program(CMAKE_C_COMPILER clang-cl HINTS ${_llvm_hints} REQUIRED)
find_program(CMAKE_LINKER lld-link HINTS ${_llvm_hints} REQUIRED)
find_program(CMAKE_AR llvm-lib HINTS ${_llvm_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER llvm-rc HINTS ${_llvm_hints})
find_program(CMAKE_MT llvm-mt HINTS ${_llvm_hints})
set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc)

# /imsvc: system headers, so the SDK's own warnings stay quiet.
set(_inc "")
foreach(d crt/include sdk/include/ucrt sdk/include/um sdk/include/shared)
  string(APPEND _inc " /imsvc\"${XWIN_DIR}/${d}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_inc}")

set(_lib "")
foreach(d crt/lib/x86 sdk/lib/um/x86 sdk/lib/ucrt/x86)
  string(APPEND _lib " /libpath:\"${XWIN_DIR}/${d}\"")
endforeach()
# No manifest step: Homebrew's llvm-mt has no libxml2 to merge one with, and
# the host's only manifest setting is /MANIFESTUAC:NO.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib} /manifest:no")

# xwin leaves out the debug CRT (libcmtd.lib) unless asked, and CMake's
# compiler check links Debug by default.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
