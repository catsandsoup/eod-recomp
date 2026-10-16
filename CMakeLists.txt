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
  file(GLOB PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/win32/*.cpp ${CMAKE_SOURCE_DIR}/runtime/gs/d3d11/*.cpp
       ${CMAKE_SOURCE_DIR}/runtime/ui/*.cpp)
  # Dear ImGui (the in-game menu, runtime/ui)
  set(IMGUI_SOURCES ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
      ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp)
  set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
  list(APPEND PLATFORM_SOURCES ${IMGUI_SOURCES})
else()
  file(GLOB PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/posix/*.cpp)
  # window, keyboard, controllers and audio through SDL3 (default: the headless test backend)
  option(EOD_SDL3 "SDL3 platform backend (Linux, macOS)" OFF)
  if(EOD_SDL3)
    if(APPLE)
      enable_language(OBJCXX)  # runtime/app/macos_app.mm (and SDL's own sources)
    endif()
    # EOD_BUNDLED_SDL: SDL3 built from source with HIDAPI over libusb, linked
    # statically. macOS has no driver for wired Xbox 360 pads (and third-party
    # ones such as PDP's); SDL reaches them through libusb, which installed SDL3
    # builds (e.g. Homebrew's) don't use. Needs libusb (brew install libusb).
    option(EOD_BUNDLED_SDL "build SDL3 from source with HIDAPI over libusb (Xbox 360 pads on macOS)" OFF)
    if(EOD_BUNDLED_SDL)
      include(FetchContent)
      set(EOD_SDL_VERSION 3.4.18)
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
      set(SDL_HIDAPI_LIBUSB_SHARED OFF CACHE BOOL "" FORCE)  # link libusb (otool -L shows it, bundle.py copies it)
      FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-${EOD_SDL_VERSION}/SDL3-${EOD_SDL_VERSION}.tar.gz
        URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(SDL3)
      get_directory_property(_sdl_libusb DIRECTORY ${sdl3_SOURCE_DIR} DEFINITION HAVE_HIDAPI_LIBUSB)
      if(NOT _sdl_libusb)
        message(FATAL_ERROR "SDL3 was configured without libusb: install libusb (brew install libusb)")
      endif()
    else()
      find_package(SDL3 REQUIRED CONFIG)
    endif()
    list(APPEND PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/sdl3/platform_sdl3.cpp)
    if(APPLE)  # Application Support, disc install, settings dialog
      list(APPEND PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/app/macos_app.mm
           ${CMAKE_SOURCE_DIR}/runtime/app/macos_vulkan.cpp ${CMAKE_SOURCE_DIR}/runtime/app/disc_import.cpp
           ${CMAKE_SOURCE_DIR}/runtime/app/iso9660.cpp)
    endif()
  endif()
  # Vulkan GS renderer (runtime/gs/vulkan) in place of the stub; it draws into the SDL3 window
  option(EOD_VULKAN "Vulkan GS renderer (Linux, macOS with MoltenVK; needs EOD_SDL3)" OFF)
  if(EOD_VULKAN)
    if(NOT EOD_SDL3)
      message(FATAL_ERROR "EOD_VULKAN needs EOD_SDL3")
    endif()
    find_package(Vulkan REQUIRED)
    list(REMOVE_ITEM RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/gs/gs_hw_stub.cpp)
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
    # arm64 (Apple Silicon): NEON stands in for SSE (runtime/include/sse_compat.h).
    # No FMA contraction, so float results match the x86 builds.
    target_compile_options(eod_gen PRIVATE -ffp-contract=off)
    target_compile_options(eod PRIVATE -ffp-contract=off)
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
  target_compile_definitions(eod PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX
                             IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
                             IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
  target_link_libraries(eod PRIVATE d3d11 dxgi gdi32 user32 winmm)
  if(MINGW)
    target_link_options(eod PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
  # settings program (writes eod.ini, starts the game)
  enable_language(RC)
  add_executable(eodsetup WIN32 ${CMAKE_SOURCE_DIR}/tools/setup/eodsetup.cpp ${CMAKE_SOURCE_DIR}/tools/setup/eodsetup.rc
                 ${CMAKE_SOURCE_DIR}/runtime/core/config.cpp ${CMAKE_SOURCE_DIR}/runtime/core/ps2mc.cpp
                 ${CMAKE_SOURCE_DIR}/runtime/core/eodsave.cpp)
  target_compile_definitions(eodsetup PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(eodsetup PRIVATE comctl32 comdlg32 dxgi ole32 shell32 gdi32 user32)
  if(MINGW)
    target_link_options(eodsetup PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(eod PRIVATE Threads::Threads m)
  if(EOD_SDL3)
    target_compile_definitions(eod PRIVATE EOD_SDL3)
    target_link_libraries(eod PRIVATE SDL3::SDL3)
    if(EOD_BUNDLED_SDL)
      target_compile_definitions(eod PRIVATE EOD_SDL_LIBUSB)
    endif()
    if(APPLE)
      target_compile_definitions(eod PRIVATE EOD_MACOS_APP)
      target_link_libraries(eod PRIVATE "-framework Cocoa")
      set_source_files_properties(${CMAKE_SOURCE_DIR}/runtime/app/macos_app.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
      # EOD_MAC_APP: also <build>/Eve of Destruction.app, with the libraries it
      # uses (SDL3, libusb, the Vulkan loader and MoltenVK) copied in and
      # ad-hoc signed (tools/macos/bundle.py). The game's files are not included.
      option(EOD_MAC_APP "build Eve of Destruction.app (macOS)" OFF)
      if(EOD_MAC_APP)
        add_custom_target(eod_app ALL
          COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/macos/bundle.py
                  "${CMAKE_BINARY_DIR}/Eve of Destruction.app" $<TARGET_FILE:eod>
          DEPENDS eod VERBATIM)
      endif()
    endif()
  endif()
  if(EOD_VULKAN)
    target_compile_definitions(eod PRIVATE EOD_VULKAN)
    target_link_libraries(eod PRIVATE Vulkan::Vulkan)
  endif()
endif()

# GS transfer chunk test (cmake -DBUILD_TESTING=ON, then ctest)
if(BUILD_TESTING)
  enable_testing()
  add_executable(gs_transfer_chunk_test tests/gs_transfer_chunk_test.cpp)
  target_include_directories(gs_transfer_chunk_test PRIVATE runtime)
  add_test(NAME gs_transfer_chunks COMMAND gs_transfer_chunk_test)
endif()

# disc image reader and disc install test (cmake -DBUILD_TESTING=ON, then ctest)
if(BUILD_TESTING AND NOT WIN32)
  enable_testing()
  add_executable(iso9660_test tests/iso9660_test.cpp runtime/app/iso9660.cpp runtime/app/disc_import.cpp)
  add_test(NAME iso9660 COMMAND iso9660_test)
endif()
