# libccratch: the modular monolith. All the fun lives here.

find_package(LibClang REQUIRED)

add_library(ccratch SHARED
    src/ccr_app.c
    src/ccr_ast.c
    src/ccr_canvas.c
    src/ccr_parse.c)

target_include_directories(ccratch
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE src)

target_compile_definitions(ccratch PRIVATE CCR_BUILDING_LIB)
target_link_libraries(ccratch PRIVATE ccr_gui ccr_warnings LibClang::LibClang m)

# Keep raylib/glfw symbols from leaking out of the .so: plugins see only
# the public ccr_* API, never the GUI backend.
target_link_options(ccratch PRIVATE -Wl,--exclude-libs,ALL)

set_target_properties(ccratch PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
