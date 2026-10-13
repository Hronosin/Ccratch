add_library(ccr_gui STATIC
    src/ccr_gui.c)

target_include_directories(ccr_gui PUBLIC include)
target_link_libraries(ccr_gui
    PUBLIC  ccr_warnings
    PRIVATE raylib raygui)
