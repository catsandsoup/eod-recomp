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
  file(GLOB PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/win32/*.cpp)
else()
  file(GLOB PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/runtime/platform/posix/*.cpp)
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
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
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
  target_link_libraries(eod PRIVATE gdi32 user32 winmm)
  if(MINGW)
    target_link_options(eod PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(eod PRIVATE Threads::Threads m)
endif()
