# BP-12: every C/C++ compilation in this tree goes through ccache.
#
# CMake 3.22 supports compiler launchers, so the cache is enabled without
# wrapping the compiler, changing PATH, or touching generated build files.
# The product build sets WC3_REQUIRE_CCACHE=ON; standalone configurations may
# leave it OFF and get a warning instead of a hard configure failure.
#
# Detection order:
#   1. -DWC3_CCACHE=... (explicit path)
#   2. CCACHE environment variable
#   3. ccache / ccache.exe on PATH
#   4. the standard MSYS2 installation locations on Windows
#
# On Windows the MSYS2 ccache binary rejects a compiler argument containing
# backslashes ("Could not find compiler ... in PATH").  CMake writes the NDK
# compiler path with Windows separators, so a tiny Python launcher normalizes
# that one argument before handing the command to ccache.  The launcher adds
# no cache logic of its own; it only fixes the path spelling.

option(WC3_USE_CCACHE "Use ccache for C/C++ compilation" ON)
option(WC3_REQUIRE_CCACHE "Fail configuration when ccache is unavailable" OFF)

if(NOT WC3_USE_CCACHE)
    if(WC3_REQUIRE_CCACHE)
        message(FATAL_ERROR
            "WC3_REQUIRE_CCACHE=ON conflicts with WC3_USE_CCACHE=OFF; "
            "enable ccache or drop the requirement explicitly.")
    endif()
    message(STATUS "[wc3] ccache: disabled (WC3_USE_CCACHE=OFF)")
    return()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "MSVC")
    message(STATUS "[wc3] ccache: skipped for MSVC")
    return()
endif()

set(_wc3_ccache "")
if(WC3_CCACHE)
    file(TO_CMAKE_PATH "${WC3_CCACHE}" _wc3_candidate)
    if(EXISTS "${_wc3_candidate}")
        set(_wc3_ccache "${_wc3_candidate}")
    else()
        message(WARNING "[wc3] WC3_CCACHE does not exist: ${WC3_CCACHE}")
    endif()
endif()

if(NOT _wc3_ccache AND DEFINED ENV{CCACHE} AND NOT "$ENV{CCACHE}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CCACHE}" _wc3_candidate)
    if(EXISTS "${_wc3_candidate}")
        set(_wc3_ccache "${_wc3_candidate}")
    endif()
endif()

if(NOT _wc3_ccache)
    find_program(_wc3_ccache NAMES ccache ccache.exe)
endif()

if(NOT _wc3_ccache AND CMAKE_HOST_WIN32)
    foreach(_wc3_candidate IN ITEMS
            "C:/msys64/usr/bin/ccache.exe"
            "C:/msys64/ucrt64/bin/ccache.exe"
            "C:/msys64/mingw64/bin/ccache.exe"
            "C:/msys64/clang64/bin/ccache.exe"
            "C:/ProgramData/chocolatey/bin/ccache.exe"
            "C:/Program Files/ccache/ccache.exe")
        if(EXISTS "${_wc3_candidate}")
            set(_wc3_ccache "${_wc3_candidate}")
            break()
        endif()
    endforeach()
endif()

if(_wc3_ccache)
    execute_process(
        COMMAND "${_wc3_ccache}" --version
        RESULT_VARIABLE _wc3_ccache_rc
        OUTPUT_VARIABLE _wc3_ccache_output
        ERROR_VARIABLE _wc3_ccache_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _wc3_ccache_rc EQUAL 0)
        message(WARNING
            "[wc3] ccache is not runnable: ${_wc3_ccache}\n${_wc3_ccache_error}")
        set(_wc3_ccache "")
    else()
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _wc3_ccache_version
               "${_wc3_ccache_output}")
    endif()
endif()

if(NOT _wc3_ccache)
    if(WC3_REQUIRE_CCACHE)
        message(FATAL_ERROR
            "ccache is required for this build (WC3_REQUIRE_CCACHE=ON) but "
            "was not found. Install ccache, set WC3_CCACHE to its executable, "
            "or configure with -DWC3_REQUIRE_CCACHE=OFF.")
    endif()
    message(WARNING
        "[wc3] ccache not found; compiling without a compiler cache. "
        "Canonical builds set WC3_REQUIRE_CCACHE=ON.")
    return()
endif()

set(WC3_CCACHE "${_wc3_ccache}" CACHE FILEPATH "ccache executable" FORCE)

set(_wc3_launcher "${_wc3_ccache}")
if(CMAKE_HOST_WIN32)
    find_program(WC3_PYTHON_EXECUTABLE NAMES python3 python python.exe)
    set(_wc3_wrapper "${WC3_ROOT}/tools/ccache_wrapper.py")
    if(WC3_PYTHON_EXECUTABLE AND EXISTS "${_wc3_wrapper}")
        set(_wc3_launcher
            "${WC3_PYTHON_EXECUTABLE};${_wc3_wrapper};${_wc3_ccache}")
    elseif(WC3_REQUIRE_CCACHE)
        message(FATAL_ERROR
            "The Windows ccache launcher needs Python and "
            "${_wc3_wrapper}; set WC3_PYTHON_EXECUTABLE or disable ccache.")
    else()
        message(WARNING
            "[wc3] ccache path wrapper unavailable; using ccache directly")
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(CMAKE_C_COMPILER_LAUNCHER "${_wc3_launcher}" CACHE STRING
        "BP-12 ccache launcher for C" FORCE)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_wc3_launcher}" CACHE STRING
        "BP-12 ccache launcher for C++" FORCE)
endif()

message(STATUS "[wc3] ccache: ${_wc3_ccache} (${_wc3_ccache_version})")
message(STATUS "[wc3] ccache launcher: ${_wc3_launcher}")
