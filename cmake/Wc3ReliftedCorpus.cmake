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

if(NOT DEFINED WC3_FLAGS_FAST_PF_AF_DEFINE)
    set(WC3_FLAGS_FAST_PF_AF_DEFINE 0)
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

find_program(WC3_ATOMICCHECK_EXE
    NAMES atomiccheck atomiccheck.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_ATOMICCHECK_EXE)
    message(FATAL_ERROR
        "atomiccheck.exe missing; run tools/lift/build_tools.ps1 before configuring")
endif()

file(READ "${WC3_RELIFTED_MANIFEST}" _wc3_manifest)
string(JSON _wc3_nmod LENGTH "${_wc3_manifest}" modules)
if(_wc3_nmod LESS 1)
    message(FATAL_ERROR "corpus_manifest.json lists no modules")
endif()

set(WC3_RELIFTED_SOURCES)
set(_wc3_check_commands)
math(EXPR _wc3_last_mod "${_wc3_nmod} - 1")

# V5.9: a stale lift tool used to drop the atomic lowering while the corpus
# still looked structurally valid.  Check the generated source before the split
# and before compilation, so the bad corpus cannot become a product.  The
# checker reads the guest instruction comments and rejects any memory xchg or
# lock-prefixed RMW that was lowered through plain rd32/wr32.
set(_wc3_atomic_args)
foreach(_mi RANGE 0 ${_wc3_last_mod})
    string(JSON _name GET "${_wc3_manifest}" modules ${_mi} name)
    foreach(_kind seed closed)
        list(APPEND _wc3_atomic_args
             --file "${WC3_RELIFTED_ROOT_ABS}/${_name}/${_kind}.c")
    endforeach()
endforeach()
file(MAKE_DIRECTORY "${WC3_ROOT}/reports/generated")
execute_process(
    COMMAND "${WC3_ATOMICCHECK_EXE}" ${_wc3_atomic_args}
            --require-any
            --report "${WC3_ROOT}/reports/generated/corpus_atomic.json"
    RESULT_VARIABLE _wc3_atomic_rc
    OUTPUT_VARIABLE _wc3_atomic_out
    ERROR_VARIABLE _wc3_atomic_err)
if(NOT _wc3_atomic_rc EQUAL 0)
    message(FATAL_ERROR
        "relifted corpus has non-atomic memory operations (rc=${_wc3_atomic_rc}).\n"
        "${_wc3_atomic_out}\n${_wc3_atomic_err}\n"
        "Rebuild the lift toolchain (tools/lift/build_tools.ps1 -Force) and "
        "relift; never hand-edit generated C.")
endif()
string(STRIP "${_wc3_atomic_out}" _wc3_atomic_out)
message(STATUS "${_wc3_atomic_out}")
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
        if(WC3_DEVIRTUALIZE_P0_1)
            # P0-1 LiftPass runs after splitcorpus, so every chunk receives the
            # direct_call.h include and prototypes for cross-TU targets.  This
            # is gated OFF by default until G-14/G-15 are green (ABI-6).
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${WC3_ROOT}/tools/lift/passes/devirtualize.py")
            set(_devirt_dir
                "${CMAKE_CURRENT_BINARY_DIR}/devirtualized/${_name}/${_kind}")
            file(MAKE_DIRECTORY "${_devirt_dir}")
            set(_transformed_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_devirt_out "${_devirt_dir}/${_chunk_name}")
                execute_process(
                    COMMAND "${Python3_EXECUTABLE}"
                            "${WC3_ROOT}/tools/lift/passes/devirtualize.py"
                            --list "${WC3_BOOT_LIST}"
                            --lift "${_chunk}"
                            --out "${_devirt_out}"
                            --json "${_devirt_out}.json"
                    RESULT_VARIABLE _devirt_rc
                    OUTPUT_VARIABLE _devirt_stdout
                    ERROR_VARIABLE _devirt_stderr)
                if(NOT _devirt_rc EQUAL 0)
                    message(FATAL_ERROR
                        "devirtualize failed for ${_chunk} (rc=${_devirt_rc}):\n"
                        "${_devirt_stdout}\n${_devirt_stderr}")
                endif()
                list(APPEND _transformed_chunks "${_devirt_out}")
            endforeach()
            set(_chunks "${_transformed_chunks}")
        endif()
        if(WC3_RETURN_ADDRESS_ELIDE_P0_6)
            # P0-6 runs after P0-1, so it sees both dispatch() and the
            # WC3_DIRECT_CALL form.  It is OFF until G-15 publishes the
            # function-level change set; the pass keeps the esp slot and skips
            # every target that may read the return address.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${WC3_ROOT}/tools/lift/passes/return_address_elide.py")
            set(_ra_dir
                "${CMAKE_CURRENT_BINARY_DIR}/return_address_elide/${_name}/${_kind}")
            file(MAKE_DIRECTORY "${_ra_dir}")
            set(_ra_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_ra_out "${_ra_dir}/${_chunk_name}")
                execute_process(
                    COMMAND "${Python3_EXECUTABLE}"
                            "${WC3_ROOT}/tools/lift/passes/return_address_elide.py"
                            --lift "${_chunk}"
                            --out "${_ra_out}"
                            --json "${_ra_out}.json"
                    RESULT_VARIABLE _ra_rc
                    OUTPUT_VARIABLE _ra_stdout
                    ERROR_VARIABLE _ra_stderr)
                if(NOT _ra_rc EQUAL 0)
                    message(FATAL_ERROR
                        "return_address_elide failed for ${_chunk} (rc=${_ra_rc}):\n"
                        "${_ra_stdout}\n${_ra_stderr}")
                endif()
                list(APPEND _ra_chunks "${_ra_out}")
            endforeach()
            set(_chunks "${_ra_chunks}")
        endif()
        if(WC3_DEAD_FLAGS_P0_2)
            # P0-2 runs after P0-1 when both are enabled, so its input is the
            # direct-call form.  It is a straight-line, per-flag kill analysis;
            # control flow and calls are barriers (see the pass header).  Gated
            # OFF until G-15 publishes the per-module golden report.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${WC3_ROOT}/tools/lift/passes/dead_flags.py")
            set(_dead_dir
                "${CMAKE_CURRENT_BINARY_DIR}/dead_flags/${_name}/${_kind}")
            file(MAKE_DIRECTORY "${_dead_dir}")
            set(_dead_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_dead_out "${_dead_dir}/${_chunk_name}")
                execute_process(
                    COMMAND "${Python3_EXECUTABLE}"
                            "${WC3_ROOT}/tools/lift/passes/dead_flags.py"
                            --lift "${_chunk}"
                            --out "${_dead_out}"
                            --json "${_dead_out}.json"
                            --quiet
                    RESULT_VARIABLE _dead_rc
                    OUTPUT_VARIABLE _dead_stdout
                    ERROR_VARIABLE _dead_stderr)
                if(NOT _dead_rc EQUAL 0)
                    message(FATAL_ERROR
                        "dead_flags failed for ${_chunk} (rc=${_dead_rc}):\n"
                        "${_dead_stdout}\n${_dead_stderr}")
                endif()
                list(APPEND _dead_chunks "${_dead_out}")
            endforeach()
            set(_chunks "${_dead_chunks}")
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
    "${WC3_ROOT}/runtime/guest"
    "${WC3_ROOT}/third_party/softfloat/source/include"
    "${WC3_ROOT}/third_party/softfloat/source/8086-SSE")
target_compile_definitions(wc3_corpus_relifted PRIVATE
    SOFTFLOAT_FAST_INT64
    LITTLEENDIAN
    THREAD_LOCAL=_Thread_local
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE}
    WC3_DEBUG=${WC3_DEBUG_DEFINE})
# The relifted corpus is guest code with a frozen ABI.  Build it at -O2, not
# -O0: the header-only runtime (cpu.h, x87.h) must inline, and generated bodies
# run once per guest instruction.  The stack protector and _FORTIFY_SOURCE add
# host canary/check calls that the legacy guest-memory targets already disable;
# no speed claim follows until G-10/PM-19 measure this corpus.
# O-32: the NDK passes -g in CMAKE_C_FLAGS, and target options are appended
# after it, so -g0 here (next to -O2) wins for the generated corpus only.
# Expected effect is compile time, link peak RSS, .a size and disk I/O
# (PM-22); no frame-time claim until G-10/PM-19, and the L_<va> set plus the
# G-15 body check must stay unchanged.
target_compile_options(wc3_corpus_relifted PRIVATE
    -O2
    -g0
    -fno-stack-protector
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
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
