# Headless tests: everything here runs without a window.

set(CCR_BIN $<TARGET_FILE:ccratch_app>)

add_test(NAME cli_version COMMAND ${CCR_BIN} --version)

# No display: a clear message, not a crash (raylib 5.5 segfaults there).
add_test(NAME gui_no_display
         COMMAND ${CMAKE_COMMAND} -E env DISPLAY=:254 WAYLAND_DISPLAY= ${CCR_BIN} --smoke-frames 1)
set_tests_properties(gui_no_display PROPERTIES
    PASS_REGULAR_EXPRESSION "could not open a window")

# Clang's builtin headers were located at configure time.
if(LIBCLANG_RESOURCE_DIR)
    add_test(NAME cli_about_resource_dir COMMAND ${CCR_BIN} --about)
    set_tests_properties(cli_about_resource_dir PROPERTIES
        PASS_REGULAR_EXPRESSION "resource dir: /")
endif()

# The tour parses cleanly and nothing in it falls through to an
# unmodelled "Other" node.
add_test(NAME dump_ast_tour
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/examples/tour.c)
set_tests_properties(dump_ast_tour PROPERTIES
    PASS_REGULAR_EXPRESSION " 0 errors"
    FAIL_REGULAR_EXPRESSION "Other \\(")

# Macro bodies used in this file must survive (MAX expands to a ternary).
add_test(NAME dump_ast_macro_body
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/examples/tour.c)
set_tests_properties(dump_ast_macro_body PROPERTIES
    PASS_REGULAR_EXPRESSION "Ternary")

# An object-like macro used as a value stays one node, named after it...
add_test(NAME dump_ast_macro_value
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/examples/tour.c)
set_tests_properties(dump_ast_macro_value PROPERTIES
    PASS_REGULAR_EXPRESSION "MacroValue ARRAY_LEN")
# ...but literals passed to a function-like macro keep their own text.
add_test(NAME dump_ast_macro_args
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/examples/tour.c)
set_tests_properties(dump_ast_macro_args PROPERTIES
    PASS_REGULAR_EXPRESSION "Int 2  \\[46:"
    FAIL_REGULAR_EXPRESSION "Int MAX")

# The callee is folded into the Call, but the arguments must stay.
add_test(NAME dump_ast_call_args
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/examples/tour.c)
set_tests_properties(dump_ast_call_args PROPERTIES
    PASS_REGULAR_EXPRESSION "Call free[^\n]*\n[^\n]*Ref p "
    FAIL_REGULAR_EXPRESSION "Ref free ")

# Broken code still produces a tree, exits with 1 and reports the error.
add_test(NAME dump_ast_broken
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/tests/data/broken.c)
set_tests_properties(dump_ast_broken PROPERTIES WILL_FAIL TRUE)

add_test(NAME dump_ast_broken_reports
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/tests/data/broken.c)
set_tests_properties(dump_ast_broken_reports PROPERTIES
    PASS_REGULAR_EXPRESSION "undeclared identifier")

# A file that does not exist is a hard failure, not a crash.
add_test(NAME dump_ast_missing
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/tests/data/nope.c)
set_tests_properties(dump_ast_missing PROPERTIES WILL_FAIL TRUE)

# Open the tour as blocks in a real window, if we can fake a display.
find_program(XVFB_RUN xvfb-run)
if(XVFB_RUN)
    add_test(NAME gui_blocks_tour
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     ${CCR_BIN} ${PROJECT_SOURCE_DIR}/examples/tour.c --smoke-frames 3)
    # several files open: the ones behind are kept, then all closed (under
    # the sanitizers, a file's memory freed twice shows here)
    add_test(NAME gui_several_files
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     ${CCR_BIN} ${PROJECT_SOURCE_DIR}/examples/tour.c ${PROJECT_SOURCE_DIR}/examples/goto.c
                     ${PROJECT_SOURCE_DIR}/tests/data/save.c --smoke-frames 3)
endif()

# Dragging blocks around edits the program correctly.
add_executable(test_editor test_editor.c)
target_link_libraries(test_editor PRIVATE ccratch_core)
if(XVFB_RUN)
    add_test(NAME editor_drag_and_drop
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     $<TARGET_FILE:test_editor> ${PROJECT_SOURCE_DIR}/examples/tour.c)
endif()

# The pie menu picks by direction.
add_executable(test_pie test_pie.c)
target_link_libraries(test_pie PRIVATE ccratch_core)
add_test(NAME pie_menu COMMAND test_pie)

# The joke agreement prints, and says where the real license is.
add_test(NAME cli_eula COMMAND ${CCR_BIN} --eula --lang en)
set_tests_properties(cli_eula PROPERTIES
    PASS_REGULAR_EXPRESSION "IHATERUSTL.*chaturanga.*Mozilla")
add_test(NAME cli_eula_ru COMMAND ${CCR_BIN} --eula --lang ru)
set_tests_properties(cli_eula_ru PROPERTIES PASS_REGULAR_EXPRESSION "чатурангу.*Mozilla")

# Language packs: plurals, fallbacks, broken packs, remembered choice.
add_executable(test_lang test_lang.c)
target_link_libraries(test_lang PRIVATE ccratch_core)
add_test(NAME lang_packs COMMAND test_lang ${CMAKE_CURRENT_BINARY_DIR}/lang_scratch)

# Every shipped pack is complete and safe; the template is a valid pack.
file(GLOB CCR_SHIPPED_PACKS ${PROJECT_SOURCE_DIR}/lang/*.lang)
foreach(pack ${CCR_SHIPPED_PACKS})
    get_filename_component(code ${pack} NAME_WE)
    add_test(NAME lang_check_${code} COMMAND ${CCR_BIN} --check-lang ${pack})
endforeach()
add_test(NAME lang_template
         COMMAND sh -c "$0 --lang-template > \"$1\" && $0 --check-lang \"$1\""
                 ${CCR_BIN} ${CMAKE_CURRENT_BINARY_DIR}/en.lang)

# goto arrows find their labels, and notice when one leaves.
add_executable(test_yaml test_yaml.c)
target_link_libraries(test_yaml PRIVATE ccratch_core)
add_test(NAME yaml COMMAND test_yaml)

add_executable(test_lib test_lib.c)
target_link_libraries(test_lib PRIVATE ccratch_core)
# the toy library's binaries, for the ABI check
add_library(toyabi SHARED data/toyabi.c)
add_library(toyabi_static STATIC data/toyabi.c)
foreach(t toyabi toyabi_static)
    set_target_properties(${t} PROPERTIES OUTPUT_NAME toy
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/abi
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/abi)
endforeach()
add_test(NAME block_packs COMMAND test_lib ${CMAKE_CURRENT_SOURCE_DIR}/data/broken_pack.yaml
                                  ${CMAKE_CURRENT_SOURCE_DIR}/data/toy.h
                                  ${CMAKE_CURRENT_SOURCE_DIR}/data/annotated.h
                                  $<TARGET_FILE:toyabi> $<TARGET_FILE:toyabi_static>)
add_dependencies(test_lib toyabi toyabi_static)
set_tests_properties(block_packs PROPERTIES
    ENVIRONMENT "CCRATCH_BLOCKS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/blocks")
# the packs that ship read clean
file(GLOB CCR_SHIPPED_PACKS ${PROJECT_SOURCE_DIR}/blocks/*.yaml)
foreach(pack ${CCR_SHIPPED_PACKS})
    get_filename_component(name ${pack} NAME_WE)
    add_test(NAME pack_${name} COMMAND ${CCR_BIN} --check-blocks ${pack})
endforeach()

add_executable(test_files test_files.c)
target_link_libraries(test_files PRIVATE ccratch_core)
add_test(NAME project_files COMMAND test_files)

add_executable(test_palette test_palette.c)
target_link_libraries(test_palette PRIVATE ccratch_core)
if(XVFB_RUN)
    add_test(NAME palette
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24" $<TARGET_FILE:test_palette>)
endif()

add_executable(test_jumps test_jumps.c)
target_link_libraries(test_jumps PRIVATE ccratch_core)
if(XVFB_RUN)
    add_test(NAME editor_goto_arrows
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     $<TARGET_FILE:test_jumps> ${PROJECT_SOURCE_DIR}/examples/goto.c)
endif()

# Every shipped theme sets every colour, and nothing else.
file(GLOB CCR_SHIPPED_THEMES ${PROJECT_SOURCE_DIR}/themes/*.theme)
foreach(theme ${CCR_SHIPPED_THEMES})
    get_filename_component(code ${theme} NAME_WE)
    add_test(NAME theme_check_${code} COMMAND ${CCR_BIN} --check-theme ${theme})
endforeach()
# A broken theme is reported line by line, not half-applied silently.
add_test(NAME theme_check_broken
         COMMAND ${CCR_BIN} --check-theme ${CMAKE_CURRENT_SOURCE_DIR}/data/broken.theme)
set_tests_properties(theme_check_broken PROPERTIES
    PASS_REGULAR_EXPRESSION "broken.theme:3: not a colour.*broken.theme:4: not a colour.*unknown key 'sparkles'")
# Each theme opens a window and draws.
if(XVFB_RUN)
    foreach(theme shrine serious masochist scratch)
        add_test(NAME gui_theme_${theme}
                 COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                         ${CCR_BIN} ${PROJECT_SOURCE_DIR}/examples/goto.c --theme ${theme}
                         --smoke-frames 3)
        set_tests_properties(gui_theme_${theme} PROPERTIES FAIL_REGULAR_EXPRESSION "no theme")
    endforeach()
endif()

# Saving: moves land in the file, everything else stays as written.
add_executable(test_save test_save.c)
target_link_libraries(test_save PRIVATE ccratch_core)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/saved)
if(XVFB_RUN)
    add_test(NAME editor_save
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     $<TARGET_FILE:test_save> ${CMAKE_CURRENT_SOURCE_DIR}/data/save.c
                     ${CMAKE_CURRENT_BINARY_DIR}/saved)
    set_tests_properties(editor_save PROPERTIES
        ENVIRONMENT "CCRATCH_BLOCKS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/blocks")
endif()

# Unchanged files come back byte for byte.
foreach(f examples/tour.c examples/goto.c tests/data/save.c)
    add_test(NAME roundtrip_${f} COMMAND ${CCR_BIN} --roundtrip ${PROJECT_SOURCE_DIR}/${f})
endforeach()

# C++ is picked by extension.
add_test(NAME dump_ast_cpp
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/tests/data/templates.cpp)
set_tests_properties(dump_ast_cpp PROPERTIES PASS_REGULAR_EXPRESSION " 0 errors")

# Rebuild every block of real files and read them back.
add_executable(test_rebuild test_rebuild.c)
target_link_libraries(test_rebuild PRIVATE ccratch_core)
foreach(f examples/tour.c tests/data/save.c)
    get_filename_component(stem ${f} NAME_WE)
    add_test(NAME rebuild_${stem}
             COMMAND test_rebuild ${CMAKE_CURRENT_BINARY_DIR}/saved/rebuilt_${stem}.c
                     ${PROJECT_SOURCE_DIR}/${f})
endforeach()
set(RAYLIB_SRC ${raylib_SOURCE_DIR}/src)
foreach(f rshapes rtextures rtext rmodels raudio rcore utils)
    add_test(NAME rebuild_raylib_${f}
             COMMAND test_rebuild ${CMAKE_CURRENT_BINARY_DIR}/saved/rebuilt_${f}.c
                     ${RAYLIB_SRC}/${f}.c -- -std=c99 -I${RAYLIB_SRC}
                     -I${RAYLIB_SRC}/external/glfw/include -DPLATFORM_DESKTOP)
endforeach()

# CMake as blocks: read, saved untouched, every body rebuilt. The corpus:
# this project's own CMake, raylib's, and the modules of the CMake at hand.
add_executable(test_cmake test_cmake.c)
target_link_libraries(test_cmake PRIVATE ccratch_core)
file(GLOB_RECURSE CCR_CMAKE_CORPUS ${PROJECT_SOURCE_DIR}/CMakeLists.txt
     ${PROJECT_SOURCE_DIR}/ccratch/CMakeLists.txt ${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt
     ${PROJECT_SOURCE_DIR}/cmake/*.cmake ${raylib_SOURCE_DIR}/CMakeLists.txt
     ${CMAKE_ROOT}/Modules/*.cmake)
add_test(NAME cmake_blocks COMMAND test_cmake ${CCR_CMAKE_CORPUS})
set_tests_properties(cmake_blocks PROPERTIES
    ENVIRONMENT "CCRATCH_BLOCKS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/blocks")
add_test(NAME roundtrip_cmake COMMAND ${CCR_BIN} --roundtrip ${PROJECT_SOURCE_DIR}/CMakeLists.txt)

# The Build button: compiler, linker and CMake output read for places;
# a real project configured, built, run and stopped.
add_executable(test_job test_job.c)
target_link_libraries(test_job PRIVATE ccratch_core)
add_test(NAME build_job COMMAND test_job)
set_tests_properties(build_job PROPERTIES TIMEOUT 240)

# The flags of the project's own build, from compile_commands.json.
add_executable(test_ccdb test_ccdb.c)
target_link_libraries(test_ccdb PRIVATE ccratch_core)
add_test(NAME compile_commands COMMAND test_ccdb)

add_executable(test_choice test_choice.c)
target_link_libraries(test_choice PRIVATE ccratch_core)
add_test(NAME slot_choices COMMAND test_choice ${CMAKE_CURRENT_SOURCE_DIR}/data/enums.c
                                   ${CMAKE_CURRENT_SOURCE_DIR}/data/uses_toy.c)
set_tests_properties(slot_choices PROPERTIES
    ENVIRONMENT "CCRATCH_BLOCKS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/blocks")

add_executable(debugme data/debugme.c)
target_compile_options(debugme PRIVATE -g -O0)
set_target_properties(debugme PROPERTIES C_STANDARD 11)
add_executable(test_gdb test_gdb.c)
target_link_libraries(test_gdb PRIVATE ccratch_core)
add_dependencies(test_gdb debugme)
add_executable(askme data/askme.c)
target_compile_options(askme PRIVATE -g -O0)
add_dependencies(test_gdb askme)
add_test(NAME debugger COMMAND test_gdb $<TARGET_FILE:debugme> ${CMAKE_CURRENT_SOURCE_DIR}/data/debugme.c
                                        $<TARGET_FILE:askme>)

add_executable(test_git test_git.c)
target_link_libraries(test_git PRIVATE ccratch_core)
add_test(NAME git COMMAND test_git)

add_executable(test_project test_project.c)
target_link_libraries(test_project PRIVATE ccratch_core)
add_test(NAME project_functions COMMAND test_project)
