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
# O-51/O-52: the NDK defaults to compiler-rt emulated TLS for the Android
# target, so every x87/SoftFloat THREAD_LOCAL access becomes a call to
# __emutls_get_address.  Native ELF TLS is supported by bionic on this
# product's minSdk 26, but the legacy libsoftfloat.a is an emulated-TLS input:
# enabling this option also rebuilds the same 302-object SoftFloat surface from
# third_party/softfloat/android_sources.txt.  It is opt-in because T2 has not
# yet proved the dlopen/TLS path; OFF reproduces the product baseline.
option(WC3_NATIVE_TLS
       "Use native ELF TLS and rebuild SoftFloat from the pinned source list" OFF)
# O-55: hardening is the product default; this switch only makes the
# unmeasured A/B possible without editing the build.  Turning it off removes
# the stack canary and Fortify from non-corpus targets (the corpus already
# carries the same flags), so it must be paired with PM-22/T0/T1 evidence.
option(WC3_HOST_HARDENING
       "Keep NDK stack-protector and Fortify on non-corpus targets" ON)
option(WC3_DEBUG "Build with debug-only diagnostics" OFF)
# P0-1 stays OFF until G-14/G-15 are green (GUEST_ABI ABI-6).  The pass is
# implemented and self-tested, but enabling it here is a build decision that
# must be paired with the golden equivalence report.
option(WC3_DEVIRTUALIZE_P0_1
       "Apply the P0-1 direct-call LiftPass to relifted corpus chunks" OFF)
# P0-2 is the conservative, straight-line subset of dead-flag elimination.  It
# is OFF until the G-15 golden report covers this pass as well; a pass that is
# not applied to the corpus cannot be mistaken for a measured speedup.
option(WC3_DEAD_FLAGS_P0_2
       "Apply the P0-2 dead-flag LiftPass to relifted corpus chunks" OFF)
# P0-6 removes only the return-address store before a direct call whose callee
# proves it never reads the entry [esp] slot.  The pass keeps the esp slot, so
# the callee's return sequence is unchanged.  OFF until G-15 carries the
# function-level change set and a whole-corpus run is clean (ABI-6/ABI-11).
option(WC3_RETURN_ADDRESS_ELIDE_P0_6
       "Apply the P0-6 return-address-store LiftPass to relifted corpus chunks"
       OFF)
# P0-10 stays OFF until a corpus audit proves no live PF/AF read; it is a
# semantic variant, not a free compiler flag.
option(WC3_FLAGS_FAST_PF_AF
       "P0-10: skip PF/AF computation in flag helpers" OFF)
set(WC3_FLAGS_FAST_PF_AF_AUDIT "" CACHE FILEPATH
    "flag_read_audit.py JSON for the active corpus (required by P0-10)")
if(WC3_FLAGS_FAST_PF_AF)
    if(NOT WC3_FLAGS_FAST_PF_AF_AUDIT OR
       NOT EXISTS "${WC3_FLAGS_FAST_PF_AF_AUDIT}")
        message(FATAL_ERROR
            "P0-10 requires WC3_FLAGS_FAST_PF_AF_AUDIT=<report.json> from "
            "tools/flag_read_audit.py; an unaudited build must keep the "
            "option OFF.")
    endif()
    file(READ "${WC3_FLAGS_FAST_PF_AF_AUDIT}" _wc3_pf_af_audit)
    if(NOT "${_wc3_pf_af_audit}" MATCHES "\"status\": \"pass\"" OR
       NOT "${_wc3_pf_af_audit}" MATCHES "\"total\": 0")
        message(FATAL_ERROR
            "P0-10 audit is not a pass: ${WC3_FLAGS_FAST_PF_AF_AUDIT}")
    endif()
    set(WC3_FLAGS_FAST_PF_AF_DEFINE 1)
    message(WARNING
        "P0-10 skips PF/AF computation.  Keep the audit report tied to the "
        "exact corpus and keep a full CPU flag snapshot (PM-10); Storm "
        "currently fails the audit.")
else()
    set(WC3_FLAGS_FAST_PF_AF_DEFINE 0)
endif()
# O-43 experiment.  Packed dynamic relocations are supported by bionic on the
# product's minSdk 26, but the size/startup effect is unmeasured (PM-19/GB-5.2),
# so the product link keeps the unpacked table unless this option is enabled.
option(WC3_PACK_DYN_RELOCS
       "Experimental: pack dynamic relocations with lld (Android only)" OFF)
# O-70: lld supports RELR and APS2 either separately or together.  Keep the
# mode explicit so the A/B run can compare `android`, `relr` and
# `android+relr` without changing the product default (the option above stays
# OFF).  DT_RELR loading on the product's minSdk 26 is a T2 check.
set(WC3_PACK_DYN_RELOCS_MODE "android" CACHE STRING
    "Dynamic relocation packing: android, relr, or android+relr")
set_property(CACHE WC3_PACK_DYN_RELOCS_MODE PROPERTY STRINGS
    android relr android+relr)
# O-42: release exposes only JNI_OnLoad, android_main and Java_* from the
# shared object.  Debug keeps the full dynamic symbol table for on-device crash
# diagnostics.  Release crash triage uses the module base plus the unstripped
# build .symtab through tools/symbolize_crash.py; the product link is not yet
# verified.
option(WC3_HIDE_DYNSYM
       "Hide every defined dynamic symbol except the JNI surface" OFF)
# O-64: lld can fold byte-identical function bodies (safe mode refuses bodies
# whose address is significant).  The option is OFF by default: ICF changes
# artifact identity, makes dladdr-based crash symbolization ambiguous and can
# give one address several guest VAs in the generated dispatch map.  Enable
# it only for an A/B
# run that also records --print-icf-sections, RELACOUNT and cold-start data,
# and keep the resulting artifact out of the product until T1/G-15 pass.
option(WC3_ICF
       "Experimental: fold identical function bodies with lld (Android only)" OFF)
set(WC3_ICF_MODE "safe" CACHE STRING
    "Identical code folding mode: safe or all")
set_property(CACHE WC3_ICF_MODE PROPERTY STRINGS safe all)
option(WC3_ICF_PRINT_SECTIONS
       "Pass --print-icf-sections when ICF is enabled" ON)
# O-65: replace external PLT calls with GOT loads.  The effect is unmeasured and
# can be negative, so this is an explicit experiment, not a product default.
option(WC3_FNO_PLT
       "Experimental: compile with -fno-plt (Android only)" OFF)
if(WC3_ICF)
    if(NOT ANDROID)
        message(FATAL_ERROR "WC3_ICF requires the Android toolchain")
    endif()
    if(NOT WC3_ICF_MODE MATCHES "^(safe|all)$")
        message(FATAL_ERROR "WC3_ICF_MODE must be safe or all")
    endif()
    message(WARNING
        "O-64 ICF is enabled (${WC3_ICF_MODE}).  It changes artifact identity; "
        "record --print-icf-sections, RELACOUNT and T1/G-15 evidence, and do "
        "not treat the result as a measured speedup.")
endif()
if(WC3_FNO_PLT AND NOT ANDROID)
    message(FATAL_ERROR "WC3_FNO_PLT requires the Android toolchain")
endif()
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
# O-65: this is a compile-time experiment, so it must be set before the target
# definitions below.  It is never part of the product default.
if(WC3_FNO_PLT)
    message(WARNING
        "O-65 -fno-plt is enabled.  Record PM-8/PM-19 and cold-start before "
        "comparing it with the default PLT build.")
    add_compile_options(-fno-plt)
endif()
# O-51/O-52: this must be a directory-level option, before every target that
# can contain a THREAD_LOCAL object (corpus, boot, shim, HLE, platform and the
# DXVK bridge).  Prebuilt vendor archives are inputs; they are not recompiled
# here and keep their own TLS model.
if(ANDROID AND WC3_NATIVE_TLS)
    add_compile_options(-fno-emulated-tls)
    message(STATUS
        "O-52: native ELF TLS enabled; verify T2 and the removed "
        "__emutls_get_address calls before claiming a speedup.")
endif()
# O-55: this is an opt-in baseline, never the product default.  The corpus
# already sets these flags; this block extends the same release policy to the
# remaining host targets (boot, shim, HLE, legacy and the DXVK bridge).
if(ANDROID AND NOT WC3_HOST_HARDENING AND NOT WC3_DEBUG)
    message(WARNING
        "O-55 host hardening is disabled.  Record PM-22 (so_bytes, "
        "compile_seconds), T0/T1 and the guest-visible delta; do not ship "
        "this configuration without an ADR/BV-10 decision.")
    add_compile_options(
        -fno-stack-protector
        -U_FORTIFY_SOURCE
        -D_FORTIFY_SOURCE=0)
endif()
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
    "${WC3_ROOT}/runtime/platform/android/asset_snapshot.c"
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
    if(WC3_PACK_DYN_RELOCS)
        if(NOT ANDROID)
            message(FATAL_ERROR "WC3_PACK_DYN_RELOCS requires the Android toolchain")
        endif()
        if(NOT WC3_PACK_DYN_RELOCS_MODE MATCHES "^(android|relr|android\\+relr)$")
            message(FATAL_ERROR
                "WC3_PACK_DYN_RELOCS_MODE must be android, relr, or android+relr")
        endif()
        # O-43/O-70: keep this opt-in until a T2 run proves bionic API 26
        # loads the selected table and PM-22/GB-5.2 show a benefit over the
        # unpacked image.
        target_link_options(wc3gameactivity PRIVATE
            "-Wl,--pack-dyn-relocs=${WC3_PACK_DYN_RELOCS_MODE}")
    endif()
    if(WC3_ICF)
        # O-64: safe mode is the only supported default for a body that may be
        # reached through the guest dispatcher; --icf=all is an explicit A/B
        # experiment, never a silent product choice.
        target_link_options(wc3gameactivity PRIVATE
            "-Wl,--icf=${WC3_ICF_MODE}")
        if(WC3_ICF_PRINT_SECTIONS)
            target_link_options(wc3gameactivity PRIVATE
                -Wl,--print-icf-sections)
        endif()
    endif()
    if(WC3_HIDE_DYNSYM)
        if(NOT ANDROID)
            message(FATAL_ERROR "WC3_HIDE_DYNSYM requires the Android toolchain")
        endif()
        target_link_options(wc3gameactivity PRIVATE
            "-Wl,--version-script=${WC3_ROOT}/cmake/wc3_exports.map")
        # A version-script edit changes the output but not the linker command
        # line, so Ninja would otherwise keep a stale .so after the map changes.
        set_property(TARGET wc3gameactivity APPEND PROPERTY
            LINK_DEPENDS "${WC3_ROOT}/cmake/wc3_exports.map")
    endif()
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
