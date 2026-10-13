cmake_minimum_required(VERSION 3.21)

project(Ccratch
    VERSION 0.0.1
    DESCRIPTION "Scratch for raw C. Segfaults included."
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# raylib ends up inside libccratch.so, so every static piece must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Everything lands next to each other so the launcher finds libccratch.so
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(CCR_PARANOID "Treat warnings as errors and enable sanitizers" OFF)

# Our own code is held to a higher standard than the dependencies.
add_library(ccr_warnings INTERFACE)
target_compile_options(ccr_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
if(CCR_PARANOID)
    target_compile_options(ccr_warnings INTERFACE
        -Werror -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ccr_warnings INTERFACE -fsanitize=address,undefined)
endif()

include(cmake/deps.cmake)

add_subdirectory(gui)       # libccr_gui   - thin wrapper over raylib + raygui
add_subdirectory(ccratch)   # libccratch   - the whole IDE, one library
add_subdirectory(app)       # ccratch      - the launcher, one line of logic
add_subdirectory(plugins)   # *.so         - everything optional
