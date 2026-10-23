# Each plugin is its own shared library exporting ccr_plugin_entry().
# See include/ccratch/ccr_plugin.h for the contract.

add_subdirectory(hello)        # the template: built, never loaded
add_subdirectory(code_hater)   # the first official one
