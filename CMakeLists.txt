cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" EOD_VERSION LIMIT_COUNT 1)
project(eod VERSION ${EOD_VERSION} LANGUAGES C CXX)
if(NOT DEFINED PROJECT_VERSION_PATCH OR PROJECT_VERSION_PATCH STREQUAL "")
  set(PROJECT_VERSION_PATCH 0)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
set_source_files_properties(runtime/ui/overlay_core.cpp PROPERTIES COMPILE_DEFINITIONS "EOD_RECOMP_VERSION=\"${PROJECT_VERSION}\"")
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
# The stamp gen/.inputs.sha256 (written by tools/recomp.py) fingerprints the
# ELF, config/ overrides and generator scripts; any change regenerates gen/.
set(EOD_GEN_STALE FALSE)
if(NOT EXISTS ${GEN_DIR}/table.c)
  set(EOD_GEN_STALE TRUE)
elseif(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_inputs.py --check ${GEN_DIR}
                  RESULT_VARIABLE r0 OUTPUT_QUIET ERROR_QUIET)
  if(r0)
    set(EOD_GEN_STALE TRUE)
  endif()
endif()
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_inputs.py --list
                  OUTPUT_VARIABLE EOD_GEN_INPUTS OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "\n" ";" EOD_GEN_INPUTS "${EOD_GEN_INPUTS}")
  foreach(f ${EOD_GEN_INPUTS})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
  endforeach()
endif()
if(EOD_GEN_STALE)
  if(NOT Python3_FOUND)
    message(FATAL_ERROR "gen/ is missing or out of date and Python 3 was not found. Run: python3 tools/analyze.py && python3 tools/recomp.py")
  endif()
  message(STATUS "Generating recompiled sources (gen/ missing or inputs changed)...")
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/analyze.py
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESULT_VARIABLE r1)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp.py
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESULT_VARIABLE r2)
  if(r1 OR r2)
    message(FATAL_ERROR "code generation failed")
  endif()
endif()
# compiled VU micro programs (optional: the interpreter runs everything without them)
option(EOD_R7_RELEASE "R7 release build: physically exclude VU0/VU1 interpreters and legacy GS rasterizer" OFF)
option(EOD_PUBLIC_VU_INTERPRETER "Use the interpreter without embedding captured VU instruction tables" OFF)
if(Python3_FOUND AND NOT EOD_R7_RELEASE AND NOT EOD_PUBLIC_VU_INTERPRETER AND NOT EXISTS ${GEN_DIR}/vu_rec.inc)
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
  ${CMAKE_SOURCE_DIR}/runtime/vfs/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/spu/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/render/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.c)
# The optional standalone Plume scene library owns this source.
list(REMOVE_ITEM RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/render/plume_scene.cpp)
list(APPEND RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/main.cpp ${CMAKE_SOURCE_DIR}/runtime/app/online_profile.cpp)
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
  # The in-game front end (runtime/ui; overlay.cpp is the Win32 plumbing) and Dear ImGui,
  # drawn by the Vulkan presenter on macOS.
  set(IMGUI_SOURCES ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
      ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp)
  set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
  list(APPEND PLATFORM_SOURCES ${IMGUI_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/ui/overlay_core.cpp
       ${CMAKE_SOURCE_DIR}/runtime/ui/frontend.cpp ${CMAKE_SOURCE_DIR}/runtime/ui/frontend_draw.cpp
       ${CMAKE_SOURCE_DIR}/runtime/ui/ui_art.cpp ${CMAKE_SOURCE_DIR}/runtime/ui/ui_preview.cpp)
endif()

if(WIN32)
  # Vulkan is the default Windows GS backend; Direct3D 11 stays selectable
  # (Renderer=d3d11). No Vulkan SDK is needed: the headers are fetched and volk
  # opens the system loader (vulkan-1.dll) at run time.
  option(EOD_VULKAN "Build the Vulkan GS backend" ON)
  if(EOD_VULKAN)
    file(GLOB VULKAN_GS_SOURCES ${CMAKE_SOURCE_DIR}/runtime/gs/vulkan/*.cpp)
    list(APPEND PLATFORM_SOURCES ${VULKAN_GS_SOURCES})
  endif()
else()
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

if(EOD_VULKAN AND (WIN32 OR APPLE))
  # Windows uses Plume by default. macOS can opt in to the shared Vulkan device
  # over MoltenVK while the standalone Vulkan GS remains the default.
  option(EOD_PLUME "Plume render interface owns the Vulkan device" ${WIN32})
  if(WIN32 OR EOD_PLUME)
    include(FetchContent)
    FetchContent_Declare(eod_vulkan_headers
      URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/vulkan-sdk-1.4.357.0
      URL_HASH SHA256=e87dce08116151f6b6d7de6b6faf41498e87e6cf848ff16fa3bd5402190ad4a3
      SOURCE_SUBDIR none
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(eod_volk
      URL https://codeload.github.com/zeux/volk/tar.gz/refs/tags/vulkan-sdk-1.4.357.0
      URL_HASH SHA256=6400c7b23e24d17e4f04bac49b55b06c4e87677d33398e90344743ec73560ca6
      SOURCE_SUBDIR none
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(eod_vulkan_headers eod_volk)
    # The Plume Vulkan translation unit supplies the one volk implementation.
    if(EOD_PLUME)
      # GitHub tarballs omit submodules: VMA is fetched at the revision plume pins.
      FetchContent_Declare(eod_plume
        URL https://codeload.github.com/renderbag/plume/tar.gz/b619e50125fca1631dddbf8b5d8e8eee8734bcec
        URL_HASH SHA256=f8367cab0cb2ccdb3804410a501eec3e6df4c3ccd2a86b5b92426e3c27545b45
        PATCH_COMMAND git apply --whitespace=nowarn ${CMAKE_SOURCE_DIR}/cmake/patches/plume-create-options.patch
        SOURCE_SUBDIR none
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_Declare(eod_vma
        URL https://codeload.github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tar.gz/29b35ea4232688c0f42cdff0c10848290760a417
        URL_HASH SHA256=c33da36cbb9328129e8c7e482107c2620b19c9a585c861e749884c4c575bf9bd
        SOURCE_SUBDIR none
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(eod_plume eod_vma)
      add_library(eod_plume STATIC ${eod_plume_SOURCE_DIR}/plume_vulkan.cpp)
      if(APPLE)
        target_sources(eod_plume PRIVATE ${eod_plume_SOURCE_DIR}/plume_apple.mm)
        set_property(TARGET eod_plume PROPERTY OBJCXX_STANDARD 17)
        target_link_libraries(eod_plume PRIVATE "-framework AppKit" "-framework IOKit")
      endif()
      target_include_directories(eod_plume SYSTEM PUBLIC ${eod_plume_SOURCE_DIR} ${eod_volk_SOURCE_DIR}
                                 ${eod_vulkan_headers_SOURCE_DIR}/include ${eod_vma_SOURCE_DIR}/include)
      # NDEBUG always: plume's debug builds turn on the validation layer and require
      # VK_EXT_debug_utils, which would change the shipped device.
      target_compile_definitions(eod_plume PRIVATE NDEBUG NOMINMAX WIN32_LEAN_AND_MEAN)
      target_compile_options(eod_plume PRIVATE -w)
      add_library(eod_plume_scene STATIC runtime/render/plume_scene.cpp)
      target_include_directories(eod_plume_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/render)
      target_link_libraries(eod_plume_scene PUBLIC eod_plume)
      set_target_properties(eod_plume eod_plume_scene PROPERTIES POSITION_INDEPENDENT_CODE ON)
      add_library(eod_editor_plume SHARED tools/world_editor/plume_surface.cpp)
      target_link_libraries(eod_editor_plume PRIVATE eod_plume_scene)
      target_compile_definitions(eod_editor_plume PRIVATE VK_NO_PROTOTYPES)
      set_target_properties(eod_editor_plume PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
      if(APPLE)
        target_sources(eod_editor_plume PRIVATE tools/world_editor/plume_surface_apple.mm)
        target_link_libraries(eod_editor_plume PRIVATE "-framework AppKit" "-framework QuartzCore")
        target_compile_definitions(eod_editor_plume PRIVATE EOD_VULKAN_LOADER="$<TARGET_FILE:Vulkan::Vulkan>")
      endif()
    else()
      list(APPEND PLATFORM_SOURCES ${eod_volk_SOURCE_DIR}/volk.c)
    endif()
  endif()
endif()

if(EOD_R7_RELEASE)
  if(NOT TARGET eod_plume_scene)
    message(FATAL_ERROR "EOD_R7_RELEASE requires the Vulkan and Plume native renderer targets")
  endif()
  # The Vulkan translation unit directly implements gs::hw in this build.
  foreach(sources RUNTIME_SOURCES PLATFORM_SOURCES)
    list(REMOVE_ITEM ${sources}
      ${CMAKE_SOURCE_DIR}/runtime/gs/d3d11/gs_d3d11.cpp
      ${CMAKE_SOURCE_DIR}/runtime/gs/gs_hw_select.cpp
      ${CMAKE_SOURCE_DIR}/runtime/gs/gs_hw_stub.cpp
      ${CMAKE_SOURCE_DIR}/runtime/vu/scene_emit.cpp
      ${CMAKE_SOURCE_DIR}/runtime/vu/reference_control.cpp)
  endforeach()
  message(STATUS "R7 release build: physically excluding VU0/VU1 interpreters and legacy GS rasterizer")
else()
  list(REMOVE_ITEM RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/vu/unretired.cpp)
endif()

option(EOD_TRACE_CALLS "record recent EE function calls for debugging" OFF)

add_library(eod_gen OBJECT ${GEN_SOURCES})
if(EOD_TRACE_CALLS)
  add_compile_definitions(EE_TRACE_CALLS)
endif()
target_include_directories(eod_gen PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include ${GEN_DIR})

add_executable(eod ${RUNTIME_SOURCES} ${PLATFORM_SOURCES} $<TARGET_OBJECTS:eod_gen>)
set_property(SOURCE src/netplay_session.cpp APPEND PROPERTY COMPILE_DEFINITIONS EOD_RELEASE_VERSION="${PROJECT_VERSION}")
set(EOD_RELAY_ENDPOINT "$ENV{EOD_RELAY_ENDPOINT}" CACHE STRING "Garage Code rendezvous hostname and UDP port")
if(NOT EOD_RELAY_ENDPOINT)
  set(EOD_RELAY_ENDPOINT "152.67.103.52:27016")
endif()
if(NOT EOD_RELAY_ENDPOINT MATCHES "^[A-Za-z0-9.-]+(:[0-9]+)?$")
  message(FATAL_ERROR "EOD_RELAY_ENDPOINT must be a hostname or IPv4 address with an optional port")
endif()
set_property(SOURCE src/netplay_relay.cpp APPEND PROPERTY COMPILE_DEFINITIONS EOD_RELAY_ENDPOINT="${EOD_RELAY_ENDPOINT}")
if(EOD_PUBLIC_VU_INTERPRETER)
  target_compile_definitions(eod PRIVATE EOD_PUBLIC_VU_INTERPRETER=1)
endif()
target_include_directories(eod PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include ${CMAKE_SOURCE_DIR}/runtime ${GEN_DIR})

# Optional standard gzip transport for bounded diagnostic cold captures.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(eod PRIVATE ZLIB::ZLIB)
  target_compile_definitions(eod PRIVATE EOD_HAVE_ZLIB)
endif()

include(CTest)
if(BUILD_TESTING)
  if(TARGET eod_plume_scene)
    add_executable(plume_scene_test tools/tests/plume_scene_test.cpp)
    target_link_libraries(plume_scene_test PRIVATE eod_plume_scene)
    target_compile_definitions(plume_scene_test PRIVATE VK_NO_PROTOTYPES)
    if(APPLE)
      target_compile_definitions(plume_scene_test PRIVATE EOD_VULKAN_LOADER="$<TARGET_FILE:Vulkan::Vulkan>")
    endif()
    add_test(NAME PlumeScene COMMAND plume_scene_test)
    set_tests_properties(PlumeScene PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  add_executable(plume_s5_budget_test tools/tests/plume_s5_budget_test.cpp)
  target_include_directories(plume_s5_budget_test PRIVATE runtime)
  add_test(NAME PlumeS5Budget COMMAND plume_s5_budget_test)
  set_tests_properties(PlumeS5Budget PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(plume_pacing_cull_test tools/tests/plume_pacing_cull_test.cpp)
  target_include_directories(plume_pacing_cull_test PRIVATE runtime runtime/include)
  add_test(NAME PlumePacingCull COMMAND plume_pacing_cull_test)
  set_tests_properties(PlumePacingCull PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(editor_test_startup_test tools/tests/editor_test_startup_test.cpp)
  add_test(NAME EditorTestStartup COMMAND editor_test_startup_test)
  if(Python3_FOUND)
    add_test(NAME AssetTools COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_custom_car.py)
    add_test(NAME AudioTools COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_audio.py)
    add_test(NAME ModProfiles COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_mod_profiles.py)
    add_test(NAME GameplayMod COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_gameplay_mod.py)
    add_test(NAME NascarImport COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_nascar_import.py)
    add_test(NAME NascarParts COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_nascar_parts.py)
    add_test(NAME Rst0Alignment COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_rst0_alignment.py)
    add_test(NAME CareerContent COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_career_content.py)
    add_test(NAME SymbolsDB COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_symbols.py)
    set_tests_properties(AssetTools NascarImport NascarParts GameplayMod AudioTools ModProfiles Rst0Alignment CareerContent SymbolsDB PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1;EOD_RUN_GAME_TESTS=0;EOD_RUN_NASCAR_GAME=0")
  endif()
  add_executable(eod_nascar_wheel_hook_test tools/tests/nascar_wheel_hook_test.cpp src/hle_nascar_wheels.cpp src/hle_nascar_class.cpp)
  target_include_directories(eod_nascar_wheel_hook_test PRIVATE runtime runtime/include src)
  add_test(NAME NascarWheelHook COMMAND eod_nascar_wheel_hook_test)
  add_executable(nascar_class_test tools/tests/nascar_class_test.cpp src/hle_nascar_class.cpp)
  target_include_directories(nascar_class_test PRIVATE src)
  add_test(NAME NascarClass4 COMMAND nascar_class_test)
  set_tests_properties(NascarClass4 PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(nascar_audio_gear_test tools/tests/nascar_audio_gear_test.cpp src/hle_nascar_audio.cpp)
  target_include_directories(nascar_audio_gear_test PRIVATE runtime src)
  target_compile_definitions(nascar_audio_gear_test PRIVATE EOD_NASCAR_AUDIO_HOST_ONLY)
  add_test(NAME NascarAudioGear COMMAND nascar_audio_gear_test)
  set_tests_properties(NascarAudioGear PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  if(UNIX)
    foreach(overlay_kind fileio stream)
      add_executable(eod_${overlay_kind}_overlay_test tools/tests/mod_overlay_test.cpp runtime/vfs/layered_vfs.cpp)
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
  add_executable(layered_vfs_test tools/tests/layered_vfs_test.cpp runtime/vfs/layered_vfs.cpp)
  target_include_directories(layered_vfs_test PRIVATE runtime runtime/include)
  add_test(NAME LayeredVfs COMMAND layered_vfs_test)
  set_tests_properties(LayeredVfs PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
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
  if(NOT WIN32)
    add_executable(eod_texture_replacements_test tools/tests/texture_replacements_test.cpp
      runtime/gs/texture_replacements.cpp)
    target_include_directories(eod_texture_replacements_test PRIVATE runtime)
    find_package(Threads REQUIRED)
    target_link_libraries(eod_texture_replacements_test PRIVATE Threads::Threads)
    add_test(NAME HostTextureReplacements COMMAND eod_texture_replacements_test)
    set_tests_properties(HostTextureReplacements PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  endif()
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
  add_executable(eod_live_cheats_test tools/tests/live_cheats_test.cpp src/game_patches.cpp src/battle_driver.cpp src/battle_navigation.cpp runtime/core/config.cpp runtime/core/eodsave.cpp)
  target_include_directories(eod_live_cheats_test PRIVATE runtime runtime/include src gen)
  add_test(NAME LiveCheats COMMAND eod_live_cheats_test)
  add_executable(eod_career_unlocks_test tools/tests/career_unlocks_test.cpp src/hle_career.cpp src/hle_showroom.cpp src/career_upgrades.cpp src/hle_nascar_class.cpp runtime/core/config.cpp runtime/core/eodsave.cpp)
  target_include_directories(eod_career_unlocks_test PRIVATE runtime runtime/include src gen)
  add_test(NAME CareerUnlocks COMMAND eod_career_unlocks_test)
  add_executable(eod_showroom_camera_test tools/tests/showroom_camera_test.cpp src/hle_showroom.cpp src/career_upgrades.cpp src/hle_nascar_class.cpp runtime/core/config.cpp runtime/core/eodsave.cpp)
  target_include_directories(eod_showroom_camera_test PRIVATE runtime runtime/include src gen)
  add_test(NAME ShowroomCamera COMMAND eod_showroom_camera_test)
  set_tests_properties(ShowroomCamera PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(ui_menu_engine_test tests/ui_menu_engine_test.cpp src/ui_menu_engine.cpp)
  target_include_directories(ui_menu_engine_test PRIVATE runtime runtime/include src)
  target_compile_definitions(ui_menu_engine_test PRIVATE TEST_ENVIRONMENT)
  add_test(NAME UIMenuEngine COMMAND ui_menu_engine_test)
  set_tests_properties(UIMenuEngine PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(ui_menu_crash_experiment_test tests/ui_menu_crash_experiment_test.cpp src/ui_menu_engine.cpp)
  target_include_directories(ui_menu_crash_experiment_test PRIVATE runtime runtime/include src)
  target_compile_definitions(ui_menu_crash_experiment_test PRIVATE TEST_ENVIRONMENT)
  add_test(NAME UIMenuCrashExperiment COMMAND ui_menu_crash_experiment_test)
  set_tests_properties(UIMenuCrashExperiment PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
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
  add_executable(session_start_test tests/session_start_test.cpp src/session_start.cpp runtime/core/config.cpp)
  target_include_directories(session_start_test PRIVATE runtime runtime/include src)
  add_test(NAME session_start_parser COMMAND session_start_test)
  add_executable(arcade_mod_badges_test tests/arcade_mod_badges_test.cpp src/arcade_mod_badges.cpp
                 runtime/gs/texture_replacements.cpp)
  target_include_directories(arcade_mod_badges_test PRIVATE runtime runtime/include)
  find_package(Threads REQUIRED)
  target_link_libraries(arcade_mod_badges_test PRIVATE Threads::Threads)
  add_test(NAME ArcadeModBadges COMMAND arcade_mod_badges_test "${CMAKE_CURRENT_BINARY_DIR}/test-arcade-mod-badges")
  add_executable(frontend_menu_test tests/frontend_menu_test.cpp runtime/ui/frontend.cpp runtime/core/config.cpp)
  target_include_directories(frontend_menu_test PRIVATE runtime runtime/include)
  add_test(NAME frontend_menu COMMAND frontend_menu_test)
  set_tests_properties(frontend_menu PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  # Custom music: decoder wrapper, shuffle ID mapping, mixer substitution, library scan
  add_executable(custom_music_test tests/custom_music_test.cpp runtime/iop/music_core.cpp runtime/iop/music_decode.cpp
                 runtime/iop/custom_music.cpp)
  target_include_directories(custom_music_test PRIVATE runtime runtime/include)
  if(APPLE)
    target_link_libraries(custom_music_test PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
  elseif(WIN32)
    target_link_libraries(custom_music_test PRIVATE mfplat mfreadwrite mfuuid ole32)
  endif()
  find_package(Threads)
  if(Threads_FOUND)
    target_link_libraries(custom_music_test PRIVATE Threads::Threads)
  endif()
  add_test(NAME CustomMusic COMMAND custom_music_test)
  set_tests_properties(CustomMusic PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(battle_bots_test tests/battle_bots_test.cpp src/battle_bots.cpp)
  target_include_directories(battle_bots_test PRIVATE runtime runtime/include src)
  add_test(NAME battle_bot_construction COMMAND battle_bots_test)
  set_tests_properties(battle_bot_construction PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_test(NAME battle_driver_controls COMMAND battle_driver_test)
  set_tests_properties(battle_driver_controls PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  set_tests_properties(LiveCheats PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_lod_runtime_test tools/tests/lod_runtime_test.cpp src/hle_lod.cpp src/hle_draw_distance.cpp src/visual_car_list.cpp runtime/core/config.cpp)
  target_include_directories(eod_lod_runtime_test PRIVATE runtime runtime/include src)
  foreach(lod_mode off one two trace invalid high maximum override)
    add_test(NAME LODRuntime-${lod_mode} COMMAND eod_lod_runtime_test ${lod_mode})
    set_tests_properties(LODRuntime-${lod_mode} PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  endforeach()
  add_test(NAME LODRuntime-maximum-distance COMMAND eod_lod_runtime_test maximum 8)
  set_tests_properties(LODRuntime-maximum-distance PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  # Compile the three real EE consumers with stubs at their draw/update boundaries.
  if(Python3_FOUND)
    set(EOD_VISUAL_CAR_TEST_GEN ${CMAKE_CURRENT_BINARY_DIR}/visual_car_list_test_gen)
    set(EOD_VISUAL_CAR_TEST_SOURCES
      ${EOD_VISUAL_CAR_TEST_GEN}/rec_045.c
      ${EOD_VISUAL_CAR_TEST_GEN}/rec_046.c
      ${EOD_VISUAL_CAR_TEST_GEN}/rec_047.c)
    set(EOD_VISUAL_CAR_TEST_INPUTS)
    foreach(input IN LISTS EOD_GEN_INPUTS)
      if(EXISTS "${input}")
        list(APPEND EOD_VISUAL_CAR_TEST_INPUTS "${input}")
      endif()
    endforeach()
    add_custom_command(OUTPUT ${EOD_VISUAL_CAR_TEST_SOURCES}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp.py
        --only 20F4A0,218E68,21C370 --out ${EOD_VISUAL_CAR_TEST_GEN}
      DEPENDS ${EOD_VISUAL_CAR_TEST_INPUTS} ${CMAKE_SOURCE_DIR}/build/functions.json
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      VERBATIM)
    set(EOD_TEXTURE_TEST_GEN ${CMAKE_CURRENT_BINARY_DIR}/texture_upload_test_gen)
    set(EOD_TEXTURE_TEST_SOURCES ${EOD_TEXTURE_TEST_GEN}/rec_101.c ${EOD_TEXTURE_TEST_GEN}/rec_102.c ${EOD_TEXTURE_TEST_GEN}/rec_108.c)
    add_custom_command(OUTPUT ${EOD_TEXTURE_TEST_SOURCES}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp.py
        --only 35FC40,360BD8,389500,365878,365618 --out ${EOD_TEXTURE_TEST_GEN}
      DEPENDS ${EOD_VISUAL_CAR_TEST_INPUTS} ${CMAKE_SOURCE_DIR}/build/functions.json
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)
    add_executable(eod_native_texture_upload_test tools/tests/native_texture_upload_test.cpp
      src/native_texture_upload.cpp runtime/core/mmi.c ${EOD_TEXTURE_TEST_SOURCES})
    target_include_directories(eod_native_texture_upload_test PRIVATE runtime runtime/include)
    add_test(NAME NativeTextureUploadProducer COMMAND eod_native_texture_upload_test)
    set_tests_properties(NativeTextureUploadProducer PROPERTIES ENVIRONMENT "EOD_NATIVE_TEXTURE_UPLOAD=1")

    add_executable(eod_visual_car_list_test tools/tests/visual_car_list_test.cpp
      src/visual_car_list.cpp ${EOD_VISUAL_CAR_TEST_SOURCES})
    target_include_directories(eod_visual_car_list_test PRIVATE runtime runtime/include)
    add_test(NAME VisualCarListNative COMMAND eod_visual_car_list_test)
    add_test(NAME VisualCarListOriginal COMMAND eod_visual_car_list_test original)
    add_test(NAME VisualCarListDiff COMMAND eod_visual_car_list_test diff)
    set_tests_properties(VisualCarListNative VisualCarListOriginal PROPERTIES ENVIRONMENT "EOD_DIFF=")
    set_tests_properties(VisualCarListDiff PROPERTIES ENVIRONMENT "EOD_DIFF=218E68")
  endif()
  add_executable(eod_render_queue_test tools/tests/render_queue_test.cpp
    src/render_queue.cpp runtime/render/scene_tap.cpp)
  target_include_directories(eod_render_queue_test PRIVATE runtime runtime/include)
  foreach(queue_mode native original diff)
    add_test(NAME RenderQueue-${queue_mode} COMMAND eod_render_queue_test ${queue_mode})
  endforeach()
  set_tests_properties(RenderQueue-native RenderQueue-original PROPERTIES ENVIRONMENT "EOD_DIFF=")
  set_tests_properties(RenderQueue-diff PROPERTIES ENVIRONMENT "EOD_DIFF=34B140")
  add_executable(eod_alpha_tiles_test tools/tests/alpha_tiles_test.cpp)
  target_include_directories(eod_alpha_tiles_test PRIVATE runtime)
  add_test(NAME AlphaPassTiles COMMAND eod_alpha_tiles_test)
  set_tests_properties(AlphaPassTiles PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_alpha_order_test tools/tests/alpha_order_test.cpp)
  target_include_directories(eod_alpha_order_test PRIVATE runtime)
  add_test(NAME AlphaPassOrder COMMAND eod_alpha_order_test)
  set_tests_properties(AlphaPassOrder PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(eod_physics_transfer_test tools/tests/physics_transfer_test.cpp)
  target_include_directories(eod_physics_transfer_test PRIVATE runtime/include)
  add_test(NAME PhysicsRecordTransfers COMMAND eod_physics_transfer_test)
  add_executable(eod_vehicle_dynamics_test tools/tests/vehicle_dynamics_test.cpp src/vehicle_dynamics.cpp)
  target_include_directories(eod_vehicle_dynamics_test PRIVATE runtime/include)
  if(MSVC)
    target_compile_options(eod_vehicle_dynamics_test PRIVATE /fp:strict)
  else()
    target_compile_options(eod_vehicle_dynamics_test PRIVATE -ffp-contract=off)
  endif()
  add_test(NAME VehicleDynamicsKernel COMMAND eod_vehicle_dynamics_test)
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
  if(TARGET eod_plume)
    # Keep Plume's pinned headers ahead of the system Vulkan headers.
    target_link_libraries(eod PRIVATE eod_plume_scene)
    target_compile_definitions(eod PRIVATE EOD_PLUME VK_NO_PROTOTYPES)
    if(EOD_R7_RELEASE)
      target_compile_definitions(eod PRIVATE EOD_R7_RELEASE=1)
      target_compile_definitions(eod_gen PRIVATE EOD_R7_RELEASE=1)
      set(EOD_R7_LINK_MAP "${CMAKE_CURRENT_BINARY_DIR}/eod-r7-$<CONFIG>.map")
      file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eod-r7-$<CONFIG>-sources.txt"
        CONTENT "eod definitions: $<JOIN:$<TARGET_PROPERTY:eod,COMPILE_DEFINITIONS>, >\neod_gen definitions: $<JOIN:$<TARGET_PROPERTY:eod_gen,COMPILE_DEFINITIONS>, >\n$<JOIN:$<TARGET_PROPERTY:eod,SOURCES>,\n>\n")
      if(APPLE)
        target_link_options(eod PRIVATE "LINKER:-map,${EOD_R7_LINK_MAP}")
      else()
        target_link_options(eod PRIVATE "LINKER:-Map,${EOD_R7_LINK_MAP}")
      endif()
    endif()
    if(APPLE)
      # Preserve the link dependency for the .app packager. Volk binds the
      # loaded library explicitly, using the bundle's loader when present.
      target_compile_definitions(eod PRIVATE EOD_VULKAN_LOADER="$<TARGET_FILE:Vulkan::Vulkan>")
    endif()
  endif()
  if(WIN32)
    target_compile_definitions(eod PRIVATE VK_NO_PROTOTYPES)
    if(NOT EOD_R7_RELEASE)
      target_compile_definitions(eod PRIVATE EOD_HW_SELECT)
    endif()
    target_include_directories(eod PRIVATE ${eod_vulkan_headers_SOURCE_DIR}/include ${eod_volk_SOURCE_DIR})
  else()
    target_link_libraries(eod PRIVATE Vulkan::Vulkan)
  endif()
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
    add_executable(eod_static_immediate_test tools/tests/static_immediate_test.cpp)
    target_include_directories(eod_static_immediate_test PRIVATE runtime)
    add_test(NAME NativeStaticImmediate COMMAND eod_static_immediate_test)
    add_executable(eod_gs_transfer_chunk_test tools/tests/gs_transfer_chunk_test.cpp)
    add_test(NAME GSTransferChunks COMMAND eod_gs_transfer_chunk_test)
    add_executable(eod_gs_cpu_write_fixture tools/tests/gs_cpu_write_fixture.cpp runtime/gs/gs_mem.cpp)
    if(Python3_FOUND AND EOD_R7_RELEASE)
      add_test(NAME R7ReleaseGate COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tools/tests/r7_release_gate.py --eod $<TARGET_FILE:eod>
        --link-map ${EOD_R7_LINK_MAP} --nm ${CMAKE_NM})
      set_tests_properties(R7ReleaseGate PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
    endif()
    if(Python3_FOUND AND NOT EOD_R7_RELEASE)
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
    if(Python3_FOUND AND NOT EOD_R7_RELEASE)
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
    # x86-64-v2 (SSE4.2/POPCNT, every x86-64 CPU since 2009) instead of the 2003 baseline:
    # SSE4.1 blends, rounding and dot products for the VU and recompiled EE code. No FMA
    # contraction, so float results stay bit-identical to the SSE2 build (netplay).
    set(EOD_X86_LEVEL "x86-64-v2" CACHE STRING "x86-64 micro-architecture level (-march)")
    target_compile_options(eod_gen PRIVATE -march=${EOD_X86_LEVEL} -mtune=generic -ffp-contract=off)
    target_compile_options(eod PRIVATE -march=${EOD_X86_LEVEL} -mtune=generic -ffp-contract=off)
  endif()
elseif(MSVC)
  target_compile_options(eod_gen PRIVATE /w /bigobj)
  target_compile_options(eod PRIVATE /W3)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_options(eod_gen PRIVATE /arch:SSE2)
    target_compile_options(eod PRIVATE /arch:SSE2)
  endif()
endif()

# Tracy frame profiler (developer builds only): CPU zones on the EE, VU1 producer and GS
# threads and Vulkan GPU zones (runtime/core/trace.h). Connect tracy-profiler or
# tracy-capture to a running game. Off in release builds: the macros compile to nothing.
option(EOD_TRACY "Tracy profiler zones (developer builds)" OFF)
if(EOD_TRACY)
  include(FetchContent)
  FetchContent_Declare(eod_tracy
    URL https://codeload.github.com/wolfpld/tracy/tar.gz/refs/tags/v0.11.1
    URL_HASH SHA256=2c11ca816f2b756be2730f86b0092920419f3dabc7a7173829ffd897d91888a1
    SOURCE_SUBDIR none
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(eod_tracy)
  target_sources(eod PRIVATE ${eod_tracy_SOURCE_DIR}/public/TracyClient.cpp)
  target_include_directories(eod PRIVATE ${eod_tracy_SOURCE_DIR}/public)
  target_compile_definitions(eod PRIVATE TRACY_ENABLE TRACY_NO_SAMPLING TRACY_NO_SYSTEM_TRACING EOD_TRACY)
  if(WIN32)
    target_link_libraries(eod PRIVATE ws2_32 dbghelp)
  endif()
endif()

if(WIN32)
  target_sources(eod PRIVATE runtime/app/windows_disc.cpp runtime/app/disc_import.cpp runtime/app/iso9660.cpp)
  target_link_libraries(eod PRIVATE comdlg32 shell32 uuid)
  # Windows 7 is the minimum supported version.
  target_compile_definitions(eod PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX
                             IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
                             IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
  if(NOT EOD_R7_RELEASE)
    target_link_libraries(eod PRIVATE d3d11 dxgi)
  endif()
  target_link_libraries(eod PRIVATE gdi32 user32 ws2_32 winmm mfplat mfreadwrite mfuuid ole32)
  if(MINGW)
    target_link_options(eod PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
  configure_file(tools/windows_version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/eod_version.rc" @ONLY)
  target_sources(eod PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/eod_version.rc")
  # settings program (writes eod.ini, starts the game)
  enable_language(RC)
  add_executable(eodsetup WIN32 ${CMAKE_SOURCE_DIR}/tools/setup/eodsetup.cpp ${CMAKE_SOURCE_DIR}/tools/setup/eodsetup.rc
                 ${CMAKE_SOURCE_DIR}/runtime/core/config.cpp ${CMAKE_SOURCE_DIR}/runtime/core/ps2mc.cpp
                 ${CMAKE_SOURCE_DIR}/runtime/core/eodsave.cpp)
  # Validate a changed disc library with the same disc ID and importer as the game.
  foreach(field IN ITEMS size crc32)
    file(STRINGS "${GEN_DIR}/table.c" identity REGEX "^const uint32_t ee_elf_${field} = ")
    string(REGEX MATCH " = ([0-9A-Fa-fx]+)" value "${identity}")
    if(NOT value)
      message(FATAL_ERROR "Generated game disc ${field} is missing")
    endif()
    set(disc_value "${CMAKE_MATCH_1}")
    string(TOUPPER "${field}" field_name)
    target_compile_definitions(eodsetup PRIVATE "EOD_DISC_${field_name}=${disc_value}")
  endforeach()
  target_sources(eodsetup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/eod_version.rc"
                 runtime/app/windows_disc.cpp runtime/app/disc_import.cpp runtime/app/iso9660.cpp)
  target_compile_definitions(eodsetup PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
  if(EOD_R7_RELEASE)
    target_compile_definitions(eodsetup PRIVATE EOD_PLUME EOD_R7_RELEASE=1)
  endif()
  target_link_libraries(eodsetup PRIVATE comctl32 comdlg32 dxgi ole32 shell32 uuid gdi32 user32)
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
    add_executable(eod_pad_rpc_test tools/input_test/pad_rpc_test.cpp runtime/iop/pad.cpp runtime/iop/misc.cpp)
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
  target_sources(eod PRIVATE runtime/app/macos_vulkan.cpp runtime/app/macos_app.mm runtime/app/microphone_permission.mm runtime/app/disc_import.cpp runtime/app/iso9660.cpp)
  target_link_libraries(eod PRIVATE "-framework AVFoundation")
  target_link_libraries(eod PRIVATE "-framework Cocoa" "-framework AudioToolbox" "-framework CoreFoundation")
  set_source_files_properties(runtime/app/macos_app.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  add_executable(eod_mac_mod_profile_test tools/tests/mac_mod_profile_test.cpp
    runtime/app/macos_app.mm runtime/core/config.cpp runtime/app/disc_import.cpp runtime/app/iso9660.cpp)
  target_include_directories(eod_mac_mod_profile_test PRIVATE runtime runtime/include)
  target_link_libraries(eod_mac_mod_profile_test PRIVATE "-framework Cocoa")
  add_test(NAME mac_mod_profiles COMMAND eod_mac_mod_profile_test "${CMAKE_CURRENT_BINARY_DIR}/test-mac-mod-profiles")
  option(EOD_MAC_APP "Build a local, ad-hoc signed macOS application bundle" OFF)
  if(EOD_MAC_APP)
    set(EOD_APP_OUTPUT "$ENV{HOME}/Library/Application Support/TDEODRecomp/local-build"
      CACHE PATH "Local app output directory (use a non-cloud-synced directory for codesign)")
    # Optional owner-only package: a directory containing integrated/ (output of
    # tools/build_integrated_profile.py). Empty keeps the bundle free of disc data.
    set(EOD_BUNDLE_MODS "" CACHE PATH "Directory containing integrated/ to bundle as Resources/Mods (empty = none)")
    if(EOD_BUNDLE_MODS)
      set(EOD_APP_ENV ${CMAKE_COMMAND} -E env "EOD_BUNDLE_MODS=${EOD_BUNDLE_MODS}")
    elseif(NOT CMAKE_VERSION VERSION_LESS 3.25)
      set(EOD_APP_ENV ${CMAKE_COMMAND} -E env --unset=EOD_BUNDLE_MODS)
    else()
      set(EOD_APP_ENV ${CMAKE_COMMAND} -E env)
    endif()
    add_custom_target(eod_app ALL
      COMMAND ${EOD_APP_ENV} ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/macos/bundle.py"
        "${EOD_APP_OUTPUT}/Eve of Destruction.app" "$<TARGET_FILE:eod>"
      DEPENDS eod VERBATIM)
  endif()
endif()
include(CTest)
if(BUILD_TESTING)
  add_executable(host_file_test tests/host_file_test.cpp)
  if(WIN32)
    add_executable(windows_player_library_test tests/windows_player_library_test.cpp runtime/app/windows_disc.cpp
                   runtime/app/disc_import.cpp runtime/app/iso9660.cpp runtime/core/config.cpp)
    target_link_libraries(windows_player_library_test PRIVATE comdlg32 shell32 ole32 uuid)
    if(MINGW)
      target_link_options(windows_player_library_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME windows_player_library COMMAND windows_player_library_test)
    # Exercise the real wide-character platform file APIs without creating UI.
    target_sources(host_file_test PRIVATE runtime/platform/win32/platform_win32.cpp runtime/core/config.cpp)
    target_include_directories(host_file_test PRIVATE runtime/include runtime)
    target_compile_definitions(host_file_test PRIVATE EOD_TEST_WIN_PLATFORM)
    target_link_libraries(host_file_test PRIVATE winmm ws2_32 gdi32)
    if(MINGW)
      target_link_options(host_file_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
  endif()
  add_test(NAME host_file COMMAND host_file_test)
  set_tests_properties(host_file PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
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
if(BUILD_TESTING)
  add_executable(world_visibility_test tests/world_visibility_test.cpp)
  target_include_directories(world_visibility_test PRIVATE runtime runtime/include)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(world_visibility_test PRIVATE -ffp-contract=off)
  endif()
  add_test(NAME world_visibility_boundaries COMMAND world_visibility_test)
endif()
if(BUILD_TESTING AND Python3_FOUND)
  add_test(NAME r7_audit_parser COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/r7_audit_test.py")
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

if(BUILD_TESTING)
  add_executable(netplay_content_test tests/netplay_content_test.cpp)
  target_include_directories(netplay_content_test PRIVATE src)
  add_test(NAME netplay_content COMMAND netplay_content_test)
  if(Python3_FOUND)
    add_test(NAME netplay_garage_relay COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_garage_relay.py)
  endif()
  add_executable(netplay_rules_test tests/netplay_rules_test.cpp)
  target_include_directories(netplay_rules_test PRIVATE src)
  add_test(NAME netplay_rules COMMAND netplay_rules_test)
  add_executable(netplay_event_completion_test tests/netplay_event_completion_test.cpp)
  target_include_directories(netplay_event_completion_test PRIVATE src)
  add_test(NAME netplay_event_completion COMMAND netplay_event_completion_test)
  add_executable(netplay_control_test tests/netplay_control_test.cpp)
  target_include_directories(netplay_control_test PRIVATE src)
  add_test(NAME netplay_control_codec COMMAND netplay_control_test)
endif()
if(BUILD_TESTING AND Python3_FOUND)
  add_test(NAME netplay_trace_compare COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/netplay_compare_test.py)
endif()
include(cmake/NetplayVoice.cmake)
eod_configure_voice(eod)
if(BUILD_TESTING)
  add_executable(music_volume_test tests/music_volume_test.cpp runtime/core/config.cpp)
  target_include_directories(music_volume_test PRIVATE runtime runtime/include)
  add_test(NAME music_volume COMMAND music_volume_test)
  set_tests_properties(music_volume PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
  add_executable(netplay_voice_test tests/netplay_voice_test.cpp src/netplay_voice.cpp)
  target_include_directories(netplay_voice_test PRIVATE src)
  eod_configure_voice(netplay_voice_test)
  add_test(NAME netplay_voice_codec COMMAND netplay_voice_test)
endif()
if(BUILD_TESTING)
  add_executable(netplay_state_test tests/netplay_state_test.cpp src/netplay_car.cpp src/netplay_names.cpp)
  target_include_directories(netplay_state_test PRIVATE src runtime runtime/include)
  add_test(NAME netplay_state_sync COMMAND netplay_state_test)
endif()
if(BUILD_TESTING)
  add_executable(netplay_protocol_test tests/netplay_protocol_test.cpp)
  target_include_directories(netplay_protocol_test PRIVATE src)
  add_test(NAME netplay_protocol_codec COMMAND netplay_protocol_test)
  add_executable(netplay_online_test tests/netplay_online_test.cpp)
  target_include_directories(netplay_online_test PRIVATE src)
  add_test(NAME netplay_online_codec COMMAND netplay_online_test)
  if(NOT WIN32)
    add_executable(netplay_online_probe tests/netplay_online_probe.cpp src/netplay_online.cpp)   # developer probe, not a test
    target_include_directories(netplay_online_probe PRIVATE src)
    find_package(Threads REQUIRED)
    target_link_libraries(netplay_online_probe PRIVATE Threads::Threads)
  endif()
endif()

# ---------------------------------------------------------------- Modding Editor
option(EOD_BUILD_EDITOR "Build legacy experimental C++ editor (independent of game)" OFF)
if(EOD_BUILD_EDITOR)
  add_subdirectory(tools/editor)
endif()

