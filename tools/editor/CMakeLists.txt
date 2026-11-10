cmake_minimum_required(VERSION 3.16)
project(eod_editor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EDITOR_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imguizmo
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
  ${CMAKE_SOURCE_DIR}/runtime/include
  ${CMAKE_SOURCE_DIR}/src
)

# Core editor library sources
set(EDITOR_CORE_SOURCES
  src/app/editor_app.cpp
  src/project/project.cpp
  src/undo/command_stack.cpp
  src/validator/schema_validator.cpp
  src/graphics/camera.cpp
  src/graphics/viewport_renderer.cpp
  src/ui/menu_bar.cpp
  src/ui/toolbar.cpp
  src/ui/scene_hierarchy.cpp
  src/ui/properties_panel.cpp
  src/ui/asset_browser.cpp
  src/ui/viewport_panel.cpp
  src/ui/console_panel.cpp
  src/ui/status_bar.cpp
  src/ui/ui_manager.cpp
)

set(EDITOR_IMGUI_SOURCES
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_sdl3.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
  third_party/imguizmo/ImGuizmo.cpp
)

# -----------------------------------------------------------------------------
# 1. Desktop Editor Application Target: eod_editor
# -----------------------------------------------------------------------------
add_executable(eod_editor
  src/main.cpp
  ${EDITOR_CORE_SOURCES}
  ${EDITOR_IMGUI_SOURCES}
)

target_include_directories(eod_editor PRIVATE ${EDITOR_INCLUDES})
target_compile_definitions(eod_editor PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

if(TARGET SDL3::SDL3)
  target_link_libraries(eod_editor PRIVATE SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
  target_link_libraries(eod_editor PRIVATE SDL3::SDL3-static)
else()
  find_package(SDL3 REQUIRED CONFIG)
  target_link_libraries(eod_editor PRIVATE SDL3::SDL3)
endif()

if(APPLE)
  target_link_libraries(eod_editor PRIVATE
    "-framework Cocoa"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework IOKit"
    "-framework CoreVideo"
  )
endif()

# -----------------------------------------------------------------------------
# 2. Standalone Headless Mod Validator Target: eod_mod_validator
# -----------------------------------------------------------------------------
add_executable(eod_mod_validator
  src/validator/validator_main.cpp
  src/validator/schema_validator.cpp
)

target_include_directories(eod_mod_validator PRIVATE ${EDITOR_INCLUDES})

# -----------------------------------------------------------------------------
# 3. Editor Subsystem Test Suite: eod_editor_tests
# -----------------------------------------------------------------------------
add_executable(eod_editor_tests
  tests/editor_tests.cpp
  src/undo/command_stack.cpp
  src/validator/schema_validator.cpp
  src/project/project.cpp
  src/graphics/camera.cpp
  src/graphics/viewport_renderer.cpp
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
)

target_include_directories(eod_editor_tests PRIVATE ${EDITOR_INCLUDES})
target_compile_definitions(eod_editor_tests PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

if(BUILD_TESTING)
  add_test(NAME EditorSubsystems COMMAND eod_editor_tests)
  set_tests_properties(EditorSubsystems PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")

  add_test(NAME ModValidatorSelfTest COMMAND eod_mod_validator --self-test)
  set_tests_properties(ModValidatorSelfTest PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
