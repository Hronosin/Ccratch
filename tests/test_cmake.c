/*
 * CMake as blocks: reading CMake, saving it untouched and changed, and the
 * CMakeLists.txt Ccratch makes for a project.
 *
 * test_cmake [FILE...]: every file given is also read, saved untouched
 * (byte for byte), and saved with every body rebuilt (the same commands
 * and comments, maybe indented differently): CMake's own modules make a
 * good thousand of them.
 */
#define _DEFAULT_SOURCE
#include "ccr_build.h"
#include "ccr_cmake.h"
#include "ccr_codegen.h"
#include "ccr_lib.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

static const ccr_node *N(const ccr_ast *a, ccr_node_id id) { return &a->nodes[id]; }
static const char *S(const ccr_ast *a, ccr_str s)         { return ccr_ast_str(a, s); }

static char *gen(const ccr_ast *a)
{
    char *t = NULL, err[256];
    size_t n = 0;
    if (!ccr_codegen(a, &t, &n, err, sizeof err)) {
        fprintf(stderr, "codegen: %s\n", err);
        ++g_failures;
        return NULL;
    }
    return t;
}

static bool parse(const char *text, ccr_ast *a)
{
    ccr_ast_init(a);
    return ccr_cmake_parse_text(text, strlen(text), "CMakeLists.txt", a) &&
           ccr_ast_count_diags(a, CCR_DIAG_ERROR) == 0;
}

/* The first node of `kind` named `name` (NULL: any), in tree order. */
static ccr_node_id find(const ccr_ast *a, ccr_node_kind kind, const char *name, int skip)
{
    for (uint32_t i = 0; i < a->node_count; ++i)
        if (N(a, i)->kind == kind && (!name || strcmp(S(a, N(a, i)->name), name) == 0) && skip-- == 0)
            return i;
    return CCR_NONE;
}

static ccr_node_id kid(const ccr_ast *a, ccr_node_id id, int i)
{
    ccr_node_id c = N(a, id)->first_child;
    while (c != CCR_NONE && i-- > 0)
        c = N(a, c)->next_sibling;
    return c;
}

static void dirty(ccr_ast *a, ccr_node_id blk) { a->nodes[blk].flags |= CCR_FLAG_DIRTY; }

static const char *g_sample =
    "# a game\n"
    "cmake_minimum_required(VERSION 3.16)\n"
    "project(game LANGUAGES C)\n"
    "\n"
    "add_executable(game\n"
    "    main.c   # the loop\n"
    "    util.c\n"
    ")\n"
    "if(WIN32)\n"
    "  set(EXTRA win.c)\n"
    "elseif(APPLE)\n"
    "  set(EXTRA mac.c)\n"
    "else()\n"
    "  # nothing extra\n"
    "endif()\n"
    "foreach(f IN LISTS EXTRA)\n"
    "  target_sources(game PRIVATE ${f})\n"
    "endforeach()\n"
    "function(say what)\n"
    "  message(STATUS \"${what}\")\n"
    "endfunction()\n"
    "message(STATUS \"done\")\n";

static void test_structure(void)
{
    ccr_ast a;
    CHECK(parse(g_sample, &a), "the sample does not read");
    const ccr_node_id hat = kid(&a, 0, 0), top = kid(&a, hat, 0);
    CHECK(N(&a, hat)->kind == CCR_NODE_FUNCTION && strcmp(S(&a, N(&a, hat)->name), "CMakeLists.txt") == 0,
          "the hat");
    int commands = 0;
    for (ccr_node_id c = N(&a, top)->first_child; c != CCR_NONE; c = N(&a, c)->next_sibling)
        ++commands;
    CHECK(commands == 7, "%d commands at the top, want 7", commands);

    const ccr_node_id exe = find(&a, CCR_NODE_CALL, "add_executable", 0);
    CHECK(exe != CCR_NONE && strcmp(S(&a, N(&a, kid(&a, exe, 2))->name), "util.c") == 0,
          "add_executable's arguments, over three lines");
    const ccr_node_id iff = find(&a, CCR_NODE_IF, NULL, 0);
    CHECK(iff != CCR_NONE && strcmp(S(&a, N(&a, kid(&a, iff, 0))->name), "WIN32") == 0, "if(WIN32)");
    const ccr_node_id wrap = kid(&a, iff, 2);
    const ccr_node_id eif = wrap != CCR_NONE ? N(&a, wrap)->first_child : CCR_NONE;
    CHECK(eif != CCR_NONE && (N(&a, eif)->flags & CCR_FLAG_ELSEIF) &&
          strcmp(S(&a, N(&a, kid(&a, eif, 0))->name), "APPLE") == 0, "elseif(APPLE)");
    const ccr_node_id els = eif != CCR_NONE ? kid(&a, eif, 2) : CCR_NONE;
    CHECK(els != CCR_NONE && N(&a, els)->first_child == CCR_NONE &&
          strcmp(S(&a, N(&a, els)->aux), "# nothing extra") == 0, "else() holding a comment only");
    CHECK(find(&a, CCR_NODE_SCOPE, "foreach", 0) != CCR_NONE &&
          find(&a, CCR_NODE_SCOPE, "function", 0) != CCR_NONE, "foreach and function");
    ccr_ast_free(&a);

    /* what does not read: told, with the line */
    static const char *const broken[] = {
        "if(A)\nmessage(x)\n",                 /* no endif */
        "endif()\n",                           /* an end without its start */
        "if(A)\nendforeach()\n",               /* the wrong end */
        "message(\"never closed)\n",
        "set(A\n",
        "if(A)\nelse()\nelse()\nendif()\n",
        "this is not cmake\n",
    };
    for (size_t i = 0; i < sizeof broken / sizeof broken[0]; ++i) {
        ccr_ast_init(&a);
        const bool ok = ccr_cmake_parse_text(broken[i], strlen(broken[i]), "x.cmake", &a);
        CHECK(!ok && ccr_ast_count_diags(&a, CCR_DIAG_ERROR) == 1, "read anyway: %s", broken[i]);
        ccr_ast_free(&a);
    }
}

static void test_edits(void)
{
    ccr_ast a;
    CHECK(parse(g_sample, &a), "the sample does not read");
    char *t = gen(&a);
    CHECK(t && strcmp(t, g_sample) == 0, "saved untouched, it is not the same");
    free(t);

    /* a word typed into a slot: only that word changes */
    const ccr_node_id exe = find(&a, CCR_NODE_CALL, "add_executable", 0);
    ccr_node_id main_c = kid(&a, exe, 1);
    a.nodes[main_c].name = ccr_ast_intern(&a, "game.c");
    a.nodes[main_c].flags |= CCR_FLAG_NEW;
    t = gen(&a);
    CHECK(t && strstr(t, "add_executable(game\n    game.c   # the loop\n    util.c\n)\n"), "typed:\n%s", t);
    free(t);

    /* a command moved into the else(): both bodies written anew */
    const ccr_node_id msg = find(&a, CCR_NODE_CALL, "message", 1);   /* message(STATUS "done") */
    const ccr_node_id top = N(&a, msg)->parent;
    const ccr_node_id iff = find(&a, CCR_NODE_IF, NULL, 0);
    const ccr_node_id els = kid(&a, N(&a, kid(&a, iff, 2))->first_child, 2);
    ccr_ast_unlink(&a, msg);
    ccr_ast_append(&a, els, msg);
    dirty(&a, top);
    dirty(&a, els);
    t = gen(&a);
    CHECK(t && strstr(t, "else()\n  message(STATUS \"done\")\n  # nothing extra\nendif()\n"),
          "moved into else():\n%s", t);
    CHECK(t && strstr(t, "endfunction()\n") && !strstr(t, "endfunction()\nmessage"), "and gone from the end:\n%s", t);
    free(t);

    /* from the palette: a command and an if/else, printed with their words */
    ccr_node_id m = ccr_cmake_build_command(&a, CCR_NONE, "message(STATUS \"hi\" )");
    ccr_ast_insert_first(&a, top, m);
    ccr_node_id s = ccr_cmake_build_scope(&a, CCR_NONE, "if", "MSVC", true);
    ccr_ast_append(&a, top, s);
    ccr_node_id in = ccr_cmake_build_command(&a, CCR_NONE, "add_compile_options(/W4)");
    ccr_ast_append(&a, kid(&a, s, 1), in);
    ccr_node_id in2 = ccr_cmake_build_command(&a, CCR_NONE, "add_compile_options(-Wall)");
    ccr_ast_append(&a, kid(&a, s, 2), in2);
    t = gen(&a);
    CHECK(t && strncmp(t, "message(STATUS \"hi\")\n# a game\ncmake_minimum_required", 50) == 0,
          "first in the file:\n%s", t);
    CHECK(t && strstr(t, "endfunction()\nif(MSVC)\n    add_compile_options(/W4)\nelse()\n"
                         "    add_compile_options(-Wall)\nendif()\n"),
          "if/else from the palette:\n%s", t);
    free(t);

    /* switched off: a comment, # and all */
    const ccr_node_id req = find(&a, CCR_NODE_CALL, "cmake_minimum_required", 0);
    a.nodes[req].flags |= CCR_FLAG_COMMENTED;
    t = gen(&a);
    CHECK(t && strstr(t, "# a game\n# cmake_minimum_required(VERSION 3.16)\nproject"), "commented:\n%s", t);
    free(t);

    /* and what comes out reads again */
    t = gen(&a);
    ccr_ast b;
    CHECK(t && parse(t, &b), "the saved file does not read:\n%s", t);
    ccr_ast_free(&b);
    free(t);
    ccr_ast_free(&a);

    /* an empty file gets its first command */
    CHECK(parse("# nothing yet", &a), "a comment-only file");
    const ccr_node_id body = kid(&a, kid(&a, 0, 0), 0);
    m = ccr_cmake_build_command(&a, CCR_NONE, "project(x)");
    ccr_ast_append(&a, body, m);
    dirty(&a, body);   /* as a drop marks it */
    t = gen(&a);
    CHECK(t && strcmp(t, "# nothing yet\nproject(x)") == 0, "into an empty file: [%s]", t);
    free(t);
    ccr_ast_free(&a);
}

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fputs(text, f);
    return fclose(f) == 0;
}

static void test_generator(void)
{
    char root[256], p[512];
    snprintf(root, sizeof root, "/tmp/ccratch_test_cmake_%d", (int)getpid());
    snprintf(p, sizeof p, "%s/src", root);
    mkdir(root, 0755);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/include", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/tools", root);
    mkdir(p, 0755);
    char files[4][512];
    snprintf(files[0], 512, "%s/src/main.c", root);
    snprintf(files[1], 512, "%s/src/util.c", root);
    snprintf(files[2], 512, "%s/include/util.h", root);
    snprintf(files[3], 512, "%s/tools/gen.c", root);
    write_text(files[0], "#include <math.h>\n#include \"toy.h\"\n#include \"util.h\"\nint main(void)\n{\n}\n");
    write_text(files[1], "#include \"util.h\"\nint twice(int x) { return 2 * x; }\n");
    write_text(files[2], "int twice(int x);\n");
    write_text(files[3], "#include <pthread.h>\nint main(void) { return 0; }\n");

    ccr_lib_load(NULL);   /* the toy pack: links "toy", finds "Toy" */
    int n = 0;
    char *text = ccr_build_cmake_text(root, NULL, &n);
    CHECK(text && n == 2, "%d files", n);
    if (text) {
        CHECK(strstr(text, "project(") && strstr(text, "add_executable(${PROJECT_NAME}\n    src/main.c\n    src/util.c\n)\n"),
              "the program:\n%s", text);
        CHECK(strstr(text, "# tools/gen.c has a main() of its own"), "the other main():\n%s", text);
        CHECK(strstr(text, "target_include_directories(${PROJECT_NAME} PRIVATE include)"), "include/:\n%s", text);
        CHECK(strstr(text, "find_package(Toy REQUIRED)\ntarget_link_libraries(${PROJECT_NAME} PRIVATE toy)"),
              "the pack's library:\n%s", text);
        CHECK(!strstr(text, "Threads"), "pthread.h is gen.c's, not the program's:\n%s", text);
        CHECK(strstr(text, "if(UNIX)\n    target_link_libraries(${PROJECT_NAME} PRIVATE m)\nendif()"),
              "libm:\n%s", text);
        ccr_ast a;
        CHECK(parse(text, &a), "what Ccratch writes, Ccratch reads");
        ccr_ast_free(&a);
        free(text);
    }

    /* the open file's main() is the program's */
    text = ccr_build_cmake_text(root, files[3], &n);
    CHECK(text && strstr(text, "add_executable(${PROJECT_NAME}\n    tools/gen.c\n    src/util.c\n)\n") &&
          strstr(text, "Threads::Threads") && strstr(text, "# src/main.c has a main()"),
          "tools/gen.c as the program:\n%s", text ? text : "");
    free(text);

    char made[600];
    CHECK(ccr_build_make_cmake(root, NULL, made, (int)sizeof made, &n) == CCR_CMAKE_MADE, "made");
    CHECK(ccr_build_make_cmake(root, NULL, made, (int)sizeof made, &n) == CCR_CMAKE_THERE, "made twice");
    ccr_ast a;
    ccr_ast_init(&a);
    CHECK(ccr_cmake_parse_file(made, &a), "read back from disk");
    ccr_ast_free(&a);
    ccr_lib_unload();

    for (int i = 0; i < 4; ++i)
        remove(files[i]);
    remove(made);
    static const char *const dirs[] = { "src", "include", "tools" };
    for (int i = 0; i < 3; ++i) {
        snprintf(p, sizeof p, "%s/%s", root, dirs[i]);
        rmdir(p);
    }
    rmdir(root);
    CHECK(ccr_build_cmake_text("/nonexistent/ccratch", NULL, &n) == NULL, "no project, no text");
}

/* Lines without their indent and trailing spaces, no blank lines: what
 * a rebuild may change and still be right. */
static size_t squeeze(const char *s, size_t n, char *o)
{
    size_t k = 0;
    bool line_start = true;
    for (size_t i = 0; i < n; ++i) {
        if (line_start && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
            continue;
        if (s[i] == '\n') {
            while (k && (o[k - 1] == ' ' || o[k - 1] == '\t'))
                --k;
            line_start = true;
        } else {
            line_start = false;
        }
        o[k++] = s[i];
    }
    return k;
}

static void test_corpus(int argc, char **argv)
{
    int read = 0, unread = 0;
    for (int f = 1; f < argc; ++f) {
        ccr_ast a;
        ccr_ast_init(&a);
        if (!ccr_cmake_parse_file(argv[f], &a) || ccr_ast_count_diags(&a, CCR_DIAG_ERROR)) {
            ++unread;   /* a template (@VAR@ lines) or not CMake at all */
            ccr_ast_free(&a);
            continue;
        }
        ++read;
        char *t = gen(&a);
        CHECK(t && strlen(t) == a.source_len && memcmp(t, a.source, a.source_len) == 0,
              "%s: not the same, untouched", argv[f]);
        free(t);
        for (uint32_t i = 0; i < a.node_count; ++i) {
            const ccr_node_id c = a.nodes[i].first_child;
            if (a.nodes[i].kind == CCR_NODE_BLOCK && !(c != CCR_NONE && (a.nodes[c].flags & CCR_FLAG_ELSEIF)))
                a.nodes[i].flags |= CCR_FLAG_DIRTY;
        }
        t = gen(&a);
        if (t) {
            const size_t tn = strlen(t);
            char *x = malloc(tn + 1), *y = malloc(a.source_len + 1);
            const size_t xn = squeeze(t, tn, x), yn = squeeze(a.source, a.source_len, y);
            size_t at = 0;
            while (at < xn && at < yn && x[at] == y[at])
                ++at;
            CHECK(xn == yn && at == xn, "%s: rebuilt, it differs at %zu:\n%.80s\n---\n%.80s", argv[f], at,
                  y + (at > 30 ? at - 30 : 0), x + (at > 30 ? at - 30 : 0));
            free(x);
            free(y);
        }
        free(t);
        ccr_ast_free(&a);
    }
    if (argc > 1)
        printf("cmake corpus: %d read, %d not CMake Ccratch reads\n", read, unread);
    CHECK(argc <= 1 || read > 0, "nothing in the corpus read");
}

int main(int argc, char **argv)
{
    CHECK(ccr_cmake_is_cmake("x/CMakeLists.txt") && ccr_cmake_is_cmake("FindToy.cmake") &&
          !ccr_cmake_is_cmake("main.c") && !ccr_cmake_is_cmake("CMakeLists.txt.bak"), "which files");
    CHECK(ccr_cmake_is_keyword("PRIVATE") && !ccr_cmake_is_keyword("C") && !ccr_cmake_is_keyword("game"),
          "keywords");
    test_structure();
    test_edits();
    test_generator();
    test_corpus(argc, argv);
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("cmake: all checks passed\n");
    return g_failures ? 1 : 0;
}
