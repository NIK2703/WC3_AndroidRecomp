# Fail the product link if a SoftFloat or x87-software arithmetic symbol made
# it into libwc3gameactivity.so.  The native x87 model is header-only and uses
# double/float; any of these names means a stale corpus object or an accidental
# vendor input still pulls the removed implementation in.
if(NOT DEFINED WC3_NM OR NOT DEFINED WC3_SO)
    message(FATAL_ERROR "CheckNoSoftFloat.cmake needs WC3_NM and WC3_SO")
endif()

execute_process(
    COMMAND "${WC3_NM}" --defined-only "${WC3_SO}"
    RESULT_VARIABLE _wc3_nm_rc
    OUTPUT_VARIABLE _wc3_nm_out
    ERROR_VARIABLE _wc3_nm_err)
if(NOT _wc3_nm_rc EQUAL 0)
    message(FATAL_ERROR
        "no_softfloat_symbols: nm failed (${_wc3_nm_rc}): ${_wc3_nm_err}")
endif()

# SoftFloat's public and internal entry points.  Keep the match anchored to a
# symbol name, not to a source-file record printed by nm.
set(_wc3_sf_re
    "(^|[ \t])(softfloat_[A-Za-z0-9_]+|extF80_[A-Za-z0-9_]+|f(16|32|64|128)_[A-Za-z0-9_]+|(ui|i)(16|32|64)_[A-Za-z0-9_]+|commonNaN[A-Za-z0-9_]+|s_(addMags|subMags|roundPack|normRoundPack)[A-Za-z0-9_]*)")
string(REGEX MATCHALL "${_wc3_sf_re}" _wc3_sf_hits "${_wc3_nm_out}")
if(_wc3_sf_hits)
    list(LENGTH _wc3_sf_hits _wc3_sf_count)
    list(REMOVE_DUPLICATES _wc3_sf_hits)
    string(REPLACE ";" "\n  " _wc3_sf_list "${_wc3_sf_hits}")
    message(FATAL_ERROR
        "no_softfloat_symbols: ${_wc3_sf_count} SoftFloat symbol(s) in ${WC3_SO}:\n"
        "  ${_wc3_sf_list}\n"
        "Relift the corpus with the native x87 lifter and remove the stale "
        "SoftFloat archive from the link.")
endif()
message(STATUS "no_softfloat_symbols: clean (${WC3_SO})")
