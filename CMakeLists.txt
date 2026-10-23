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

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(cmake/deps.cmake)

add_subdirectory(gui)       # libccr_gui   - thin wrapper over raylib
add_subdirectory(ccratch)   # libccratch   - the whole IDE, one library
add_subdirectory(app)       # ccratch      - the launcher, one line of logic
add_subdirectory(plugins)   # *.so         - everything optional

# Language packs and themes are plain files found at run time, next to the program in
# share/ccratch/lang (bin/../share/...). Copied there on every build, so a
# pack edited in lang/ shows up after `cmake --build`.
file(GLOB CCR_LANG_PACKS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/lang/*.lang ${PROJECT_SOURCE_DIR}/lang/*.explain)
set(CCR_LANG_OUT ${CMAKE_BINARY_DIR}/share/ccratch/lang)
add_custom_target(ccratch_lang ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CCR_LANG_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CCR_LANG_PACKS} ${CCR_LANG_OUT}
    DEPENDS ${CCR_LANG_PACKS}
    COMMENT "Language packs")
file(GLOB CCR_THEMES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/themes/*.theme)
set(CCR_THEME_OUT ${CMAKE_BINARY_DIR}/share/ccratch/themes)
add_custom_target(ccratch_themes ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CCR_THEME_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CCR_THEMES} ${CCR_THEME_OUT}
    DEPENDS ${CCR_THEMES}
    COMMENT "Themes")

file(GLOB CCR_BLOCK_PACKS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/blocks/*.yaml)
set(CCR_BLOCKS_OUT ${CMAKE_BINARY_DIR}/share/ccratch/blocks)
add_custom_target(ccratch_blocks ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CCR_BLOCKS_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CCR_BLOCK_PACKS} ${CCR_BLOCKS_OUT}
    DEPENDS ${CCR_BLOCK_PACKS}
    COMMENT "Block packs")

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
