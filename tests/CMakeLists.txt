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

# C++: real nodes, drawn, and an edit inside a method that still compiles.
add_executable(test_cpp test_cpp.c)
target_link_libraries(test_cpp PRIVATE ccratch_core)
if(XVFB_RUN)
    add_test(NAME cpp_blocks
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x800x24"
                     $<TARGET_FILE:test_cpp> ${PROJECT_SOURCE_DIR}/examples/game.cpp
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/stdlib.c)
endif()
add_test(NAME cpp_roundtrip COMMAND ${CCR_BIN} --roundtrip ${PROJECT_SOURCE_DIR}/examples/game.cpp)
set_tests_properties(cpp_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION "identical")

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
file(GLOB CCR_SHIPPED_PACKS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/lang/*.lang)
foreach(pack ${CCR_SHIPPED_PACKS})
    get_filename_component(code ${pack} NAME_WE)
    add_test(NAME lang_check_${code} COMMAND ${CCR_BIN} --check-lang ${pack})
endforeach()
# The block explanations: English says something of every block, the
# others only of blocks that exist.
file(GLOB CCR_SHIPPED_EXPLAIN CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/lang/*.explain)
foreach(ex ${CCR_SHIPPED_EXPLAIN})
    get_filename_component(code ${ex} NAME_WE)
    if(code STREQUAL "en")
        add_test(NAME explain_check_${code} COMMAND ${CCR_BIN} --check-explain ${ex} --complete)
    else()
        add_test(NAME explain_check_${code} COMMAND ${CCR_BIN} --check-explain ${ex})
    endif()
endforeach()

# ...and every letter of them has a font that draws it.
add_executable(test_fonts test_fonts.c)
target_include_directories(test_fonts PRIVATE ${PROJECT_SOURCE_DIR}/ccratch/src)
add_test(NAME font_coverage COMMAND test_fonts ${PROJECT_SOURCE_DIR}/lang ${PROJECT_SOURCE_DIR}/assets/fonts)
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

# an assembly file left out (a main of its own): no assembly section, and
# no endif() without its if
add_test(NAME make_cmake_asm_left_out
         COMMAND sh -c "${CCR_BIN} --make-cmake ${PROJECT_SOURCE_DIR}/tests/data/two_mains > ${CMAKE_CURRENT_BINARY_DIR}/two_mains.cmake && ${CCR_BIN} --dump-ast ${CMAKE_CURRENT_BINARY_DIR}/two_mains.cmake")
set_tests_properties(make_cmake_asm_left_out PROPERTIES
    PASS_REGULAR_EXPRESSION " 0 errors"
    FAIL_REGULAR_EXPRESSION "does not fit")

# Rebuild every block of real files and read them back.
add_executable(test_rebuild test_rebuild.c)
target_link_libraries(test_rebuild PRIVATE ccratch_core)
foreach(f examples/tour.c tests/data/save.c tests/data/rebuild_decls.c tests/data/rebuild_retype.c)
    get_filename_component(stem ${f} NAME_WE)
    add_test(NAME rebuild_${stem}
             COMMAND test_rebuild ${CMAKE_CURRENT_BINARY_DIR}/saved/rebuilt_${stem}.c
                     ${PROJECT_SOURCE_DIR}/${f})
endforeach()
# braces of braceless bodies: after the code, not in a comment; else if kept
set(CCR_REBUILT_COMMENTS ${CMAKE_CURRENT_BINARY_DIR}/saved/rebuilt_rebuild_comments.c)
add_test(NAME rebuild_braces_and_comments
         COMMAND sh -c "$<TARGET_FILE:test_rebuild> ${CCR_REBUILT_COMMENTS} ${PROJECT_SOURCE_DIR}/tests/data/rebuild_comments.c && cat ${CCR_REBUILT_COMMENTS}")
set_tests_properties(rebuild_braces_and_comments PROPERTIES
    PASS_REGULAR_EXPRESSION "if \\(x > 1\\) \\{ // big.*if \\(x < 0\\) \\{.*else if \\(x == 0\\) \\{ // .*else if \\(x == 5\\) \\{"
    FAIL_REGULAR_EXPRESSION "else \\{|// big \\{|MISMATCH")
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

# Assembly as blocks: .s/.S/.asm files, asm() in C. The corpus: gcc's own
# assembly of this project's C and raylib's (AT&T and Intel), and NASM.
add_executable(test_asm test_asm.c)
target_link_libraries(test_asm PRIVATE ccratch_core)
add_test(NAME asm_blocks COMMAND test_asm
         ${PROJECT_SOURCE_DIR}/examples/hello.s ${PROJECT_SOURCE_DIR}/examples/hello.asm
         --from-c ${PROJECT_SOURCE_DIR}/examples/tour.c
         --from-c ${PROJECT_SOURCE_DIR}/ccratch/src/ccr_codegen.c
         --flags "-I${PROJECT_SOURCE_DIR}/ccratch/src -I${PROJECT_SOURCE_DIR}/include"
         --from-c ${PROJECT_SOURCE_DIR}/ccratch/src/ccr_editor.c
         --flags "-std=c99 -I${raylib_SOURCE_DIR}/src -I${raylib_SOURCE_DIR}/src/external/glfw/include -DPLATFORM_DESKTOP"
         --from-c ${raylib_SOURCE_DIR}/src/rshapes.c
         --from-c ${raylib_SOURCE_DIR}/src/rtext.c)
set_tests_properties(asm_blocks PROPERTIES TIMEOUT 300)
# ARM: clang writes and assembles it for any target, no cross toolchain
find_program(CCR_CLANG clang)
find_program(CCR_LLVM_OBJDUMP llvm-objdump)
if(CCR_CLANG AND CCR_LLVM_OBJDUMP)
    add_test(NAME asm_blocks_arm COMMAND test_asm
             ${PROJECT_SOURCE_DIR}/examples/hello_arm64.s ${PROJECT_SOURCE_DIR}/examples/hello_arm.s
             --target aarch64-linux-gnu ${CMAKE_CURRENT_SOURCE_DIR}/data/asm/corpus.c
             --target armv7a-linux-gnueabihf ${CMAKE_CURRENT_SOURCE_DIR}/data/asm/corpus.c
             --target thumbv7-linux-gnueabihf ${CMAKE_CURRENT_SOURCE_DIR}/data/asm/corpus.c
             --target x86_64-linux-gnu ${CMAKE_CURRENT_SOURCE_DIR}/data/asm/corpus.c)
    set_tests_properties(asm_blocks_arm PROPERTIES TIMEOUT 300)
endif()
add_test(NAME code_hater_asm_lines COMMAND ${CCR_BIN} --lang en --check-code
         ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/inline_lines.c)
set_tests_properties(code_hater_asm_lines PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "inline_lines\\.c:12:[0-9]+: error: .code_hater. NAK: rbp trashed"
    FAIL_REGULAR_EXPRESSION "rcx trashed|r11 trashed")
foreach(f examples/hello.s examples/hello.asm examples/inline.c examples/hello_arm64.s examples/hello_arm.s)
    get_filename_component(stem ${f} NAME)
    add_test(NAME roundtrip_asm_${stem} COMMAND ${CCR_BIN} --roundtrip ${PROJECT_SOURCE_DIR}/${f})
    set_tests_properties(roundtrip_asm_${stem} PROPERTIES PASS_REGULAR_EXPRESSION "identical")
endforeach()

# The Build button: compiler, linker and CMake output read for places;
# a real project configured, built, run and stopped.
add_executable(test_job test_job.c)
target_link_libraries(test_job PRIVATE ccratch_core)
add_test(NAME build_job COMMAND test_job)

# Other machines: one project of C and assembly, built for every target
# there are tools for here (a cross gcc, zig, the NDK), run in qemu
add_executable(test_target test_target.c)
target_link_libraries(test_target PRIVATE ccratch_core)
add_test(NAME build_targets COMMAND test_target ${PROJECT_SOURCE_DIR}/examples)
set_tests_properties(build_targets PROPERTIES TIMEOUT 600)
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

add_executable(test_rename test_rename.c)
target_link_libraries(test_rename PRIVATE ccratch_core)
add_test(NAME rename_project COMMAND test_rename)

add_executable(test_arrange test_arrange.c)
target_link_libraries(test_arrange PRIVATE ccratch_core)
add_test(NAME arrange_text COMMAND test_arrange)
# the shipped arrangements read
file(GLOB CCR_SHIPPED_ARRANGEMENTS ${PROJECT_SOURCE_DIR}/arrangements/*.arrange)
foreach(arr ${CCR_SHIPPED_ARRANGEMENTS})
    get_filename_component(code ${arr} NAME_WE)
    add_test(NAME arrange_check_${code} COMMAND ${CCR_BIN} --check-arrangement ${arr})
endforeach()

add_executable(test_search test_search.c)
target_link_libraries(test_search PRIVATE ccratch_core)
add_test(NAME search COMMAND test_search)

add_executable(test_gitlog test_gitlog.c)
target_link_libraries(test_gitlog PRIVATE ccratch_core)
add_test(NAME git_history COMMAND test_gitlog)

# build.sh and run.sh at least parse (they are for people who never open them)
add_test(NAME scripts_parse COMMAND bash -n ${PROJECT_SOURCE_DIR}/build.sh ${PROJECT_SOURCE_DIR}/run.sh)

# Plugins: Code Hater, loaded from share/ccratch/plugins next to the
# program, with an empty config (a user's "plugin.code_hater = off" must
# not make these pass for the wrong reason). Every check fires on bad.c,
# none on good.c, and switched off it says nothing.
set(CCR_HATER_ENV "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-config"
                  "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-data" "CCRATCH_PLUGIN_DIR=")
set(CCR_HATER_BAD ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/bad.c)
foreach(expect
        "11:[0-9]+: error: .code_hater. NAK: returns the address of a local"
        "16:[0-9]+: warning: .code_hater. realloc into the same pointer"
        "22:[0-9]+: warning: .code_hater. memory leak"
        "23:[0-9]+: warning: .code_hater. malloc's result not checked"
        "23:[0-9]+: warning: .code_hater. strcpy\\(\\): no length"
        "30:[0-9]+: error: .code_hater. NAK: double free"
        "35:[0-9]+: nit: .code_hater. sizeof\\(type\\)"
        "38:[0-9]+: error: .code_hater. NAK: use after free"
        "44:[0-9]+: error: .code_hater. NAK: gets\\(\\)"
        "46:[0-9]+: warning: .code_hater. = in a condition"
        "49:[0-9]+: nit: .code_hater. four levels deep"
        "50:[0-9]+: warning: .code_hater. sprintf\\(\\) without a size"
        "59:[0-9]+: error: .code_hater. NAK: Rust detected"
        "59:[0-9]+: error: .code_hater. NAK: Rust propaganda"
        "66:[0-9]+: warning: .code_hater. variable-length array"
        "76:[0-9]+: warning: .code_hater. goto backwards"
        "83:[0-9]+: error: .code_hater. NAK: returns a local array"
        "88:[0-9]+: nit: .code_hater. cast of malloc")
    string(REGEX REPLACE "[^a-z0-9]+" "_" tag "${expect}")
    string(SUBSTRING "${tag}" 0 60 tag)
    add_test(NAME code_hater_${tag}
             COMMAND ${CCR_BIN} --lang en --check-code ${CCR_HATER_BAD})
    set_tests_properties(code_hater_${tag} PROPERTIES
        ENVIRONMENT "${CCR_HATER_ENV}"
        PASS_REGULAR_EXPRESSION "bad\\.c:${expect}")
endforeach()
# context: Rust in a table or a search is data; pushing it is a NAK,
# mentioning it a nit; the same thing in one block said once; letters as
# letters; and Code Hater knows its own rants when it reads itself
set(CCR_HATER_CONTEXT ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/context.c)
foreach(expect "21:[0-9]+: error: .code_hater. NAK: Rust propaganda"
               "22:[0-9]+: nit: .code_hater. Rust, mentioned"
               "And 2 more like it in this block"
               "Привет, Rust!")
    string(REGEX REPLACE "[^a-zA-Z0-9]+" "_" tag "${expect}")
    string(SUBSTRING "${tag}" 0 40 tag)
    add_test(NAME code_hater_context_${tag} COMMAND ${CCR_BIN} --lang en --check-code ${CCR_HATER_CONTEXT})
    set_tests_properties(code_hater_context_${tag} PROPERTIES
        ENVIRONMENT "${CCR_HATER_ENV}"
        PASS_REGULAR_EXPRESSION "${expect}")
endforeach()
add_test(NAME code_hater_context_data COMMAND ${CCR_BIN} --lang en --check-code ${CCR_HATER_CONTEXT})
set_tests_properties(code_hater_context_data PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "context\\.c:21:"
    FAIL_REGULAR_EXPRESSION "context\\.c:(8|9|10|13|15):|mentions_rust|\\\\320")
add_test(NAME code_hater_self
         COMMAND ${CCR_BIN} --lang en --check-code ${PROJECT_SOURCE_DIR}/plugins/code_hater/code_hater.c)
set_tests_properties(code_hater_self PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "praise: .code_hater. this is me"
    FAIL_REGULAR_EXPRESSION "Rust propaganda|Rust detected|Rust, mentioned")
# assembly: .s files and asm() in C; clean ones say nothing
foreach(expect "bad\\.s:7:[0-9]+: nit: .code_hater. mov 0 into eax"
               "bad\\.s:9:[0-9]+: error: .code_hater. NAK: the stack is off by 8 bytes at ret"
               "bad\\.s:10:[0-9]+: warning: .code_hater. nothing ever gets here"
               "bad\\.s:14:[0-9]+: warning: .code_hater. g falls into whatever comes next"
               "bad\\.s:15:[0-9]+: error: .code_hater. NAK: r12 is not yours"
               "bad\\.s:2:[0-9]+: nit: .code_hater. no .note.GNU-stack"
               "inline_bad\\.c:6:[0-9]+: error: .code_hater. NAK: rcx trashed"
               "inline_bad\\.c:14:[0-9]+: warning: .code_hater. asm writes memory"
               "inline_bad\\.c:20:[0-9]+: nit: .code_hater. rdtsc: there is a builtin")
    string(REGEX REPLACE "[^a-z0-9]+" "_" tag "${expect}")
    string(SUBSTRING "${tag}" 0 50 tag)
    add_test(NAME code_hater_asm_${tag} COMMAND ${CCR_BIN} --lang en --check-code
             ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/bad.s ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/inline_bad.c)
    set_tests_properties(code_hater_asm_${tag} PROPERTIES
        ENVIRONMENT "${CCR_HATER_ENV}"
        PASS_REGULAR_EXPRESSION "${expect}")
endforeach()
foreach(expect "bad_arm64\\.s:6:[0-9]+: error: .code_hater. NAK: x19 is not yours"
               "bad_arm64\\.s:7:[0-9]+: error: .code_hater. NAK: bl eats the way back"
               "bad_arm64\\.s:16:[0-9]+: error: .code_hater. NAK: sp is off by 16 bytes"
               "bad_arm64\\.s:17:[0-9]+: warning: .code_hater. nothing ever gets here"
               "bad_arm64\\.s:21:[0-9]+: warning: .code_hater. h falls into"
               "bad_arm\\.s:8:[0-9]+: error: .code_hater. NAK: r4 is not yours"
               "bad_arm\\.s:17:[0-9]+: error: .code_hater. NAK: sp is off by 8 bytes")
    string(REGEX REPLACE "[^a-z0-9]+" "_" tag "${expect}")
    string(SUBSTRING "${tag}" 0 50 tag)
    add_test(NAME code_hater_arm_${tag} COMMAND ${CCR_BIN} --lang en --check-code
             ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/bad_arm64.s ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/bad_arm.s)
    set_tests_properties(code_hater_arm_${tag} PROPERTIES
        ENVIRONMENT "${CCR_HATER_ENV}"
        PASS_REGULAR_EXPRESSION "${expect}")
endforeach()
add_test(NAME code_hater_arm_lr_saved COMMAND ${CCR_BIN} --lang en --check-code
         ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/bad_arm.s)
set_tests_properties(code_hater_arm_lr_saved PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "r4 is not yours"
    FAIL_REGULAR_EXPRESSION "bl eats")
add_test(NAME code_hater_asm_honest COMMAND ${CCR_BIN} --lang en --check-code
         ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/inline_bad.c)
set_tests_properties(code_hater_asm_honest PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "inline_bad\\.c:6:"
    FAIL_REGULAR_EXPRESSION "inline_bad\\.c:2[6-9]:")
foreach(f examples/hello.s examples/hello.asm examples/inline.c examples/hello_arm64.s examples/hello_arm.s)
    get_filename_component(stem ${f} NAME)
    add_test(NAME code_hater_quiet_${stem} COMMAND ${CCR_BIN} --lang en --check-code ${PROJECT_SOURCE_DIR}/${f})
    set_tests_properties(code_hater_quiet_${stem} PROPERTIES
        ENVIRONMENT "${CCR_HATER_ENV}"
        FAIL_REGULAR_EXPRESSION "warning:|error:")
endforeach()
# how brutal: the mother cat (the default) rubs your nose in it; grumpy
# just says it; and with the nits switched off there are none
add_test(NAME code_hater_mother_cat COMMAND ${CCR_BIN} --lang en --check-code ${CCR_HATER_BAD})
set_tests_properties(code_hater_mother_cat PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "nose rubbed in it")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hater-grumpy/ccratch/config
     "plugin.code_hater.brutality = grumpy\nplugin.code_hater.nits = off\n")
add_test(NAME code_hater_grumpy COMMAND ${CCR_BIN} --lang en --check-code ${CCR_HATER_BAD})
set_tests_properties(code_hater_grumpy PROPERTIES
    ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-grumpy;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-data;CCRATCH_PLUGIN_DIR="
    PASS_REGULAR_EXPRESSION "NAK: use after free"
    FAIL_REGULAR_EXPRESSION "nose rubbed in it|: nit: ")

# what it once got wrong: compiler output, other machines, macros, trees
# clang lost pieces of (a header not found), Rust's words in C
add_test(NAME code_hater_quiet_c
         COMMAND ${CCR_BIN} --lang en --check-code ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/quiet.c)
set_tests_properties(code_hater_quiet_c PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "quiet\\.c:[0-9]+:[0-9]+: praise: .code_hater. a goto, finally"
    FAIL_REGULAR_EXPRESSION "(error|warning|nit): .code_hater.")
foreach(quiet quiet.s quiet_arm64.s quiet_arm.s quiet_riscv.s)
    string(REGEX REPLACE "[^a-z0-9]+" "_" tag "${quiet}")
    add_test(NAME code_hater_${tag}
             COMMAND ${CCR_BIN} --lang en --check-code ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/${quiet})
    set_tests_properties(code_hater_${tag} PROPERTIES
        ENVIRONMENT "${CCR_HATER_ENV}"
        PASS_REGULAR_EXPRESSION "${quiet}: nothing to say")
endforeach()
# what a compiler writes is right by definition: Code Hater must find no
# error and no warning in it (only nits), for every machine it can build for
set(CCR_HATER_ORACLE_SRC ${PROJECT_SOURCE_DIR}/ccratch/src/ccr_yaml.c)
set(CCR_HATER_ORACLE "cc:${CMAKE_C_COMPILER}")
find_program(CCR_AARCH64_GCC aarch64-linux-gnu-gcc)
find_program(CCR_ARMHF_GCC arm-linux-gnueabihf-gcc)
if(CCR_AARCH64_GCC)
    list(APPEND CCR_HATER_ORACLE "a64:${CCR_AARCH64_GCC}")
endif()
if(CCR_ARMHF_GCC)
    list(APPEND CCR_HATER_ORACLE "arm:${CCR_ARMHF_GCC}" "thumb:${CCR_ARMHF_GCC} -mthumb")
endif()
foreach(entry ${CCR_HATER_ORACLE})
    string(REGEX REPLACE ":.*" "" tag "${entry}")
    string(REGEX REPLACE "^[^:]*:" "" cc "${entry}")
    foreach(opt O0 O2)
        set(out ${CMAKE_CURRENT_BINARY_DIR}/hater_oracle_${tag}_${opt}.s)
        add_test(NAME code_hater_compiler_${tag}_${opt}
                 COMMAND sh -c "${cc} -S -w -${opt} -I${PROJECT_SOURCE_DIR}/ccratch/src ${CCR_HATER_ORACLE_SRC} -o ${out} && ${CCR_BIN} --lang en --check-code ${out}")
        set_tests_properties(code_hater_compiler_${tag}_${opt} PROPERTIES
            ENVIRONMENT "${CCR_HATER_ENV}"
            PASS_REGULAR_EXPRESSION "hater_oracle_${tag}_${opt}\\.s"
            FAIL_REGULAR_EXPRESSION "(error|warning): .code_hater.")
    endforeach()
endforeach()
add_test(NAME code_hater_good
         COMMAND ${CCR_BIN} --lang en --check-code ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/good.c)
set_tests_properties(code_hater_good PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "good\\.c: nothing to say")
add_test(NAME code_hater_russian
         COMMAND ${CCR_BIN} --lang ru --check-code ${CCR_HATER_BAD})
set_tests_properties(code_hater_russian PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "NAK: обращение после free")
# switched off in the config: silence
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/hater-off/ccratch)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hater-off/ccratch/config "plugin.code_hater = off\n")
add_test(NAME code_hater_off
         COMMAND ${CCR_BIN} --lang en --check-code ${CCR_HATER_BAD})
set_tests_properties(code_hater_off PROPERTIES
    ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-off;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-data"
    PASS_REGULAR_EXPRESSION "bad\\.c: nothing to say")
add_test(NAME plugins_list COMMAND ${CCR_BIN} --list-plugins)
set_tests_properties(plugins_list PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "code_hater +1\\.[0-9]+\\.[0-9]+ +.*share/ccratch/plugins/code_hater\\.so")
# the template still loads (it is built, not installed)
add_test(NAME plugins_template COMMAND ${CCR_BIN} --list-plugins)
set_tests_properties(plugins_template PROPERTIES
    ENVIRONMENT "CCRATCH_PLUGIN_DIR=${CMAKE_BINARY_DIR}/examples/plugins;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/hater-config"
    PASS_REGULAR_EXPRESSION "hello +0\\.0\\.1")
# ...and C++: raw new is a nit, using namespace in a header a warning,
# and C's nits about f() and casting malloc stay quiet
add_test(NAME code_hater_cpp_new COMMAND ${CCR_BIN} --lang en --check-code ${PROJECT_SOURCE_DIR}/examples/game.cpp)
set_tests_properties(code_hater_cpp_new PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "game\\.cpp:[0-9]+:[0-9]+: nit: .code_hater. raw new"
    FAIL_REGULAR_EXPRESSION "is not \\(void\\)")
add_test(NAME code_hater_cpp_header COMMAND ${CCR_BIN} --lang en --check-code ${CMAKE_CURRENT_SOURCE_DIR}/data/hater/using.hpp)
set_tests_properties(code_hater_cpp_header PROPERTIES
    ENVIRONMENT "${CCR_HATER_ENV}"
    PASS_REGULAR_EXPRESSION "using\\.hpp:4:[0-9]+: warning: .code_hater. using namespace in a header")
