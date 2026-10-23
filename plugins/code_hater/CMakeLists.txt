# Code Hater: the first official plugin. Built next to the program, in
# share/ccratch/plugins, where Ccratch looks for plugins.
add_library(ccr_plugin_code_hater MODULE code_hater.c)
target_include_directories(ccr_plugin_code_hater PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ccr_plugin_code_hater PRIVATE ccr_warnings)
set_target_properties(ccr_plugin_code_hater PROPERTIES
    C_VISIBILITY_PRESET hidden
    PREFIX ""
    OUTPUT_NAME "code_hater"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/share/ccratch/plugins)
