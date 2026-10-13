# Headless tests: everything here runs without a window.

set(CCR_BIN $<TARGET_FILE:ccratch_app>)

add_test(NAME cli_version COMMAND ${CCR_BIN} --version)

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
endif()

# C++ is picked by extension.
add_test(NAME dump_ast_cpp
         COMMAND ${CCR_BIN} --dump-ast ${PROJECT_SOURCE_DIR}/tests/data/templates.cpp)
set_tests_properties(dump_ast_cpp PROPERTIES PASS_REGULAR_EXPRESSION " 0 errors")
