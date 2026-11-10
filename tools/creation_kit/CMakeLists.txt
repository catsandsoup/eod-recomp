cmake_minimum_required(VERSION 3.16)
project(eod_creation_kit_checks LANGUAGES NONE)
include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(EOD_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
foreach(suite IN ITEMS creation_kit_isolation modding_backend texture_preview texture_edit track_environment vif_preview asset_registry texture_export mod_package mod_install vif_patch world_collision eod_mcp sky_export vehicle_export vehicle_import world_export)
  add_test(NAME CreationKit_${suite}
    COMMAND "${Python3_EXECUTABLE}" "${EOD_REPOSITORY}/tools/tests/test_${suite}.py")
  set_tests_properties(CreationKit_${suite} PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endforeach()
if(APPLE)
  find_program(SWIFT_EXECUTABLE swift REQUIRED)
  add_test(NAME CreationKit_Swift COMMAND "${SWIFT_EXECUTABLE}" test
    --package-path "${EOD_REPOSITORY}/tools/editor_macos" --build-system native
    --scratch-path "${CMAKE_CURRENT_BINARY_DIR}/swift-tests")
  set_tests_properties(CreationKit_Swift PROPERTIES ENVIRONMENT "EOD_HEADLESS=1;EOD_MUTE=1")
endif()
