add_executable(ccratch_app main.c)
set_target_properties(ccratch_app PROPERTIES
    OUTPUT_NAME ccratch
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN/../lib")
target_link_libraries(ccratch_app PRIVATE ccratch ccr_warnings)
