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

find_program(WC3_W3L_COMPAT_EXE
    NAMES w3l_compat w3l_compat.exe
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
set(_wc3_compat_jsons)
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
               NOT "${WC3_SAVE_LIST_DISPLAY_EXE}" IS_NEWER_THAN "${_save_src}")
                set(_save_need 0)
            endif()
            if(_save_need)
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
                string(STRIP "${_save_out}" _save_out)
                message(STATUS "${_save_out}")
            endif()
            file(READ "${_save_src}" _save_text)
            string(REGEX MATCHALL "WC3_SAVE_LIST_STRIP_W3Z"
                   _save_markers "${_save_text}")
            list(LENGTH _save_markers _save_marker_count)
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
                    execute_process(
                        COMMAND "${WC3_DEVIRTUALIZE_EXE}"
                                --list "${WC3_BOOT_LIST}"
                                --lift "${_chunk}"
                                --out "${_devirt_out}"
                                --json "${_devirt_json}"
                        RESULT_VARIABLE _devirt_rc
                        OUTPUT_VARIABLE _devirt_stdout
                        ERROR_VARIABLE _devirt_stderr)
                    if(NOT _devirt_rc EQUAL 0)
                        message(FATAL_ERROR
                            "devirtualize failed for ${_chunk} "
                            "(rc=${_devirt_rc}):\n"
                            "${_devirt_stdout}\n${_devirt_stderr}")
                    endif()
                endif()
                list(APPEND _transformed_chunks "${_devirt_out}")
            endforeach()
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
            set(_ra_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_ra_out "${_ra_dir}/${_chunk_name}")
                set(_ra_json "${_ra_out}.json")
                if(NOT EXISTS "${_ra_out}" OR
                   NOT EXISTS "${_ra_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_ra_out}" OR
                   "${WC3_RETURN_ADDRESS_ELIDE_EXE}" IS_NEWER_THAN "${_ra_out}")
                    execute_process(
                        COMMAND "${WC3_RETURN_ADDRESS_ELIDE_EXE}"
                                --lift "${_chunk}"
                                --out "${_ra_out}"
                                --json "${_ra_json}"
                                --quiet
                        RESULT_VARIABLE _ra_rc
                        OUTPUT_VARIABLE _ra_stdout
                        ERROR_VARIABLE _ra_stderr)
                    if(NOT _ra_rc EQUAL 0)
                        message(FATAL_ERROR
                            "return_address_elide failed for ${_chunk} "
                            "(rc=${_ra_rc}):\n"
                            "${_ra_stdout}\n${_ra_stderr}")
                    endif()
                endif()
                list(APPEND _ra_chunks "${_ra_out}")
            endforeach()
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
            set(_dead_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_dead_out "${_dead_dir}/${_chunk_name}")
                set(_dead_json "${_dead_out}.json")
                if(NOT EXISTS "${_dead_out}" OR
                   NOT EXISTS "${_dead_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_dead_out}" OR
                   "${WC3_DEAD_FLAGS_EXE}" IS_NEWER_THAN "${_dead_out}")
                    execute_process(
                        COMMAND "${WC3_DEAD_FLAGS_EXE}"
                                --lift "${_chunk}"
                                --out "${_dead_out}"
                                --json "${_dead_json}"
                                --quiet
                        RESULT_VARIABLE _dead_rc
                        OUTPUT_VARIABLE _dead_stdout
                        ERROR_VARIABLE _dead_stderr)
                    if(NOT _dead_rc EQUAL 0)
                        message(FATAL_ERROR
                            "dead_flags failed for ${_chunk} "
                            "(rc=${_dead_rc}):\n"
                            "${_dead_stdout}\n${_dead_stderr}")
                    endif()
                endif()
                list(APPEND _dead_chunks "${_dead_out}")
            endforeach()
            set(_chunks "${_dead_chunks}")
        endif()
        # D3b first increment: a label-free leaf function without a guest
        # call can keep its XMM values in a local XMM[8] and write them back
        # only before an exit.  Host helpers are allowed; they must not read
        # CPU.xmm directly.  This pass is part of the normal relifted-corpus
        # pipeline and has no opt-in switch.  It runs after P0-2 and before the
        # W3L profile, which must see the final instruction text.
        if(NOT WC3_XMM_LOCAL_EXE)
            message(FATAL_ERROR
                "xmm_local.exe missing; run tools/lift/build_tools.ps1 "
                "before configuring")
        endif()
        set(_xmm_dir
            "${CMAKE_CURRENT_BINARY_DIR}/xmm_local/${_name}/${_kind}")
        file(MAKE_DIRECTORY "${_xmm_dir}")
        set(_xmm_chunks)
        foreach(_chunk IN LISTS _chunks)
            get_filename_component(_chunk_name "${_chunk}" NAME)
            set(_xmm_out "${_xmm_dir}/${_chunk_name}")
            set(_xmm_json "${_xmm_out}.json")
            if(NOT EXISTS "${_xmm_out}" OR
               NOT EXISTS "${_xmm_json}" OR
               "${_chunk}" IS_NEWER_THAN "${_xmm_out}" OR
               "${WC3_XMM_LOCAL_EXE}" IS_NEWER_THAN "${_xmm_out}")
                execute_process(
                    COMMAND "${WC3_XMM_LOCAL_EXE}"
                            --lift "${_chunk}"
                            --out "${_xmm_out}"
                            --json "${_xmm_json}"
                            --quiet
                    RESULT_VARIABLE _xmm_rc
                    OUTPUT_VARIABLE _xmm_stdout
                    ERROR_VARIABLE _xmm_stderr)
                if(NOT _xmm_rc EQUAL 0)
                    message(FATAL_ERROR
                        "xmm_local failed for ${_chunk} "
                        "(rc=${_xmm_rc}):\n"
                        "${_xmm_stdout}\n${_xmm_stderr}")
                endif()
            endif()
            list(APPEND _xmm_chunks "${_xmm_out}")
        endforeach()
        set(_chunks "${_xmm_chunks}")
        if(WC3_W3L_COMPAT)
            # The W3L/PvPGN/rubattle profile runs after every other enabled
            # pass, so it sees the final instruction text.  The profile and
            # latency are part of the output path: switching either one can
            # never reuse a stale chunk from another profile.
            set(_compat_dir
                "${CMAKE_CURRENT_BINARY_DIR}/w3l_compat/${_name}/${_kind}/${WC3_W3L_PROFILE}-${WC3_W3L_LATENCY}")
            file(MAKE_DIRECTORY "${_compat_dir}")
            set(_compat_chunks)
            foreach(_chunk IN LISTS _chunks)
                get_filename_component(_chunk_name "${_chunk}" NAME)
                set(_compat_out "${_compat_dir}/${_chunk_name}")
                set(_compat_json "${_compat_out}.json")
                if(NOT EXISTS "${_compat_out}" OR
                   NOT EXISTS "${_compat_json}" OR
                   "${_chunk}" IS_NEWER_THAN "${_compat_out}" OR
                   "${WC3_W3L_PASS}" IS_NEWER_THAN "${_compat_out}")
                    execute_process(
                        COMMAND "${WC3_W3L_PASS}"
                                --profile "${WC3_W3L_PROFILE}"
                                --latency "${WC3_W3L_LATENCY}"
                                --lift "${_chunk}"
                                --out "${_compat_out}"
                                --json "${_compat_json}"
                                --quiet
                        RESULT_VARIABLE _compat_rc
                        OUTPUT_VARIABLE _compat_stdout
                        ERROR_VARIABLE _compat_stderr)
                    if(NOT _compat_rc EQUAL 0)
                        message(FATAL_ERROR
                            "w3l_compat failed for ${_chunk} (rc=${_compat_rc}):\n"
                            "${_compat_stdout}\n${_compat_stderr}")
                    endif()
                endif()
                list(APPEND _compat_chunks "${_compat_out}")
                list(APPEND _wc3_compat_jsons "${_compat_json}")
            endforeach()
            set(_chunks "${_compat_chunks}")
        endif()
        list(APPEND WC3_RELIFTED_SOURCES ${_chunks})
    endforeach()

    if(WC3_MANIFEST_TOOL)
        list(APPEND _wc3_check_commands
             COMMAND "${WC3_MANIFEST_TOOL}" "${_module_dir}" --strict
                     --active-list "${_module_dir}/corpus.list.h" --top 5)
    endif()
endforeach()

if(WC3_W3L_COMPAT)
    # Per-chunk runs cannot use --require-all, because sites are spread over
    # several chunks.  Verify the union instead: every site of the selected
    # profile must be applied exactly once, or the configure fails closed.
    set(_wc3_compat_manifest
        "${CMAKE_CURRENT_BINARY_DIR}/w3l_compat_change_sets.txt")
    file(WRITE "${_wc3_compat_manifest}" "")
    foreach(_json IN LISTS _wc3_compat_jsons)
        file(APPEND "${_wc3_compat_manifest}" "${_json}\n")
    endforeach()
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

if(NOT WC3_RELIFTED_SOURCES)
    message(FATAL_ERROR "relifted corpus has no translation units")
endif()

# D4/D4b: the call-frame ladder needs a complete-corpus summary, because a
# caller and its callee may be split into different translation units.  Run one
# text pass over the final chunks after every other pass, then compile its
# outputs.  The pass selects a leaf, tail-free or draining frame per direct call
# target; unknown targets and tailful calls in unproven caller bodies keep the
# full WC3_DIRECT_CALL.
set(_wc3_leaf_dir "${CMAKE_CURRENT_BINARY_DIR}/leaf_call")
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
    file(WRITE "${_wc3_leaf_list}" "")
    foreach(_wc3_src IN LISTS WC3_RELIFTED_SOURCES)
        file(APPEND "${_wc3_leaf_list}" "${_wc3_src}\n")
    endforeach()
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
    file(WRITE "${_wc3_leaf_fingerprint_file}"
        "${_wc3_leaf_fingerprint}\n")
    file(TOUCH "${_wc3_leaf_stamp}")
endif()
set(WC3_RELIFTED_SOURCES "${_wc3_leaf_outputs}")

add_library(wc3_corpus_relifted STATIC ${WC3_RELIFTED_SOURCES})
target_include_directories(wc3_corpus_relifted PRIVATE
    "${WC3_ROOT}/runtime/cpu"
    "${WC3_ROOT}/runtime/guest")
target_compile_definitions(wc3_corpus_relifted PRIVATE
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

if(WC3_W3L_COMPAT)
    # The pass emits direct calls to the native helpers and uses the
    # WC3_W3L_LATENCY macro.  Force-include one header instead of editing the
    # include block of every chunk; both the include path and the definition
    # exist only in a compat build.
    target_include_directories(wc3_corpus_relifted PRIVATE
        "${WC3_ROOT}/runtime/compat")
    target_compile_definitions(wc3_corpus_relifted PRIVATE
        WC3_W3L_LATENCY=${WC3_W3L_LATENCY})
    target_compile_options(wc3_corpus_relifted PRIVATE
        -include "${WC3_ROOT}/runtime/compat/w3l_compat.h")
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
