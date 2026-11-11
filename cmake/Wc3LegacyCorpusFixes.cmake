# Fixups for the pinned legacy corpus.
#
# The legacy corpus is consumed as prebuilt objects from the pinned upstream
# tree.  A LiftPass cannot edit an object, so the one affected translation unit
# is rebuilt from its pinned generated source and the save-list pass is applied
# before compilation.  The original object is removed from the link set; the
# replacement defines exactly the same 500 L_* functions (verified against the
# pinned source/object hashes below).
#
# This is the default corpus path.  Without this module a normal
# build_app.ps1/build_apk.ps1 build silently loses the save-list fix, which is
# exactly the regression this file exists to prevent.

if(NOT WC3_LEGACY_ROOT)
    message(FATAL_ERROR "Wc3LegacyCorpusFixes.cmake requires WC3_LEGACY_ROOT")
endif()
find_program(WC3_SAVE_LIST_DISPLAY_EXE
    NAMES save_list_display save_list_display.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_SAVE_LIST_DISPLAY_EXE)
    message(FATAL_ERROR
        "save_list_display.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring")
endif()

# Both hashes are part of the pinned upstream identity.  A changed generated
# source or a changed prebuilt object means the replacement no longer matches
# the corpus that the rest of the build links.  Fail closed and require a
# deliberate pin update plus a T2 re-check.
set(WC3_LEGACY_GAME_EXTRA_SOURCE
    "${WC3_LEGACY_ROOT}/build27/chunks/game_extra/game_extra_0012.c")
set(WC3_LEGACY_GAME_EXTRA_OBJECT
    "${WC3_LEGACY_ROOT}/build/android/chunks/game_extra/game_extra_0012.o")
set(WC3_LEGACY_GAME_EXTRA_SOURCE_SHA256
    "920171454fba8971fbccd28e52a72d265b1f66c0a81a5bc8d494f46ee73ee638")
set(WC3_LEGACY_GAME_EXTRA_OBJECT_SHA256
    "9cb584e45bfcd5af6b60c40fefcaec61381057e6f46b7f80278e45d5dc1e04aa")
set(WC3_LEGACY_SAVE_LIST_SITES 13)
set(WC3_LEGACY_SAVE_LIST_MARKERS 15) # 13 inserted blocks + 2 marker-block lines

foreach(_pair
        "${WC3_LEGACY_GAME_EXTRA_SOURCE};${WC3_LEGACY_GAME_EXTRA_SOURCE_SHA256}"
        "${WC3_LEGACY_GAME_EXTRA_OBJECT};${WC3_LEGACY_GAME_EXTRA_OBJECT_SHA256}")
    list(GET _pair 0 _path)
    list(GET _pair 1 _expected)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR
            "legacy save-list fix input missing: ${_path}\n"
            "The default build needs the pinned upstream generated corpus; "
            "restore the read-only legacy tree at the revision in "
            "config/build_upstream.json.")
    endif()
    file(SHA256 "${_path}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR
            "legacy save-list fix input changed: ${_path}\n"
            "sha256 ${_actual} != pinned ${_expected}\n"
            "Do not apply the pass to an unverified corpus.  Update the pin "
            "and re-run T2 before changing this file.")
    endif()
endforeach()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WC3_SAVE_LIST_DISPLAY_EXE}"
    "${WC3_LEGACY_GAME_EXTRA_SOURCE}")

set(_wc3_fix_dir "${CMAKE_CURRENT_BINARY_DIR}/legacy_corpus_fixes")
set(_wc3_fix_source "${_wc3_fix_dir}/game_extra_0012.c")
set(_wc3_fix_json
    "${WC3_ROOT}/reports/generated/save_list_display_legacy.json")
file(MAKE_DIRECTORY "${_wc3_fix_dir}")
file(MAKE_DIRECTORY "${WC3_ROOT}/reports/generated")

set(_wc3_save_list_need 1)
if(EXISTS "${_wc3_fix_source}" AND EXISTS "${_wc3_fix_json}" AND
   NOT "${WC3_LEGACY_GAME_EXTRA_SOURCE}" IS_NEWER_THAN "${_wc3_fix_source}" AND
   NOT "${WC3_SAVE_LIST_DISPLAY_EXE}" IS_NEWER_THAN "${_wc3_fix_source}")
    set(_wc3_save_list_need 0)
endif()
if(_wc3_save_list_need)
    execute_process(
        COMMAND "${WC3_SAVE_LIST_DISPLAY_EXE}"
                --lift "${WC3_LEGACY_GAME_EXTRA_SOURCE}"
                --out "${_wc3_fix_source}"
                --json "${_wc3_fix_json}"
                --expect-sites ${WC3_LEGACY_SAVE_LIST_SITES}
                --marker
        RESULT_VARIABLE _wc3_save_list_rc
        OUTPUT_VARIABLE _wc3_save_list_out
        ERROR_VARIABLE _wc3_save_list_err)
    if(NOT _wc3_save_list_rc EQUAL 0)
        message(FATAL_ERROR
            "save_list_display failed for the legacy corpus "
            "(rc=${_wc3_save_list_rc}):\n"
            "${_wc3_save_list_out}\n${_wc3_save_list_err}")
    endif()
    string(STRIP "${_wc3_save_list_out}" _wc3_save_list_out)
    message(STATUS "${_wc3_save_list_out}")
endif()

file(READ "${_wc3_fix_source}" _wc3_fix_text)
string(REGEX MATCHALL "WC3_SAVE_LIST_STRIP_W3Z" _wc3_fix_markers
       "${_wc3_fix_text}")
list(LENGTH _wc3_fix_markers _wc3_fix_marker_count)
if(NOT _wc3_fix_marker_count EQUAL ${WC3_LEGACY_SAVE_LIST_MARKERS})
    message(FATAL_ERROR
        "legacy save-list corpus has ${_wc3_fix_marker_count} markers, "
        "expected ${WC3_LEGACY_SAVE_LIST_MARKERS}")
endif()

# The pinned prebuilt object was compiled with the legacy tree's
# recomp_todo.h: every RECOMP_TODO site calls wc3_lift_todo() with the guest
# state instead of a bare abort().  Reproduce that contract here so the
# replacement object is behaviorally equivalent outside the patched block.
set(_wc3_todo_header "${_wc3_fix_dir}/recomp_todo.h")
file(WRITE "${_wc3_todo_header}" [=[
#ifndef WC3_LEGACY_RECOMP_TODO_H
#define WC3_LEGACY_RECOMP_TODO_H
#ifdef __cplusplus
extern "C" {
#endif
void wc3_lift_todo(unsigned va, const char *text, unsigned eip, unsigned esp,
                   unsigned ebp, unsigned eax);
#ifdef __cplusplus
}
#endif
#define RECOMP_TODO(va, text) \
    wc3_lift_todo((unsigned)(va), (text), (unsigned)(c)->eip, \
                  (unsigned)(c)->esp, (unsigned)(c)->ebp, (unsigned)(c)->eax)
#endif
]=])

add_library(wc3_legacy_fix_save_list STATIC "${_wc3_fix_source}")
target_include_directories(wc3_legacy_fix_save_list PRIVATE
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_legacy_fix_save_list PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
    # The pinned object was built with the runtime image delta.  The product
    # keeps g_image_delta at zero (fixed identity mapping), but matching the
    # original object's relocation contract keeps this TU indistinguishable
    # from the rest of the legacy corpus.
    GVA_DELTA=1
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE}
    WC3_DEBUG=${WC3_DEBUG_DEFINE})
target_compile_options(wc3_legacy_fix_save_list PRIVATE
    -O2
    -g0
    -include "${_wc3_todo_header}"
    -fno-stack-protector
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-parentheses-equality)
set_target_properties(wc3_legacy_fix_save_list PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

message(STATUS
    "legacy corpus: save-list pass applied to game_extra_0012 "
    "(${WC3_LEGACY_SAVE_LIST_SITES} sites)")
