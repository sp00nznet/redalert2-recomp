# =============================================================================
# Red Alert 2: Yuri's Revenge - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py        # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall x86 + CMake + Ninja -> build/ra2.exe
#   build\ra2.exe --run --watchdog 60
# =============================================================================
cmake_minimum_required(VERSION 3.20)
# The C runtime linked in (/MT): ra2.exe runs on a PC without the x86 Visual
# C++ redistributable (a clean test PC had none, and neither may a player).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(ra2 C)

# The native hosts (Linux, macOS) may be 64-bit: the guest's memory is then
# one block in the host (win32hle's guest_mem.c). The Windows host is 32-bit.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
  message(FATAL_ERROR "the Windows host must be 32-bit: use build.cmd (vcvarsall x86)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ../pcrecomp, or ../tools where older setups put it
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/../pcrecomp" AND EXISTS "${CMAKE_SOURCE_DIR}/../tools/runtime")
  set(PCRECOMP "${CMAKE_SOURCE_DIR}/../tools" CACHE PATH "pcrecomp checkout")
else()
  set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
endif()
# RA2_TARGET: gamemd (Yuri's Revenge, the default) or game (Red Alert 2), the
# lift run_lift.py --target made; build each in its own folder.
set(RA2_TARGET "gamemd" CACHE STRING "gamemd (Yuri's Revenge) or game (Red Alert 2)")
if(RA2_TARGET STREQUAL "game")
  set(GEN_DIR src/recomp/gen_game)
else()
  set(GEN_DIR src/recomp/gen)
endif()
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${GEN_DIR}/*.c")
# macOS writes ._* metadata files beside sources on some external drives (#31)
list(FILTER GEN_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN_DIR} is empty -- run ./setup.sh (Setup.cmd on Windows) first: it lifts each game")
endif()

# ---- the web: the lifted C on runtime/win32hle, compiled to WebAssembly
# with Emscripten (build-web.sh: emcmake cmake ...). One linear memory, so
# the guest's 32-bit addresses are the program's own (RECOMP_FLAT_MEMORY):
# the image goes at 0x00400000 and the toolchain's static data, stack and
# heap start above it (GLOBAL_BASE, 16 MB). The game runs on a pthread
# (PROXY_TO_PTHREAD) so it may block as a Win32 program does; its files come
# from WASMFS: the player's own game folder, imported into the browser's
# storage, and saves beside it (src/web/web.c). The movies play through the
# page (web/movies.js).
if(EMSCRIPTEN)
  set(HLE ${PCRECOMP}/runtime/win32hle)
  file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
  list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\.c$")
  add_executable(ra2 src/linux/main.c src/linux/blowfish.c src/linux/script.c src/linux/mods.c src/linux/menu.c
                     src/web/web.c src/runtime/hdvox.c src/runtime/queuefive.c ${HLE_SOURCES} ${GEN_SOURCES})
  # yr.js/yr.wasm (Yuri's Revenge) or ra2.js/ra2.wasm (Red Alert 2): both
  # can sit on one page, which offers the games it finds (web/index.html)
  if(RA2_TARGET STREQUAL "game")
    set_target_properties(ra2 PROPERTIES OUTPUT_NAME "ra2" SUFFIX ".js")
  else()
    set_target_properties(ra2 PROPERTIES OUTPUT_NAME "yr" SUFFIX ".js")
  endif()
  target_include_directories(ra2 PRIVATE src/linux src/runtime src/web ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
  set(WEB_GLOBAL_BASE 0x1000000)
  target_compile_definitions(ra2 PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS RECOMP_FLAT_MEMORY HLE_FIXED_END=${WEB_GLOBAL_BASE}u)
  set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
  target_compile_options(ra2 PRIVATE -fno-strict-aliasing -fwrapv -pthread --use-port=sdl2 --use-port=sdl2_ttf)
  set(WEB_LINK_OPT "-O1" CACHE STRING "optimisation at link (wasm-opt)")
  set(WEB_ASSERTIONS 0 CACHE STRING "Emscripten runtime assertions (1: readable aborts, slower)")
  # -g0: the configuration's -g would otherwise keep DWARF for 270 MB of lifted C in the link
  target_link_options(ra2 PRIVATE ${WEB_LINK_OPT} -g0 --profiling-funcs -pthread --use-port=sdl2 --use-port=sdl2_ttf
    -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8
    -sWASMFS -lfetchfs.js -lopfs.js
    -sOFFSCREEN_FRAMEBUFFER   # SDL's GL calls from the game's thread are proxied to the browser's main thread, which owns the canvas
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sGLOBAL_BASE=16MB
    -sINITIAL_MEMORY=1024MB -sMAXIMUM_MEMORY=2048MB -sALLOW_MEMORY_GROWTH
    -sSTACK_SIZE=16MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB
    -sEXIT_RUNTIME -sENVIRONMENT=web,worker
    -sASSERTIONS=${WEB_ASSERTIONS} -sALLOW_UNIMPLEMENTED_SYSCALLS
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToNewUTF8,UTF8ToString
    -sMODULARIZE=0 -sEXPORT_NAME=RA2)
  return()
endif()

# ---- native Linux and macOS: the lifted C on runtime/win32hle. 32-bit
# (cmake/linux-i386.cmake) a guest address is a host address; 64-bit
# (build-macos.sh, or Linux x86-64/arm64) it is an offset into the guest's
# memory.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
  # The Bink movies (intro, cutscenes) need ffmpeg; the rest of the game does
  # not. WITH_FFMPEG=OFF builds without it (no movies) so hosts where 32-bit
  # ffmpeg is a pain to get -- e.g. Arch, where lib32-ffmpeg is only in the AUR
  # -- can skip that one dependency. bink.c stubs the movie player out.
  option(WITH_FFMPEG "Play Bink movies via ffmpeg (needs its 32-bit libraries)" ON)
  if(WITH_FFMPEG)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
  endif()
  set(HLE ${PCRECOMP}/runtime/win32hle)
  file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
  list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\.c$|/[.]_[^/]*$")
  add_executable(ra2 src/linux/main.c src/linux/blowfish.c src/linux/script.c src/linux/mods.c src/linux/menu.c src/runtime/hdvox.c src/runtime/queuefive.c ${HLE_SOURCES} ${GEN_SOURCES})
  target_include_directories(ra2 PRIVATE src/linux src/runtime ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
  target_compile_definitions(ra2 PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS)
  if(WITH_FFMPEG)
    target_compile_definitions(ra2 PRIVATE HLE_WITH_FFMPEG)
  endif()
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(ra2 PRIVATE RECOMP_FLAT_MEMORY)
  else()
    # 64-bit: where the guest's 4 GB goes in the host. A fixed address lets
    # the lifted code add a constant, not a global it reloads after every
    # store. On a Mac 16 TB up is free on arm64 and x86-64 alike (above the
    # executable, the shared cache and the system's heaps, below the
    # stacks); on Linux, where some arm64 kernels give a process 512 GB, the
    # default is wherever mmap puts it. Empty: wherever mmap puts it.
    if(APPLE)
      set(RA2_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
    else()
      set(RA2_MEM_BASE "" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
    endif()
    if(RA2_MEM_BASE)
      target_compile_definitions(ra2 PRIVATE RECOMP_MEM_BASE=${RA2_MEM_BASE})
    endif()
  endif()
  set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
  target_compile_options(ra2 PRIVATE -fno-strict-aliasing -fwrapv)
  target_link_libraries(ra2 PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
  if(WITH_FFMPEG)
    target_link_libraries(ra2 PRIVATE PkgConfig::FFMPEG)
  endif()
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
  src/runtime/queuefive.c
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
# free. The image has to end well below 0x77000000, where Windows maps the
# 32-bit ntdll.dll (a different spot each boot): one that reached it would not
# start at all ("The subsystem needed to support the image type is not
# present", #30). So big buffers are malloc'd, not static arrays.
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
set_source_files_properties(src/runtime/host.c src/runtime/input.c src/runtime/mods.c src/runtime/oracle.c src/runtime/present.c src/runtime/hdvox.c src/runtime/queuefive.c PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(ra2 PRIVATE user32 gdi32 kernel32 ole32 uuid ddraw dxguid winmm d3d11 dxgi shell32)

option(RA2_TRACE "record a ring buffer of lifted function entries" OFF)
if(RA2_TRACE)
  target_compile_definitions(ra2 PRIVATE RECOMP_TRACE=1)
endif()
