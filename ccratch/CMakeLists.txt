# libccratch: the modular monolith. All the fun lives here.

find_package(LibClang REQUIRED)

# The icon is baked into the library: no asset files to lose.
set(CCR_ICON_SRC ${PROJECT_SOURCE_DIR}/assets/icon.png)
set(CCR_ICON_C   ${CMAKE_CURRENT_BINARY_DIR}/generated/ccr_icon.c)
add_custom_command(
    OUTPUT  ${CCR_ICON_C}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CCR_ICON_SRC} -DOUTPUT=${CCR_ICON_C}
            -DNAME=ccr_icon_png -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CCR_ICON_SRC} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding the icon")

# The code, compiled once. libccratch.so is made from it, and so are the
# tests, which need the internals the .so keeps hidden.
add_library(ccratch_core OBJECT
    src/ccr_app.c
    src/ccr_ast.c
    src/ccr_blocks.c
    src/ccr_canvas.c
    src/ccr_codegen.c
    src/ccr_editor.c
    src/ccr_parse.c
    ${CCR_ICON_C})

target_include_directories(ccratch_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include src)

target_compile_definitions(ccratch_core PRIVATE CCR_BUILDING_LIB)
if(LIBCLANG_RESOURCE_DIR)
    target_compile_definitions(ccratch_core PRIVATE
        CCR_CLANG_RESOURCE_DIR="${LIBCLANG_RESOURCE_DIR}")
endif()
target_link_libraries(ccratch_core
    PUBLIC ccr_gui ccr_warnings LibClang::LibClang m ${CMAKE_DL_LIBS})
set_target_properties(ccratch_core PROPERTIES C_VISIBILITY_PRESET hidden)

add_library(ccratch SHARED $<TARGET_OBJECTS:ccratch_core>)
target_include_directories(ccratch PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ccratch PRIVATE ccratch_core)

# Keep raylib/glfw symbols from leaking out of the .so: plugins see only
# the public ccr_* API, never the GUI backend.
target_link_options(ccratch PRIVATE -Wl,--exclude-libs,ALL)

set_target_properties(ccratch PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
