add_library(ccr_gui STATIC
    src/ccr_gui.c)

target_include_directories(ccr_gui PUBLIC include)
# GLFW comes inside raylib; its header lets us check for a display first
target_include_directories(ccr_gui SYSTEM PRIVATE ${raylib_SOURCE_DIR}/src/external/glfw/include)
target_link_libraries(ccr_gui
    PUBLIC  ccr_warnings
    PRIVATE raylib)
