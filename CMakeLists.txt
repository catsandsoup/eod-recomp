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
  # where the build without it is the headless test backend).
  #
  # EOD_BUNDLED_SDL builds SDL3 from source (pinned release) with HIDAPI over
  # libusb, linked statically into eod. That is how Xbox 360 wired pads (Rock
  # Candy and other third-party 360 pads) work on macOS, which has no 360
  # driver: Homebrew's SDL3 is built without libusb and never sees them.
  # libusb itself comes from Homebrew (brew install libusb) and is linked
  # dynamically, so a .app bundle must ship libusb-1.0.0.dylib (docs/MACOS.md).
  # OFF: use an installed SDL3 (find_package), e.g. Homebrew's, without 360 pads.
  if(APPLE)
    option(EOD_BUNDLED_SDL "build SDL3 from source with libusb HIDAPI (Xbox 360 pads)" ON)
  else()
    option(EOD_BUNDLED_SDL "build SDL3 from source with libusb HIDAPI (Xbox 360 pads)" OFF)
  endif()
  if(EOD_BUNDLED_SDL)
    include(FetchContent)
    set(EOD_SDL_VERSION 3.4.18)
    # Homebrew's libusb (SDL's FindLibUSB uses pkg-config, then find_library)
    foreach(prefix /opt/homebrew/opt/libusb /usr/local/opt/libusb)
      if(EXISTS ${prefix}/include/libusb-1.0/libusb.h)
        list(APPEND CMAKE_PREFIX_PATH ${prefix})
        set(LibUSB_ROOT ${prefix})
      endif()
    endforeach()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI_LIBUSB ON CACHE BOOL "" FORCE)
    # link libusb directly instead of dlopen()ing it by an absolute Homebrew
    # path: otool -L shows the dependency and bundling tools can rewrite it
    set(SDL_HIDAPI_LIBUSB_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/releases/download/release-${EOD_SDL_VERSION}/SDL3-${EOD_SDL_VERSION}.tar.gz
      URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL3)
    # SDL silently drops libusb when it can't find it; for 360 pads it is the point
    get_directory_property(_sdl_libusb DIRECTORY ${sdl3_SOURCE_DIR} DEFINITION HAVE_HIDAPI_LIBUSB)
    if(NOT _sdl_libusb)
      message(FATAL_ERROR "bundled SDL3 was configured without libusb: brew install libusb "
                          "(or -DEOD_BUNDLED_SDL=OFF for an installed SDL3 without Xbox 360 pads)")
    endif()
    message(STATUS "SDL3 ${EOD_SDL_VERSION} from source, HIDAPI over libusb")
    set(SDL3_FOUND TRUE)
  elseif(APPLE)
    find_package(SDL3 REQUIRED CONFIG)
  else()
    find_package(SDL3 QUIET CONFIG)
  endif()
  if(SDL3_FOUND)
    list(APPEND PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/sdl3/platform_sdl3.cpp
                                 ${CMAKE_SOURCE_DIR}/runtime/platform/sdl3/sdl_input.cpp)
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
    if(EOD_BUNDLED_SDL)
      target_compile_definitions(eod PRIVATE EOD_SDL_LIBUSB)
    endif()
    target_link_libraries(eod PRIVATE SDL3::SDL3)
    # controller tests without hardware: SDL virtual joysticks posing as an
    # Xbox 360 / PDP Rock Candy pad (port assignment, triggers, hot-plug)
    enable_testing()
    add_executable(eod_input_test ${CMAKE_SOURCE_DIR}/tools/input_test/input_test.cpp
                   ${CMAKE_SOURCE_DIR}/runtime/platform/sdl3/sdl_input.cpp)
    target_include_directories(eod_input_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include ${CMAKE_SOURCE_DIR}/runtime)
    target_link_libraries(eod_input_test PRIVATE SDL3::SDL3)
    add_executable(eod_pad_rpc_test tools/input_test/pad_rpc_test.cpp runtime/iop/pad.cpp)
    target_include_directories(eod_pad_rpc_test PRIVATE runtime/include runtime)
    add_executable(eod_mc_test tools/input_test/mc_persistence_test.cpp runtime/iop/mc.cpp runtime/core/config.cpp)
    target_include_directories(eod_mc_test PRIVATE runtime/include runtime)
    add_executable(eod_keyboard_test tools/input_test/keyboard_test.cpp runtime/core/config.cpp)
    target_include_directories(eod_keyboard_test PRIVATE runtime/include runtime)
    add_test(NAME keyboard COMMAND eod_keyboard_test)
    add_executable(eod_pad_script_test tools/input_test/pad_script_test.cpp)
    target_include_directories(eod_pad_script_test PRIVATE runtime/include runtime)
    add_test(NAME pad_script COMMAND eod_pad_script_test)
    add_test(NAME mc_persistence COMMAND eod_mc_test)
    add_test(NAME pad_rpc COMMAND eod_pad_rpc_test)
    add_test(NAME input COMMAND eod_input_test)
    set_tests_properties(input pad_rpc mc_persistence pad_script keyboard PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  endif()
endif()
