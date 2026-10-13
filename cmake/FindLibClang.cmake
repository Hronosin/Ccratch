# Finds libclang (the C API of Clang) and defines the imported target
# LibClang::LibClang.
#
# Search order:
#   1. LIBCLANG_ROOT, if set (expects include/ and lib/ under it)
#   2. llvm-config on PATH
#   3. the usual system locations (Fedora: clang-devel puts it in /usr)

find_program(LLVM_CONFIG_EXECUTABLE
    NAMES llvm-config llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17)

set(_libclang_include_hints "")
set(_libclang_lib_hints "")

if(LIBCLANG_ROOT)
    list(APPEND _libclang_include_hints ${LIBCLANG_ROOT}/include)
    list(APPEND _libclang_lib_hints ${LIBCLANG_ROOT}/lib ${LIBCLANG_ROOT}/lib64)
endif()

if(LLVM_CONFIG_EXECUTABLE)
    execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --includedir
        OUTPUT_VARIABLE _llvm_inc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --libdir
        OUTPUT_VARIABLE _llvm_lib OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    list(APPEND _libclang_include_hints ${_llvm_inc})
    list(APPEND _libclang_lib_hints ${_llvm_lib})
endif()

find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS ${_libclang_include_hints})
find_library(LIBCLANG_LIBRARY NAMES clang libclang HINTS ${_libclang_lib_hints})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibClang
    REQUIRED_VARS LIBCLANG_LIBRARY LIBCLANG_INCLUDE_DIR
    FAIL_MESSAGE "libclang not found. Fedora: sudo dnf install clang-devel  |  Debian/Ubuntu: sudo apt install libclang-dev")

if(LibClang_FOUND AND NOT TARGET LibClang::LibClang)
    add_library(LibClang::LibClang UNKNOWN IMPORTED)
    set_target_properties(LibClang::LibClang PROPERTIES
        IMPORTED_LOCATION "${LIBCLANG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBCLANG_INCLUDE_DIR}")
endif()

# --- Clang's resource dir --------------------------------------------------
# Builtin headers (stddef.h, stdarg.h, ...) live in clang's "resource dir".
# libclang guesses it relative to its own .so, and some layouts break that
# guess (Fedora: libclang in /usr/lib64, headers in /usr/lib/clang/N).
# The clang driver knows the real answer, so ask it, and fall back to
# looking around. The result is baked into Ccratch and passed to libclang
# as -resource-dir. Override with -DLIBCLANG_RESOURCE_DIR=... or at run
# time with the CCRATCH_CLANG_RESOURCE_DIR environment variable.

if(NOT LIBCLANG_RESOURCE_DIR)
    set(_clang_hints "")
    if(LLVM_CONFIG_EXECUTABLE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --bindir
            OUTPUT_VARIABLE _llvm_bin OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        list(APPEND _clang_hints ${_llvm_bin})
    endif()
    find_program(LIBCLANG_CLANG_EXECUTABLE
        NAMES clang clang-22 clang-21 clang-20 clang-19 clang-18 clang-17
        HINTS ${_clang_hints})

    set(_resource "")
    if(LIBCLANG_CLANG_EXECUTABLE)
        execute_process(COMMAND ${LIBCLANG_CLANG_EXECUTABLE} -print-resource-dir
            OUTPUT_VARIABLE _rd OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_rd AND EXISTS "${_rd}/include/stddef.h")
            get_filename_component(_resource "${_rd}" REALPATH)
        endif()
    endif()

    # No clang driver around: look next to libclang and in the usual places.
    if(NOT _resource AND LIBCLANG_LIBRARY)
        get_filename_component(_libdir "${LIBCLANG_LIBRARY}" DIRECTORY)
        file(GLOB _candidates
            "${_libdir}/clang/*/include/stddef.h"
            "${_libdir}/../lib/clang/*/include/stddef.h"
            "/usr/lib/clang/*/include/stddef.h"
            "/usr/lib64/clang/*/include/stddef.h")
        if(_candidates)
            list(SORT _candidates COMPARE NATURAL ORDER DESCENDING)
            list(GET _candidates 0 _best)
            get_filename_component(_resource "${_best}/../.." REALPATH)
        endif()
    endif()

    if(_resource)
        set(LIBCLANG_RESOURCE_DIR "${_resource}" CACHE PATH "Clang resource dir (builtin headers)")
    endif()
endif()

if(LIBCLANG_RESOURCE_DIR)
    message(STATUS "Clang resource dir: ${LIBCLANG_RESOURCE_DIR}")
else()
    message(WARNING "Clang's builtin headers (stddef.h) were not found. "
                    "Parsing will report missing headers. Fedora: sudo dnf install clang")
endif()

mark_as_advanced(LIBCLANG_INCLUDE_DIR LIBCLANG_LIBRARY LLVM_CONFIG_EXECUTABLE
                 LIBCLANG_CLANG_EXECUTABLE LIBCLANG_RESOURCE_DIR)
