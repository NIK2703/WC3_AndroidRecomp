# Relifted multi-module corpus link.
#
# Included by cmake/Wc3LegacyBoot.cmake when WC3_RELIFTED_CORPUS_ROOT points at
# the output of corpusclosure (build/corpus-full by default).  The corpus is
# generated C, not a source tree: every module is split at function boundaries
# into bounded translation units, then compiled into one static library.  The
# merged list and its hash come from corpus_manifest.json; the dispatcher list
# was already generated from the same file by Wc3LegacyBoot.cmake.
#
# The split is deterministic and runs at configure time only when the source
# or the splitter changed.  This keeps incremental builds sane without a
# second source-of-truth list (BP-5.1/BP-5.2).

if(NOT WC3_RELIFTED_ROOT_ABS)
    message(FATAL_ERROR "Wc3ReliftedCorpus.cmake included without WC3_RELIFTED_ROOT_ABS")
endif()

find_program(WC3_SPLITCORPUS_EXE
    NAMES splitcorpus splitcorpus.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_SPLITCORPUS_EXE)
    message(FATAL_ERROR
        "splitcorpus.exe missing; run tools/lift/build_tools.ps1 before configuring")
endif()

find_program(WC3_MANIFEST_TOOL
    NAMES manifest manifest.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

file(READ "${WC3_RELIFTED_MANIFEST}" _wc3_manifest)
string(JSON _wc3_nmod LENGTH "${_wc3_manifest}" modules)
if(_wc3_nmod LESS 1)
    message(FATAL_ERROR "corpus_manifest.json lists no modules")
endif()

set(WC3_RELIFTED_SOURCES)
set(_wc3_check_commands)
math(EXPR _wc3_last_mod "${_wc3_nmod} - 1")
foreach(_mi RANGE 0 ${_wc3_last_mod})
    string(JSON _name GET "${_wc3_manifest}" modules ${_mi} name)
    set(_module_dir "${WC3_RELIFTED_ROOT_ABS}/${_name}")
    foreach(_kind seed closed)
        set(_src "${_module_dir}/${_kind}.c")
        if(NOT EXISTS "${_src}")
            message(FATAL_ERROR "relifted corpus source missing: ${_src}")
        endif()
        set(_chunk_dir "${CMAKE_CURRENT_BINARY_DIR}/relifted/${_name}")
        set(_stamp "${_chunk_dir}/${_kind}.stamp")
        if(NOT EXISTS "${_stamp}" OR
           "${_src}" IS_NEWER_THAN "${_stamp}" OR
           "${WC3_SPLITCORPUS_EXE}" IS_NEWER_THAN "${_stamp}")
            file(MAKE_DIRECTORY "${_chunk_dir}")
            file(GLOB _old_chunks "${_chunk_dir}/${_name}_${_kind}_*.c")
            if(_old_chunks)
                file(REMOVE ${_old_chunks})
            endif()
            execute_process(
                COMMAND "${WC3_SPLITCORPUS_EXE}" "${_src}" "${_chunk_dir}"
                        "${_name}_${_kind}" 800 16
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE _err)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR
                    "splitcorpus failed for ${_src} (rc=${_rc}):\n${_out}\n${_err}")
            endif()
            file(TOUCH "${_stamp}")
        endif()
        file(GLOB _chunks "${_chunk_dir}/${_name}_${_kind}_*.c")
        if(NOT _chunks)
            message(FATAL_ERROR "splitcorpus produced no chunks for ${_src}")
        endif()
        list(APPEND WC3_RELIFTED_SOURCES ${_chunks})
    endforeach()

    if(WC3_MANIFEST_TOOL)
        list(APPEND _wc3_check_commands
             COMMAND "${WC3_MANIFEST_TOOL}" "${_module_dir}" --strict
                     --active-list "${_module_dir}/corpus.list.h" --top 5)
    endif()
endforeach()

if(NOT WC3_RELIFTED_SOURCES)
    message(FATAL_ERROR "relifted corpus has no translation units")
endif()

add_library(wc3_corpus_relifted STATIC ${WC3_RELIFTED_SOURCES})
target_include_directories(wc3_corpus_relifted PRIVATE
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_corpus_relifted PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
    WC3_DEBUG=${WC3_DEBUG_DEFINE})
target_compile_options(wc3_corpus_relifted PRIVATE
    -O0
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-parentheses-equality)
set_target_properties(wc3_corpus_relifted PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(_wc3_check_commands)
    add_custom_target(wc3_corpus_manifest_check
        ${_wc3_check_commands}
        COMMENT "Auditing relifted corpus definitions against module lists"
        VERBATIM)
    # A link is not evidence that the manifest still matches the generated C:
    # make the audit part of the product build when the host tool is present.
    add_dependencies(wc3gameactivity wc3_corpus_manifest_check)
endif()

list(LENGTH WC3_RELIFTED_SOURCES _wc3_nchunks)
message(STATUS
    "relifted corpus: ${_wc3_nmod} modules, ${_wc3_nchunks} chunk(s)")
