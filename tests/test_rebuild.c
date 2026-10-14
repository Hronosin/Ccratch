/*
 * Stress test for saving: marks every Block of a real file as changed, so
 * the code generator rebuilds all of them from their statements' text,
 * then checks that clang reads the result back into the same tree.
 *
 *   test_rebuild OUT.c FILE.c [-- clang args...]
 *
 * Blocks that cannot be rebuilt safely (#if between statements, blocks
 * made by macros) are left alone and counted.
 *
 * Then expressions: every one that may be taken out of its slot is (1)
 * marked as moved into that same slot, and (2) replaced by typed text
 * equal to its own, as if deleted and typed back. Both must give back the
 * file byte for byte: what is copied around a slot is exactly right.
 */
#include "ccr_ast.h"
#include "ccr_codegen.h"
#include "ccr_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generates, and compares with the original file. */
static int same_as_source(const ccr_ast *ast, const char *what, const char *path)
{
    char *text = NULL, err[256];
    size_t len = 0;
    if (!ccr_codegen(ast, &text, &len, err, sizeof err)) {
        fprintf(stderr, "%s: %s: codegen refused: %s\n", path, what, err);
        return 1;
    }
    int bad = len != ast->source_len || memcmp(text, ast->source, len) != 0;
    if (bad) {
        size_t i = 0;
        while (i < len && i < ast->source_len && text[i] == ast->source[i])
            ++i;
        fprintf(stderr, "%s: %s: differs at byte %zu: \"%.40s\" vs \"%.40s\"\n", path, what, i,
                text + i, ast->source + i);
    }
    free(text);
    return bad;
}

static int expressions(const char *path, const char *const *args, int nargs)
{
    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(path, args, nargs, &ast))
        return 1;
    unsigned movable = 0;
    for (ccr_node_id i = 0; i < ast.node_count; ++i)
        if (ccr_codegen_can_move(&ast, i)) {
            ast.nodes[i].flags |= CCR_FLAG_MOVED;
            ++movable;
        }
    int fails = same_as_source(&ast, "every expression moved into its own slot", path);
    ccr_ast_free(&ast);

    ccr_ast_init(&ast);
    if (!ccr_parse_file(path, args, nargs, &ast))
        return 1;
    unsigned retyped = 0;
    const uint32_t parsed = ast.node_count;
    for (ccr_node_id i = 0; i < parsed; ++i) {
        if (!ccr_codegen_can_move(&ast, i))
            continue;
        ccr_node_id up = i;   /* inside one retyped already: not in the file any more */
        while (ast.nodes[up].parent != CCR_NONE)
            up = ast.nodes[up].parent;
        if (up != 0)
            continue;
        char text[4096];
        uint32_t n = ast.nodes[i].src_e - ast.nodes[i].src_b;
        if (n >= sizeof text)
            continue;
        memcpy(text, ast.source + ast.nodes[i].src_b, n);
        text[n] = '\0';
        ccr_node_id t = ccr_ast_add(&ast, CCR_NODE_TEXT, CCR_NONE);
        if (t == CCR_NONE)
            return 1;
        ast.nodes[t].flags |= CCR_FLAG_NEW;
        ast.nodes[t].name = ccr_ast_intern(&ast, text);
        ast.nodes[t].place_b = ast.nodes[i].place_b;
        ast.nodes[t].place_e = ast.nodes[i].place_e;
        ccr_ast_insert_after(&ast, i, t);
        ccr_ast_unlink(&ast, i);
        ++retyped;
    }
    fails += same_as_source(&ast, "every expression deleted and typed back", path);
    printf("%s: %u expressions can move, %u retyped, %s\n", path, movable, retyped,
           fails ? "MISMATCH" : "byte for byte");
    ccr_ast_free(&ast);
    return fails;
}

static void histogram(const ccr_ast *ast, unsigned *h)
{
    memset(h, 0, sizeof(unsigned) * CCR_NODE_KIND_COUNT);
    for (ccr_node_id i = 0; i < ast->node_count; ++i)
        h[ast->nodes[i].kind]++;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s OUT.c FILE.c [-- clang args]\n", argv[0]);
        return 2;
    }
    const char *out_path = argv[1], *in_path = argv[2];
    const char *const *args = NULL;
    int nargs = 0;
    if (argc > 4 && strcmp(argv[3], "--") == 0) {
        args = (const char *const *)&argv[4];
        nargs = argc - 4;
    }

    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(in_path, args, nargs, &ast)) {
        fprintf(stderr, "cannot parse %s\n", in_path);
        return 2;
    }
    const uint32_t errors_before = ccr_ast_count_diags(&ast, CCR_DIAG_ERROR);

    /* every Block that can be rebuilt is "changed"; ones under a macro-made
     * block are skipped too, since their text lives inside the macro call */
    unsigned rebuilt = 0, skipped = 0;
    for (ccr_node_id i = 0; i < ast.node_count; ++i) {
        ccr_node *n = &ast.nodes[i];
        if (n->kind != CCR_NODE_BLOCK || (n->flags & (CCR_FLAG_LOOSE | CCR_FLAG_DEAD)))
            continue;
        bool safe = !(n->flags & CCR_FLAG_UNSAFE_GAP);
        for (ccr_node_id p = n->parent; p != CCR_NONE && safe; p = ast.nodes[p].parent)
            if (ast.nodes[p].kind == CCR_NODE_BLOCK && (ast.nodes[p].flags & CCR_FLAG_UNSAFE_GAP))
                safe = false;
        if (safe) {
            n->flags |= CCR_FLAG_DIRTY;
            ++rebuilt;
        } else {
            ++skipped;
        }
    }

    char *text = NULL, err[256];
    size_t len = 0;
    if (!ccr_codegen(&ast, &text, &len, err, sizeof err)) {
        fprintf(stderr, "%s: codegen refused: %s\n", in_path, err);
        return 1;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f || fwrite(text, 1, len, f) != len) {
        fprintf(stderr, "cannot write %s\n", out_path);
        return 2;
    }
    fclose(f);
    free(text);

    ccr_ast back;
    ccr_ast_init(&back);
    if (!ccr_parse_file(out_path, args, nargs, &back)) {
        fprintf(stderr, "cannot parse the rebuilt file\n");
        return 1;
    }
    const uint32_t errors_after = ccr_ast_count_diags(&back, CCR_DIAG_ERROR);

    unsigned h1[CCR_NODE_KIND_COUNT], h2[CCR_NODE_KIND_COUNT];
    histogram(&ast, h1);
    histogram(&back, h2);
    int fails = 0;
    if (errors_after > errors_before) {
        fprintf(stderr, "%s: %u errors after rebuilding (was %u)\n", in_path, errors_after,
                errors_before);
        ccr_ast_dump_diags(&back, stderr);
        ++fails;
    }
    for (int k = 0; k < CCR_NODE_KIND_COUNT; ++k) {
        if (h1[k] != h2[k]) {
            fprintf(stderr, "%s: %s nodes: %u before, %u after\n", in_path,
                    ccr_node_kind_name((ccr_node_kind)k), h1[k], h2[k]);
            ++fails;
        }
    }
    printf("%s: %u blocks rebuilt, %u left alone (#if inside), %s\n", in_path, rebuilt, skipped,
           fails ? "MISMATCH" : "same tree");
    ccr_ast_free(&back);
    ccr_ast_free(&ast);
    fails += expressions(in_path, args, nargs);
    return fails ? 1 : 0;
}
