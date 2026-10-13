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

mark_as_advanced(LIBCLANG_INCLUDE_DIR LIBCLANG_LIBRARY LLVM_CONFIG_EXECUTABLE)
