# Canonical Android build for wc3_android_recomp.
#
# There is deliberately no Windows target here.  The product target is the
# Android arm64-v8a libwc3gameactivity.so; the platform test is a device-side
# T0/T2 harness, not a shipped artifact.  The optional headless target lives in
# its own CMake file and is never part of this link graph.
cmake_minimum_required(VERSION 3.22)
project(wc3_android_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# BP-4.2 / BP-3.3: the read-only legacy tree is a pinned INPUT declared in
# config/build_upstream.json.  No machine-specific absolute path belongs in
# this file.  A checkout can override with -DWC3_LEGACY_ROOT=... without
# editing the tree, but the pinned revision is still the source of truth.
set(WC3_UPSTREAM_PIN "${CMAKE_CURRENT_SOURCE_DIR}/config/build_upstream.json")
if(NOT EXISTS "${WC3_UPSTREAM_PIN}")
    message(FATAL_ERROR "upstream pin missing: ${WC3_UPSTREAM_PIN}")
endif()
file(READ "${WC3_UPSTREAM_PIN}" WC3_UPSTREAM_JSON)
string(JSON WC3_UPSTREAM_ROOT GET "${WC3_UPSTREAM_JSON}" upstream path)
string(JSON WC3_UPSTREAM_REV GET "${WC3_UPSTREAM_JSON}" upstream revision)
if(NOT WC3_UPSTREAM_ROOT OR NOT WC3_UPSTREAM_REV)
    message(FATAL_ERROR
        "upstream pin must declare upstream.path and upstream.revision")
endif()
set(WC3_LEGACY_ROOT "${WC3_UPSTREAM_ROOT}" CACHE PATH
    "Read-only legacy tree (pinned upstream.path, rev ${WC3_UPSTREAM_REV})")
message(STATUS "pinned upstream: ${WC3_LEGACY_ROOT} @ ${WC3_UPSTREAM_REV}")

# The boot link graph can consume either the pinned legacy corpus or the
# relifted multi-module corpus produced by corpusclosure.  The relifted root
# contains corpus_manifest.json and wc3_all_list.h; see cmake/Wc3ReliftedCorpus.cmake.
set(WC3_RELIFTED_CORPUS_ROOT "" CACHE PATH
    "Generated multi-module corpus root (e.g. build/corpus-full); empty uses the legacy corpus")

# This default was computed but never used before, so a platform-only build
# without the pinned corpus failed instead of skipping the boot link graph.
if(WC3_RELIFTED_CORPUS_ROOT OR
   EXISTS "${WC3_LEGACY_ROOT}/build/android/boot_probe/probe_list.h")
    set(WC3_WITH_LEGACY_BOOT_DEFAULT ON)
else()
    set(WC3_WITH_LEGACY_BOOT_DEFAULT OFF)
endif()

option(WC3_BUILD_PLATFORM_TEST "Build the arm64 device platform test" ON)
option(WC3_BUILD_APP "Build libwc3gameactivity.so" OFF)
option(WC3_WITH_LEGACY_BOOT "Link the transitional lifted corpus and as-built HLE"
       ${WC3_WITH_LEGACY_BOOT_DEFAULT})
option(WC3_DEBUG "Build with debug-only diagnostics" OFF)
if(WC3_DEBUG)
    set(WC3_DEBUG_DEFINE 1)
else()
    set(WC3_DEBUG_DEFINE 0)
endif()

set(WC3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include("${WC3_ROOT}/cmake/Wc3CompilerCache.cmake")
set(WC3_INCLUDES
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/runtime/boot"
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/platform"
    "${WC3_ROOT}/runtime/platform/android"
    "${WC3_ROOT}/third_party/softfloat/source/include")

add_library(wc3_platform STATIC
    "${WC3_ROOT}/runtime/platform/platform.c"
    "${WC3_ROOT}/runtime/platform/android/platform_android.c")
target_include_directories(wc3_platform PUBLIC
    "${WC3_ROOT}/runtime/platform"
    "${WC3_ROOT}/runtime/platform/android")
target_compile_definitions(wc3_platform PUBLIC WC3_DEBUG=${WC3_DEBUG_DEFINE})

add_library(wc3_guest STATIC
    "${WC3_ROOT}/runtime/guest/guest_memory.c"
    "${WC3_ROOT}/runtime/guest/guest_modules.c"
    "${WC3_ROOT}/runtime/guest/pe.c")
target_include_directories(wc3_guest PUBLIC ${WC3_INCLUDES})
target_link_libraries(wc3_guest PUBLIC wc3_platform)

if(WC3_BUILD_PLATFORM_TEST)
    add_executable(wc3_platform_test
        "${WC3_ROOT}/tests/platform_android_test.c")
    target_link_libraries(wc3_platform_test PRIVATE wc3_guest wc3_platform
        android log aaudio m)
endif()

if(WC3_BUILD_APP)
    if(NOT GAMEACTIVITY_ROOT)
        message(FATAL_ERROR "GAMEACTIVITY_ROOT must point at the extracted games-activity AAR")
    endif()

    set(GA_INCLUDE "${GAMEACTIVITY_ROOT}/prefab/modules/game-activity_static/include")
    set(GA_ARCHIVE
        "${GAMEACTIVITY_ROOT}/prefab/modules/game-activity_static/libs/android.arm64-v8a/libgame-activity_static.a")
    if(NOT EXISTS "${GA_INCLUDE}")
        message(FATAL_ERROR "GameActivity include directory missing: ${GA_INCLUDE}")
    endif()
    if(NOT EXISTS "${GA_ARCHIVE}")
        message(FATAL_ERROR "GameActivity archive missing: ${GA_ARCHIVE}")
    endif()

    add_library(game_activity_static STATIC IMPORTED)
    set_target_properties(game_activity_static PROPERTIES
        IMPORTED_LOCATION "${GA_ARCHIVE}")
    target_include_directories(game_activity_static INTERFACE "${GA_INCLUDE}")

    add_library(wc3gameactivity SHARED
        "${WC3_ROOT}/apps/android_gameactivity/src/main/cpp/host_shell.c")
    target_include_directories(wc3gameactivity PRIVATE ${WC3_INCLUDES})
    target_link_libraries(wc3gameactivity PRIVATE
        wc3_platform
        -Wl,--whole-archive game_activity_static -Wl,--no-whole-archive
        android log aaudio m)
    if(NOT WC3_WITH_LEGACY_BOOT)
        target_link_libraries(wc3gameactivity PRIVATE wc3_guest)
    endif()
    target_compile_options(wc3gameactivity PRIVATE -fPIC -O2 -Wall -Wextra)
    target_link_options(wc3gameactivity PRIVATE
        -Wl,-z,max-page-size=16384
        -Wl,-Bsymbolic
        -Wl,-u,JNI_OnLoad)
    set_target_properties(wc3gameactivity PROPERTIES
        OUTPUT_NAME wc3gameactivity
        PREFIX "lib")

    # Transitional first-run link graph: the lifted corpus + the quarantined
    # as-built HLE, consumed as inputs through runtime/hle/legacy_shim.  The
    # option is ON by default because the product path needs it; a platform-
    # only build can set WC3_WITH_LEGACY_BOOT=OFF.
    if(WC3_WITH_LEGACY_BOOT)
        include("${WC3_ROOT}/cmake/Wc3LegacyBoot.cmake")
    endif()
endif()
