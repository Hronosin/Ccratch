/*
 * C++ as blocks: examples/game.cpp reads into real nodes (a namespace, a
 * class with its methods, a template, a range-for, new and delete,
 * operators instead of operator<< calls), draws, and edits inside a
 * method of a class in a namespace save as C++ that still compiles.
 *
 *   test_cpp game.cpp
 *
 * Needs a window (text is measured with the GUI font): run under xvfb.
 */
#define _DEFAULT_SOURCE   /* mkstemps */
#include "ccr_ast.h"
#include "ccr_codegen.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_parse.h"
#include "ccr_template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static ccr_node_id find(const ccr_ast *a, ccr_node_kind kind, const char *name)
{
    for (ccr_node_id i = 0; i < a->node_count; ++i)
        if (N(a, i)->kind == kind && !(N(a, i)->flags & CCR_FLAG_DEAD) &&
            (!name || strcmp(ccr_ast_str(a, N(a, i)->name), name) == 0))
            return i;
    return CCR_NONE;
}

static int count(const ccr_ast *a, ccr_node_kind kind)
{
    int n = 0;
    for (ccr_node_id i = 0; i < a->node_count; ++i)
        n += N(a, i)->kind == kind && !(N(a, i)->flags & CCR_FLAG_DEAD) && N(a, i)->parent != CCR_NONE;
    return n;
}

static void test_tree(const ccr_ast *a)
{
    CHECK(a->cpp, "a .cpp file is C++");
    const ccr_node_id ns = find(a, CCR_NODE_NAMESPACE, "game");
    CHECK(ns != CCR_NONE, "no namespace game");
    const ccr_node_id cls = find(a, CCR_NODE_STRUCT, "Player");
    CHECK(cls != CCR_NONE && N(a, cls)->parent == ns, "Player is not a class in game");
    CHECK(cls != CCR_NONE && (N(a, cls)->flags & CCR_FLAG_CLASS), "Player is written `class`");
    CHECK(count(a, CCR_NODE_ACCESS) == 2, "public: and private: (%d)", count(a, CCR_NODE_ACCESS));
    const ccr_node_id heal = find(a, CCR_NODE_FUNCTION, "heal");
    CHECK(heal != CCR_NONE && N(a, heal)->parent == cls, "heal is a method of Player");
    CHECK(find(a, CCR_NODE_FUNCTION, "clamp") != CCR_NONE, "the template clamp");
    CHECK(count(a, CCR_NODE_FOR_RANGE) == 1, "one range-for");
    CHECK(count(a, CCR_NODE_NEW) == 1 && count(a, CCR_NODE_DELETE) == 1, "new and delete");
    CHECK(find(a, CCR_NODE_REF, "std::cout") != CCR_NONE, "std::cout, as written");
    /* operator<< calls are operators again, and no implicit constructor is left */
    CHECK(find(a, CCR_NODE_CALL, "operator<<") == CCR_NONE, "an operator<< call is left");
    CHECK(find(a, CCR_NODE_CALL, "basic_string") == CCR_NONE, "an implicit std::string constructor is left");
    CHECK(find(a, CCR_NODE_CALL, "vector") == CCR_NONE, "an implicit vector constructor is left");
    const ccr_node_id shift = find(a, CCR_NODE_BINARY_OP, NULL);
    CHECK(shift != CCR_NONE, "no binary operators at all");
    /* only the lambda is still a dragon */
    int others = 0;
    for (ccr_node_id i = 0; i < a->node_count; ++i)
        if (N(a, i)->kind == CCR_NODE_OTHER && !(N(a, i)->flags & CCR_FLAG_DEAD))
            ++others;
    CHECK(others == 1, "%d dragons (only the lambda should be one)", others);
}

/* --------------------------------------------------------- editing */

static void frame(ccr_editor *ed, ccr_vec2 m, bool pressed, bool down, bool released)
{
    ccr_editor_input in = { m, pressed, down, released, 1.0f, false, 0.0f };
    ccr_editor_update(ed, &in);
}

static void drag(ccr_editor *ed, ccr_vec2 from, ccr_vec2 to)
{
    frame(ed, from, true, true, false);
    for (int i = 1; i <= 8; ++i) {
        const float t = (float)i / 8.0f;
        frame(ed, (ccr_vec2){ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t }, false, true, false);
    }
    frame(ed, to, false, false, true);
}

/* Saves; true if g++ (when there is one) compiles what came out. */
static bool compiles(const ccr_editor *ed, char **text)
{
    char err[256] = "";
    size_t len = 0;
    *text = NULL;
    if (!ccr_codegen(&ed->ast, text, &len, err, sizeof err)) {
        fprintf(stderr, "codegen: %s\n", err);
        return false;
    }
    if (system("command -v c++ >/dev/null 2>&1") != 0)
        return true;   /* nothing to compile with: the text checks will do */
    char path[] = "/tmp/ccratch-cpp-XXXXXX.cpp";
    const int fd = mkstemps(path, 4);
    if (fd < 0)
        return false;
    const bool wrote = write(fd, *text, len) == (ssize_t)len;
    close(fd);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "c++ -std=c++20 -fsyntax-only %s", path);
    const bool ok = wrote && system(cmd) == 0;
    unlink(path);
    return ok;
}

/* In heal(), the `if` goes above `hp_ += amount`: an edit inside a method
 * of a class inside a namespace. */
static void test_edit_method(ccr_editor *ed)
{
    const ccr_ast *a = &ed->ast;
    const ccr_node_id heal = find(a, CCR_NODE_FUNCTION, "heal");
    ccr_node_id body = CCR_NONE;
    for (ccr_node_id c = N(a, heal)->first_child; c != CCR_NONE; c = N(a, c)->next_sibling)
        if (N(a, c)->kind == CCR_NODE_BLOCK)
            body = c;
    CHECK(body != CCR_NONE, "heal has no body");
    if (body == CCR_NONE)
        return;
    const ccr_node_id add = N(a, body)->first_child;
    const ccr_node_id iff = add != CCR_NONE ? N(a, add)->next_sibling : CCR_NONE;
    CHECK(iff != CCR_NONE && N(a, iff)->kind == CCR_NODE_IF, "heal: += then if");
    if (iff == CCR_NONE)
        return;
    ccr_rect ra, ri;
    CHECK(ccr_editor_node_rect(ed, add, &ra) && ccr_editor_node_rect(ed, iff, &ri), "heal's blocks are drawn");
    /* the if (and nothing under it) goes right above the += */
    drag(ed, (ccr_vec2){ ri.x + 6.0f, ri.y + 6.0f }, (ccr_vec2){ ra.x + 6.0f, ra.y + 6.0f - 4.0f });
    CHECK(ccr_editor_loose_count(ed) == 0, "the if did not snap back in (%u loose)", ccr_editor_loose_count(ed));
    CHECK(N(a, body)->first_child == iff, "the if is not first in heal");

    char *text = NULL;
    CHECK(compiles(ed, &text), "the edited file does not compile");
    if (text) {
        const char *i = strstr(text, "if (hp_ > 100)"), *p = strstr(text, "hp_ += amount;");
        CHECK(i && p && i < p, "heal is not if-then-add:\n%s", text);
        CHECK(strstr(text, "namespace game {") && strstr(text, "class Player {"), "the namespace or the class went");
    }
    free(text);
}

/* From the palette into the file's own class: a method under heal(), and
 * std::cout into hit(); <iostream> is there already, nothing else needed. */
static void test_palette_into_class(ccr_editor *ed)
{
    const ccr_ast *a = &ed->ast;
    const ccr_node_id alive = find(a, CCR_NODE_FUNCTION, "alive");
    ccr_rect ra;
    CHECK(ccr_editor_node_rect(ed, alive, &ra), "alive is drawn");
    /* hold a new method, let it go where alive() starts: under heal() */
    const ccr_vec2 at = { ra.x + 2.0f, ra.y + 2.0f };
    ccr_editor_spawn_held(ed, CCR_TEMPLATE_METHOD, (ccr_vec2){ at.x + 300.0f, at.y }, (ccr_vec2){ 4.0f, 4.0f });
    for (int i = 0; i <= 8; ++i)
        frame(ed, (ccr_vec2){ at.x + 300.0f - 37.0f * (float)i, at.y + 4.0f }, false, true, false);
    frame(ed, (ccr_vec2){ at.x + 4.0f, at.y + 4.0f }, false, false, true);
    const ccr_node_id update = find(a, CCR_NODE_FUNCTION, "update");
    CHECK(update != CCR_NONE && N(a, update)->parent == find(a, CCR_NODE_STRUCT, "Player"),
          "the new method is not in Player (%u loose)", ccr_editor_loose_count(ed));

    char *text = NULL;
    CHECK(compiles(ed, &text), "with a new method, the file does not compile");
    if (text) {
        const char *h = strstr(text, "void heal("), *u = strstr(text, "\n    void update()\n"), *al = strstr(text, "bool alive()");
        CHECK(h && u && al && h < u && u < al, "update() is not between heal and alive, indented:\n%s", text);
    }
    free(text);
}

static bool snap_at(const ccr_editor *ed, ccr_snap_kind kind, ccr_node_id ref, ccr_vec2 *out)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        for (uint32_t k = 0; k < ed->scripts[i].blocks.snap_count; ++k) {
            const ccr_snap *sn = &ed->scripts[i].blocks.snaps[k];
            if (sn->kind == kind && sn->ref == ref) {
                out->x = sn->at.x + ed->scripts[i].pos.x + 6.0f;
                out->y = sn->at.y + ed->scripts[i].pos.y + 6.0f;
                return true;
            }
        }
    return false;
}

/* Takes `t` from the palette and lets it go at `target` (a snap point). */
static ccr_node_id spawn_at(ccr_editor *ed, ccr_template t, ccr_vec2 target)
{
    const ccr_vec2 start = { 5000.0f, 2000.0f };
    CHECK(ccr_editor_spawn(ed, t, start), "could not take %s from the palette", ccr_template_label(t));
    const ccr_node_id loose = ed->scripts[ed->script_count - 1].root;
    const ccr_node_id made = ed->ast.nodes[loose].first_child;
    const ccr_vec2 drop = { target.x - 6.0f + 16.0f, target.y - 6.0f + 16.0f };
    for (int i = 1; i <= 6; ++i) {
        const float k = (float)i / 6.0f;
        frame(ed, (ccr_vec2){ start.x + (drop.x - start.x) * k, start.y + (drop.y - start.y) * k }, false, true, false);
    }
    frame(ed, drop, false, false, true);
    return made;
}

/* What is typed into a new block's empty slots: the first one gets `text`. */
static void fill(ccr_editor *ed, ccr_node_id id, const char *text)
{
    for (ccr_node_id c = N(&ed->ast, id)->first_child; c != CCR_NONE; c = N(&ed->ast, c)->next_sibling)
        if (N(&ed->ast, c)->kind == CCR_NODE_TEXT && !(N(&ed->ast, c)->flags & CCR_FLAG_WORD)) {
            ccr_editor_set_text(ed, c, text);
            return;
        }
}

/* Every C++ statement block from the palette, one under another in main,
 * and the file's own (class, namespace, using): it still compiles. */
static void test_every_cpp_block(ccr_editor *ed)
{
    const ccr_ast *a = &ed->ast;
    ccr_node_id after = find(a, CCR_NODE_DELETE, NULL);
    CHECK(after != CCR_NONE, "no delete boss;");
    if (after == CCR_NONE)
        return;
    static const struct { ccr_template t; const char *slot; } blocks[] = {
        { CCR_TEMPLATE_STRING_DECL, NULL },  { CCR_TEMPLATE_VECTOR_DECL, NULL },
        { CCR_TEMPLATE_PUSH_BACK, NULL },    { CCR_TEMPLATE_SORT, NULL },
        { CCR_TEMPLATE_FOR_RANGE, "v" },     { CCR_TEMPLATE_COUT, NULL },
        { CCR_TEMPLATE_CIN, NULL },          { CCR_TEMPLATE_GETLINE, NULL },
        { CCR_TEMPLATE_DELETE_PTR, "static_cast<int *>(nullptr)" },
    };
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; ++i) {
        ccr_vec2 at;
        CHECK(snap_at(ed, CCR_SNAP_AFTER, after, &at), "no place under node %u", after);
        const ccr_node_id made = spawn_at(ed, blocks[i].t, at);
        CHECK(N(a, made)->parent == N(a, after)->parent, "%s did not snap", ccr_template_label(blocks[i].t));
        if (blocks[i].slot)
            fill(ed, made, blocks[i].slot);
        after = made;
    }
    /* cin >> x: an x to read into */
    ccr_vec2 at;
    snap_at(ed, CCR_SNAP_AFTER, find(a, CCR_NODE_DELETE, NULL), &at);
    const ccr_node_id x = spawn_at(ed, CCR_TEMPLATE_DECL, at);
    CHECK(x != CCR_NONE, "int x");
    /* the file's own: dropped anywhere, they find their place */
    spawn_at(ed, CCR_TEMPLATE_USING_NS, (ccr_vec2){ 4000.0f, 100.0f });
    spawn_at(ed, CCR_TEMPLATE_NAMESPACE_DEF, (ccr_vec2){ 4000.0f, 300.0f });
    spawn_at(ed, CCR_TEMPLATE_CLASS_DEF, (ccr_vec2){ 4000.0f, 500.0f });

    char *text = NULL;
    CHECK(compiles(ed, &text), "the C++ blocks do not compile");
    if (text) {
        static const char *const want[] = { "std::string s = \"\";", "std::vector<int> v;", "v.push_back(x);",
                                            "std::sort(v.begin(), v.end());", "for (auto x : v) {",
                                            "std::cout << \"Hello!\" << '\\n';", "std::cin >> x;",
                                            "std::getline(std::cin, s);", "delete static_cast<int *>(nullptr);",
                                            "using namespace std;", "namespace game {", "class Player {",
                                            "public:" };
        for (size_t i = 0; i < sizeof want / sizeof want[0]; ++i)
            CHECK(strstr(text, want[i]) != NULL, "no `%s` in:\n%s", want[i], text);
        CHECK(strstr(text, "#include <algorithm>") != NULL, "std::sort did not bring <algorithm>");
    }
    free(text);
}

/* Every slot of a new block, in order: NULL keeps what is there. */
static void fill_all(ccr_editor *ed, ccr_node_id id, const char *const *values, int n)
{
    int i = 0;
    for (ccr_node_id c = N(&ed->ast, id)->first_child; c != CCR_NONE && i < n; c = N(&ed->ast, c)->next_sibling)
        if (N(&ed->ast, c)->kind == CCR_NODE_TEXT && !(N(&ed->ast, c)->flags & CCR_FLAG_WORD)) {
            if (values[i])
                ccr_editor_set_text(ed, c, values[i]);
            ++i;
        }
}

/* C's standard library: every statement block into a file that includes
 * nothing; their headers come along, and it compiles. */
static int test_std_c(const char *path)
{
    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(path, NULL, 0, &ast)) {
        fprintf(stderr, "could not parse %s\n", path);
        return 1;
    }
    ccr_editor ed;
    if (!ccr_editor_init(&ed, &ast))
        return 1;
    static const struct { ccr_template t; const char *v[3]; } blocks[] = {
        { CCR_TEMPLATE_PRINTF, { NULL } },     { CCR_TEMPLATE_PUTS, { NULL } },
        { CCR_TEMPLATE_SCANF, { NULL, "&n" } }, { CCR_TEMPLATE_FGETS, { NULL } },
        { CCR_TEMPLATE_MEMCPY, { NULL } },     { CCR_TEMPLATE_MEMSET, { NULL } },
        { CCR_TEMPLATE_SRAND, { NULL } },      { CCR_TEMPLATE_FPRINTF, { "stderr", NULL, "n" } },
        { CCR_TEMPLATE_FCLOSE, { "stdin" } },  { CCR_TEMPLATE_FREE, { "p" } },
        { CCR_TEMPLATE_EXIT, { NULL } },
    };
    ccr_node_id after = find(&ed.ast, CCR_NODE_DECL_GROUP, NULL);   /* under void *dst, *src */
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0] && after != CCR_NONE; ++i) {
        ccr_vec2 at;
        CHECK(snap_at(&ed, CCR_SNAP_AFTER, after, &at), "no place under node %u", after);
        const ccr_node_id made = spawn_at(&ed, blocks[i].t, at);
        CHECK(N(&ed.ast, made)->parent == N(&ed.ast, after)->parent, "%s did not snap",
              ccr_template_label(blocks[i].t));
        fill_all(&ed, made, blocks[i].v, 3);
        after = made;
    }
    char err[256] = "", *text = NULL;
    size_t len = 0;
    CHECK(ccr_codegen(&ed.ast, &text, &len, err, sizeof err), "codegen: %s", err);
    if (text) {
        for (const char *h = "stdio.h\0string.h\0stdlib.h\0"; *h; h += strlen(h) + 1) {
            char inc[64];
            snprintf(inc, sizeof inc, "#include <%s>", h);
            CHECK(strstr(text, inc) != NULL, "no %s in:\n%s", inc, text);
        }
        if (system("command -v cc >/dev/null 2>&1") == 0) {
            char tmp[] = "/tmp/ccratch-std-XXXXXX.c";
            const int fd = mkstemps(tmp, 2);
            const bool wrote = fd >= 0 && write(fd, text, len) == (ssize_t)len;
            if (fd >= 0)
                close(fd);
            char cmd[256];
            snprintf(cmd, sizeof cmd, "cc -std=c11 -Wall -Werror=implicit-function-declaration -fsyntax-only %s", tmp);
            CHECK(wrote && system(cmd) == 0, "the C file does not compile:\n%s", text);
            unlink(tmp);
        }
    }
    free(text);
    ccr_editor_free(&ed);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.cpp\n", argv[0]);
        return 2;
    }
    if (!ccr_gui_init(1280, 800, "ccratch test")) {
        fprintf(stderr, "no window (run under xvfb-run)\n");
        return 2;
    }
    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(argv[1], NULL, 0, &ast)) {
        fprintf(stderr, "could not parse %s\n", argv[1]);
        return 2;
    }
    test_tree(&ast);
    ccr_editor ed;
    if (!ccr_editor_init(&ed, &ast)) {
        fprintf(stderr, "editor init failed\n");
        return 2;
    }
    test_edit_method(&ed);
    test_palette_into_class(&ed);
    test_every_cpp_block(&ed);
    ccr_editor_free(&ed);
    if (argc > 2)
        test_std_c(argv[2]);
    ccr_gui_shutdown();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("cpp: all checks passed\n");
    return g_failures ? 1 : 0;
}
