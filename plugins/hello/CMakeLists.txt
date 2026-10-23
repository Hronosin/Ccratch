add_library(ccr_plugin_hello MODULE hello.c)
target_include_directories(ccr_plugin_hello PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ccr_plugin_hello PRIVATE ccr_warnings)
set_target_properties(ccr_plugin_hello PROPERTIES
    C_VISIBILITY_PRESET hidden
    PREFIX ""
    OUTPUT_NAME "hello"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/plugins)   # not where Ccratch looks
