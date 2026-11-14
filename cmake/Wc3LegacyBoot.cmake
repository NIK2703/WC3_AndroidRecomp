# Transitional first-run link: lifted corpus + as-built HLE.
#
# The lifted C objects and the as-built Android HLE are normal target inputs.
# They are consumed as pinned sources, not as build scripts.  Target modules
# share the registry through runtime/hle/hle_bridge.
#
# Set WC3_WITH_ASBUILT_HLE=OFF for configurations that only build platform
# targets; the product path in this milestone needs the corpus.

if(NOT DEFINED WC3_FLAGS_FAST_PF_AF_DEFINE)
    set(WC3_FLAGS_FAST_PF_AF_DEFINE 0)
endif()

if(WC3_RELIFTED_CORPUS_ROOT)
    set(_wc3_corpus_kind "relifted")
    set(_wc3_save_list_sites 2)
    get_filename_component(WC3_RELIFTED_ROOT_ABS "${WC3_RELIFTED_CORPUS_ROOT}"
                           ABSOLUTE BASE_DIR "${WC3_ROOT}")
    set(WC3_BOOT_LIST "${WC3_RELIFTED_ROOT_ABS}/wc3_all_list.h")
    set(WC3_RELIFTED_MANIFEST "${WC3_RELIFTED_ROOT_ABS}/corpus_manifest.json")
    if(NOT EXISTS "${WC3_BOOT_LIST}")
        message(FATAL_ERROR "relifted corpus list missing: ${WC3_BOOT_LIST}")
    endif()
    if(NOT EXISTS "${WC3_RELIFTED_MANIFEST}")
        message(FATAL_ERROR "relifted corpus manifest missing: ${WC3_RELIFTED_MANIFEST}")
    endif()
    file(READ "${WC3_RELIFTED_MANIFEST}" _wc3_manifest)
    string(JSON _wc3_verdict GET "${_wc3_manifest}" verdict)
    string(JSON _wc3_merged_sha GET "${_wc3_manifest}" merged sha256)
    string(JSON _wc3_merged_entries GET "${_wc3_manifest}" merged entries)
    if(NOT _wc3_verdict STREQUAL "pass")
        message(FATAL_ERROR "relifted corpus manifest verdict is ${_wc3_verdict}, not pass")
    endif()
    file(SHA256 "${WC3_BOOT_LIST}" _wc3_list_sha)
    if(NOT _wc3_list_sha STREQUAL _wc3_merged_sha)
        message(FATAL_ERROR
            "relifted list hash ${_wc3_list_sha} does not match manifest ${_wc3_merged_sha}")
    endif()
    string(JSON _wc3_npref LENGTH "${_wc3_manifest}" merged prefixes)
    set(WC3_RELIFTED_PREFIXES)
    if(_wc3_npref GREATER 0)
        math(EXPR _wc3_last "${_wc3_npref} - 1")
        foreach(_i RANGE 0 ${_wc3_last})
            string(JSON _p GET "${_wc3_manifest}" merged prefixes ${_i})
            list(APPEND WC3_RELIFTED_PREFIXES "${_p}")
        endforeach()
    endif()
    if(NOT WC3_RELIFTED_PREFIXES)
        message(FATAL_ERROR "relifted corpus manifest has no dispatch prefixes")
    endif()
    # ver-IX86-3.dll is downloaded at Battle.net login, but its DllMain and
    # CheckRevision are lifted code.  A corpus built without the 0x354 module
    # makes probe_load_dll call dispatch_unknown(0x3540287F) and abort the
    # process.  The module used to be added out-of-band and was lost by a
    # relift; fail configure instead of building a product that cannot log in
    # (docs/VER_IX86_DLLMAIN_CRASH.md).
    if(NOT "354" IN_LIST WC3_RELIFTED_PREFIXES)
        message(FATAL_ERROR
            "relifted corpus has no 354 prefix: ver-IX86-3 is missing from the "
            "declared lift inputs. Add it to config/corpus.1.27b.json, relift, "
            "and rebuild the corpus (docs/VER_IX86_DLLMAIN_CRASH.md).")
    endif()
    message(STATUS "relifted corpus: ${_wc3_merged_entries} definitions, list sha256 ${_wc3_list_sha}")
else()
    message(FATAL_ERROR
        "SoftFloat was removed from the product.  The prebuilt legacy corpus "
        "contains SoftFloat calls and cannot be linked; configure with "
        "-DWC3_RELIFTED_CORPUS_ROOT=build/corpus-full after relifting with the "
        "native x87 lifter.")
endif()
if(NOT ANDROID)
    message(FATAL_ERROR "the product corpus requires the Android toolchain")
endif()

# The legacy boot path uses the C gen_boot_dispatch tool; it has no Python
# dependency.  Keep the boot dispatch generated from the same merged list as
# the relifted corpus.

find_program(WC3_GEN_BOOT_DISPATCH_EXE
    NAMES gen_boot_dispatch gen_boot_dispatch.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_GEN_BOOT_DISPATCH_EXE)
    message(FATAL_ERROR
        "gen_boot_dispatch.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring")
endif()

set(WC3_BOOT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/legacy_boot_generated")
set(WC3_BOOT_DISPATCH_LIST "${WC3_BOOT_GEN_DIR}/boot_dispatch_list.h")
# O-24/O-53: the dispatcher map is published as a generated constant instead
# of being rebuilt at every startup.  Both headers come from the same tool run;
# the map stores guest VAs and function pointers directly, so there are no
# g_lift indices that could drift from the generated list.
set(WC3_BOOT_DISPATCH_HASH "${WC3_BOOT_GEN_DIR}/boot_dispatch_hash.h")
file(MAKE_DIRECTORY "${WC3_BOOT_GEN_DIR}")

set(_wc3_prefix_args)
set(_wc3_expect_args)
foreach(_p IN LISTS WC3_RELIFTED_PREFIXES)
    list(APPEND _wc3_prefix_args --prefix "${_p}")
endforeach()
list(APPEND _wc3_expect_args --expect-count "${_wc3_merged_entries}")

add_custom_command(
    OUTPUT
        "${WC3_BOOT_DISPATCH_LIST}"
        "${WC3_BOOT_DISPATCH_HASH}"
    COMMAND "${WC3_GEN_BOOT_DISPATCH_EXE}"
            --list "${WC3_BOOT_LIST}"
            --out "${WC3_BOOT_DISPATCH_LIST}"
            --hash-out "${WC3_BOOT_DISPATCH_HASH}"
            ${_wc3_prefix_args}
            ${_wc3_expect_args}
    DEPENDS
        "${WC3_GEN_BOOT_DISPATCH_EXE}"
        "${WC3_BOOT_LIST}"
    COMMENT "Generating boot dispatcher VA list and hash table"
    VERBATIM)
add_custom_target(wc3_boot_dispatch_gen
    DEPENDS "${WC3_BOOT_DISPATCH_LIST}" "${WC3_BOOT_DISPATCH_HASH}")

# --- target-side boot code ------------------------------------------------
add_library(wc3_boot STATIC
    "${WC3_ROOT}/runtime/boot/app_boot.c"
    "${WC3_ROOT}/runtime/boot/dispatch_boot.c"
    "${WC3_ROOT}/runtime/boot/init_array_probe.c"
    "${WC3_ROOT}/runtime/cpu/dyn_x86.c"
    "${WC3_ROOT}/runtime/diag/wc3_lock_trace.c")
add_dependencies(wc3_boot wc3_boot_dispatch_gen)
target_include_directories(wc3_boot PRIVATE
    "${WC3_BOOT_GEN_DIR}"
    "${WC3_ROOT}/runtime/diag"
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/hle/msvcr80/convert"
    "${WC3_ROOT}/runtime/hle/msvcr80/crt"
    "${WC3_ROOT}/runtime/hle/msvcr80/error"
    "${WC3_ROOT}/runtime/hle/msvcr80/file"
    "${WC3_ROOT}/runtime/hle/msvcr80/stdio"
    "${WC3_ROOT}/runtime/hle/msvcr80/heap"
    "${WC3_ROOT}/runtime/hle/msvcr80/math"
    "${WC3_ROOT}/runtime/hle/msvcr80/rand"
    "${WC3_ROOT}/runtime/hle/msvcr80/secure"
    "${WC3_ROOT}/runtime/hle/msvcr80/sort"
    "${WC3_ROOT}/runtime/hle/msvcr80/string"
    "${WC3_ROOT}/runtime/hle/msvcr80/time"
    "${WC3_ROOT}/runtime/hle/win32/advapi32/registry"
    "${WC3_ROOT}/runtime/hle/win32/comctl32/imagelist"
    "${WC3_ROOT}/runtime/hle/win32/shell32/dragdrop"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/dc"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/device"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/objects"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/palette"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/memory"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/file"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/process"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/resource"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/string"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/time"
    "${WC3_ROOT}/runtime/hle/win32/user32/display"
    "${WC3_ROOT}/runtime/hle/win32/user32/menu"
    "${WC3_ROOT}/runtime/hle/win32/user32/scroll"
    "${WC3_ROOT}/runtime/hle/win32/user32/user_object"
    "${WC3_ROOT}/runtime/hle/win32/user32/window"
    "${WC3_ROOT}/runtime/hle/win32/user32/window_facts"
    "${WC3_ROOT}/runtime/hle/win32/winmm"
    "${WC3_ROOT}/runtime/hle/win32/dsound"
    "${WC3_ROOT}/runtime/hle/win32/ws2_32"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest")
target_compile_definitions(wc3_boot PRIVATE
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE})
if(WC3_DEBUG)
    target_compile_definitions(wc3_boot PRIVATE WC3_DEBUG=1)
endif()
target_compile_options(wc3_boot PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare)
set_target_properties(wc3_boot PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- registry bridge shared by target HLE modules --------------------------
add_library(wc3_hle_bridge STATIC
    "${WC3_ROOT}/runtime/hle/hle_bridge.c"
    "${WC3_ROOT}/runtime/hle/abi/hle_abi.c")
target_include_directories(wc3_hle_bridge PRIVATE
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/hle/abi"
    "${WC3_ROOT}/runtime/hle/android"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/runtime/platform")
target_compile_definitions(wc3_hle_bridge PRIVATE
    WC3_DEBUG=${WC3_DEBUG_DEFINE})
target_compile_options(wc3_hle_bridge PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_hle_bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- optional W3L/PvPGN/rubattle compat helpers ----------------------------
# Not HLE and not part of the exact baseline: this library exists only when
# WC3_W3L_COMPAT=ON, and the generated corpus calls it through the LiftPass.
if(WC3_W3L_COMPAT)
    add_library(wc3_compat STATIC
        "${WC3_ROOT}/runtime/compat/w3l_compat.c")
    target_include_directories(wc3_compat PRIVATE
        "${WC3_ROOT}/runtime/compat"
        "${WC3_ROOT}/runtime/cpu"
        "${WC3_ROOT}/runtime/guest")
    target_compile_definitions(wc3_compat PRIVATE
        GVA_DELTA=0
        WC3_W3L_LATENCY=${WC3_W3L_LATENCY}
        WC3_DEBUG=${WC3_DEBUG_DEFINE})
    target_compile_options(wc3_compat PRIVATE
        -fno-strict-aliasing
        -fwrapv
        -ffp-contract=off
        -Wno-unused-parameter
        -Wno-sign-compare
        -U_FORTIFY_SOURCE
        -D_FORTIFY_SOURCE=0)
    set_target_properties(wc3_compat PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# --- target HLE modules (M5 migration, H6.2 order) -------------------------
# Each module is a normal target translation unit: no as-built headers, OS
# services only through Wc3Platform, and registration over the shared table
# through the bridge.
add_library(wc3_hle_modules STATIC
    "${WC3_ROOT}/runtime/hle/msvcr80/convert/convert_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/crt/crt_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/error/strerror_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/file/file_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/stdio/stdio_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/heap/heap_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/math/math_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/rand/rand_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/secure/secure_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/sort/sort_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/string/string_hle.c"
    "${WC3_ROOT}/runtime/hle/msvcr80/time/crt_time_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/advapi32/registry/registry_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/comctl32/imagelist/imagelist_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/shell32/dragdrop/dragdrop_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/dc/gdi_dc_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/device/gdi_device_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/objects/gdi_obj_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/palette/gdi_palette_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/memory/memory_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/file/kernel32_file_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/process/process_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/process/system_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/resource/resource_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/string/string_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/time/time_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/user32/display/display_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/user32/menu/menu_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/user32/scroll/scroll_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/user32/user_object/user_object_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/user32/window/window_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/user32/window_facts/window_facts_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/winmm/mss_timer_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/winmm/winmm_guard_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/dsound/dsound_hle.c"
    "${WC3_ROOT}/runtime/hle/win32/ws2_32/ws2_32_hle.c")

# DirectSound audio architecture: DirectSound is the only transport.
# mss_timer_hle keeps Mss32's USER32 mixer timer alive; winmm_guard_hle
# makes the frozen waveOut imports explicit no-driver rows; the as-built
# waveOut sink is gone.
# All diagnostics in these slices are WC3_DEBUG-gated.

target_include_directories(wc3_hle_modules PRIVATE
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/diag"
    "${WC3_ROOT}/runtime/hle/msvcr80/convert"
    "${WC3_ROOT}/runtime/hle/msvcr80/crt"
    "${WC3_ROOT}/runtime/hle/msvcr80/error"
    "${WC3_ROOT}/runtime/hle/msvcr80/file"
    "${WC3_ROOT}/runtime/hle/msvcr80/stdio"
    "${WC3_ROOT}/runtime/hle/msvcr80/heap"
    "${WC3_ROOT}/runtime/hle/msvcr80/math"
    "${WC3_ROOT}/runtime/hle/msvcr80/rand"
    "${WC3_ROOT}/runtime/hle/msvcr80/secure"
    "${WC3_ROOT}/runtime/hle/msvcr80/sort"
    "${WC3_ROOT}/runtime/hle/msvcr80/string"
    "${WC3_ROOT}/runtime/hle/msvcr80/time"
    "${WC3_ROOT}/runtime/hle/win32/advapi32/registry"
    "${WC3_ROOT}/runtime/hle/win32/comctl32/imagelist"
    "${WC3_ROOT}/runtime/hle/win32/shell32/dragdrop"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/dc"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/device"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/objects"
    "${WC3_ROOT}/runtime/hle/win32/gdi32/palette"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/memory"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/file"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/process"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/resource"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/string"
    "${WC3_ROOT}/runtime/hle/win32/kernel32/time"
    "${WC3_ROOT}/runtime/hle/win32/user32/display"
    "${WC3_ROOT}/runtime/hle/win32/user32/menu"
    "${WC3_ROOT}/runtime/hle/win32/user32/scroll"
    "${WC3_ROOT}/runtime/hle/win32/user32/user_object"
    "${WC3_ROOT}/runtime/hle/win32/user32/window"
    "${WC3_ROOT}/runtime/hle/win32/user32/window_facts"
    "${WC3_ROOT}/runtime/hle/win32/winmm"
    "${WC3_ROOT}/runtime/hle/win32/dsound"
    "${WC3_ROOT}/runtime/hle/win32/ws2_32"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/runtime/platform")
target_compile_definitions(wc3_hle_modules PRIVATE
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE})
if(WC3_DEBUG)
    target_compile_definitions(wc3_hle_modules PRIVATE WC3_DEBUG=1)
endif()
if(WC3_W3L_COMPAT AND WC3_W3L_PROFILE STREQUAL "rubattle")
    target_compile_definitions(wc3_hle_modules PRIVATE
        WC3_W3L_GATEWAY_RUBATTLE=1)
endif()
target_compile_options(wc3_hle_modules PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare)
target_link_libraries(wc3_hle_modules PUBLIC wc3_hle_bridge wc3_platform)
set_target_properties(wc3_hle_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- as-built Android HLE (normal target inputs) --------------------------
add_library(wc3_hle_android STATIC
    "${WC3_ROOT}/runtime/hle/android/boot_probe.c"
    "${WC3_ROOT}/runtime/hle/android/hle_android.c"
    "${WC3_ROOT}/runtime/hle/android/pe32.c"
    "${WC3_ROOT}/runtime/hle/android/guest_mem.c"
    "${WC3_ROOT}/runtime/hle/android/hle_widescreen127.c")
target_include_directories(wc3_hle_android PRIVATE
    "${WC3_ROOT}/runtime/hle/android"
    "${WC3_ROOT}/runtime/diag"
    "${WC3_ROOT}/runtime/cpu")
target_compile_definitions(wc3_hle_android PRIVATE
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE}
    WC3_NO_MAIN=1)
if(WC3_DEBUG)
    target_compile_definitions(wc3_hle_android PRIVATE WC3_DEBUG=1)
endif()
if(WC3_W3L_COMPAT AND WC3_W3L_PROFILE STREQUAL "rubattle")
    target_compile_definitions(wc3_hle_android PRIVATE
        WC3_W3L_GATEWAY_RUBATTLE=1)
endif()
target_compile_options(wc3_hle_android PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_hle_android PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- thin D3D9 bridge: source-built guest COM surface ----------------------
# Phase 1b, step 1: the guest COM surface and the lock layer are compiled from
# target-owned sources.  The previous tree's hle_d3d9_thin.o is gone; the
# generated dispatcher (thin_gen_call/thin_method_hw) is still the pinned
# hle_d3d9_thin_gen.o because it is pure marshalling with no wrapper state and
# its generator/spec are not vendored yet.  It calls the source-built
# thin_after_out()/thin_hw_call() below, so the leak and OOM fixes apply to
# every generated method.  ThinGuestObj, ThinObj, ThinSlot and the mangled
# thin_lock_* signatures stay compatible with the frozen guest ABI; this is a
# source port, not a new ABI.
set(WC3_D3D9_GEN_OBJECT
    "${WC3_LEGACY_ROOT}/build/android/boot_probe/hle_d3d9_thin_gen.o")
add_library(wc3_hle_d3d9 OBJECT
    "${WC3_ROOT}/runtime/hle/d3d9/d3d9_thin.cpp"
    "${WC3_ROOT}/runtime/hle/d3d9/d3d9_thin_lock.cpp")
target_include_directories(wc3_hle_d3d9 PRIVATE
    "${WC3_ROOT}/runtime/hle/d3d9"
    "${WC3_ROOT}/runtime/hle/android"
    "${WC3_ROOT}/runtime/diag"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/third_party/dxvk/shim"
    "${WC3_ROOT}/third_party/dxvk/3.1.1/include/native/directx")
target_compile_features(wc3_hle_d3d9 PRIVATE cxx_std_17)
target_compile_definitions(wc3_hle_d3d9 PRIVATE
    WC3_DEBUG=${WC3_DEBUG_DEFINE}
    WC3_THIN_SKIP_WRITEONLY_COPYIN=${WC3_THIN_SKIP_WRITEONLY_COPYIN_DEFINE}
    NOMINMAX
    _WIN32_WINNT=0xa00)
target_compile_options(wc3_hle_d3d9 PRIVATE
    -fshort-wchar
    -fdeclspec
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_hle_d3d9 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- DXVK entry point ------------------------------------------------------
# Target-owned shim.  The system Vulkan loader is the default; a user-installed
# ADPkg driver is opened by vulkan_driver_android.c through libadrenotools and
# only its vkGetInstanceProcAddr is handed to the two DXVK copies.
add_library(wc3_dxvk_entry STATIC
    "${WC3_ROOT}/runtime/gfx/android/dxvk_entry.cpp"
    "${WC3_ROOT}/runtime/gfx/android/dxvk_trace.cpp")
target_include_directories(wc3_dxvk_entry PRIVATE
    "${WC3_ROOT}/runtime/platform"
    "${WC3_ROOT}/runtime/platform/android"
    "${WC3_ROOT}/runtime/diag")
target_compile_features(wc3_dxvk_entry PRIVATE cxx_std_17)
target_compile_definitions(wc3_dxvk_entry PRIVATE
    WC3_DEBUG=${WC3_DEBUG_DEFINE})
target_compile_options(wc3_dxvk_entry PRIVATE
    -fno-strict-aliasing
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_dxvk_entry PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- libadrenotools / liblinkernsbypass ------------------------------------
# Pinned legacy build inputs for ADR-036.  They are BSD-2-Clause vendor
# binaries, not build scripts; third_party/VENDOR_MANIFEST.md records the
# licence and source evidence.  A source vendor replaces these archives
# without changing the C interface in vulkan_driver_android.c.
set(WC3_ADRENOTOOLS_DIR
    "${WC3_LEGACY_ROOT}/build/android-shared/adrenotools")
set(WC3_ADRENOTOOLS_ARCHIVE "${WC3_ADRENOTOOLS_DIR}/libadrenotools.a")
set(WC3_LINKERNSBYPASS_ARCHIVE
    "${WC3_ADRENOTOOLS_DIR}/liblinkernsbypass.a")
set(WC3_ADRENOTOOLS_HASHES
    "libadrenotools.a=22d71eb19343af4a0a524c860b7caf258a7b92fdb1cd71c2eb44d49a91a69f1d"
    "liblinkernsbypass.a=91a5893282ce4839e887e35e8726c2e5c6436c9e8192031039339abd968222fc")
foreach(_entry IN LISTS WC3_ADRENOTOOLS_HASHES)
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _expected)
    set(_path "${WC3_ADRENOTOOLS_DIR}/${_name}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "legacy adrenotools input missing: ${_path}")
    endif()
    file(SHA256 "${_path}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR
            "adrenotools input ${_name} sha256 ${_actual} != ${_expected}")
    endif()
endforeach()

add_library(wc3_adrenotools STATIC IMPORTED)
set_target_properties(wc3_adrenotools PROPERTIES
    IMPORTED_LOCATION "${WC3_ADRENOTOOLS_ARCHIVE}")
add_library(wc3_linkernsbypass STATIC IMPORTED)
set_target_properties(wc3_linkernsbypass PROPERTIES
    IMPORTED_LOCATION "${WC3_LINKERNSBYPASS_ARCHIVE}")

# --- lifted corpus and the thin D3D9 -> DXVK bridge ------------------------
set(WC3_CORPUS_LIBS)
include("${WC3_ROOT}/cmake/Wc3ReliftedCorpus.cmake")
list(APPEND WC3_CORPUS_LIBS wc3_corpus_relifted)
# The relifted corpus is source-built, so it has no prebuilt object list.
set(WC3_CORPUS_OBJECTS)

# V10/T2-save: the overlapped-file and thread-handle fixes are part of the
# product, not an experiment.  A source revert used to reach an APK silently;
# fail configure instead.  This is a presence guard, not a semantic proof: T2
# remains the behavioral gate.
set(_wc3_hle_source "${WC3_ROOT}/runtime/hle/android/hle_android.c")
if(NOT EXISTS "${_wc3_hle_source}")
    message(FATAL_ERROR "as-built HLE source missing: ${_wc3_hle_source}")
endif()
file(READ "${_wc3_hle_source}" _wc3_hle_text)
foreach(_need
        "h_ov_offset"
        "pwrite("
        "guest_thread_get(h)"
        "guest_thread_is_done")
    string(FIND "${_wc3_hle_text}" "${_need}" _wc3_need_pos)
    if(_wc3_need_pos EQUAL -1)
        message(FATAL_ERROR
            "required save-I/O fix is missing from ${_wc3_hle_source}: "
            "text '${_need}' not found")
    endif()
endforeach()

# Both DXVK archives are built from the vendored sources in third_party/dxvk
# by tools/android/build_dxvk.ps1.  The old tree supplies no DXVK binary: these
# archives are target build products, not pinned inputs.  The debug variant
# differs because the Win32 shim carries WC3_DEBUG_GRAPHICS.
if(WC3_DEBUG)
    set(_wc3_dxvk_variant debug)
else()
    set(_wc3_dxvk_variant release)
endif()
set(WC3_DXVK_DIR
    "${WC3_ROOT}/build/android/dxvk/${_wc3_dxvk_variant}"
    CACHE PATH "Directory containing the source-built isolated DXVK archives")
set(WC3_DXVK_ARCHIVES
    "${WC3_DXVK_DIR}/libdxvk1_iso.a"
    "${WC3_DXVK_DIR}/libdxvk3_iso.a")

foreach(_required
        ${WC3_CORPUS_OBJECTS} ${WC3_D3D9_GEN_OBJECT} ${WC3_DXVK_ARCHIVES})
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR
            "target input missing: ${_required}\n"
            "Build the vendored DXVK archives first: "
            "powershell -File tools/android/build_dxvk.ps1 -Variant "
            "${_wc3_dxvk_variant}")
    endif()
endforeach()

target_sources(wc3gameactivity PRIVATE
    "${WC3_ROOT}/runtime/app/overlay_actions_legacy.c"
    "${WC3_ROOT}/runtime/gfx/dxvk_probe_cache.c"
    "${WC3_ROOT}/runtime/platform/android/vulkan_driver_package.c"
    "${WC3_ROOT}/runtime/platform/android/vulkan_driver_android.c"
    ${WC3_CORPUS_OBJECTS}
    ${WC3_D3D9_GEN_OBJECT}
    $<TARGET_OBJECTS:wc3_hle_d3d9>)
target_include_directories(wc3gameactivity PRIVATE
    "${WC3_ROOT}/runtime/diag")

target_link_libraries(wc3gameactivity PRIVATE
    wc3_boot
    wc3_hle_modules
    wc3_hle_bridge
    wc3_guest
    wc3_hle_android
    ${WC3_CORPUS_LIBS}
    wc3_dxvk_entry
    wc3_adrenotools
    wc3_linkernsbypass
    -Wl,--whole-archive
    ${WC3_DXVK_ARCHIVES}
    -Wl,--no-whole-archive
    -Wl,-Bstatic
    c++
    c++abi
    -Wl,-Bdynamic
    dl
    m)

if(WC3_W3L_COMPAT)
    # Static link order: after the corpus that references wc3_w3l_*.
    target_link_libraries(wc3gameactivity PRIVATE wc3_compat)
endif()

# The widescreen wrapper is linked with --wrap so neither the generated
# dispatcher nor the generated corpus is edited for the two RenderEdge hooks.
target_link_options(wc3gameactivity PRIVATE
    -Wl,--wrap=L_6F126E30
    -Wl,--wrap=L_6F654E20
    -Wl,--wrap=wc3_vulkan_major_minor
    -Wl,--wrap=wc3_dxvk1_Direct3DCreate9
    -Wl,--wrap=wc3_dxvk3_Direct3DCreate9
    # DXVK 1.10.3 references this symbol directly.  The target-owned hidden
    # shim replaces -lvulkan; force the archive member in regardless of link
    # order so the product has no DT_NEEDED libvulkan.so.
    -Wl,--undefined=vkGetInstanceProcAddr
    # V10/T2-save: the save-list marker is emitted by the applied LiftPass and
    # is checked in the final .so.  --undefined keeps the marker member even if
    # a future link uses section GC.
    -Wl,--undefined=wc3_save_list_display_fix_marker
    -nostdlib++)

target_compile_definitions(wc3gameactivity PRIVATE WC3_WITH_ASBUILT_HLE=1)

# One machine-readable contract for build_app.ps1/build_apk.ps1.  It is
# written at configure time from the same inputs that selected the corpus, so
# the scripts verify the artifact that was actually built, not a remembered
# command line.
file(MAKE_DIRECTORY "${WC3_ROOT}/reports/generated")
# string(CONCAT), not set(): a multi-argument set() creates a CMake list and
# file(WRITE) joins its elements with semicolons, which is not valid JSON.
string(CONCAT _wc3_fix_json
    "{\n"
    "  \"schema\": 1,\n"
    "  \"corpus\": \"${_wc3_corpus_kind}\",\n"
    "  \"fixes\": {\n"
    "    \"save_list_display\": {\n"
    "      \"pass_rev\": 5,\n"
    "      \"sites\": ${_wc3_save_list_sites},\n"
    "      \"marker\": \"WC3_SAVE_LIST_STRIP_W3Z\"\n"
    "    },\n"
    "    \"overlapped_save_io\": {\n"
    "      \"source\": \"runtime/hle/android/hle_android.c\",\n"
    "      \"marker\": \"pwrite\"\n"
    "    }\n"
    "  }\n"
    "}\n")
file(WRITE "${WC3_ROOT}/reports/generated/required_fixes.json"
     "${_wc3_fix_json}")
message(STATUS
    "required fixes: ${_wc3_corpus_kind} corpus, "
    "save-list ${_wc3_save_list_sites} site(s), overlapped save I/O present")
