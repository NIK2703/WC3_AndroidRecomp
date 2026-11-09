# Transitional first-run link: lifted corpus + quarantined as-built HLE.
#
# The lifted C objects and the as-built HLE still live in the read-only legacy
# tree while the corpus is re-lifted into this tree.  They are consumed as
# pinned inputs, not as build scripts.  Target code reaches the quarantine only
# through runtime/hle/legacy_shim (R15/H6.3).
#
# Set WC3_WITH_LEGACY_BOOT=OFF for configurations that only build platform
# targets; the product path in this milestone needs the corpus.

if(NOT DEFINED WC3_FLAGS_FAST_PF_AF_DEFINE)
    set(WC3_FLAGS_FAST_PF_AF_DEFINE 0)
endif()

if(WC3_RELIFTED_CORPUS_ROOT)
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
    message(STATUS "relifted corpus: ${_wc3_merged_entries} definitions, list sha256 ${_wc3_list_sha}")
else()
    set(WC3_BOOT_LIST "${WC3_LEGACY_ROOT}/build/android/boot_probe/probe_list.h")
    if(NOT EXISTS "${WC3_BOOT_LIST}")
        message(FATAL_ERROR "legacy corpus manifest missing: ${WC3_BOOT_LIST}")
    endif()
endif()
if(NOT WC3_NATIVE_TLS AND
   NOT EXISTS "${WC3_LEGACY_ROOT}/build/android/libsoftfloat.a")
    message(FATAL_ERROR
        "legacy softfloat archive missing: ${WC3_LEGACY_ROOT}/build/android/libsoftfloat.a")
endif()

# O-52: the legacy archive defines __emutls_v.* variables, so compiling the
# corpus with -fno-emulated-tls while still linking that archive produces
# undefined native-TLS symbols.  When native TLS is requested, rebuild exactly
# the archive's 302-source surface from the target-tree vendor input.  The
# manifest is pinned next to the sources; no file is discovered by globbing.
if(WC3_NATIVE_TLS)
    if(NOT ANDROID)
        message(FATAL_ERROR "WC3_NATIVE_TLS requires the Android toolchain")
    endif()
    set(_wc3_softfloat_manifest
        "${WC3_ROOT}/third_party/softfloat/android_sources.txt")
    if(NOT EXISTS "${_wc3_softfloat_manifest}")
        message(FATAL_ERROR
            "SoftFloat source manifest missing: ${_wc3_softfloat_manifest}")
    endif()
    file(STRINGS "${_wc3_softfloat_manifest}" _wc3_softfloat_rel)
    set(_wc3_softfloat_sources)
    foreach(_rel IN LISTS _wc3_softfloat_rel)
        string(STRIP "${_rel}" _rel)
        if(_rel STREQUAL "" OR _rel MATCHES "^#")
            continue()
        endif()
        string(REPLACE "\\" "/" _rel "${_rel}")
        list(APPEND _wc3_softfloat_sources
            "${WC3_ROOT}/third_party/softfloat/${_rel}")
    endforeach()
    if(NOT _wc3_softfloat_sources)
        message(FATAL_ERROR "SoftFloat source manifest is empty")
    endif()
    # The legacy archive has exactly 302 members; keep the replacement pinned
    # to that surface so a manifest edit is a deliberate ABI decision, not a
    # silent source-set change.
    list(LENGTH _wc3_softfloat_sources _wc3_softfloat_count)
    if(NOT _wc3_softfloat_count EQUAL 302)
        message(FATAL_ERROR
            "SoftFloat manifest has ${_wc3_softfloat_count} sources, expected 302")
    endif()
    add_library(wc3_softfloat STATIC ${_wc3_softfloat_sources})
    target_include_directories(wc3_softfloat PRIVATE
        "${WC3_ROOT}/third_party/softfloat"
        "${WC3_ROOT}/third_party/softfloat/source/include"
        "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
    target_compile_definitions(wc3_softfloat PRIVATE
        SOFTFLOAT_FAST_INT64
        LITTLEENDIAN
        THREAD_LOCAL=_Thread_local)
    target_compile_options(wc3_softfloat PRIVATE
        -O2
        -fno-strict-aliasing
        -fwrapv
        -ffp-contract=off
        -Wno-unused-parameter)
    set_target_properties(wc3_softfloat PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
else()
    add_library(wc3_softfloat STATIC IMPORTED)
    set_target_properties(wc3_softfloat PROPERTIES
        IMPORTED_LOCATION
            "${WC3_LEGACY_ROOT}/build/android/libsoftfloat.a")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
if(WC3_RELIFTED_CORPUS_ROOT)
    foreach(_p IN LISTS WC3_RELIFTED_PREFIXES)
        list(APPEND _wc3_prefix_args --prefix "${_p}")
    endforeach()
    list(APPEND _wc3_expect_args --expect-count "${_wc3_merged_entries}")
else()
    list(APPEND _wc3_prefix_args
         --prefix 004 --prefix 350 --prefix 351 --prefix 352
         --prefix 353 --prefix 600 --prefix 6F)
endif()

add_custom_command(
    OUTPUT
        "${WC3_BOOT_DISPATCH_LIST}"
        "${WC3_BOOT_DISPATCH_HASH}"
    COMMAND "${Python3_EXECUTABLE}" "${WC3_ROOT}/tools/gen_boot_dispatch.py"
            --list "${WC3_BOOT_LIST}"
            --out "${WC3_BOOT_DISPATCH_LIST}"
            --hash-out "${WC3_BOOT_DISPATCH_HASH}"
            ${_wc3_prefix_args}
            ${_wc3_expect_args}
    DEPENDS
        "${WC3_ROOT}/tools/gen_boot_dispatch.py"
        "${WC3_BOOT_LIST}"
    COMMENT "Generating boot dispatcher VA list and hash table"
    VERBATIM)
add_custom_target(wc3_boot_dispatch_gen
    DEPENDS "${WC3_BOOT_DISPATCH_LIST}" "${WC3_BOOT_DISPATCH_HASH}")

# --- target-side boot code (no quarantine headers) ------------------------
add_library(wc3_boot STATIC
    "${WC3_ROOT}/runtime/boot/app_boot.c"
    "${WC3_ROOT}/runtime/boot/dispatch_boot.c"
    "${WC3_ROOT}/runtime/boot/init_array_probe.c"
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
    "${WC3_ROOT}/runtime/hle/win32/ws2_32"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_boot PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
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

# --- the one allowed door into runtime/legacy -----------------------------
add_library(wc3_legacy_shim STATIC
    "${WC3_ROOT}/runtime/hle/legacy_shim.c"
    "${WC3_ROOT}/runtime/hle/abi/hle_abi.c")
target_include_directories(wc3_legacy_shim PRIVATE
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/hle/abi"
    "${WC3_ROOT}/runtime/legacy"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/runtime/platform"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_legacy_shim PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local)
target_compile_options(wc3_legacy_shim PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_legacy_shim PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- target HLE modules (M5 migration, H6.2 order) -------------------------
# Each module is a normal target translation unit: no legacy headers, OS
# services only through Wc3Platform, and registration over the as-built table
# through the shim.  The quarantine file itself is never edited.
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
    "${WC3_ROOT}/runtime/hle/win32/winmm/winmm_fast_bind.c"
    "${WC3_ROOT}/runtime/hle/win32/ws2_32/ws2_32_hle.c")

# O-91/BV-3: the rejected v2 sink is debug/headless evidence only.  Release
# still gets winmm_fast_bind.c, so the product fast registry does not depend on
# v2.  WC3_DEBUG is the variant value supplied by build_app.ps1; a direct CMake
# invocation must keep that value consistent.
if(WC3_DEBUG)
    target_sources(wc3_hle_modules PRIVATE
        "${WC3_ROOT}/runtime/hle/win32/winmm/waveout_hle.c")
endif()

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
    "${WC3_ROOT}/runtime/hle/win32/ws2_32"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/runtime/platform"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_hle_modules PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE})
if(WC3_DEBUG)
    target_compile_definitions(wc3_hle_modules PRIVATE WC3_DEBUG=1)
endif()
target_compile_options(wc3_hle_modules PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare)
target_link_libraries(wc3_hle_modules PUBLIC wc3_legacy_shim wc3_platform)
set_target_properties(wc3_hle_modules PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- quarantined as-built sources (read-only inputs) ----------------------
add_library(wc3_legacy_boot STATIC
    "${WC3_ROOT}/runtime/legacy/boot_probe.c"
    "${WC3_ROOT}/runtime/legacy/hle_android.c"
    "${WC3_ROOT}/runtime/legacy/pe32.c"
    "${WC3_ROOT}/runtime/legacy/guest_mem.c"
    "${WC3_ROOT}/runtime/legacy/hle_widescreen127.c")
target_include_directories(wc3_legacy_boot PRIVATE
    "${WC3_ROOT}/runtime/legacy"
    "${WC3_ROOT}/runtime/diag"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_legacy_boot PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE}
    WC3_NO_MAIN=1)
if(WC3_DEBUG)
    target_compile_definitions(wc3_legacy_boot PRIVATE WC3_DEBUG=1)
endif()
target_compile_options(wc3_legacy_boot PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_legacy_boot PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- DXVK entry point ------------------------------------------------------
# Rebuilt from source rather than inherited from the legacy tree so the
# product does not silently acquire a stale WC3_ADRENOTOOLS dependency.  The
# system Vulkan loader is the default; custom ICDs stay explicit opt-in.
add_library(wc3_dxvk_entry STATIC
    "${WC3_LEGACY_ROOT}/runtime/android/dxvk_entry.cpp")
target_include_directories(wc3_dxvk_entry PRIVATE
    "${WC3_LEGACY_ROOT}/runtime/android/dxvk_shim"
    "${WC3_LEGACY_ROOT}/third_party/dxvk-src/include/native/directx"
    "${WC3_LEGACY_ROOT}/runtime/host"
    "${WC3_LEGACY_ROOT}/build27"
    "${WC3_LEGACY_ROOT}/runtime/android"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/legacy"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_features(wc3_dxvk_entry PRIVATE cxx_std_17)
target_compile_definitions(wc3_dxvk_entry PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
    NOMINMAX
    _WIN32_WINNT=0xa00)
target_compile_options(wc3_dxvk_entry PRIVATE
    -fshort-wchar
    -fdeclspec
    -fno-strict-aliasing
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0)
set_target_properties(wc3_dxvk_entry PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- lifted corpus and the thin D3D9 -> DXVK bridge ------------------------
set(WC3_CORPUS_OBJECTS)
set(WC3_CORPUS_LIBS)
if(WC3_RELIFTED_CORPUS_ROOT)
    include("${WC3_ROOT}/cmake/Wc3ReliftedCorpus.cmake")
    list(APPEND WC3_CORPUS_LIBS wc3_corpus_relifted)
else()
    set(WC3_CORPUS_DIRS
        war3 war3_closure storm_a35
        game game_extra game_vt game_vtc
        ijl mss_a35 mssfast_a35 mp3dec_a35)
    foreach(dir IN LISTS WC3_CORPUS_DIRS)
        file(GLOB _chunk_objects
            "${WC3_LEGACY_ROOT}/build/android/chunks/${dir}/*.o")
        list(APPEND WC3_CORPUS_OBJECTS ${_chunk_objects})
    endforeach()
    list(APPEND WC3_CORPUS_OBJECTS
        "${WC3_LEGACY_ROOT}/build/android/boot_probe/war3_27_extra2.o"
        "${WC3_LEGACY_ROOT}/build/android/boot_probe/storm_27_extra2_a35.o")
endif()

set(WC3_D3D9_OBJECTS
    "${WC3_LEGACY_ROOT}/build/android/boot_probe/hle_d3d9_thin.o"
    "${WC3_LEGACY_ROOT}/build/android/boot_probe/hle_d3d9_thin_gen.o"
    "${WC3_LEGACY_ROOT}/build/android/boot_probe/hle_d3d9_thin_lock.o")

set(WC3_DXVK_ARCHIVES
    "${WC3_LEGACY_ROOT}/build/android/libdxvk1_iso.a"
    "${WC3_LEGACY_ROOT}/build/android/libdxvk3_iso.a")

foreach(_required
        ${WC3_CORPUS_OBJECTS} ${WC3_D3D9_OBJECTS} ${WC3_DXVK_ARCHIVES})
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "legacy boot input missing: ${_required}")
    endif()
endforeach()

target_sources(wc3gameactivity PRIVATE
    "${WC3_ROOT}/runtime/app/overlay_actions_legacy.c"
    "${WC3_ROOT}/runtime/gfx/dxvk_probe_cache.c"
    ${WC3_CORPUS_OBJECTS}
    ${WC3_D3D9_OBJECTS})

target_link_libraries(wc3gameactivity PRIVATE
    wc3_boot
    wc3_hle_modules
    wc3_legacy_shim
    wc3_guest
    wc3_legacy_boot
    ${WC3_CORPUS_LIBS}
    wc3_dxvk_entry
    wc3_softfloat
    -Wl,--whole-archive
    ${WC3_DXVK_ARCHIVES}
    -Wl,--no-whole-archive
    -Wl,-Bstatic
    c++
    c++abi
    -Wl,-Bdynamic
    vulkan
    dl
    m)

# The widescreen wrapper is linked with --wrap so neither the generated
# dispatcher nor the generated corpus is edited for the two RenderEdge hooks.
target_link_options(wc3gameactivity PRIVATE
    -Wl,--wrap=L_6F126E30
    -Wl,--wrap=L_6F654E20
    -Wl,--wrap=wc3_vulkan_major_minor
    -nostdlib++)

target_compile_definitions(wc3gameactivity PRIVATE WC3_WITH_LEGACY_BOOT=1)
