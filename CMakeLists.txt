cmake_minimum_required(VERSION 3.16)
project(eod C CXX)
# Enable Objective-C++ at the parent scope before SDL enables it in subdirs.
if(APPLE)
  enable_language(OBJCXX)
endif()

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

# Optional standard gzip transport for bounded diagnostic cold captures.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(eod PRIVATE ZLIB::ZLIB)
  target_compile_definitions(eod PRIVATE EOD_HAVE_ZLIB)
endif()

include(CTest)
if(BUILD_TESTING)
  if(Python3_FOUND)
    add_test(NAME AssetTools COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_custom_car.py)
    add_test(NAME AudioTools COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_audio.py)
    add_test(NAME GameplayMod COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_gameplay_mod.py)
    add_test(NAME NascarImport COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_nascar_import.py)
    set_tests_properties(AssetTools NascarImport GameplayMod AudioTools PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1;EOD_RUN_GAME_TESTS=0;EOD_RUN_NASCAR_GAME=0")
  endif()
  if(UNIX)
    foreach(overlay_kind fileio stream)
      add_executable(eod_${overlay_kind}_overlay_test tools/tests/mod_overlay_test.cpp)
      target_include_directories(eod_${overlay_kind}_overlay_test PRIVATE runtime runtime/include)
      target_compile_options(eod_${overlay_kind}_overlay_test PRIVATE -ffunction-sections -fdata-sections)
      if(APPLE)
        target_link_options(eod_${overlay_kind}_overlay_test PRIVATE -Wl,-dead_strip)
      else()
        target_link_options(eod_${overlay_kind}_overlay_test PRIVATE -Wl,--gc-sections)
      endif()
      if(overlay_kind STREQUAL "stream")
        target_compile_definitions(eod_${overlay_kind}_overlay_test PRIVATE TEST_STREAM_OVERLAY)
      endif()
      add_test(NAME ModOverlay-${overlay_kind} COMMAND eod_${overlay_kind}_overlay_test)
      set_tests_properties(ModOverlay-${overlay_kind} PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
    endforeach()
  endif()
  if(ZLIB_FOUND)
    add_executable(eod_gs_capture_io_test tools/tests/gs_capture_io_test.cpp)
    target_link_libraries(eod_gs_capture_io_test PRIVATE ZLIB::ZLIB)
    target_compile_definitions(eod_gs_capture_io_test PRIVATE EOD_HAVE_ZLIB)
    add_test(NAME GSCaptureIO COMMAND eod_gs_capture_io_test)
    set_tests_properties(GSCaptureIO PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  endif()
  add_executable(eod_alpha_range_test tools/tests/alpha_range_test.cpp)
  target_include_directories(eod_alpha_range_test PRIVATE runtime)
  add_test(NAME AlphaPassRange COMMAND eod_alpha_range_test)
  set_tests_properties(AlphaPassRange PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_hud_edge_test tools/tests/hud_edge_test.cpp src/hle_hud.cpp)
  target_include_directories(eod_hud_edge_test PRIVATE runtime runtime/include)
  add_test(NAME HUDEdges COMMAND eod_hud_edge_test)
  add_test(NAME HUDEdgesDisabled COMMAND eod_hud_edge_test disabled)
  add_test(NAME HUDEdgesTrace COMMAND eod_hud_edge_test)
  set_tests_properties(HUDEdges HUDEdgesDisabled PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  set_tests_properties(HUDEdgesTrace PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1;EOD_HUD_TRACE=1")
  add_executable(eod_recovery_test tools/tests/recovery_test.cpp src/hle_recovery.cpp runtime/core/config.cpp)
  target_include_directories(eod_recovery_test PRIVATE runtime runtime/include)
  add_test(NAME ManualRecovery COMMAND eod_recovery_test)
  set_tests_properties(ManualRecovery PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_live_cheats_test tools/tests/live_cheats_test.cpp src/game_patches.cpp runtime/core/config.cpp runtime/core/eodsave.cpp)
  target_include_directories(eod_live_cheats_test PRIVATE runtime runtime/include src gen)
  add_test(NAME LiveCheats COMMAND eod_live_cheats_test)
  add_executable(battle_driver_test tests/battle_driver_test.cpp src/battle_driver.cpp src/battle_navigation.cpp runtime/core/config.cpp)
  target_include_directories(battle_driver_test PRIVATE runtime runtime/include src gen)
  add_executable(battle_navigation_test tests/battle_navigation_test.cpp src/battle_navigation.cpp)
  target_include_directories(battle_navigation_test PRIVATE src)
  add_test(NAME battle_course_navigation COMMAND battle_navigation_test)
  set_tests_properties(battle_course_navigation PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(battle_events_test tests/battle_events_test.cpp src/battle_events.cpp)
  target_include_directories(battle_events_test PRIVATE runtime runtime/include src)
  add_test(NAME battle_event_resources COMMAND battle_events_test)
  set_tests_properties(battle_event_resources PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(battle_bots_test tests/battle_bots_test.cpp src/battle_bots.cpp)
  target_include_directories(battle_bots_test PRIVATE runtime runtime/include src)
  add_test(NAME battle_bot_construction COMMAND battle_bots_test)
  set_tests_properties(battle_bot_construction PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_test(NAME battle_driver_controls COMMAND battle_driver_test)
  set_tests_properties(battle_driver_controls PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  set_tests_properties(LiveCheats PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_lod_runtime_test tools/tests/lod_runtime_test.cpp src/hle_lod.cpp runtime/core/config.cpp)
  target_include_directories(eod_lod_runtime_test PRIVATE runtime runtime/include src)
  foreach(lod_mode off one two trace invalid high maximum override)
    add_test(NAME LODRuntime-${lod_mode} COMMAND eod_lod_runtime_test ${lod_mode})
    set_tests_properties(LODRuntime-${lod_mode} PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  endforeach()
  add_executable(eod_alpha_tiles_test tools/tests/alpha_tiles_test.cpp)
  target_include_directories(eod_alpha_tiles_test PRIVATE runtime)
  add_test(NAME AlphaPassTiles COMMAND eod_alpha_tiles_test)
  set_tests_properties(AlphaPassTiles PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_alpha_order_test tools/tests/alpha_order_test.cpp)
  target_include_directories(eod_alpha_order_test PRIVATE runtime)
  add_test(NAME AlphaPassOrder COMMAND eod_alpha_order_test)
  set_tests_properties(AlphaPassOrder PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_native_camera_test tools/tests/native_camera_test.cpp
    src/hle_camera.cpp runtime/core/config.cpp)
  target_include_directories(eod_native_camera_test PRIVATE runtime runtime/include)
  foreach(camera_aspect "4:3" "16:9" "16:10" "stretch")
    add_test(NAME NativeCamera-${camera_aspect} COMMAND eod_native_camera_test "${camera_aspect}")
  endforeach()
endif()

if(EOD_VULKAN)
  # PCSX2 uses xxHash for texture-content keys. Header-only inclusion keeps
  # the personal app self-contained; pin both source revision and archive hash.
  include(FetchContent)
  FetchContent_Declare(eod_xxhash
    URL https://codeload.github.com/Cyan4973/xxHash/tar.gz/e626a72bc2321cd320e953a0ccf1584cad60f363
    URL_HASH SHA256=34e71c085636fbc1726482996d92719d0a16e6a6097970ecf2990a27b3821162
    SOURCE_SUBDIR headers-only
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(eod_xxhash)
  target_include_directories(eod PRIVATE ${eod_xxhash_SOURCE_DIR})
  target_compile_definitions(eod PRIVATE EOD_VULKAN)
  target_link_libraries(eod PRIVATE Vulkan::Vulkan)
  include(CTest)
  if(BUILD_TESTING)
    add_executable(eod_present_timing_test tools/tests/present_timing_test.cpp)
    add_test(NAME VulkanPresentTiming COMMAND eod_present_timing_test)
    add_executable(eod_gs_pixel_math_test tools/tests/gs_pixel_math_test.cpp)
    add_test(NAME GSPixelArithmetic COMMAND eod_gs_pixel_math_test)
    add_executable(eod_gs_sprite_edges_test tools/tests/gs_sprite_edges_test.cpp)
    add_test(NAME GSSpriteEdges COMMAND eod_gs_sprite_edges_test)
    add_executable(eod_gs_page_coverage_test tools/tests/gs_page_coverage_test.cpp runtime/gs/gs_mem.cpp)
    add_test(NAME GSPageCoverage COMMAND eod_gs_page_coverage_test)
    add_executable(eod_gpu_upload_blocks_test tools/tests/gpu_upload_blocks_test.cpp runtime/gs/gs_mem.cpp)
    add_test(NAME GPUUploadBlocks COMMAND eod_gpu_upload_blocks_test)
    add_executable(eod_gs_transfer_chunk_test tools/tests/gs_transfer_chunk_test.cpp)
    add_test(NAME GSTransferChunks COMMAND eod_gs_transfer_chunk_test)
    add_executable(eod_gs_cpu_write_fixture tools/tests/gs_cpu_write_fixture.cpp runtime/gs/gs_mem.cpp)
    if(Python3_FOUND)
      add_test(NAME VulkanGSCPUWrites COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_cpu_writes.py --eod $<TARGET_FILE:eod>
        --generator $<TARGET_FILE:eod_gs_cpu_write_fixture> --work-dir ${CMAKE_BINARY_DIR}/gs-cpu-write-test)
      set_tests_properties(VulkanGSCPUWrites PROPERTIES SKIP_RETURN_CODE 77)
      add_test(NAME VulkanGSCPUWrites6x COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_cpu_writes.py --eod $<TARGET_FILE:eod>
        --generator $<TARGET_FILE:eod_gs_cpu_write_fixture> --scale 6 --work-dir ${CMAKE_BINARY_DIR}/gs-cpu-write-test-6x)
      set_tests_properties(VulkanGSCPUWrites6x PROPERTIES SKIP_RETURN_CODE 77)
      foreach(upload_scale 1 6)
        add_test(NAME VulkanGSUploads${upload_scale}x COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_cpu_writes.py --eod $<TARGET_FILE:eod>
          --generator $<TARGET_FILE:eod_gs_cpu_write_fixture> --scale ${upload_scale}
          --gpu-uploads 1 --work-dir ${CMAKE_BINARY_DIR}/gs-upload-test-${upload_scale}x)
        set_tests_properties(VulkanGSUploads${upload_scale}x PROPERTIES SKIP_RETURN_CODE 77)
      endforeach()
    endif()
    add_executable(eod_gs_pixel_fixture tools/tests/gs_pixel_fixture.cpp runtime/gs/gs_mem.cpp)
    if(Python3_FOUND)
      if(ZLIB_FOUND)
        add_test(NAME GSCaptureReplay COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_capture_replay.py --eod $<TARGET_FILE:eod>
          --generator $<TARGET_FILE:eod_gs_pixel_fixture>
          --work-dir ${CMAKE_BINARY_DIR}/gs-capture-replay-test)
        set_tests_properties(GSCaptureReplay PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
      endif()
      add_test(NAME VulkanGSPixels COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_pixels.py --eod $<TARGET_FILE:eod>
        --generator $<TARGET_FILE:eod_gs_pixel_fixture> --work-dir ${CMAKE_BINARY_DIR}/gs-pixel-test)
      set_tests_properties(VulkanGSPixels PROPERTIES SKIP_RETURN_CODE 77)
      foreach(alpha_scale 1 6)
        add_test(NAME VulkanGSAlphaRange${alpha_scale}x COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_alpha_range.py --eod $<TARGET_FILE:eod>
          --generator $<TARGET_FILE:eod_gs_pixel_fixture> --scale ${alpha_scale}
          --work-dir ${CMAKE_BINARY_DIR}/gs-alpha-range-${alpha_scale}x)
        set_tests_properties(VulkanGSAlphaRange${alpha_scale}x PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME VulkanGSStateCache${alpha_scale}x COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_state_cache.py --eod $<TARGET_FILE:eod>
          --generator $<TARGET_FILE:eod_gs_pixel_fixture> --scale ${alpha_scale}
          --work-dir ${CMAKE_BINARY_DIR}/gs-state-cache-${alpha_scale}x)
        set_tests_properties(VulkanGSStateCache${alpha_scale}x PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME VulkanGSTextureHash${alpha_scale}x COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/tests/test_gs_state_cache.py --eod $<TARGET_FILE:eod>
          --generator $<TARGET_FILE:eod_gs_pixel_fixture> --scale ${alpha_scale}
          --optimization texture-hash --work-dir ${CMAKE_BINARY_DIR}/gs-texture-hash-${alpha_scale}x)
        set_tests_properties(VulkanGSTextureHash${alpha_scale}x PROPERTIES SKIP_RETURN_CODE 77)
      endforeach()
    endif()
  endif()
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

# Optional personal macOS app; game data is never included in the bundle.
if(APPLE)
  target_sources(eod PRIVATE runtime/app/macos_vulkan.cpp runtime/app/macos_app.mm runtime/app/disc_import.cpp runtime/app/iso9660.cpp)
  target_link_libraries(eod PRIVATE "-framework Cocoa")
  set_source_files_properties(runtime/app/macos_app.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  option(EOD_MAC_APP "Build a local, ad-hoc signed macOS application bundle" OFF)
  if(EOD_MAC_APP)
    set(EOD_APP_OUTPUT "$ENV{HOME}/Library/Application Support/TDEODRecomp/local-build"
      CACHE PATH "Local app output directory (use a non-cloud-synced directory for codesign)")
    add_custom_target(eod_app ALL
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/macos/bundle.py"
        "${EOD_APP_OUTPUT}/Eve of Destruction.app" "$<TARGET_FILE:eod>"
      DEPENDS eod VERBATIM)
  endif()
endif()
include(CTest)
if(BUILD_TESTING AND NOT WIN32)
  add_executable(iso9660_test tests/iso9660_test.cpp runtime/app/iso9660.cpp runtime/app/disc_import.cpp)
  add_test(NAME iso9660 COMMAND iso9660_test)
endif()
if(BUILD_TESTING AND NOT WIN32)
  add_executable(disc_import_cli tests/disc_import_cli.cpp runtime/app/iso9660.cpp runtime/app/disc_import.cpp)
endif()
if(APPLE AND BUILD_TESTING)
  add_executable(macos_startup_test tests/macos_startup_test.mm runtime/app/macos_app.mm
    runtime/app/disc_import.cpp runtime/app/iso9660.cpp runtime/core/config.cpp)
  target_link_libraries(macos_startup_test PRIVATE "-framework Cocoa")
  add_test(NAME macos_startup_headless COMMAND macos_startup_test)
  set_tests_properties(macos_startup_headless PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
if(APPLE AND BUILD_TESTING)
  add_executable(macos_vulkan_paths_test tests/macos_vulkan_paths_test.cpp runtime/app/macos_vulkan.cpp)
  add_test(NAME macos_vulkan_paths COMMAND macos_vulkan_paths_test)
  set_tests_properties(macos_vulkan_paths PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
if(APPLE AND BUILD_TESTING AND SDL3_FOUND)
  add_executable(audio_volume_test tests/audio_volume_test.cpp runtime/core/config.cpp runtime/core/prof.cpp)
  target_include_directories(audio_volume_test PRIVATE runtime/include runtime ${GEN_DIR})
  target_link_libraries(audio_volume_test PRIVATE SDL3::SDL3)
  add_test(NAME audio_volume_offline COMMAND audio_volume_test)
  set_tests_properties(audio_volume_offline PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
if(BUILD_TESTING AND Python3_FOUND)
  set(NATIVE_WEAPON_REFERENCE "${CMAKE_CURRENT_BINARY_DIR}/native_weapon_reference.c")
  add_custom_command(OUTPUT "${NATIVE_WEAPON_REFERENCE}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/extract_native_weapon_reference.py" "${GEN_DIR}" "${NATIVE_WEAPON_REFERENCE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/extract_native_weapon_reference.py" ${GEN_SOURCES} VERBATIM)
  add_executable(native_weapon_test tests/native_weapon_test.cpp "${NATIVE_WEAPON_REFERENCE}")
  target_include_directories(native_weapon_test PRIVATE runtime/include)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(native_weapon_test PRIVATE -fwrapv -fno-strict-aliasing -ffp-contract=off)
  endif()
  add_test(NAME native_chicken_weapon COMMAND native_weapon_test)
  set_tests_properties(native_chicken_weapon PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")

  set(MOVIE_BITS_REFERENCE "${CMAKE_CURRENT_BINARY_DIR}/movie_bits_reference.c")
  add_custom_command(OUTPUT "${MOVIE_BITS_REFERENCE}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/extract_movie_bits_reference.py" "${GEN_DIR}" "${MOVIE_BITS_REFERENCE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/extract_movie_bits_reference.py" ${GEN_SOURCES} VERBATIM)
  add_executable(movie_bits_test tests/movie_bits_test.cpp src/movie_bits.cpp "${MOVIE_BITS_REFERENCE}")
  target_include_directories(movie_bits_test PRIVATE runtime/include runtime)
  target_compile_definitions(movie_bits_test PRIVATE EOD_MOVIE_BITS_TEST)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(movie_bits_test PRIVATE -fwrapv -fno-strict-aliasing)
  endif()
  add_test(NAME movie_bits_differential COMMAND movie_bits_test)
  set_tests_properties(movie_bits_differential PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
if(BUILD_TESTING AND NOT WIN32)
  add_executable(display_composition_test tests/display_composition_test.cpp runtime/core/config.cpp)
  if(APPLE AND SDL3_FOUND)
    target_sources(display_composition_test PRIVATE runtime/platform/sdl3/platform_sdl3.cpp runtime/platform/sdl3/sdl_input.cpp)
    target_link_libraries(display_composition_test PRIVATE SDL3::SDL3)
    target_compile_definitions(display_composition_test PRIVATE EOD_DISPLAY_BACKEND_TEST)
    target_include_directories(display_composition_test PRIVATE runtime/include runtime ${GEN_DIR})
  endif()
  add_test(NAME display_composition_cpu COMMAND display_composition_test)
  set_tests_properties(display_composition_cpu PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()

if(BUILD_TESTING AND NOT WIN32)
  add_executable(eod_worker_shutdown_test tools/tests/worker_shutdown_test.cpp
    runtime/hw/offload.cpp runtime/platform/posix/platform_posix.cpp)
  target_include_directories(eod_worker_shutdown_test PRIVATE runtime/include runtime)
  target_compile_definitions(eod_worker_shutdown_test PRIVATE EOD_SDL3)
  target_link_libraries(eod_worker_shutdown_test PRIVATE Threads::Threads)
  add_test(NAME WorkerShutdown COMMAND eod_worker_shutdown_test)
  set_tests_properties(WorkerShutdown PROPERTIES TIMEOUT 10 ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
