# Transitional first-run link: lifted corpus + quarantined as-built HLE.
#
# The lifted C objects and the as-built HLE still live in the read-only legacy
# tree while the corpus is re-lifted into this tree.  They are consumed as
# pinned inputs, not as build scripts.  Target code reaches the quarantine only
# through runtime/hle/legacy_shim (R15/H6.3).
#
# Set WC3_WITH_LEGACY_BOOT=OFF for configurations that only build platform
# targets; the product path in this milestone needs the corpus.

if(NOT EXISTS "${WC3_LEGACY_ROOT}/build/android/boot_probe/probe_list.h")
    message(FATAL_ERROR
        "legacy corpus manifest missing: ${WC3_LEGACY_ROOT}/build/android/boot_probe/probe_list.h")
endif()
if(NOT EXISTS "${WC3_LEGACY_ROOT}/build/android/libsoftfloat.a")
    message(FATAL_ERROR
        "legacy softfloat archive missing: ${WC3_LEGACY_ROOT}/build/android/libsoftfloat.a")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(WC3_BOOT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/legacy_boot_generated")
set(WC3_BOOT_DISPATCH_LIST "${WC3_BOOT_GEN_DIR}/boot_dispatch_list.h")
file(MAKE_DIRECTORY "${WC3_BOOT_GEN_DIR}")

add_custom_command(
    OUTPUT "${WC3_BOOT_DISPATCH_LIST}"
    COMMAND "${Python3_EXECUTABLE}" "${WC3_ROOT}/tools/gen_boot_dispatch.py"
            --list "${WC3_LEGACY_ROOT}/build/android/boot_probe/probe_list.h"
            --out "${WC3_BOOT_DISPATCH_LIST}"
            --prefix 004 --prefix 350 --prefix 351 --prefix 352
            --prefix 353 --prefix 600 --prefix 6F
    DEPENDS
        "${WC3_ROOT}/tools/gen_boot_dispatch.py"
        "${WC3_LEGACY_ROOT}/build/android/boot_probe/probe_list.h"
    COMMENT "Generating boot dispatcher VA list"
    VERBATIM)
add_custom_target(wc3_boot_dispatch_gen DEPENDS "${WC3_BOOT_DISPATCH_LIST}")

# --- target-side boot code (no quarantine headers) ------------------------
add_library(wc3_boot STATIC
    "${WC3_ROOT}/runtime/boot/app_boot.c"
    "${WC3_ROOT}/runtime/boot/dispatch_boot.c")
add_dependencies(wc3_boot wc3_boot_dispatch_gen)
target_include_directories(wc3_boot PRIVATE
    "${WC3_BOOT_GEN_DIR}"
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_boot PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local)
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
    "${WC3_ROOT}/runtime/hle/legacy_shim.c")
target_include_directories(wc3_legacy_shim PRIVATE
    "${WC3_ROOT}/runtime/hle"
    "${WC3_ROOT}/runtime/legacy"
    "${WC3_ROOT}/runtime/cpu"
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

# --- quarantined as-built sources (read-only inputs) ----------------------
add_library(wc3_legacy_boot STATIC
    "${WC3_ROOT}/runtime/legacy/boot_probe.c"
    "${WC3_ROOT}/runtime/legacy/hle_android.c"
    "${WC3_ROOT}/runtime/legacy/pe32.c"
    "${WC3_ROOT}/runtime/legacy/guest_mem.c"
    "${WC3_ROOT}/runtime/legacy/hle_widescreen127.c")
target_include_directories(wc3_legacy_boot PRIVATE
    "${WC3_ROOT}/runtime/legacy"
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_legacy_boot PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
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

# --- pinned lifted corpus and the thin D3D9 -> DXVK bridge -----------------
set(WC3_CORPUS_DIRS
    war3 war3_closure storm_a35
    game game_extra game_vt game_vtc
    ijl mss_a35 mssfast_a35 mp3dec_a35)
set(WC3_CORPUS_OBJECTS)
foreach(dir IN LISTS WC3_CORPUS_DIRS)
    file(GLOB _chunk_objects
        "${WC3_LEGACY_ROOT}/build/android/chunks/${dir}/*.o")
    list(APPEND WC3_CORPUS_OBJECTS ${_chunk_objects})
endforeach()
list(APPEND WC3_CORPUS_OBJECTS
    "${WC3_LEGACY_ROOT}/build/android/boot_probe/war3_27_extra2.o"
    "${WC3_LEGACY_ROOT}/build/android/boot_probe/storm_27_extra2_a35.o")

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
    ${WC3_CORPUS_OBJECTS}
    ${WC3_D3D9_OBJECTS})

target_link_libraries(wc3gameactivity PRIVATE
    wc3_boot
    wc3_legacy_shim
    wc3_legacy_boot
    wc3_dxvk_entry
    "${WC3_LEGACY_ROOT}/build/android/libsoftfloat.a"
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
    -nostdlib++)

target_compile_definitions(wc3gameactivity PRIVATE WC3_WITH_LEGACY_BOOT=1)
