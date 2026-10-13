# =============================================================================
# Red Alert 2: Yuri's Revenge - static recompilation host
#
#   py -3 run_lift.py        # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall x86 + CMake + Ninja -> build/ra2.exe
#   build\ra2.exe --run --watchdog 60
#
# Windows: 32-bit, MSVC x86 (or clang-cl). Linux: 32-bit, gcc -m32
# (build-linux.sh). macOS: 64-bit, Apple clang (build-macos.sh).
# =============================================================================
cmake_minimum_required(VERSION 3.20)
# The C runtime linked in (/MT): ra2.exe runs on a PC without the x86 Visual
# C++ redistributable (a clean test PC had none, and neither may a player).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(ra2 C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT APPLE)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall x86), or on Linux build-linux.sh")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../tools" CACHE PATH "pcrecomp checkout")
# RA2_TARGET: gamemd (Yuri's Revenge, the default) or game (Red Alert 2), the
# lift run_lift.py --target made; build each in its own folder.
set(RA2_TARGET "gamemd" CACHE STRING "gamemd (Yuri's Revenge) or game (Red Alert 2)")
if(RA2_TARGET STREQUAL "game")
  set(GEN_DIR src/recomp/gen_game)
else()
  set(GEN_DIR src/recomp/gen)
endif()
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN_DIR} is empty -- run ./setup.sh (Setup.cmd on Windows) first: it lifts each game")
endif()

# ---- native Linux and macOS: the lifted C on runtime/win32hle
# Linux is a 32-bit program (cmake/linux-i386.cmake), the guest's addresses
# its own. macOS has no 32-bit programs: a 64-bit one keeps the guest's 4 GB
# in an arena at RA2_MEM_BASE (runtime/win32hle/hle_mem.c), and every guest
# address is an offset into it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
  set(HLE ${PCRECOMP}/runtime/win32hle)
  file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
  list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\.c$")
  add_executable(ra2 src/linux/main.c src/linux/blowfish.c src/linux/script.c src/linux/mods.c src/runtime/hdvox.c ${HLE_SOURCES} ${GEN_SOURCES})
  target_include_directories(ra2 PRIVATE src/linux src/runtime ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
  target_compile_definitions(ra2 PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS HLE_WITH_FFMPEG)
  if(APPLE)
    # Free on arm64 and x86-64 alike: above the executable, the shared cache
    # and the heaps the system places, below the stacks.
    set(RA2_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (macOS)")
    target_compile_definitions(ra2 PRIVATE RECOMP_MEM_BASE=${RA2_MEM_BASE} _DARWIN_C_SOURCE)
  else()
    target_compile_definitions(ra2 PRIVATE RECOMP_FLAT_MEMORY)
  endif()
  set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
  target_compile_options(ra2 PRIVATE -fno-strict-aliasing -fwrapv)
  target_link_libraries(ra2 PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF PkgConfig::FFMPEG pthread m)
  return()
endif()

if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()

add_executable(ra2
  src/runtime/host.c
  src/runtime/input.c
  src/runtime/mods.c
  src/runtime/oracle.c
  src/runtime/present.c
  src/runtime/hdvox.c
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(ra2 PRIVATE
  src/runtime ${GEN_DIR} ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
# RECOMP_RETADDR=0u: a stack imbalance during bring-up leaks a harmless 0, not
# 0xDEAD0000 into a size argument (forcecommander).
target_compile_definitions(ra2 PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                    RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so the guest's 0x00400000..0x00B7A000 is
# free.
target_link_options(ra2 PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                               /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                               /MANIFESTUAC:NO)
set(GEN_OPT "/O2" CACHE STRING "optimisation for the generated C")
# Visual Studio 2026's compiler (19.50 and later) drops the sign test of a
# 16-bit value shifted to the top of a register (test ax,ax; jl), so a -1
# index got past a bounds check and a skirmish crashed as it started (issue
# #2). Its older optimiser compiles it correctly.
if(MSVC_VERSION GREATER_EQUAL 1950)
  string(APPEND GEN_OPT ";/d2SSAOptimizer-")
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(src/runtime/host.c src/runtime/input.c src/runtime/mods.c src/runtime/oracle.c src/runtime/present.c src/runtime/hdvox.c PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(ra2 PRIVATE user32 gdi32 kernel32 ole32 uuid ddraw dxguid winmm d3d11 dxgi shell32)

option(RA2_TRACE "record a ring buffer of lifted function entries" OFF)
if(RA2_TRACE)
  target_compile_definitions(ra2 PRIVATE RECOMP_TRACE=1)
endif()
