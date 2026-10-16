cmake_minimum_required(VERSION 3.16)
project(eod C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python3 COMPONENTS Interpreter)
set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)

# ---------------------------------------------------------------- generation
# gen/ holds the C translation of SLUS_209.10.  Regenerate it with
#   python3 tools/analyze.py && python3 tools/recomp.py
if(NOT EXISTS ${GEN_DIR}/table.c)
  if(NOT Python3_FOUND)
    message(FATAL_ERROR "gen/ is missing and Python 3 was not found to generate it")
  endif()
  message(STATUS "Generating recompiled sources...")
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/analyze.py
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESULT_VARIABLE r1)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp.py
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESULT_VARIABLE r2)
  if(r1 OR r2)
    message(FATAL_ERROR "code generation failed")
  endif()
endif()
# compiled VU micro programs (optional: the interpreter runs everything without them)
if(Python3_FOUND AND NOT EXISTS ${GEN_DIR}/vu_rec.inc)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/vurecomp.py
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
file(GLOB GEN_SOURCES ${GEN_DIR}/rec_*.c)
list(APPEND GEN_SOURCES ${GEN_DIR}/table.c)

# --------------------------------------------------------------------- runtime
file(GLOB_RECURSE RUNTIME_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/core/*.c ${CMAKE_SOURCE_DIR}/runtime/core/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/kernel/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/hw/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gs/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/vu/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/iop/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/spu/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.c)
list(APPEND RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/main.cpp)
if(WIN32)
  file(GLOB PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/win32/*.cpp ${CMAKE_SOURCE_DIR}/runtime/gs/d3d11/*.cpp)
else()
  file(GLOB PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/posix/*.cpp)
  # SDL3: window, audio and controllers (required on macOS; optional elsewhere,
  # where the build without it is the headless test backend)
  if(APPLE)
    find_package(SDL3 REQUIRED CONFIG)
  else()
    find_package(SDL3 QUIET CONFIG)
  endif()
  if(SDL3_FOUND)
    list(APPEND PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/sdl3/platform_sdl3.cpp)
  endif()
endif()

if(NOT WIN32)
  option(EOD_VULKAN "Build the Vulkan GS backend" ON)
  if(EOD_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND AND SDL3_FOUND)
      list(REMOVE_ITEM RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/gs/gs_hw_stub.cpp)
    else()
      set(EOD_VULKAN OFF)
      message(STATUS "Vulkan GS disabled (requires Vulkan headers/loader and SDL3)")
    endif()
  endif()
endif()

option(EOD_TRACE_CALLS "record recent EE function calls for debugging" OFF)

add_library(eod_gen OBJECT ${GEN_SOURCES})
if(EOD_TRACE_CALLS)
  add_compile_definitions(EE_TRACE_CALLS)
endif()
target_include_directories(eod_gen PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include ${GEN_DIR})

add_executable(eod ${RUNTIME_SOURCES} ${PLATFORM_SOURCES} $<TARGET_OBJECTS:eod_gen>)
target_include_directories(eod PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include ${CMAKE_SOURCE_DIR}/runtime ${GEN_DIR})

if(EOD_VULKAN)
  target_compile_definitions(eod PRIVATE EOD_VULKAN)
  target_link_libraries(eod PRIVATE Vulkan::Vulkan)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  # Generated code relies on wrapping arithmetic and type punning.
  target_compile_options(eod_gen PRIVATE -fno-strict-aliasing -fwrapv -w)
  target_compile_options(eod PRIVATE -fno-strict-aliasing -fwrapv -Wall -Wno-unused-function)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    # Apple Silicon / arm64: NEON is baseline (runtime/include/sse_compat.h).
    # No FMA contraction: the game's float results must match x86 SSE.
    target_compile_options(eod_gen PRIVATE -ffp-contract=off)
    target_compile_options(eod PRIVATE -ffp-contract=off)
    if(APPLE)
      target_compile_options(eod_gen PRIVATE -mcpu=apple-m1)
      target_compile_options(eod PRIVATE -mcpu=apple-m1)
    endif()
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # Pentium 4 / Core 2 baseline; SSE math avoids x87 excess precision.
    target_compile_options(eod_gen PRIVATE -msse2 -mfpmath=sse -march=pentium4 -mtune=core2)
    target_compile_options(eod PRIVATE -msse2 -mfpmath=sse -march=pentium4 -mtune=core2)
  else()
    target_compile_options(eod_gen PRIVATE -mtune=core2)
    target_compile_options(eod PRIVATE -mtune=core2)
  endif()
elseif(MSVC)
  target_compile_options(eod_gen PRIVATE /w /bigobj)
  target_compile_options(eod PRIVATE /W3)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_options(eod_gen PRIVATE /arch:SSE2)
    target_compile_options(eod PRIVATE /arch:SSE2)
  endif()
endif()

if(WIN32)
  # Windows 7 is the minimum supported version.
  target_compile_definitions(eod PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(eod PRIVATE d3d11 dxgi gdi32 user32 winmm)
  if(MINGW)
    target_link_options(eod PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
  # settings program (writes eod.ini, starts the game)
  enable_language(RC)
  add_executable(eodsetup WIN32 ${CMAKE_SOURCE_DIR}/tools/setup/eodsetup.cpp ${CMAKE_SOURCE_DIR}/tools/setup/eodsetup.rc
                 ${CMAKE_SOURCE_DIR}/runtime/core/config.cpp)
  target_compile_definitions(eodsetup PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(eodsetup PRIVATE comctl32 dxgi ole32 shell32 gdi32 user32)
  if(MINGW)
    target_link_options(eodsetup PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(eod PRIVATE Threads::Threads m)
  if(SDL3_FOUND)
    target_compile_definitions(eod PRIVATE EOD_SDL3)
    target_link_libraries(eod PRIVATE SDL3::SDL3)
  endif()
endif()
