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

# BV-5.3: WC3_DEBUG_TAGS is the single source of subsystem tags.  A tag is a
# compile definition only in a debug build; release never activates one even
# if a caller passes a non-empty list.
set(WC3_DEBUG_TAG_DEFINITIONS)
if(WC3_DEBUG)
    foreach(_wc3_tag IN LISTS WC3_DEBUG_TAGS)
        string(STRIP "${_wc3_tag}" _wc3_tag)
        if(_wc3_tag)
            string(TOUPPER "${_wc3_tag}" _wc3_tag)
            list(APPEND WC3_DEBUG_TAG_DEFINITIONS "WC3_DEBUG_${_wc3_tag}=1")
        endif()
    endforeach()
endif()
if(WC3_DEBUG_TAG_DEFINITIONS)
    list(REMOVE_DUPLICATES WC3_DEBUG_TAG_DEFINITIONS)
    add_compile_definitions(${WC3_DEBUG_TAG_DEFINITIONS})
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

    # Winlator C touch engine, vendored under third_party.  It is a pinned
    # third-party input library, not imported build logic: the overlay adapter
    # in runtime/platform/android/controls is the only consumer.  Vendor code
    # keeps its own warnings/strings (R9/VENDOR_MANIFEST.md).
    file(GLOB_RECURSE WC3_WINLATOR_INPUT_SOURCES CONFIGURE_DEPENDS
        "${WC3_ROOT}/third_party/winlator-input/*.c")
    add_library(winlator_input STATIC ${WC3_WINLATOR_INPUT_SOURCES})
    target_include_directories(winlator_input PUBLIC
        "${WC3_ROOT}/third_party/winlator-input")
    target_compile_options(winlator_input PRIVATE -w)
    set_target_properties(winlator_input PROPERTIES
        C_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON)

    # Overlay host boundary: profile/view model, software RGBA8 renderer,
    # Android SurfaceView presentation, the input ring/wake/adapter, and the
    # direct-JNI haptics sink.  No guest ABI or legacy header is visible here;
    # the action bridge (runtime/app) is the only file that reaches the shim.
    add_library(wc3_overlay STATIC
        "${WC3_ROOT}/runtime/platform/android/controls/controls.c"
        "${WC3_ROOT}/runtime/platform/android/controls/haptics_android.c"
        "${WC3_ROOT}/runtime/platform/android/controls/input_adapter.c"
        "${WC3_ROOT}/runtime/platform/android/controls/input_ring.c"
        "${WC3_ROOT}/runtime/platform/android/controls/input_wake.c"
        "${WC3_ROOT}/runtime/platform/android/controls/overlay.c"
        "${WC3_ROOT}/runtime/platform/android/controls/overlay_android.c"
        "${WC3_ROOT}/runtime/platform/android/controls/overlay_canvas.c"
        "${WC3_ROOT}/runtime/platform/android/controls/overlay_profile.c")
    target_include_directories(wc3_overlay PUBLIC
        "${WC3_ROOT}/runtime/platform/android/controls"
        "${WC3_ROOT}/runtime/diag"
        "${WC3_ROOT}/third_party/winlator-input")
    target_link_libraries(wc3_overlay PUBLIC winlator_input android log
        jnigraphics m)
    target_compile_definitions(wc3_overlay PUBLIC
        WC3_DEBUG=${WC3_DEBUG_DEFINE})
    target_compile_options(wc3_overlay PRIVATE -Wall -Wextra -O2)
    set_target_properties(wc3_overlay PROPERTIES
        C_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON)

    add_library(wc3gameactivity SHARED
        "${WC3_ROOT}/apps/android_gameactivity/src/main/cpp/host_shell.c")
    target_include_directories(wc3gameactivity PRIVATE
        ${WC3_INCLUDES}
        "${WC3_ROOT}/runtime/app")
    target_link_libraries(wc3gameactivity PRIVATE
        wc3_platform
        wc3_overlay
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
