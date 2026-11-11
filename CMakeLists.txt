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
    set(WC3_WITH_ASBUILT_HLE_DEFAULT ON)
else()
    set(WC3_WITH_ASBUILT_HLE_DEFAULT OFF)
endif()

option(WC3_BUILD_PLATFORM_TEST "Build the arm64 device platform test" ON)
option(WC3_BUILD_APP "Build libwc3gameactivity.so" OFF)
option(WC3_WITH_ASBUILT_HLE "Link the transitional lifted corpus and as-built HLE"
       ${WC3_WITH_ASBUILT_HLE_DEFAULT})
# O-51/O-52: the NDK defaults to compiler-rt emulated TLS for the Android
# target, so every x87/SoftFloat THREAD_LOCAL access becomes a call to
# __emutls_get_address.  Native ELF TLS is supported by bionic on this
# product's minSdk 26, but the legacy libsoftfloat.a is an emulated-TLS input:
# enabling this option also rebuilds the same 302-object SoftFloat surface from
# third_party/softfloat/android_sources.txt.  Product builds with the relifted
# corpus use native TLS by default; the corpus-less legacy exact-baseline build
# keeps the old emulated-TLS archive.
set(_wc3_native_tls_default OFF)
if(ANDROID AND WC3_RELIFTED_CORPUS_ROOT)
    set(_wc3_native_tls_default ON)
endif()
option(WC3_NATIVE_TLS
       "Use native ELF TLS and rebuild SoftFloat from the pinned source list"
       ${_wc3_native_tls_default})
# O-52: initial-exec is not deployable for a dlopen'ed Android app library.
# Bionic only allocates a static TLS offset before the process starts; after
# that, an IE relocation in a dlopen'ed module fails at load with
# "using IE access model".  Keep the evidence in the audit, but never let a
# build select the dead end.
#
# O-52: CPU-owned SoftFloat state is the product default whenever the relifted
# corpus is present.  It is not an opt-in performance switch: a plain product
# configure must not silently fall back to the older TLS path.  A corpus-less
# legacy exact-baseline build is the only supported reason to leave it OFF.
set(_wc3_softfloat_context_default OFF)
if(ANDROID AND WC3_RELIFTED_CORPUS_ROOT)
    set(_wc3_softfloat_context_default ON)
endif()
option(WC3_SOFTFLOAT_CONTEXT
       "Pass CPU-owned SoftFloat state explicitly through the x87 hot path"
       ${_wc3_softfloat_context_default})
set(WC3_TLS_MODEL "global-dynamic" CACHE STRING
    "TLS model for native-TLS product code (only global-dynamic is valid)")
set_property(CACHE WC3_TLS_MODEL PROPERTY STRINGS global-dynamic)
if(NOT WC3_TLS_MODEL STREQUAL "global-dynamic")
    message(FATAL_ERROR
        "WC3_TLS_MODEL='${WC3_TLS_MODEL}' is not supported: bionic rejects "
        "initial-exec TLS in a dlopen'ed Android app library (static_offset "
        "== SIZE_MAX).  Use global-dynamic or WC3_SOFTFLOAT_CONTEXT=ON.")
endif()
# O-55: hardening is the product default; this switch only makes the
# unmeasured A/B possible without editing the build.  Turning it off removes
# the stack canary and Fortify from non-corpus targets (the corpus already
# carries the same flags), so it must be paired with PM-22/T0/T1 evidence.
option(WC3_HOST_HARDENING
       "Keep NDK stack-protector and Fortify on non-corpus targets" ON)
option(WC3_DEBUG "Build with debug-only diagnostics" OFF)
# Phase 2 of docs/THIN_D3D9_COPY_PLAN.md.  D3DUSAGE_WRITEONLY promises the
# application will not read the resource, so the copy-in from DXVK's mapping
# into the low shadow is not observable.  This is part of the normal Android
# graphics path; a debug build can still force the old copy-in with
# WC3_THIN_WO_SKIP for a no-rebuild A/B.
set(_wc3_thin_skip_writeonly_default OFF)
if(ANDROID)
    set(_wc3_thin_skip_writeonly_default ON)
endif()
option(WC3_THIN_SKIP_WRITEONLY_COPYIN
       "Skip the thin D3D9 buffer copy-in for D3DUSAGE_WRITEONLY resources"
       ${_wc3_thin_skip_writeonly_default})
if(WC3_THIN_SKIP_WRITEONLY_COPYIN)
    set(WC3_THIN_SKIP_WRITEONLY_COPYIN_DEFINE 1)
else()
    set(WC3_THIN_SKIP_WRITEONLY_COPYIN_DEFINE 0)
endif()
# P0-1/P0-2/P0-6 are part of the normal relifted-corpus build.  They require
# the generated corpus; the legacy prebuilt chunks cannot be rewritten.  The
# passes stay individually addressable for an exactness A/B, but a product
# configure must not silently produce the older, slower form.
set(_wc3_lift_opt_default OFF)
if(ANDROID AND WC3_RELIFTED_CORPUS_ROOT)
    set(_wc3_lift_opt_default ON)
endif()
# P0-1: replace dispatch(c, VA) with the direct lifted entry point where the
# callee is known to the same corpus.  The pass is self-tested and keeps the
# HLE/hash path for every unresolved target.
option(WC3_DEVIRTUALIZE_P0_1
       "Apply the P0-1 direct-call LiftPass to relifted corpus chunks"
       ${_wc3_lift_opt_default})
# P0-2: conservative, straight-line dead-flag elimination.  Control flow and
# calls are barriers; the pass never removes a flag across either.
option(WC3_DEAD_FLAGS_P0_2
       "Apply the P0-2 dead-flag LiftPass to relifted corpus chunks"
       ${_wc3_lift_opt_default})
# P0-6: elide only the return-address store before a direct call whose callee
# proves it never reads the entry [esp] slot.  The callee's return sequence and
# the esp slot are unchanged.
option(WC3_RETURN_ADDRESS_ELIDE_P0_6
       "Apply the P0-6 return-address-store LiftPass to relifted corpus chunks"
       ${_wc3_lift_opt_default})
# P0-10 stays OFF until a corpus audit proves no live PF/AF read; it is a
# semantic variant, not a free compiler flag.
option(WC3_FLAGS_FAST_PF_AF
       "P0-10: skip PF/AF computation in flag helpers" OFF)
set(WC3_FLAGS_FAST_PF_AF_AUDIT "" CACHE FILEPATH
    "flag_read_audit JSON for the active corpus (required by P0-10)")
if(WC3_FLAGS_FAST_PF_AF)
    if(NOT WC3_FLAGS_FAST_PF_AF_AUDIT OR
       NOT EXISTS "${WC3_FLAGS_FAST_PF_AF_AUDIT}")
        message(FATAL_ERROR
            "P0-10 requires WC3_FLAGS_FAST_PF_AF_AUDIT=<report.json> from "
            "build/lift-tools/flag_read_audit.exe; an unaudited build must "
            "keep the option OFF.")
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
# O-43: packed dynamic relocations are supported by bionic on the product's
# minSdk 26.  They are part of every Android product build: the smaller load
# image is the default, not an opt-in experiment.
set(_wc3_pack_dyn_relocs_default OFF)
if(ANDROID)
    set(_wc3_pack_dyn_relocs_default ON)
endif()
option(WC3_PACK_DYN_RELOCS
       "Pack dynamic relocations with lld (Android only)"
       ${_wc3_pack_dyn_relocs_default})
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
# O-64: lld can fold byte-identical function bodies.  Safe mode is the product
# default: it refuses bodies whose address is significant and is the only mode
# that may be reached through the guest dispatcher.  ICF changes artifact
# identity and makes dladdr-based crash symbolization ambiguous; keep
# --print-icf-sections, RELACOUNT and cold-start data tied to the artifact.
set(_wc3_icf_default OFF)
if(ANDROID)
    set(_wc3_icf_default ON)
endif()
option(WC3_ICF
       "Fold identical function bodies with lld (Android only)"
       ${_wc3_icf_default})
set(WC3_ICF_MODE "safe" CACHE STRING
    "Identical code folding mode: safe or all")
set_property(CACHE WC3_ICF_MODE PROPERTY STRINGS safe all)
option(WC3_ICF_PRINT_SECTIONS
       "Pass --print-icf-sections when ICF is enabled" ON)
# O-65: replace external PLT calls with GOT loads.  This is part of the normal
# Android product build; keep the option only so an explicit exactness/baseline
# A/B can turn it off without editing CMake.
set(_wc3_fno_plt_default OFF)
if(ANDROID)
    set(_wc3_fno_plt_default ON)
endif()
option(WC3_FNO_PLT
       "Compile with -fno-plt (Android only)"
       ${_wc3_fno_plt_default})
if(WC3_ICF)
    if(NOT ANDROID)
        message(FATAL_ERROR "WC3_ICF requires the Android toolchain")
    endif()
    if(NOT WC3_ICF_MODE MATCHES "^(safe|all)$")
        message(FATAL_ERROR "WC3_ICF_MODE must be safe or all")
    endif()
    message(STATUS
        "O-64 ICF is enabled (${WC3_ICF_MODE}); record "
        "--print-icf-sections and RELACOUNT with the artifact.")
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

# W3L / PvPGN / rubattle compatibility profile.  This is a build profile, not
# a third applicationId: the exact baseline is WC3_W3L_COMPAT=OFF, and the
# compat code and corpus edits exist only when it is ON.  The profile edits
# generated C through the compiled w3l_compat LiftPass; no source file under
# build/corpus-full is modified in place.  See docs/W3L_COMPAT.md.
if(WC3_RELIFTED_CORPUS_ROOT AND WC3_BUILD_APP AND WC3_WITH_ASBUILT_HLE)
    set(WC3_W3L_COMPAT_DEFAULT ON)
else()
    set(WC3_W3L_COMPAT_DEFAULT OFF)
endif()
option(WC3_W3L_COMPAT
       "Apply the W3L/PvPGN/rubattle compatibility profile to the relifted corpus"
       ${WC3_W3L_COMPAT_DEFAULT})
set(WC3_W3L_PROFILE "rubattle" CACHE STRING
    "W3L compat profile: rubattle (PvPGN + rubattle extras) or pvpgn")
set_property(CACHE WC3_W3L_PROFILE PROPERTY STRINGS rubattle pvpgn)
set(WC3_W3L_LATENCY "100" CACHE STRING
    "wl27 delay_reducer latency in milliseconds (30..255)")
if(WC3_W3L_COMPAT)
    if(NOT WC3_RELIFTED_CORPUS_ROOT)
        message(FATAL_ERROR
            "WC3_W3L_COMPAT=ON requires WC3_RELIFTED_CORPUS_ROOT; the legacy "
            "prebuilt corpus cannot be patched through a LiftPass.  Pass "
            "-W3LCompat off for an exact baseline build.")
    endif()
    if(NOT WC3_BUILD_APP OR NOT WC3_WITH_ASBUILT_HLE)
        message(FATAL_ERROR
            "WC3_W3L_COMPAT=ON requires WC3_BUILD_APP=ON and "
            "WC3_WITH_ASBUILT_HLE=ON; the compat LiftPass and wc3_compat "
            "library are part of the product link graph.")
    endif()
    if(NOT WC3_W3L_PROFILE MATCHES "^(rubattle|pvpgn)$")
        message(FATAL_ERROR
            "WC3_W3L_PROFILE must be rubattle or pvpgn, got '${WC3_W3L_PROFILE}'")
    endif()
    if(NOT WC3_W3L_LATENCY MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "WC3_W3L_LATENCY must be an integer in 30..255, got "
            "'${WC3_W3L_LATENCY}'")
    elseif(WC3_W3L_LATENCY LESS 30 OR WC3_W3L_LATENCY GREATER 255)
        message(FATAL_ERROR
            "WC3_W3L_LATENCY must be in the wl27 range 30..255, got "
            "${WC3_W3L_LATENCY}")
    endif()
    message(STATUS
        "w3l compat: profile ${WC3_W3L_PROFILE}, latency ${WC3_W3L_LATENCY} ms")
endif()

include("${WC3_ROOT}/cmake/Wc3CompilerCache.cmake")
# O-65: this is a compile-time option, so it must be set before the target
# definitions below.  It is on in the normal Android product build.
if(WC3_FNO_PLT)
    message(STATUS
        "O-65 -fno-plt is enabled; record PM-8/PM-19 and cold-start with the "
        "artifact.")
    add_compile_options(-fno-plt)
endif()
# O-51/O-52: this must be a directory-level option, before every target that
# can contain a THREAD_LOCAL object (corpus, boot, shim, HLE, platform and the
# DXVK bridge).  Prebuilt vendor archives are inputs; they are not recompiled
# here and keep their own TLS model.
if(ANDROID AND WC3_NATIVE_TLS)
    add_compile_options(-fno-emulated-tls -ftls-model=global-dynamic)
    message(STATUS
        "O-52: native ELF TLS enabled (global-dynamic); verify T2 and the "
        "removed __emutls_get_address calls before claiming a speedup.")
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
# O-52: no TLS in the x87 hot path.  Generate a SoftFloat source tree whose
# reachable functions take CPU.sf explicitly.  The generated tree is a build
# artifact; vendor source stays read-only.  The relifted corpus is required for
# the same reason as WC3_NATIVE_TLS: prebuilt chunk objects already contain the
# old inline x87 calls and cannot see the context entry points.
if(WC3_SOFTFLOAT_CONTEXT)
    if(NOT ANDROID)
        message(FATAL_ERROR "WC3_SOFTFLOAT_CONTEXT requires the Android toolchain")
    endif()
    if(NOT WC3_RELIFTED_CORPUS_ROOT)
        message(FATAL_ERROR
            "WC3_SOFTFLOAT_CONTEXT=ON requires WC3_RELIFTED_CORPUS_ROOT: the "
            "prebuilt legacy chunk objects keep their old x87/SoftFloat calls. "
            "Product builds default to build/corpus-full; a corpus-less legacy "
            "exact-baseline build must leave the option OFF.")
    endif()
    find_program(WC3_SOFTFLOAT_CONTEXT_GEN_EXE
        NAMES softfloat_context_gen softfloat_context_gen.exe
        HINTS "${WC3_ROOT}/build/lift-tools"
        NO_DEFAULT_PATH)
    if(NOT WC3_SOFTFLOAT_CONTEXT_GEN_EXE)
        message(FATAL_ERROR
            "softfloat_context_gen.exe missing; run "
            "tools/lift/build_tools.ps1 before configuring")
    endif()
    set(WC3_SOFTFLOAT_CTX_DIR "${CMAKE_BINARY_DIR}/softfloat_ctx")
    set(_wc3_ctx_manifest
        "${WC3_ROOT}/third_party/softfloat/android_sources.txt")
    file(STRINGS "${_wc3_ctx_manifest}" _wc3_ctx_rel)
    set(_wc3_ctx_deps
        "${WC3_SOFTFLOAT_CONTEXT_GEN_EXE}"
        "${_wc3_ctx_manifest}")
    foreach(_rel IN LISTS _wc3_ctx_rel)
        string(STRIP "${_rel}" _rel)
        if(_rel STREQUAL "" OR _rel MATCHES "^#")
            continue()
        endif()
        string(REPLACE "\\" "/" _rel "${_rel}")
        list(APPEND _wc3_ctx_deps
            "${WC3_ROOT}/third_party/softfloat/${_rel}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_wc3_ctx_deps})
    set(_wc3_ctx_target "${CMAKE_C_COMPILER_TARGET}")
    if(NOT _wc3_ctx_target)
        set(_wc3_ctx_target "aarch64-linux-android26")
    endif()
    set(_wc3_ctx_stamp "${WC3_SOFTFLOAT_CTX_DIR}/.generated")
    set(_wc3_ctx_stale FALSE)
    if(NOT EXISTS "${WC3_SOFTFLOAT_CTX_DIR}/sources.cmake" OR
       NOT EXISTS "${_wc3_ctx_stamp}")
        set(_wc3_ctx_stale TRUE)
    else()
        foreach(_wc3_ctx_dep IN LISTS _wc3_ctx_deps)
            if("${_wc3_ctx_dep}" IS_NEWER_THAN "${_wc3_ctx_stamp}")
                set(_wc3_ctx_stale TRUE)
                break()
            endif()
        endforeach()
        if("${CMAKE_C_COMPILER}" IS_NEWER_THAN "${_wc3_ctx_stamp}")
            set(_wc3_ctx_stale TRUE)
        endif()
    endif()
    if(_wc3_ctx_stale)
        execute_process(
            COMMAND "${WC3_SOFTFLOAT_CONTEXT_GEN_EXE}"
                --root "${WC3_ROOT}"
                --out "${WC3_SOFTFLOAT_CTX_DIR}"
                --clang "${CMAKE_C_COMPILER}"
                --target "${_wc3_ctx_target}"
                --jobs 8
            RESULT_VARIABLE _wc3_ctx_result
            OUTPUT_VARIABLE _wc3_ctx_stdout
            ERROR_VARIABLE _wc3_ctx_stderr)
        if(NOT _wc3_ctx_result EQUAL 0)
            message(FATAL_ERROR
                "SoftFloat context generation failed (${_wc3_ctx_result}):\n"
                "${_wc3_ctx_stdout}\n${_wc3_ctx_stderr}")
        endif()
        file(TOUCH "${_wc3_ctx_stamp}")
    endif()
    include("${WC3_SOFTFLOAT_CTX_DIR}/sources.cmake")
    if(NOT WC3_SOFTFLOAT_CTX_SOURCES)
        message(FATAL_ERROR "SoftFloat context source list is empty")
    endif()
    add_compile_definitions(WC3_SOFTFLOAT_CONTEXT=1)
    include_directories("${WC3_SOFTFLOAT_CTX_DIR}")
    message(STATUS
        "O-52: explicit SoftFloat state in CPU; ${_wc3_ctx_stdout}")
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
    "${WC3_ROOT}/runtime/platform/android/net_android.c"
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
    if(NOT WC3_WITH_ASBUILT_HLE)
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

    # Transitional first-run link graph: the lifted corpus + the as-built
    # Android HLE, consumed as normal inputs and sharing the registry through
    # runtime/hle/hle_bridge.  The option is ON by default because the product
    # path needs it; a platform-only build can set WC3_WITH_ASBUILT_HLE=OFF.
    if(WC3_WITH_ASBUILT_HLE)
        include("${WC3_ROOT}/cmake/Wc3LegacyBoot.cmake")
    endif()

    # O-51/O-52: the TLS A/B is meaningful only if the final .so is audited.
    # The target checks symbols, the TLS program header and direct emutls call
    # sites; it runs after the link and fails when WC3_NATIVE_TLS=ON still
    # leaves product emutls references.  Prebuilt vendor archives (GameActivity,
    # DXVK, and the toolchain C++ runtime) carry their own emulated-TLS
    # references; those symbols are classified as vendor by name rather than
    # being mistaken for lifted-corpus code.  The C++ runtime is a static
    # toolchain input, not a WC3 source file.
    if(ANDROID)
        find_program(WC3_AUDIT_TLS_EXE
            NAMES audit_tls audit_tls.exe
            HINTS "${WC3_ROOT}/build/lift-tools"
            NO_DEFAULT_PATH)
        if(NOT WC3_AUDIT_TLS_EXE)
            message(FATAL_ERROR
                "audit_tls.exe missing; run tools/lift/build_tools.ps1 "
                "before configuring")
        endif()
        set(_wc3_tls_expect "emulated")
        if(WC3_NATIVE_TLS)
            set(_wc3_tls_expect "native")
        endif()
        set(_wc3_tls_vendor_archives "${GA_ARCHIVE}")
        if(WC3_WITH_ASBUILT_HLE)
            list(APPEND _wc3_tls_vendor_archives ${WC3_DXVK_ARCHIVES})
        endif()
        if(CMAKE_NM)
            get_filename_component(_wc3_ndk_bin "${CMAKE_NM}" DIRECTORY)
            get_filename_component(_wc3_ndk_prebuilt "${_wc3_ndk_bin}" DIRECTORY)
            if(ANDROID_ABI STREQUAL "arm64-v8a")
                set(_wc3_android_triple "aarch64-linux-android")
            elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
                set(_wc3_android_triple "arm-linux-androideabi")
            elseif(ANDROID_ABI STREQUAL "x86")
                set(_wc3_android_triple "i686-linux-android")
            elseif(ANDROID_ABI STREQUAL "x86_64")
                set(_wc3_android_triple "x86_64-linux-android")
            else()
                set(_wc3_android_triple "")
            endif()
            if(_wc3_android_triple)
                foreach(_wc3_cxx_runtime libc++_static.a libc++abi.a)
                    set(_wc3_archive
                        "${_wc3_ndk_prebuilt}/sysroot/usr/lib/${_wc3_android_triple}/${_wc3_cxx_runtime}")
                    if(NOT EXISTS "${_wc3_archive}")
                        message(FATAL_ERROR
                            "TLS audit vendor archive missing: ${_wc3_archive}")
                    endif()
                    list(APPEND _wc3_tls_vendor_archives "${_wc3_archive}")
                endforeach()
            endif()
        endif()
        set(_wc3_tls_tool_args)
        if(CMAKE_NM)
            list(APPEND _wc3_tls_tool_args --nm "${CMAKE_NM}")
        endif()
        if(CMAKE_READELF)
            list(APPEND _wc3_tls_tool_args --readelf "${CMAKE_READELF}")
        endif()
        if(CMAKE_OBJDUMP)
            list(APPEND _wc3_tls_tool_args --objdump "${CMAKE_OBJDUMP}")
        endif()
        set(_wc3_tls_vendor_args)
        foreach(_wc3_archive IN LISTS _wc3_tls_vendor_archives)
            list(APPEND _wc3_tls_vendor_args
                 --vendor-archive "${_wc3_archive}")
        endforeach()
        add_custom_target(wc3_tls_audit
            COMMAND "${WC3_AUDIT_TLS_EXE}"
                --so "$<TARGET_FILE:wc3gameactivity>"
                --expect ${_wc3_tls_expect}
                --json "${CMAKE_CURRENT_BINARY_DIR}/tls_audit.json"
                ${_wc3_tls_tool_args}
                ${_wc3_tls_vendor_args}
            DEPENDS wc3gameactivity
            COMMENT "Auditing ELF TLS model (${_wc3_tls_expect})"
            VERBATIM)
    endif()
endif()
