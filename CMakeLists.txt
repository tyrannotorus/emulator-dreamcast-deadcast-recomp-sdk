cmake_minimum_required(VERSION 3.24)
project(deadcast_recomp_sdk VERSION 1.0.0 LANGUAGES C CXX)

# Deadcast's recomp modules: recompiled builds of single Dreamcast games, each an executable Deadcast runs
# instead of its emulation. engine/ translates a game's SH-4 code to C++ at build time and provides the runtime
# it runs on; src/framework/ is the port layer every module shares. A module is its own
# repository, built from the builder's own disc: tools/build-module.sh. DREAMCOMP_MODULE_DIR names it.

# Engine defaults (set before the engine's own option() calls, which then keep them).
set(DREAM_WERROR OFF CACHE BOOL "Warnings as errors in engine targets")
# The generated code is split once per hardware thread so it compiles in parallel;
# -DDREAM_GEN_SHARDS=1 keeps it in one file.
cmake_host_system_information(RESULT _dc_threads QUERY NUMBER_OF_LOGICAL_CORES)
if(_dc_threads LESS 1)
  set(_dc_threads 1)
elseif(_dc_threads GREATER 64)
  set(_dc_threads 64)
endif()
set(DREAM_GEN_SHARDS ${_dc_threads} CACHE STRING "Parts the generated game code is split into")

# NextUI handhelds (TrimUI Brick): the renderer's window on /dev/fb0 (engine/render,
# DREAM_RENDER_NEXTUI), the handheld's SDL2 for sound, Deadcast's menu. A host build (no
# DREAMCOMP_NEXTUI) is for the translator only.
option(DREAMCOMP_NEXTUI "Build the modules for a NextUI handheld" OFF)
if(DREAMCOMP_NEXTUI)
  set(DREAM_RENDER_NEXTUI ON CACHE BOOL "" FORCE)
endif()
# Every build computes the same floating point: no implicit fused multiply-adds (clang fuses a*b+c
# by default on ARM64, which changes FTRV's and FIPR's rounding). FMAC is fused explicitly with
# std::fmaf (sh4/ops.h). The game's targets get the same in dreamcomp_add_port.
set(DREAMCOMP_FP_OPTIONS $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-ffp-contract=off>
                         $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-ffp-contract=off>)
add_compile_options(${DREAMCOMP_FP_OPTIONS})
add_subdirectory(engine)
if(NOT DREAMCOMP_NEXTUI)
  return()
endif()
include(cmake/Port.cmake)
# The engine's warnings for the port layer too (dream_target_defaults also changes floating point,
# which the port layer keeps as it is).
set(DREAMCOMP_WARNINGS
    "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wno-sign-conversion>")

# The port layer linked into the game executable. An OBJECT library, because its extensions
# register themselves from static constructors that a static library would drop.
add_library(dreamcomp_core OBJECT src/framework/core.cpp src/framework/settings.cpp
                                  src/framework/hud.cpp src/framework/sharp2d.cpp
                                  src/framework/host_link.cpp)
target_include_directories(dreamcomp_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dreamcomp_core PUBLIC dream::runtime dream::render)
target_compile_features(dreamcomp_core PUBLIC cxx_std_20)
target_compile_options(dreamcomp_core PRIVATE ${DREAMCOMP_WARNINGS})
target_compile_definitions(dreamcomp_core PRIVATE DREAMCOMP_NEXTUI)
if(TARGET dream_render_vk)
  target_link_libraries(dreamcomp_core PUBLIC dream::render_vk)
  target_compile_definitions(dreamcomp_core PRIVATE DREAM_WITH_RENDERER)
endif()

# The module
if(NOT DREAMCOMP_MODULE_DIR)
  message(FATAL_ERROR "DREAMCOMP_MODULE_DIR is not set (tools/build-module.sh)")
endif()
add_subdirectory(${DREAMCOMP_MODULE_DIR}/game ${CMAKE_BINARY_DIR}/module)
