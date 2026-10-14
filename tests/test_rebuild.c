/*
 * Stress test for saving: marks every Block of a real file as changed, so
 * the code generator rebuilds all of them from their statements' text,
 * then checks that clang reads the result back into the same tree.
 *
 *   test_rebuild OUT.c FILE.c [-- clang args...]
 *
 * Blocks that cannot be rebuilt safely (#if between statements, blocks
 * made by macros) are left alone and counted.
 */
#include "ccr_ast.h"
#include "ccr_codegen.h"
#include "ccr_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return fails ? 1 : 0;
}
