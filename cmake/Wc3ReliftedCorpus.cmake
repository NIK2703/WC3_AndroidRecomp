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

if(WC3_PATCH_PIPELINE)
    find_program(WC3_PYTHON3 NAMES python3 python)
    if(NOT WC3_PYTHON3)
        message(FATAL_ERROR
            "WC3_PATCH_PIPELINE=ON needs python3 (or python) on PATH")
    endif()
    execute_process(
        COMMAND "${WC3_PYTHON3}" -c
                "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
        RESULT_VARIABLE _patch_py_rc)
    if(NOT _patch_py_rc EQUAL 0)
        message(FATAL_ERROR
            "WC3_PATCH_PIPELINE=ON needs Python 3.10 or newer, got "
            "${WC3_PYTHON3}")
    endif()
endif()

# The old per-stage path checks that every pass binary is newer than its
# source.  Keep that guard on the pipeline path too; the pipeline hashes the
# binary, but it cannot know that the binary was built from the current C file.
function(wc3_require_fresh_pass exe src)
    if(NOT EXISTS "${exe}")
        message(FATAL_ERROR "patch pass missing: ${exe}")
    endif()
    if(EXISTS "${src}" AND "${src}" IS_NEWER_THAN "${exe}")
        message(FATAL_ERROR
            "${exe} is older than ${src}; run "
            "tools/lift/build_tools.ps1 -Force before configuring")
    endif()
endfunction()

find_program(WC3_MANIFEST_TOOL
    NAMES manifest manifest.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_DEVIRTUALIZE_EXE
    NAMES devirtualize devirtualize.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_SAVE_LIST_DISPLAY_EXE
    NAMES save_list_display save_list_display.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_DEAD_FLAGS_EXE
    NAMES dead_flags dead_flags.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_RETURN_ADDRESS_ELIDE_EXE
    NAMES return_address_elide return_address_elide.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_XMM_LOCAL_EXE
    NAMES xmm_local xmm_local.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_XMM_LANE_EXE
    NAMES xmm_lane xmm_lane.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_W3L_COMPAT_EXE
    NAMES w3l_compat w3l_compat.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_SELECTION_EXE
    NAMES selection selection.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)

find_program(WC3_LEAF_CALL_EXE
    NAMES leaf_call leaf_call.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_LEAF_CALL_EXE)
    message(FATAL_ERROR
        "leaf_call.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring")
endif()

find_program(WC3_X87_FPSR_EXE
    NAMES x87_fpsr x87_fpsr.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(WC3_X87_FPSR AND NOT WC3_X87_FPSR_EXE)
    message(FATAL_ERROR
        "x87_fpsr.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring with -DWC3_X87_FPSR=ON")
endif()

find_program(WC3_X87_INVALID_OUTLINE_EXE
    NAMES x87_invalid_outline x87_invalid_outline.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(WC3_X87_INVALID_OUTLINE AND NOT WC3_X87_INVALID_OUTLINE_EXE)
    message(FATAL_ERROR
        "x87_invalid_outline.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring with -DWC3_X87_INVALID_OUTLINE=ON")
endif()
if(NOT WC3_X87_INVALID_OUTLINE_MIN_SITES MATCHES "^[0-9]+$"
   OR WC3_X87_INVALID_OUTLINE_MIN_SITES LESS 1)
    message(FATAL_ERROR
        "WC3_X87_INVALID_OUTLINE_MIN_SITES must be a positive integer")
endif()

find_program(WC3_GUEST_BASE_EXE
    NAMES guest_base guest_base.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(WC3_GUEST_BASE_LOCAL AND NOT WC3_GUEST_BASE_EXE)
    message(FATAL_ERROR
        "guest_base.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring with -DWC3_GUEST_BASE_LOCAL=ON")
endif()

find_program(WC3_ESP_LOCAL_EXE
    NAMES esp_local esp_local.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_ESP_LOCAL_EXE)
    message(FATAL_ERROR
        "esp_local.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring the relifted corpus")
endif()

find_program(WC3_PARALLEL_RAYCAST_EXE
    NAMES parallel_raycast parallel_raycast.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(WC3_PARALLEL_RAYCAST AND NOT WC3_PARALLEL_RAYCAST_EXE)
    message(FATAL_ERROR
        "parallel_raycast.exe missing; run tools/lift/build_tools.ps1 "
        "before configuring with -DWC3_PARALLEL_RAYCAST=ON")
endif()

find_program(WC3_ATOMICCHECK_EXE
    NAMES atomiccheck atomiccheck.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_ATOMICCHECK_EXE)
    message(FATAL_ERROR
        "atomiccheck.exe missing; run tools/lift/build_tools.ps1 before configuring")
endif()

find_program(WC3_JOBPOOL_EXE
    NAMES jobpool jobpool.exe
    HINTS "${WC3_ROOT}/build/lift-tools"
    NO_DEFAULT_PATH)
if(NOT WC3_JOBPOOL_EXE)
    message(FATAL_ERROR
        "jobpool.exe missing; run tools/lift/build_tools.ps1 before configuring")
endif()

set(WC3_CORPUS_JOBS "0" CACHE STRING
    "Parallel lift-tool jobs per corpus stage (0 = host CPU count + 2, capped)")
set(WC3_CORPUS_RETRIES "2" CACHE STRING
    "Retries for a failed parallel lift-tool job (transient file locks)")
set(WC3_CORPUS_TIMING "OFF" CACHE BOOL
    "Print elapsed time for corpus generation stages")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/jobs")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/jobpool_logs")

function(wc3_run_jobpool _jobfile _stage)
    if(NOT EXISTS "${_jobfile}")
        return()
    endif()
    file(SIZE "${_jobfile}" _wc3_job_size)
    if(_wc3_job_size EQUAL 0)
        return()
    endif()
    execute_process(
        COMMAND "${WC3_JOBPOOL_EXE}"
                --file "${_jobfile}"
                --jobs "${WC3_CORPUS_JOBS}"
                --retries "${WC3_CORPUS_RETRIES}"
                --log-dir "${CMAKE_CURRENT_BINARY_DIR}/jobpool_logs"
        RESULT_VARIABLE _wc3_job_rc)
    if(NOT _wc3_job_rc EQUAL 0)
        message(FATAL_ERROR
            "${_stage} failed (rc=${_wc3_job_rc}); "
            "see ${CMAKE_CURRENT_BINARY_DIR}/jobpool_logs")
    endif()
endfunction()

file(READ "${WC3_RELIFTED_MANIFEST}" _wc3_manifest)
string(JSON _wc3_nmod LENGTH "${_wc3_manifest}" modules)
if(_wc3_nmod LESS 1)
    message(FATAL_ERROR "corpus_manifest.json lists no modules")
endif()

set(WC3_RELIFTED_SOURCES)
set(_wc3_check_commands)
set(_wc3_compat_jsons)
set(_wc3_sel_jsons)
math(EXPR _wc3_last_mod "${_wc3_nmod} - 1")

if(WC3_W3L_COMPAT)
    if(NOT WC3_W3L_COMPAT_EXE)
        message(FATAL_ERROR
            "w3l_compat.exe missing; run tools/lift/build_tools.ps1 "
            "before configuring")
    endif()
    # The compiled pass is byte-identical to the Python reference on the full
    # corpus; Python stays as the change-set contract, not as a configure-time
    # dependency.
    set(WC3_W3L_PASS "${WC3_W3L_COMPAT_EXE}")
endif()

# V5.9: a stale lift tool used to drop the atomic lowering while the corpus
# still looked structurally valid.  Check the generated source before the split
# and before compilation, so the bad corpus cannot become a product.  The
# checker reads the guest instruction comments and rejects any memory xchg or
# lock-prefixed RMW that was lowered through plain rd32/wr32.  The pipeline
# gates the post-save chunks itself, so the raw pre-split scan is only the
# fallback path.
if(NOT WC3_PATCH_PIPELINE)
set(_wc3_atomic_args)
set(_wc3_atomic_sources)
foreach(_mi RANGE 0 ${_wc3_last_mod})
    string(JSON _name GET "${_wc3_manifest}" modules ${_mi} name)
    foreach(_kind seed closed)
        set(_wc3_atomic_src "${WC3_RELIFTED_ROOT_ABS}/${_name}/${_kind}.c")
        list(APPEND _wc3_atomic_sources "${_wc3_atomic_src}")
        list(APPEND _wc3_atomic_args --file "${_wc3_atomic_src}")
    endforeach()
endforeach()
file(MAKE_DIRECTORY "${WC3_ROOT}/reports/generated")
set(_wc3_atomic_report "${WC3_ROOT}/reports/generated/corpus_atomic.json")
set(_wc3_atomic_stamp "${CMAKE_CURRENT_BINARY_DIR}/atomiccheck.stamp")
set(_wc3_atomic_out_file "${CMAKE_CURRENT_BINARY_DIR}/atomiccheck.out")
set(_wc3_atomic_fp "")
file(TIMESTAMP "${WC3_ATOMICCHECK_EXE}" _wc3_atomic_ts "%Y%m%d%H%M%S")
file(SIZE "${WC3_ATOMICCHECK_EXE}" _wc3_atomic_sz)
string(APPEND _wc3_atomic_fp "exe:${_wc3_atomic_ts}:${_wc3_atomic_sz}")
foreach(_wc3_atomic_src IN LISTS _wc3_atomic_sources)
    if(EXISTS "${_wc3_atomic_src}")
        file(TIMESTAMP "${_wc3_atomic_src}" _wc3_atomic_ts "%Y%m%d%H%M%S")
        file(SIZE "${_wc3_atomic_src}" _wc3_atomic_sz)
        string(APPEND _wc3_atomic_fp
            "|${_wc3_atomic_src}:${_wc3_atomic_ts}:${_wc3_atomic_sz}")
    else()
        string(APPEND _wc3_atomic_fp "|${_wc3_atomic_src}:missing")
    endif()
endforeach()
set(_wc3_atomic_need 1)
if(EXISTS "${_wc3_atomic_stamp}" AND
   EXISTS "${_wc3_atomic_out_file}" AND
   EXISTS "${_wc3_atomic_report}")
    file(READ "${_wc3_atomic_stamp}" _wc3_atomic_fp_old)
    string(STRIP "${_wc3_atomic_fp_old}" _wc3_atomic_fp_old)
    if("${_wc3_atomic_fp_old}" STREQUAL "${_wc3_atomic_fp}")
        set(_wc3_atomic_need 0)
    endif()
endif()
string(TIMESTAMP _wc3_atomic_t0 "%s")
if(_wc3_atomic_need)
    execute_process(
        COMMAND "${WC3_ATOMICCHECK_EXE}" ${_wc3_atomic_args}
                --require-any
                --report "${_wc3_atomic_report}"
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
    file(WRITE "${_wc3_atomic_stamp}" "${_wc3_atomic_fp}\n")
    file(WRITE "${_wc3_atomic_out_file}" "${_wc3_atomic_out}\n")
else()
    file(READ "${_wc3_atomic_out_file}" _wc3_atomic_out)
    string(STRIP "${_wc3_atomic_out}" _wc3_atomic_out)
endif()
string(TIMESTAMP _wc3_atomic_t1 "%s")
if(WC3_CORPUS_TIMING)
    if(_wc3_atomic_need)
        math(EXPR _wc3_atomic_dt "${_wc3_atomic_t1} - ${_wc3_atomic_t0}")
        message(STATUS "corpus timing: atomiccheck: ${_wc3_atomic_dt}s")
    else()
        message(STATUS "corpus timing: atomiccheck: cached")
    endif()
endif()
message(STATUS "${_wc3_atomic_out}")
endif()

if(WC3_RETURN_ADDRESS_ELIDE_P0_6 AND NOT WC3_PATCH_PIPELINE)
    # P0-6 needs a *global* function summary: after splitcorpus a callee
    # usually lives in another chunk, so a per-chunk index cannot prove safety
    # and the pass degenerates to a no-op (2.5k elisions instead of ~44k on the
    # game module).  Phase A analyzes the pre-split module sources once and
    # writes a compact "<VA> <0|1>" map; every per-chunk run consumes it with
    # --index.  seed.c and closed.c are disjoint halves of a module, so both
    # are inputs.  The analysis is textual, and the raw (pre-devirtualize) form
    # is a conservative superset of the direct-call form: a function the map
    # marks safe cannot become a return-address reader when devirtualize only
    # resolves call targets.
    if(NOT WC3_RETURN_ADDRESS_ELIDE_EXE)
        message(FATAL_ERROR
            "return_address_elide.exe missing; run "
            "tools/lift/build_tools.ps1 before configuring")
    endif()
    # Fail closed on a stale pass: the index is derived from the executable, so
    # an older binary would silently define both the map and every chunk.
    set(_ra_pass_src
        "${WC3_ROOT}/tools/lift/csrc_local/return_address_elide.c")
    if(EXISTS "${_ra_pass_src}" AND
       "${_ra_pass_src}" IS_NEWER_THAN "${WC3_RETURN_ADDRESS_ELIDE_EXE}")
        message(FATAL_ERROR
            "return_address_elide.exe is older than tools/lift/csrc_local/"
            "return_address_elide.c; run tools/lift/build_tools.ps1 -Force "
            "before configuring")
    endif()
    set(_ra_index_dir "${CMAKE_CURRENT_BINARY_DIR}/return_address_elide")
    set(_ra_index "${_ra_index_dir}/index.txt")
    file(MAKE_DIRECTORY "${_ra_index_dir}")
    set(_ra_index_args)
    set(_ra_index_sources)
    foreach(_ra_mi RANGE 0 ${_wc3_last_mod})
        string(JSON _ra_name GET "${_wc3_manifest}" modules ${_ra_mi} name)
        foreach(_ra_kind seed closed)
            set(_ra_src "${WC3_RELIFTED_ROOT_ABS}/${_ra_name}/${_ra_kind}.c")
            list(APPEND _ra_index_sources "${_ra_src}")
            list(APPEND _ra_index_args --index-input "${_ra_src}")
        endforeach()
    endforeach()
    set(_ra_index_need 0)
    if(NOT EXISTS "${_ra_index}")
        set(_ra_index_need 1)
    endif()
    if("${WC3_RETURN_ADDRESS_ELIDE_EXE}" IS_NEWER_THAN "${_ra_index}")
        set(_ra_index_need 1)
    endif()
    foreach(_ra_src IN LISTS _ra_index_sources)
        if("${_ra_src}" IS_NEWER_THAN "${_ra_index}")
            set(_ra_index_need 1)
        endif()
    endforeach()
    if(_ra_index_need)
        string(TIMESTAMP _ra_index_t0 "%s")
        execute_process(
            COMMAND "${WC3_RETURN_ADDRESS_ELIDE_EXE}"
                    --index-out "${_ra_index}"
                    ${_ra_index_args}
            RESULT_VARIABLE _ra_index_rc
            OUTPUT_VARIABLE _ra_index_out
            ERROR_VARIABLE _ra_index_err)
        if(NOT _ra_index_rc EQUAL 0)
            message(FATAL_ERROR
                "return_address_elide index failed (rc=${_ra_index_rc}):\n"
                "${_ra_index_out}\n${_ra_index_err}")
        endif()
        string(TIMESTAMP _ra_index_t1 "%s")
        if(WC3_CORPUS_TIMING)
            math(EXPR _ra_index_dt "${_ra_index_t1} - ${_ra_index_t0}")
            message(STATUS "corpus timing: return_address_elide index: ${_ra_index_dt}s")
        endif()
        string(STRIP "${_ra_index_out}" _ra_index_out)
        message(STATUS "${_ra_index_out}")
    endif()
endif()

foreach(_mi RANGE 0 ${_wc3_last_mod})
    string(JSON _name GET "${_wc3_manifest}" modules ${_mi} name)
    set(_module_dir "${WC3_RELIFTED_ROOT_ABS}/${_name}")
    foreach(_kind seed closed)
        set(_src "${_module_dir}/${_kind}.c")
        if(NOT EXISTS "${_src}")
            message(FATAL_ERROR "relifted corpus source missing: ${_src}")
        endif()
        if(_name STREQUAL "game" AND _kind STREQUAL "closed")
            # The save-list display fix is a product invariant, not an
            # optional experiment.  Apply it to a build-dir copy before the
            # split so every chunk and every relifted build carries it.  The
            # pass is idempotent and preserves mtime when the source is
            # already patched, so an incremental configure does not resplit.
            if(NOT WC3_SAVE_LIST_DISPLAY_EXE)
                message(FATAL_ERROR
                    "save_list_display.exe missing; run "
                    "tools/lift/build_tools.ps1 before configuring")
            endif()
            set(_save_dir
                "${CMAKE_CURRENT_BINARY_DIR}/save_list_display/${_name}/${_kind}")
            set(_save_src "${_save_dir}/${_kind}.c")
            set(_save_json
                "${WC3_ROOT}/reports/generated/save_list_display_relifted.json")
            file(MAKE_DIRECTORY "${_save_dir}")
            set(_save_need 1)
            if(EXISTS "${_save_src}" AND EXISTS "${_save_json}" AND
               NOT "${_src}" IS_NEWER_THAN "${_save_src}" AND
               NOT "${WC3_SAVE_LIST_DISPLAY_EXE}" IS_NEWER_THAN "${_save_src}" AND
               NOT "${_src}" IS_NEWER_THAN "${_save_json}" AND
               NOT "${WC3_SAVE_LIST_DISPLAY_EXE}" IS_NEWER_THAN "${_save_json}")
                set(_save_need 0)
            endif()
            if(_save_need)
                string(TIMESTAMP _save_t0 "%s")
                execute_process(
                    COMMAND "${WC3_SAVE_LIST_DISPLAY_EXE}"
                            --lift "${_src}"
                            --out "${_save_src}"
                            --json "${_save_json}"
                            --expect-sites 2
                            --marker
                    RESULT_VARIABLE _save_rc
                    OUTPUT_VARIABLE _save_out
                    ERROR_VARIABLE _save_err)
                if(NOT _save_rc EQUAL 0)
                    message(FATAL_ERROR
                        "save_list_display failed for ${_src} "
                        "(rc=${_save_rc}):\n"
                        "${_save_out}\n${_save_err}")
                endif()
                string(TIMESTAMP _save_t1 "%s")
                if(WC3_CORPUS_TIMING)
                    math(EXPR _save_dt "${_save_t1} - ${_save_t0}")
                    message(STATUS
                        "corpus timing: save_list_display ${_name}/${_kind}: ${_save_dt}s")
                endif()
                string(STRIP "${_save_out}" _save_out)
                message(STATUS "${_save_out}")
            endif()
            file(READ "${_save_json}" _save_report)
            string(JSON _save_marker_count
                   ERROR_VARIABLE _save_json_err
                   GET "${_save_report}" marker_count)
            if(_save_json_err)
                message(FATAL_ERROR
                    "cannot read marker_count from ${_save_json}: "
                    "${_save_json_err}")
            endif()
            if(NOT _save_marker_count EQUAL 4)
                message(FATAL_ERROR
                    "relifted game corpus has ${_save_marker_count} save-list "
                    "markers, expected 4 (2 sites + 2 marker-block lines)")
            endif()
            set(_src "${_save_src}")
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
            string(TIMESTAMP _split_t0 "%s")
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
            string(TIMESTAMP _split_t1 "%s")
            if(WC3_CORPUS_TIMING)
                math(EXPR _split_dt "${_split_t1} - ${_split_t0}")
                message(STATUS
                    "corpus timing: splitcorpus ${_name}/${_kind}: ${_split_dt}s")
            endif()
            file(TOUCH "${_stamp}")
        endif()
        file(GLOB _chunks "${_chunk_dir}/${_name}_${_kind}_*.c")
        if(NOT _chunks)
            message(FATAL_ERROR "splitcorpus produced no chunks for ${_src}")
        endif()
        # Legacy per-stage path.  It runs only when the registry pipeline is
        # explicitly disabled for an A/B fallback.
        if(NOT WC3_PATCH_PIPELINE)
        if(WC3_DEVIRTUALIZE_P0_1)
            # P0-1 LiftPass runs after splitcorpus, so every chunk receives the
            # direct_call.h include and prototypes for cross-TU targets.  The
            # compiled pass is byte-identical to the Python reference on the
            # full corpus; Python stays as the change-set contract, not as a
            # configure-time dependency.
            if(NOT WC3_DEVIRTUALIZE_EXE)
                message(FATAL_ERROR
                    "devirtualize.exe missing; run tools/lift/build_tools.ps1 "
                    "before configuring")
            endif()
            set(_devirt_dir
                "${CMAKE_CURRENT_BINARY_DIR}/devirtualized/${_name}/${_kind}")
            file(MAKE_DIRECTORY "${_devirt_dir}")
            set(_devirt_jobs
                "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_devirtualize.txt")
            set(_devirt_job_text "")
            set(_transformed_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_devirt_out "${_devirt_dir}/${_chunk_name}")
                set(_devirt_json "${_devirt_out}.json")
                if(NOT EXISTS "${_devirt_out}" OR
                   NOT EXISTS "${_devirt_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_devirt_out}" OR
                   "${WC3_BOOT_LIST}" IS_NEWER_THAN "${_devirt_out}" OR
                   "${WC3_DEVIRTUALIZE_EXE}" IS_NEWER_THAN "${_devirt_out}")
                    string(APPEND _devirt_job_text
                        "9\n${WC3_DEVIRTUALIZE_EXE}\n"
                        "--list\n${WC3_BOOT_LIST}\n"
                        "--lift\n${_chunk}\n"
                        "--out\n${_devirt_out}\n"
                        "--json\n${_devirt_json}\n")
                endif()
                list(APPEND _transformed_chunks "${_devirt_out}")
            endforeach()
            file(WRITE "${_devirt_jobs}" "${_devirt_job_text}")
            wc3_run_jobpool("${_devirt_jobs}" "devirtualize ${_name}/${_kind}")
            set(_chunks "${_transformed_chunks}")
        endif()
        if(WC3_RETURN_ADDRESS_ELIDE_P0_6)
            # P0-6 runs after P0-1, so it sees both dispatch() and the
            # WC3_DIRECT_CALL form.  The pass keeps the esp slot and skips
            # every target that may read the return address.  The compiled
            # pass is byte-identical to the Python reference on the full
            # corpus; Python stays as the change-set contract, not as a
            # configure-time dependency.
            if(NOT WC3_RETURN_ADDRESS_ELIDE_EXE)
                message(FATAL_ERROR
                    "return_address_elide.exe missing; run "
                    "tools/lift/build_tools.ps1 before configuring")
            endif()
            set(_ra_dir
                "${CMAKE_CURRENT_BINARY_DIR}/return_address_elide/${_name}/${_kind}")
            file(MAKE_DIRECTORY "${_ra_dir}")
            set(_ra_jobs
                "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_return_address_elide.txt")
            set(_ra_job_text "")
            set(_ra_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_ra_out "${_ra_dir}/${_chunk_name}")
                set(_ra_json "${_ra_out}.json")
                if(NOT EXISTS "${_ra_out}" OR
                   NOT EXISTS "${_ra_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_ra_out}" OR
                   "${_ra_index}" IS_NEWER_THAN "${_ra_out}" OR
                   "${WC3_RETURN_ADDRESS_ELIDE_EXE}" IS_NEWER_THAN "${_ra_out}")
                    string(APPEND _ra_job_text
                        "10\n${WC3_RETURN_ADDRESS_ELIDE_EXE}\n"
                        "--lift\n${_chunk}\n"
                        "--out\n${_ra_out}\n"
                        "--json\n${_ra_json}\n"
                        "--index\n${_ra_index}\n"
                        "--quiet\n")
                endif()
                list(APPEND _ra_chunks "${_ra_out}")
            endforeach()
            file(WRITE "${_ra_jobs}" "${_ra_job_text}")
            wc3_run_jobpool("${_ra_jobs}" "return_address_elide ${_name}/${_kind}")
            set(_chunks "${_ra_chunks}")
        endif()
        if(WC3_DEAD_FLAGS_P0_2)
            # P0-2 runs after P0-1 when both are enabled, so its input is the
            # direct-call form.  It is a straight-line, per-flag kill analysis;
            # control flow and calls are barriers (see the pass header).  The
            # compiled pass is byte-identical to the Python reference on the
            # full corpus; Python stays as the change-set contract.
            if(NOT WC3_DEAD_FLAGS_EXE)
                message(FATAL_ERROR
                    "dead_flags.exe missing; run tools/lift/build_tools.ps1 "
                    "before configuring")
            endif()
            set(_dead_dir
                "${CMAKE_CURRENT_BINARY_DIR}/dead_flags/${_name}/${_kind}")
            file(MAKE_DIRECTORY "${_dead_dir}")
            set(_dead_jobs
                "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_dead_flags.txt")
            set(_dead_job_text "")
            set(_dead_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_dead_out "${_dead_dir}/${_chunk_name}")
                set(_dead_json "${_dead_out}.json")
                if(NOT EXISTS "${_dead_out}" OR
                   NOT EXISTS "${_dead_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_dead_out}" OR
                   "${WC3_DEAD_FLAGS_EXE}" IS_NEWER_THAN "${_dead_out}")
                    string(APPEND _dead_job_text
                        "8\n${WC3_DEAD_FLAGS_EXE}\n"
                        "--lift\n${_chunk}\n"
                        "--out\n${_dead_out}\n"
                        "--json\n${_dead_json}\n"
                        "--quiet\n")
                endif()
                list(APPEND _dead_chunks "${_dead_out}")
            endforeach()
            file(WRITE "${_dead_jobs}" "${_dead_job_text}")
            wc3_run_jobpool("${_dead_jobs}" "dead_flags ${_name}/${_kind}")
            set(_chunks "${_dead_chunks}")
        endif()
        # D3b first increment: a label-free leaf function without a guest
        # call can keep its XMM values in a local XMM[8] and write them back
        # only before an exit.  Host helpers are allowed; they must not read
        # CPU.xmm directly.  The block fallback and its lazy entry-load
        # elision are always enabled in the canonical relifted-corpus
        # pipeline.  The profile component in the output path makes a change
        # of these flags regenerate cached chunks.  It runs after P0-2 and
        # before the W3L profile, which must see the final instruction text.
        if(NOT WC3_XMM_LOCAL_EXE)
            message(FATAL_ERROR
                "xmm_local.exe missing; run tools/lift/build_tools.ps1 "
                "before configuring")
        endif()
        set(_xmm_profile "block_lazy")
        set(_xmm_dir
            "${CMAKE_CURRENT_BINARY_DIR}/xmm_local/${_xmm_profile}/${_name}/${_kind}")
        file(MAKE_DIRECTORY "${_xmm_dir}")
        set(_xmm_jobs
            "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_xmm_local.txt")
        set(_xmm_job_text "")
        set(_xmm_chunks)
        foreach(_chunk IN LISTS _chunks)
            get_filename_component(_chunk_name "${_chunk}" NAME)
            set(_xmm_out "${_xmm_dir}/${_chunk_name}")
            set(_xmm_json "${_xmm_out}.json")
            if(NOT EXISTS "${_xmm_out}" OR
               NOT EXISTS "${_xmm_json}" OR
               "${_chunk}" IS_NEWER_THAN "${_xmm_out}" OR
               "${WC3_XMM_LOCAL_EXE}" IS_NEWER_THAN "${_xmm_out}")
                string(APPEND _xmm_job_text
                    "10\n${WC3_XMM_LOCAL_EXE}\n"
                    "--lift\n${_chunk}\n"
                    "--out\n${_xmm_out}\n"
                    "--json\n${_xmm_json}\n"
                    "--block\n"
                    "--block-lazy\n"
                    "--quiet\n")
            endif()
            list(APPEND _xmm_chunks "${_xmm_out}")
        endforeach()
        file(WRITE "${_xmm_jobs}" "${_xmm_job_text}")
        wc3_run_jobpool("${_xmm_jobs}" "xmm_local ${_name}/${_kind}")
        set(_chunks "${_xmm_chunks}")
        # D3c first increment: exact upper-lane zero reuse.  It must see the
        # xmm_local text (it rewrites the local XMM[8] stores), and it runs
        # before the profile and selection passes so they operate on the
        # reduced form.  The transform is exact and has no option of its own;
        # the output path carries the xmm_local profile so a different profile
        # can never reuse a stale chunk.
        if(NOT WC3_XMM_LANE_EXE)
            message(FATAL_ERROR
                "xmm_lane.exe missing; run tools/lift/build_tools.ps1 "
                "before configuring")
        endif()
        set(_lane_dir
            "${CMAKE_CURRENT_BINARY_DIR}/xmm_lane/${_xmm_profile}/${_name}/${_kind}")
        file(MAKE_DIRECTORY "${_lane_dir}")
        set(_lane_jobs
            "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_xmm_lane.txt")
        set(_lane_job_text "")
        set(_lane_chunks)
        foreach(_chunk IN LISTS _chunks)
            get_filename_component(_chunk_name "${_chunk}" NAME)
            set(_lane_out "${_lane_dir}/${_chunk_name}")
            set(_lane_json "${_lane_out}.json")
            if(NOT EXISTS "${_lane_out}" OR
               NOT EXISTS "${_lane_json}" OR
               "${_chunk}" IS_NEWER_THAN "${_lane_out}" OR
               "${WC3_XMM_LANE_EXE}" IS_NEWER_THAN "${_lane_out}")
                string(APPEND _lane_job_text
                    "8\n${WC3_XMM_LANE_EXE}\n"
                    "--lift\n${_chunk}\n"
                    "--out\n${_lane_out}\n"
                    "--json\n${_lane_json}\n"
                    "--quiet\n")
            endif()
            list(APPEND _lane_chunks "${_lane_out}")
        endforeach()
        file(WRITE "${_lane_jobs}" "${_lane_job_text}")
        wc3_run_jobpool("${_lane_jobs}" "xmm_lane ${_name}/${_kind}")
        set(_chunks "${_lane_chunks}")
        if(WC3_W3L_COMPAT)
            # The W3L/PvPGN/rubattle profile runs after every other enabled
            # pass, so it sees the final instruction text.  The profile and
            # latency are part of the output path: switching either one can
            # never reuse a stale chunk from another profile.
            set(_compat_dir
                "${CMAKE_CURRENT_BINARY_DIR}/w3l_compat/${_name}/${_kind}/${WC3_W3L_PROFILE}-${WC3_W3L_LATENCY}")
            file(MAKE_DIRECTORY "${_compat_dir}")
            set(_compat_jobs
                "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_w3l_compat.txt")
            set(_compat_job_text "")
            set(_compat_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_compat_out "${_compat_dir}/${_chunk_name}")
                set(_compat_json "${_compat_out}.json")
                if(NOT EXISTS "${_compat_out}" OR
                   NOT EXISTS "${_compat_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_compat_out}" OR
                   "${WC3_W3L_PASS}" IS_NEWER_THAN "${_compat_out}")
                    string(APPEND _compat_job_text
                        "12\n${WC3_W3L_PASS}\n"
                        "--profile\n${WC3_W3L_PROFILE}\n"
                        "--latency\n${WC3_W3L_LATENCY}\n"
                        "--lift\n${_chunk}\n"
                        "--out\n${_compat_out}\n"
                        "--json\n${_compat_json}\n"
                        "--quiet\n")
                endif()
                list(APPEND _compat_chunks "${_compat_out}")
                list(APPEND _wc3_compat_jsons "${_compat_json}")
            endforeach()
            file(WRITE "${_compat_jobs}" "${_compat_job_text}")
            wc3_run_jobpool("${_compat_jobs}" "w3l_compat ${_name}/${_kind}")
            set(_chunks "${_compat_chunks}")
        endif()
        if(WC3_SELECTION)
            # The 12/24/40-unit selection profile runs after every other
            # enabled pass, so it sees the final instruction text.  The pass
            # edits the generated C only; no source file under
            # build/corpus-full is modified in place.  Each chunk reports its
            # own site counts and the union is verified after all modules.
            if(NOT WC3_SELECTION_EXE)
                message(FATAL_ERROR
                    "selection.exe missing; run tools/lift/build_tools.ps1 "
                    "before configuring")
            endif()
            # A stale pass silently re-emits the old source text while the
            # configure still reports success.  Refuse that state instead of
            # letting an executable older than its source define the product.
            set(_sel_pass_src
                "${WC3_ROOT}/tools/lift/csrc_local/selection.c")
            if(EXISTS "${_sel_pass_src}" AND
               "${_sel_pass_src}" IS_NEWER_THAN "${WC3_SELECTION_EXE}")
                message(FATAL_ERROR
                    "selection.exe is older than tools/lift/csrc_local/"
                    "selection.c; run tools/lift/build_tools.ps1 -Force "
                    "before configuring")
            endif()
            # The input chunk comes from the w3l_compat directory, whose path
            # already carries the profile and latency.  Mirror that tag here:
            # otherwise a rubattle build could reuse a pvpgn-patched chunk
            # whose mtime happens to be newer than the new input.
            if(WC3_W3L_COMPAT)
                set(_sel_tag
                    "${WC3_W3L_PROFILE}-${WC3_W3L_LATENCY}-layout40")
            else()
                set(_sel_tag "plain-layout40")
            endif()
            set(_sel_dir
                "${CMAKE_CURRENT_BINARY_DIR}/selection/${_name}/${_kind}/${_sel_tag}")
            file(MAKE_DIRECTORY "${_sel_dir}")
            set(_sel_jobs
                "${CMAKE_CURRENT_BINARY_DIR}/jobs/${_name}_${_kind}_selection.txt")
            set(_sel_job_text "")
            set(_sel_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_sel_out "${_sel_dir}/${_chunk_name}")
                set(_sel_json "${_sel_out}.json")
                if(NOT EXISTS "${_sel_out}" OR
                   NOT EXISTS "${_sel_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_sel_out}" OR
                   "${WC3_SELECTION_EXE}" IS_NEWER_THAN "${_sel_out}")
                    string(APPEND _sel_job_text
                        "8\n${WC3_SELECTION_EXE}\n"
                        "--lift\n${_chunk}\n"
                        "--out\n${_sel_out}\n"
                        "--json\n${_sel_json}\n"
                        "--quiet\n")
                endif()
                list(APPEND _sel_chunks "${_sel_out}")
                list(APPEND _wc3_sel_jsons "${_sel_json}")
            endforeach()
            file(WRITE "${_sel_jobs}" "${_sel_job_text}")
            wc3_run_jobpool("${_sel_jobs}" "selection ${_name}/${_kind}")
            set(_chunks "${_sel_chunks}")
        endif()
        endif()
        list(APPEND WC3_RELIFTED_SOURCES ${_chunks})
    endforeach()

    if(WC3_MANIFEST_TOOL)
        list(APPEND _wc3_check_commands
             COMMAND "${WC3_MANIFEST_TOOL}" "${_module_dir}" --strict
                     --active-list "${_module_dir}/corpus.list.h" --top 5)
    endif()
endforeach()

# Registry-driven cutover.  The split tree is already post-save, so the save
# stage stays in the split loop and this branch owns only the later transforms.
if(WC3_PATCH_PIPELINE)
    set(_patch_base "${CMAKE_CURRENT_BINARY_DIR}/relifted")
    set(_patch_index "${CMAKE_CURRENT_BINARY_DIR}/patch-index.tsv")
    set(_patch_out "${CMAKE_CURRENT_BINARY_DIR}/patch-overlay")
    set(_patch_engine_dir "${WC3_ROOT}/build/lift-tools")
    wc3_require_fresh_pass("${WC3_ATOMICCHECK_EXE}"
        "${WC3_ROOT}/tools/lift/csrc_local/atomiccheck.c")
    wc3_require_fresh_pass("${WC3_XMM_LOCAL_EXE}"
        "${WC3_ROOT}/tools/lift/csrc_local/xmm_local.c")
    wc3_require_fresh_pass("${WC3_XMM_LANE_EXE}"
        "${WC3_ROOT}/tools/lift/csrc_local/xmm_lane.c")
    wc3_require_fresh_pass("${WC3_LEAF_CALL_EXE}"
        "${WC3_ROOT}/tools/lift/csrc_local/leaf_call.c")
    wc3_require_fresh_pass("${WC3_ESP_LOCAL_EXE}"
        "${WC3_ROOT}/tools/lift/csrc_local/esp_local.c")
    if(WC3_X87_FPSR)
        wc3_require_fresh_pass("${WC3_X87_FPSR_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/x87_fpsr.c")
    endif()
    if(WC3_X87_INVALID_OUTLINE)
        wc3_require_fresh_pass("${WC3_X87_INVALID_OUTLINE_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/x87_invalid_outline.c")
    endif()
    if(WC3_GUEST_BASE_LOCAL)
        wc3_require_fresh_pass("${WC3_GUEST_BASE_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/guest_base.c")
    endif()
    if(WC3_RETURN_ADDRESS_ELIDE_P0_6)
        wc3_require_fresh_pass("${WC3_RETURN_ADDRESS_ELIDE_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/return_address_elide.c")
    endif()
    if(WC3_DEVIRTUALIZE_P0_1)
        wc3_require_fresh_pass("${WC3_DEVIRTUALIZE_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/devirtualize.c")
    endif()
    if(WC3_DEAD_FLAGS_P0_2)
        wc3_require_fresh_pass("${WC3_DEAD_FLAGS_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/dead_flags.c")
    endif()
    if(WC3_W3L_COMPAT)
        wc3_require_fresh_pass("${WC3_W3L_PASS}"
            "${WC3_ROOT}/tools/lift/csrc_local/w3l_compat.c")
    endif()
    if(WC3_SELECTION)
        wc3_require_fresh_pass("${WC3_SELECTION_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/selection.c")
    endif()
    if(WC3_PARALLEL_RAYCAST)
        wc3_require_fresh_pass("${WC3_PARALLEL_RAYCAST_EXE}"
            "${WC3_ROOT}/tools/lift/csrc_local/parallel_raycast.c")
    endif()
    set(_patch_stage_args)
    list(APPEND _patch_stage_args --patch atomiccheck)
    if(WC3_RETURN_ADDRESS_ELIDE_P0_6)
        list(APPEND _patch_stage_args
             --patch return_address_elide_index
             --patch return_address_elide)
    endif()
    if(WC3_DEVIRTUALIZE_P0_1)
        list(APPEND _patch_stage_args --patch devirtualize)
    endif()
    if(WC3_DEAD_FLAGS_P0_2)
        list(APPEND _patch_stage_args --patch dead_flags)
    endif()
    list(APPEND _patch_stage_args --patch xmm_local --patch xmm_lane)
    if(WC3_W3L_COMPAT)
        list(APPEND _patch_stage_args --patch w3l_compat)
    endif()
    if(WC3_SELECTION)
        list(APPEND _patch_stage_args --patch selection)
    endif()
    list(APPEND _patch_stage_args --patch leaf_call)
    if(WC3_X87_FPSR)
        list(APPEND _patch_stage_args --patch x87_fpsr)
    endif()
    if(WC3_X87_INVALID_OUTLINE)
        # Must follow x87_fpsr: eligible functions are rewritten there and
        # leave no plain x87_* calls for this stage to outline.
        list(APPEND _patch_stage_args --patch x87_invalid_outline)
    endif()
    if(WC3_PARALLEL_RAYCAST)
        list(APPEND _patch_stage_args --patch parallel_raycast)
    endif()
    if(WC3_GUEST_BASE_LOCAL)
        list(APPEND _patch_stage_args --patch local_base)
    endif()
    # Must be last: every earlier stage matches exact generated-C templates,
    # and esp_local inserts its own synchronisation lines.
    list(APPEND _patch_stage_args --patch esp_local)
    set(_patch_param_args)
    file(SHA256 "${WC3_ROOT}/tools/lift/patch_pipeline.py" _patch_pipeline_sha)
    list(APPEND _patch_param_args --param "pipeline_sha=${_patch_pipeline_sha}")
    list(APPEND _patch_param_args --param
         "x87_invalid_outline_min_sites=${WC3_X87_INVALID_OUTLINE_MIN_SITES}")
    if(WC3_LEAF_GS_COOKIE)
        list(APPEND _patch_param_args --param "gs_cookie=1")
    else()
        list(APPEND _patch_param_args --param "gs_cookie=0")
    endif()
    if(WC3_W3L_COMPAT)
        list(APPEND _patch_param_args
             --param "profile=${WC3_W3L_PROFILE}"
             --param "latency=${WC3_W3L_LATENCY}")
    endif()
    if(WC3_DEVIRTUALIZE_P0_1)
        file(RELATIVE_PATH _patch_boot_list "${WC3_ROOT}" "${WC3_BOOT_LIST}")
        list(APPEND _patch_param_args --param "boot_list=${_patch_boot_list}")
    endif()
    execute_process(
        COMMAND "${WC3_PYTHON3}" "${WC3_ROOT}/tools/lift/patch_pipeline.py"
                index --base "${_patch_base}" --out "${_patch_index}"
        RESULT_VARIABLE _patch_index_rc
        OUTPUT_VARIABLE _patch_index_out
        ERROR_VARIABLE _patch_index_err)
    if(NOT _patch_index_rc EQUAL 0)
        message(FATAL_ERROR
            "patch pipeline index failed (rc=${_patch_index_rc}):\n"
            "${_patch_index_out}\n${_patch_index_err}")
    endif()
    string(STRIP "${_patch_index_out}" _patch_index_out)
    if(_patch_index_out)
        message(STATUS "${_patch_index_out}")
    endif()
    execute_process(
        COMMAND "${WC3_PYTHON3}" "${WC3_ROOT}/tools/lift/patch_pipeline.py"
                apply --base "${_patch_base}" --index "${_patch_index}"
                --out "${_patch_out}" --engine-dir "${_patch_engine_dir}"
                ${_patch_stage_args} ${_patch_param_args}
                --reuse --force --quiet
        RESULT_VARIABLE _patch_rc
        OUTPUT_VARIABLE _patch_out_log
        ERROR_VARIABLE _patch_err_log)
    if(NOT _patch_rc EQUAL 0)
        message(FATAL_ERROR
            "patch pipeline apply failed (rc=${_patch_rc}):\n"
            "${_patch_out_log}\n${_patch_err_log}")
    endif()
    string(STRIP "${_patch_out_log}" _patch_out_log)
    if(_patch_out_log)
        message(STATUS "${_patch_out_log}")
    endif()
    file(READ "${_patch_out}/overlay.json" _patch_manifest)
    string(JSON _patch_nchunks LENGTH "${_patch_manifest}" chunks)
    if(_patch_nchunks LESS 1)
        message(FATAL_ERROR "patch pipeline produced no chunks")
    endif()
    set(WC3_RELIFTED_SOURCES)
    math(EXPR _patch_last "${_patch_nchunks} - 1")
    foreach(_patch_i RANGE 0 ${_patch_last})
        string(JSON _patch_chunk GET
               "${_patch_manifest}" chunks ${_patch_i} chunk)
        list(APPEND WC3_RELIFTED_SOURCES "${_patch_out}/${_patch_chunk}")
    endforeach()
    message(STATUS
        "patch pipeline: ${_patch_nchunks} chunk(s) -> ${_patch_out}")
else()
if(WC3_W3L_COMPAT)
    # Per-chunk runs cannot use --require-all, because sites are spread over
    # several chunks.  Verify the union instead: every site of the selected
    # profile must be applied exactly once, or the configure fails closed.
    set(_wc3_compat_manifest
        "${CMAKE_CURRENT_BINARY_DIR}/w3l_compat_change_sets.txt")
    set(_wc3_compat_manifest_text "")
    foreach(_json IN LISTS _wc3_compat_jsons)
        string(APPEND _wc3_compat_manifest_text "${_json}\n")
    endforeach()
    file(WRITE "${_wc3_compat_manifest}" "${_wc3_compat_manifest_text}")
    execute_process(
        COMMAND "${WC3_W3L_PASS}"
                --profile "${WC3_W3L_PROFILE}"
                --latency "${WC3_W3L_LATENCY}"
                --verify-coverage "@${_wc3_compat_manifest}"
                --quiet
        RESULT_VARIABLE _wc3_cov_rc
        OUTPUT_VARIABLE _wc3_cov_stdout
        ERROR_VARIABLE _wc3_cov_stderr)
    if(NOT _wc3_cov_rc EQUAL 0)
        message(FATAL_ERROR
            "w3l_compat coverage check failed (rc=${_wc3_cov_rc}):\n"
            "${_wc3_cov_stdout}\n${_wc3_cov_stderr}")
    endif()
endif()

if(WC3_SELECTION)
    # The 12/24/40 selection profile is spread over several chunks, so the
    # union of the per-chunk counts must match the fixed totals of the whole
    # game corpus.  A missing or duplicated site fails the configure.
    set(_wc3_sel_manifest
        "${CMAKE_CURRENT_BINARY_DIR}/selection_change_sets.txt")
    set(_wc3_sel_manifest_text "")
    foreach(_json IN LISTS _wc3_sel_jsons)
        string(APPEND _wc3_sel_manifest_text "${_json}\n")
    endforeach()
    file(WRITE "${_wc3_sel_manifest}" "${_wc3_sel_manifest_text}")
    execute_process(
        COMMAND "${WC3_SELECTION_EXE}"
                --verify-coverage "@${_wc3_sel_manifest}"
                --quiet
        RESULT_VARIABLE _wc3_sel_cov_rc
        OUTPUT_VARIABLE _wc3_sel_cov_stdout
        ERROR_VARIABLE _wc3_sel_cov_stderr)
    if(NOT _wc3_sel_cov_rc EQUAL 0)
        message(FATAL_ERROR
            "selection coverage check failed (rc=${_wc3_sel_cov_rc}):\n"
            "${_wc3_sel_cov_stdout}\n${_wc3_sel_cov_stderr}")
    endif()
endif()

if(NOT WC3_RELIFTED_SOURCES)
    message(FATAL_ERROR "relifted corpus has no translation units")
endif()

# D4/D4b: the call-frame ladder needs a complete-corpus summary, because a
# caller and its callee may be split into different translation units.  Run one
# text pass over the final chunks after every other pass, then compile its
# outputs.  The pass selects a leaf, tail-free or draining frame per direct call
# target; unknown targets and tailful calls in unproven caller bodies keep the
# full WC3_DIRECT_CALL.
# Tag the leaf_call tree with every pass that changes the chunk text.  With a
# fixed leaf_call/<basename> path an incremental build kept an object compiled
# from a pre-selection chunk, even though the regenerated source was newer:
# the linked product then had the helper library but no calls to it.  A
# distinct directory gives the generated source and its object a distinct
# identity instead of relying on timestamp invalidation alone.
set(_wc3_leaf_tag "plain")
if(WC3_W3L_COMPAT)
    set(_wc3_leaf_tag "${WC3_W3L_PROFILE}-${WC3_W3L_LATENCY}")
endif()
if(WC3_SELECTION)
    string(APPEND _wc3_leaf_tag "-layout40")
endif()
set(_wc3_leaf_dir "${CMAKE_CURRENT_BINARY_DIR}/leaf_call/${_wc3_leaf_tag}")
set(_wc3_leaf_list "${CMAKE_CURRENT_BINARY_DIR}/leaf_call_inputs.txt")
set(_wc3_leaf_report "${WC3_ROOT}/reports/generated/leaf_call.json")
set(_wc3_leaf_stamp "${_wc3_leaf_dir}/.stamp")
set(_wc3_leaf_fingerprint_file "${_wc3_leaf_dir}/.inputs")
string(SHA256 _wc3_leaf_fingerprint
    "${WC3_LEAF_CALL_EXE}|${WC3_RELIFTED_SOURCES}")
set(_wc3_leaf_outputs)
foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
    get_filename_component(_wc3_base "${_wc3_src}" NAME)
    list(APPEND _wc3_leaf_outputs "${_wc3_leaf_dir}/${_wc3_base}")
endforeach()
list(LENGTH _wc3_leaf_outputs _wc3_leaf_count)
list(REMOVE_DUPLICATES _wc3_leaf_outputs)
list(LENGTH _wc3_leaf_outputs _wc3_leaf_unique)
if(NOT _wc3_leaf_count EQUAL _wc3_leaf_unique)
    message(FATAL_ERROR "leaf_call output basenames are not unique")
endif()

set(_wc3_leaf_need 0)
if(NOT EXISTS "${_wc3_leaf_stamp}" OR
   NOT EXISTS "${_wc3_leaf_fingerprint_file}" OR
   "${WC3_LEAF_CALL_EXE}" IS_NEWER_THAN "${_wc3_leaf_stamp}")
    set(_wc3_leaf_need 1)
else()
    file(READ "${_wc3_leaf_fingerprint_file}" _wc3_leaf_fingerprint_old)
    string(STRIP "${_wc3_leaf_fingerprint_old}" _wc3_leaf_fingerprint_old)
    if(NOT _wc3_leaf_fingerprint_old STREQUAL _wc3_leaf_fingerprint)
        set(_wc3_leaf_need 1)
    endif()
    if(NOT _wc3_leaf_need)
        foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
            if("${_wc3_src}" IS_NEWER_THAN "${_wc3_leaf_stamp}")
                set(_wc3_leaf_need 1)
                break()
            endif()
        endforeach()
    endif()
endif()
if(NOT _wc3_leaf_need)
    if(NOT EXISTS "${_wc3_leaf_report}")
        set(_wc3_leaf_need 1)
    endif()
endif()
if(NOT _wc3_leaf_need)
    foreach(_wc3_out IN LISTS _wc3_leaf_outputs)
        if(NOT EXISTS "${_wc3_out}")
            set(_wc3_leaf_need 1)
            break()
        endif()
    endforeach()
endif()

if(_wc3_leaf_need)
    file(REMOVE_RECURSE "${_wc3_leaf_dir}")
    file(MAKE_DIRECTORY "${_wc3_leaf_dir}")
    get_filename_component(_wc3_leaf_report_dir "${_wc3_leaf_report}" DIRECTORY)
    file(MAKE_DIRECTORY "${_wc3_leaf_report_dir}")
    set(_wc3_leaf_list_text "")
    foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
        string(APPEND _wc3_leaf_list_text "${_wc3_src}\n")
    endforeach()
    file(WRITE "${_wc3_leaf_list}" "${_wc3_leaf_list_text}")
    string(TIMESTAMP _wc3_leaf_t0 "%s")
    execute_process(
        COMMAND "${WC3_LEAF_CALL_EXE}"
                --inputs "${_wc3_leaf_list}"
                --out-dir "${_wc3_leaf_dir}"
                --report "${_wc3_leaf_report}"
                --quiet
        RESULT_VARIABLE _wc3_leaf_rc
        OUTPUT_VARIABLE _wc3_leaf_out
        ERROR_VARIABLE _wc3_leaf_err)
    if(NOT _wc3_leaf_rc EQUAL 0)
        message(FATAL_ERROR
            "leaf_call failed (rc=${_wc3_leaf_rc}).\n"
            "${_wc3_leaf_out}\n${_wc3_leaf_err}")
    endif()
    string(TIMESTAMP _wc3_leaf_t1 "%s")
    if(WC3_CORPUS_TIMING)
        math(EXPR _wc3_leaf_dt "${_wc3_leaf_t1} - ${_wc3_leaf_t0}")
        message(STATUS "corpus timing: leaf_call: ${_wc3_leaf_dt}s")
    endif()
    file(WRITE "${_wc3_leaf_fingerprint_file}"
        "${_wc3_leaf_fingerprint}\n")
    file(TOUCH "${_wc3_leaf_stamp}")
endif()
# Belt-and-braces invalidation: every generated chunk depends on the stamp
# written after the complete leaf_call run.  This also covers generators that
# do not compare a source file against an object reliably after configure-time
# regeneration.
if(_wc3_leaf_outputs)
    set_property(SOURCE ${_wc3_leaf_outputs}
        PROPERTY OBJECT_DEPENDS "${_wc3_leaf_stamp}")
endif()
set(WC3_RELIFTED_SOURCES "${_wc3_leaf_outputs}")

# Optional post-lift pass: replace the single
# WC3_DIRECT_CALL_CLEAN(L_6F749F90, c) with the runtime wrapper.  It runs on
# the final leaf chunks so leaf_call's classification of that call is kept.
# The stage has the same fingerprint/stamp discipline as leaf_call: the
# generated chunks and their objects must change identity when any input or
# the tool changes, not only their timestamps.
if(WC3_PARALLEL_RAYCAST)
    set(_wc3_ray_dir "${CMAKE_CURRENT_BINARY_DIR}/parallel_raycast/${_wc3_leaf_tag}")
    set(_wc3_ray_list "${CMAKE_CURRENT_BINARY_DIR}/parallel_raycast_inputs.txt")
    set(_wc3_ray_report "${WC3_ROOT}/reports/generated/parallel_raycast.json")
    set(_wc3_ray_stamp "${_wc3_ray_dir}/.stamp")
    set(_wc3_ray_fingerprint_file "${_wc3_ray_dir}/.inputs")
    string(SHA256 _wc3_ray_fingerprint
        "dispatch-v1|${WC3_PARALLEL_RAYCAST_EXE}|${WC3_RELIFTED_SOURCES}")
    set(_wc3_ray_outputs)
    foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
        get_filename_component(_wc3_base "${_wc3_src}" NAME)
        list(APPEND _wc3_ray_outputs "${_wc3_ray_dir}/${_wc3_base}")
    endforeach()
    list(LENGTH _wc3_ray_outputs _wc3_ray_count)
    list(REMOVE_DUPLICATES _wc3_ray_outputs)
    list(LENGTH _wc3_ray_outputs _wc3_ray_unique)
    if(NOT _wc3_ray_count EQUAL _wc3_ray_unique)
        message(FATAL_ERROR "parallel_raycast output basenames are not unique")
    endif()

    set(_wc3_ray_need 0)
    if(NOT EXISTS "${_wc3_ray_stamp}" OR
       NOT EXISTS "${_wc3_ray_fingerprint_file}" OR
       "${WC3_PARALLEL_RAYCAST_EXE}" IS_NEWER_THAN "${_wc3_ray_stamp}")
        set(_wc3_ray_need 1)
    else()
        file(READ "${_wc3_ray_fingerprint_file}" _wc3_ray_fingerprint_old)
        string(STRIP "${_wc3_ray_fingerprint_old}" _wc3_ray_fingerprint_old)
        if(NOT _wc3_ray_fingerprint_old STREQUAL _wc3_ray_fingerprint)
            set(_wc3_ray_need 1)
        endif()
        if(NOT _wc3_ray_need)
            foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
                if("${_wc3_src}" IS_NEWER_THAN "${_wc3_ray_stamp}")
                    set(_wc3_ray_need 1)
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(NOT _wc3_ray_need AND NOT EXISTS "${_wc3_ray_report}")
        set(_wc3_ray_need 1)
    endif()
    if(NOT _wc3_ray_need)
        foreach(_wc3_out IN LISTS _wc3_ray_outputs)
            if(NOT EXISTS "${_wc3_out}")
                set(_wc3_ray_need 1)
                break()
            endif()
        endforeach()
    endif()

    if(_wc3_ray_need)
        file(REMOVE_RECURSE "${_wc3_ray_dir}")
        file(MAKE_DIRECTORY "${_wc3_ray_dir}")
        get_filename_component(_wc3_ray_report_dir "${_wc3_ray_report}" DIRECTORY)
        file(MAKE_DIRECTORY "${_wc3_ray_report_dir}")
        set(_wc3_ray_list_text "")
        foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
            string(APPEND _wc3_ray_list_text "${_wc3_src}\n")
        endforeach()
        file(WRITE "${_wc3_ray_list}" "${_wc3_ray_list_text}")
        string(TIMESTAMP _wc3_ray_t0 "%s")
        execute_process(
            COMMAND "${WC3_PARALLEL_RAYCAST_EXE}"
                    --inputs "${_wc3_ray_list}"
                    --out-dir "${_wc3_ray_dir}"
                    --report "${_wc3_ray_report}"
                    --expect-sites 1
                    --vertex
                    --expect-vertex-sites 2
                    --triangle
                    --expect-triangle-sites 1
                    --vertex-counter
                    --expect-vertex-counter-sites 1
                    --quiet
            RESULT_VARIABLE _wc3_ray_rc
            OUTPUT_VARIABLE _wc3_ray_out
            ERROR_VARIABLE _wc3_ray_err)
        if(NOT _wc3_ray_rc EQUAL 0)
            message(FATAL_ERROR
                "parallel_raycast failed (rc=${_wc3_ray_rc}).\n"
                "${_wc3_ray_out}\n${_wc3_ray_err}")
        endif()
        string(TIMESTAMP _wc3_ray_t1 "%s")
        if(WC3_CORPUS_TIMING)
            math(EXPR _wc3_ray_dt "${_wc3_ray_t1} - ${_wc3_ray_t0}")
            message(STATUS "corpus timing: parallel_raycast: ${_wc3_ray_dt}s")
        endif()
        file(WRITE "${_wc3_ray_fingerprint_file}"
            "${_wc3_ray_fingerprint}\n")
        file(TOUCH "${_wc3_ray_stamp}")
    endif()
    if(_wc3_ray_outputs)
        set_property(SOURCE ${_wc3_ray_outputs}
            PROPERTY OBJECT_DEPENDS "${_wc3_ray_stamp}")
    endif()
    set(WC3_RELIFTED_SOURCES "${_wc3_ray_outputs}")
endif()

endif()

add_library(wc3_corpus_relifted STATIC ${WC3_RELIFTED_SOURCES})
target_include_directories(wc3_corpus_relifted PRIVATE
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest")
target_compile_definitions(wc3_corpus_relifted PRIVATE
    GVA_DELTA=0
    WC3_FLAGS_FAST_PF_AF=${WC3_FLAGS_FAST_PF_AF_DEFINE}
    WC3_DEBUG=${WC3_DEBUG_DEFINE})
# The relifted corpus is guest code with a frozen ABI.  Build it at the
# configured corpus level (O2 by default, O3 only in the optimized release
# profile), not -O0: the header-only runtime (cpu.h, x87.h) must inline, and
# generated bodies run once per guest instruction.  The stack protector and
# _FORTIFY_SOURCE add host canary/check calls that the legacy guest-memory
# targets already disable; no speed claim follows until G-10/PM-19 measure
# this corpus.
# O-32: the NDK passes -g in CMAKE_C_FLAGS, and target options are appended
# after it, so -g0 here (next to the optimization level) wins for the
# generated corpus only.  Expected effect is compile time, link peak RSS, .a
# size and disk I/O (PM-22); no frame-time claim until G-10/PM-19, and the
# L_<va> set plus the G-15 body check must stay unchanged.
if(NOT DEFINED WC3_CORPUS_COMPILE_OPTIONS OR
   "${WC3_CORPUS_COMPILE_OPTIONS}" STREQUAL "")
    if(DEFINED WC3_CORPUS_OPT_LEVEL_EFFECTIVE AND
       WC3_CORPUS_OPT_LEVEL_EFFECTIVE)
        set(WC3_CORPUS_COMPILE_OPTIONS
            "-${WC3_CORPUS_OPT_LEVEL_EFFECTIVE}")
    else()
        set(WC3_CORPUS_COMPILE_OPTIONS -O2)
    endif()
endif()
target_compile_options(wc3_corpus_relifted PRIVATE
    ${WC3_CORPUS_COMPILE_OPTIONS}
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
if(WC3_CORPUS_WERROR)
    target_compile_options(wc3_corpus_relifted PRIVATE -Werror)
endif()
set_target_properties(wc3_corpus_relifted PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(WC3_W3L_COMPAT)
    # The pass emits direct calls to the native helpers and uses the
    # WC3_W3L_LATENCY macro.  Force-include one header instead of editing the
    # include block of every chunk; both the include path and the definition
    # exist only in a compat build.
    target_include_directories(wc3_corpus_relifted PRIVATE
        "${WC3_ROOT}/runtime/compat")
    target_compile_definitions(wc3_corpus_relifted PRIVATE
        WC3_W3L_LATENCY=${WC3_W3L_LATENCY})
    # CMake de-duplicates option tokens when it generates the compile command:
    # two separate "-include <path>" entries collapse into one "-include"
    # followed by both paths, and clang then treats the second header as a
    # second input file.  SHELL: keeps the flag and its path as one unit, so
    # ccache receives the two arguments it understands; a joined
    # "-includeE:/..." token is rewritten by MSYS2 ccache into a broken path.
    target_compile_options(wc3_corpus_relifted PRIVATE
        "SHELL:-include ${WC3_ROOT}/runtime/compat/w3l_compat.h")
endif()

if(WC3_SELECTION)
    # The 12/24/40 selection pass emits direct calls to the native helpers in
    # runtime/features.  Force-include the declarations instead of editing the
    # include block of every chunk; the header exists only in a
    # selection build.
    target_include_directories(wc3_corpus_relifted PRIVATE
        "${WC3_ROOT}/runtime/features")
    target_compile_options(wc3_corpus_relifted PRIVATE
        "SHELL:-include ${WC3_ROOT}/runtime/features/selection.h")
endif()

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
