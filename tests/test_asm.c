/*
 * Assembly as blocks: .s/.S/.asm files read, saved untouched and changed;
 * asm("...") in C taken apart and written again; an instruction dropped
 * among C statements saved as __asm__ volatile("...").
 *
 * test_asm [--from-c FILE.c...] [FILE.s...]: each assembly file is also
 * read, saved untouched (byte for byte) and with every body rebuilt (the
 * same statements, maybe spaced differently); when it can be assembled
 * (as / nasm), the rebuilt one must assemble to the very same bytes. A C
 * file after --from-c is compiled to assembly first (cc -S, AT&T and
 * Intel): gcc's own output makes a big corpus. --target TRIPLE FILE.c:
 * clang's for another machine (aarch64-linux-gnu, armv7a-linux-gnueabihf,
 * thumbv7...), assembled by clang for it too.
 */
#define _DEFAULT_SOURCE
#include "ccr_asm.h"
#include "ccr_codegen.h"
#include "ccr_parse.h"
#include "ccr_template.h"

#include <stdbool.h>
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

static bool parse(const char *text, ccr_asm_syntax syn, ccr_ast *a)
{
    ccr_ast_init(a);
    return ccr_asm_parse_text(text, strlen(text), "hello.s", syn, false, a) &&
           ccr_ast_count_diags(a, CCR_DIAG_ERROR) == 0;
}

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

static int count_kids(const ccr_ast *a, ccr_node_id id)
{
    int n = 0;
    for (ccr_node_id c = N(a, id)->first_child; c != CCR_NONE; c = N(a, c)->next_sibling)
        ++n;
    return n;
}

static void dirty(ccr_ast *a, ccr_node_id blk) { a->nodes[blk].flags |= CCR_FLAG_DIRTY; }

static const char *g_att =
    "\t.file\t\"hello.c\"\n"
    "\t.text\n"
    "\t.section\t.rodata.str1.1,\"aMS\",@progbits,1\n"
    ".LC0:\n"
    "\t.string\t\"%d\\n\"   # the format\n"
    "\t.text\n"
    "\t.globl\tmain\n"
    "\t.type\tmain, @function\n"
    "main:\n"
    "\tpushq\t%rbx\n"
    "\tmovl\t$0, %ebx\n"
    "\n"
    ".L3:\n"
    "\taddl\t$1, %ebx   /* one more */\n"
    "\tcmpl\t$5, %ebx\n"
    "\tjne\t.L3\n"
    "\tmovl\t%ebx, %esi\n"
    "\tleaq\t.LC0(%rip), %rdi\n"
    "\tmovl\t$0, %eax\n"
    "\tcall\tprintf@PLT\n"
    "\tpopq\t%rbx\n"
    "\tret\n"
    "\t.size\tmain, .-main\n"
    "\t.section\t.note.GNU-stack,\"\",@progbits\n";

static void test_structure(void)
{
    ccr_ast a;
    CHECK(parse(g_att, CCR_ASM_ATT, &a), "the sample does not read");
    CHECK(count_kids(&a, 0) == 4, "%d hats, want 4 (.rodata, .text, main, .note)", count_kids(&a, 0));
    const ccr_node_id h0 = kid(&a, 0, 0), h2 = kid(&a, 0, 2);
    CHECK(strncmp(S(&a, N(&a, h0)->name), ".rodata", 7) == 0, "first hat: %s", S(&a, N(&a, h0)->name));
    CHECK(strcmp(S(&a, N(&a, h2)->name), "main") == 0 && strcmp(S(&a, N(&a, h2)->type), "label") == 0,
          "main's hat: %s", S(&a, N(&a, h2)->name));
    const ccr_node_id go = find(&a, CCR_NODE_GOTO, ".L3", 0);
    CHECK(go != CCR_NONE && strcmp(S(&a, N(&a, go)->op), "jne") == 0 && (N(&a, go)->flags & CCR_FLAG_ASM),
          "jne .L3 is a jump with its arrow");
    CHECK(find(&a, CCR_NODE_LABEL, ".L3", 0) != CCR_NONE && find(&a, CCR_NODE_LABEL, "main", 0) == CCR_NONE,
          ".L3 a label, main a hat");
    const ccr_node_id call = find(&a, CCR_NODE_CALL, "call", 0);
    CHECK(call != CCR_NONE && strcmp(S(&a, N(&a, kid(&a, call, 0))->name), "printf@PLT") == 0,
          "call printf@PLT: a call, not a jump");
    const ccr_node_id lea = find(&a, CCR_NODE_CALL, "leaq", 0);
    CHECK(lea != CCR_NONE && count_kids(&a, lea) == 2 &&
          strcmp(S(&a, N(&a, kid(&a, lea, 0))->name), ".LC0(%rip)") == 0, "leaq's operands");
    const ccr_node_id sec = find(&a, CCR_NODE_CALL, ".section", 0);
    CHECK(sec != CCR_NONE && count_kids(&a, sec) == 4, ".section's 4 operands, commas in quotes and all");
    CHECK(ccr_asm_category("movl") == 2 /* variable */ && ccr_asm_category("addl") == 1 &&
          ccr_asm_category("jne") == 0 && ccr_asm_category(".globl") == 6 && ccr_asm_category("db") == 5,
          "colours");

    char *t = gen(&a);
    CHECK(t && strcmp(t, g_att) == 0, "untouched: not the same:\n%s", t);
    free(t);

    /* every body rebuilt: the very same, spacing and comments included */
    for (uint32_t i = 0; i < a.node_count; ++i)
        if (N(&a, i)->kind == CCR_NODE_BLOCK)
            dirty(&a, i);
    t = gen(&a);
    CHECK(t && strcmp(t, g_att) == 0, "rebuilt: not the same:\n%s", t);
    free(t);

    /* a moved instruction, an operand typed in, new ones from the palette */
    const ccr_node_id body = kid(&a, h2, 0);
    const ccr_node_id mov = find(&a, CCR_NODE_CALL, "movl", 0);
    ccr_ast_unlink(&a, mov);
    ccr_ast_append(&a, body, mov);
    a.nodes[kid(&a, mov, 0)].name = ccr_ast_intern(&a, "$7");
    a.nodes[kid(&a, mov, 0)].flags |= CCR_FLAG_NEW;
    ccr_node_id nop = ccr_asm_build(&a, CCR_NONE, "nop", CCR_ASM_ATT);
    ccr_ast_insert_first(&a, body, nop);
    ccr_node_id j = ccr_asm_build_jump(&a, CCR_NONE, "jmp");
    ccr_ast_append(&a, body, j);
    t = gen(&a);
    CHECK(t && strstr(t, "main:\n\tnop\n\tpushq\t%rbx\n\n.L3:"), "nop first, the blank line kept:\n%s", t);
    CHECK(t && strstr(t, "\t.size\tmain, .-main\n\tmovl\t$7, %ebx\n\tjmp .Lloop\n\t.section\t.note"),
          "moved to the end, with its new operand; a new jump:\n%s", t);
    free(t);

    /* switched off */
    a.nodes[nop].flags |= CCR_FLAG_COMMENTED;
    t = gen(&a);
    CHECK(t && strstr(t, "main:\n\t# nop\n"), "commented out:\n%s", t);
    ccr_ast b;
    CHECK(t && parse(t, CCR_ASM_ATT, &b), "the saved file does not read");
    ccr_ast_free(&b);
    free(t);
    ccr_ast_free(&a);

    /* an empty file, then its first instruction */
    CHECK(parse("# nothing yet\n", CCR_ASM_ATT, &a), "a comment-only file");
    CHECK(count_kids(&a, 0) == 1, "one hat to start from");
    const ccr_node_id eb = kid(&a, kid(&a, 0, 0), 0);
    ccr_node_id r = ccr_asm_build(&a, CCR_NONE, "ret", CCR_ASM_ATT);
    ccr_ast_append(&a, eb, r);
    dirty(&a, eb);
    t = gen(&a);
    CHECK(t && strcmp(t, "# nothing yet\n\tret") == 0, "into an empty file: [%s]", t);
    free(t);
    ccr_ast_free(&a);
}

static const char *g_nasm =
    "; hello in NASM\n"
    "        global  _start\n"
    "\n"
    "        section .text\n"
    "_start: mov     rax, 1          ; write\n"
    "        lea     rsi, [rel msg]\n"
    ".loop:  dec     rcx\n"
    "        jnz     short .loop\n"
    "%macro say 1\n"
    "        mov rax, %1\n"
    "%endmacro\n"
    "%ifdef DEBUG\n"
    "        int3\n"
    "%else\n"
    "        nop\n"
    "%endif\n"
    "        section .data\n"
    "msg     db      \"Hello, world!\", 10\n"
    "len     equ     $ - msg\n";

static void test_nasm(void)
{
    ccr_ast a;
    CHECK(parse(g_nasm, CCR_ASM_NASM, &a), "NASM does not read");
    const ccr_node_id start = find(&a, CCR_NODE_FUNCTION, "_start", 0);
    CHECK(start != CCR_NONE, "_start is a hat");
    const ccr_node_id first = start != CCR_NONE ? kid(&a, kid(&a, start, 0), 0) : CCR_NONE;
    CHECK(first != CCR_NONE && strcmp(S(&a, N(&a, first)->name), "mov") == 0, "`_start: mov` on one line");
    CHECK(find(&a, CCR_NODE_GOTO, ".loop", 0) != CCR_NONE, "jnz short .loop jumps to .loop");
    const ccr_node_id mac = find(&a, CCR_NODE_SCOPE, "%macro", 0);
    CHECK(mac != CCR_NONE && strcmp(S(&a, N(&a, kid(&a, mac, 0))->name), "say 1") == 0 &&
          count_kids(&a, kid(&a, mac, 1)) == 1, "%%macro with its body");
    const ccr_node_id iff = find(&a, CCR_NODE_IF, "%ifdef", 0);
    CHECK(iff != CCR_NONE && count_kids(&a, iff) == 3, "%%ifdef ... %%else ... %%endif");
    const ccr_node_id db = find(&a, CCR_NODE_CALL, "db", 0);
    CHECK(db != CCR_NONE && strcmp(S(&a, N(&a, db)->type), "msg") == 0 && count_kids(&a, db) == 2,
          "msg db: the label goes with it");
    const ccr_node_id equ = find(&a, CCR_NODE_CALL, "equ", 0);
    CHECK(equ != CCR_NONE && strcmp(S(&a, N(&a, kid(&a, equ, 0))->name), "$ - msg") == 0, "len equ $ - msg");

    for (uint32_t i = 0; i < a.node_count; ++i)
        if (N(&a, i)->kind == CCR_NODE_BLOCK)
            dirty(&a, i);
    char *t = gen(&a);
    CHECK(t && strstr(t, "_start: mov     rax, 1          ; write\n        lea") &&
          strstr(t, "%else\n        nop\n%endif\n") && strstr(t, "msg     db      \"Hello, world!\", 10\n"),
          "NASM rebuilt:\n%s", t);
    free(t);

    /* into the macro: printed with the file's indent, NASM's way */
    ccr_node_id m = ccr_asm_build(&a, CCR_NONE, "msg2 db 'bye', 0", CCR_ASM_NASM);
    ccr_ast_append(&a, kid(&a, mac, 1), m);
    ccr_node_id s = ccr_asm_build_scope(&a, CCR_NONE, "%rep", "4", false);
    ccr_ast_append(&a, kid(&a, s, 1), ccr_asm_build(&a, CCR_NONE, "nop", CCR_ASM_NASM));
    ccr_ast_append(&a, kid(&a, mac, 1), s);
    t = gen(&a);
    CHECK(t && strstr(t, "        mov rax, %1\n        msg2 db 'bye', 0\n        %rep 4\n        nop\n        "
                         "%endrep\n%endmacro"),
          "new statements in NASM:\n%s", t);
    free(t);
    a.nodes[m].flags |= CCR_FLAG_COMMENTED;
    t = gen(&a);
    CHECK(t && strstr(t, "        ; msg2 db 'bye', 0\n"), "NASM comments with ';':\n%s", t);
    free(t);
    ccr_ast_free(&a);

    /* an .endm without its .macro: no mouths, but read all the same */
    CHECK(parse("\tnop\n.endm\n\tret\n", CCR_ASM_ATT, &a), "a stray .endm");
    t = gen(&a);
    CHECK(t && strcmp(t, "\tnop\n.endm\n\tret\n") == 0, "kept as it was");
    free(t);
    ccr_ast_free(&a);
}

/* ------------------------------------------------------------- ARM */

static const char *g_arm64 =
    "// AArch64\n"
    "\t.text\n"
    "\t.globl\tf\n"
    "\t.type\tf, @function\n"
    "f:\n"
    "\tstp\tx29, x30, [sp, #-16]!   // save\n"
    "\tmov\tw8, #0\n"
    ".L1:\n"
    "\tcbz\tx0, .L2\n"
    "\tldr\tw9, [x0], #4\n"
    "\tb.ne\t.L1\n"
    ".L2:\n"
    "\tldp\tx29, x30, [sp], #16\n"
    "\tret\n";

static const char *g_arm32 =
    "@ ARM\n"
    "\t.syntax unified\n"
    "\t.globl\tg\n"
    "\t.type\tg, %function\n"
    "g:\n"
    "\tpush\t{r4, lr}   @ save\n"
    "\tmov\tr4, #1\n"
    "# 12 \"g.c\"\n"
    ".L3:\n"
    "\tsubs\tr4, r4, #1\n"
    "\tbne\t.L3\n"
    "\tpop\t{r4, pc}\n";

static void test_arm(void)
{
    CHECK(ccr_asm_syntax_of("x.s", g_arm64, strlen(g_arm64)) == CCR_ASM_ARM64, "AArch64 recognised");
    CHECK(ccr_asm_syntax_of("x.s", g_arm32, strlen(g_arm32)) == CCR_ASM_ARM32, "32-bit ARM recognised");
    CHECK(ccr_asm_syntax_of("x.s", g_att, strlen(g_att)) == CCR_ASM_ATT, "x86 stays x86");
    ccr_ast a;
    CHECK(parse(g_arm64, CCR_ASM_ARM64, &a), "AArch64 does not read");
    const ccr_node_id mov = find(&a, CCR_NODE_CALL, "mov", 0);
    CHECK(mov != CCR_NONE && count_kids(&a, mov) == 2 && strcmp(S(&a, N(&a, kid(&a, mov, 1))->name), "#0") == 0,
          "# is a number, not a comment");
    const ccr_node_id cbz = find(&a, CCR_NODE_GOTO, ".L2", 0);
    CHECK(cbz != CCR_NONE && strcmp(S(&a, N(&a, cbz)->op), "cbz") == 0 && count_kids(&a, cbz) == 1 &&
          strcmp(S(&a, N(&a, kid(&a, cbz, 0))->name), "x0") == 0, "cbz x0, .L2: a jump, testing x0");
    CHECK(find(&a, CCR_NODE_GOTO, ".L1", 0) != CCR_NONE, "b.ne .L1");
    const ccr_node_id stp = find(&a, CCR_NODE_CALL, "stp", 0);
    CHECK(stp != CCR_NONE && count_kids(&a, stp) == 3 &&
          strcmp(S(&a, N(&a, kid(&a, stp, 2))->name), "[sp, #-16]!") == 0, "stp's address, writeback and all");
    CHECK(ccr_asm_ends_flow("ret") && ccr_asm_ends_flow("b") && !ccr_asm_ends_flow("b.ne") &&
          !ccr_asm_ends_flow("bl") && ccr_asm_is_jump("bne") && ccr_asm_is_jump("b.eq") && !ccr_asm_is_jump("bl") &&
          !ccr_asm_is_jump("bic") && ccr_asm_is_jump("beq.n"), "ARM's jumps");
    CHECK(ccr_asm_category("ldr") == 2 && ccr_asm_category("add") == 1 && ccr_asm_category("addeq") == 1 &&
          ccr_asm_category("bl") == 0 && ccr_asm_category("stp") == 2, "ARM's colours");
    /* everything printed anew: the same text, the way Ccratch spaces it */
    for (uint32_t i = 0; i < a.node_count; ++i)
        if (N(&a, i)->flags & CCR_FLAG_ASM)
            a.nodes[i].flags |= CCR_FLAG_NEW;
    char *t = gen(&a);
    CHECK(t && strstr(t, "\tcbz x0, .L2\n") && strstr(t, "\tstp x29, x30, [sp, #-16]!   // save\n") && strstr(t, ".L1:\n"),
          "reprinted:\n%s", t);
    free(t);
    ccr_ast_free(&a);

    CHECK(parse(g_arm32, CCR_ASM_ARM32, &a), "ARM does not read");
    const ccr_node_id push = find(&a, CCR_NODE_CALL, "push", 0);
    CHECK(push != CCR_NONE && count_kids(&a, push) == 1 &&
          strcmp(S(&a, N(&a, kid(&a, push, 0))->name), "{r4, lr}") == 0, "push {r4, lr}: one operand");
    CHECK(find(&a, CCR_NODE_GOTO, ".L3", 0) != CCR_NONE, "bne .L3");
    ccr_node_id nop = ccr_asm_build(&a, CCR_NONE, "mov r0, #0", CCR_ASM_ARM32);
    ccr_ast_insert_first(&a, kid(&a, find(&a, CCR_NODE_FUNCTION, "g", 0), 0), nop);
    a.nodes[nop].flags |= CCR_FLAG_COMMENTED;
    dirty(&a, kid(&a, find(&a, CCR_NODE_FUNCTION, "g", 0), 0));
    t = gen(&a);
    CHECK(t && strstr(t, "g:\n\t@ mov r0, #0\n\tpush\t{r4, lr}   @ save\n"), "ARM comments with @:\n%s", t);
    free(t);
    ccr_ast_free(&a);
}

/* ------------------------------------------------------- asm() in C */

static const char *g_inline =
    "int add(int a, int b)\n"
    "{\n"
    "    int r;\n"
    "    asm(\"movl %1, %0\\n\\t\"\n"
    "        \"addl %2, %0\"\n"
    "        : \"=r\"(r)\n"
    "        : \"r\"(a), \"r\"(b)\n"
    "        : \"cc\");\n"
    "    __asm__ volatile(\"nop\");\n"
    "    return r;\n"
    "}\n";

static bool parse_c(const char *text, ccr_ast *a)
{
    ccr_ast_init(a);
    static const char *const args[] = { "-std=gnu11" };
    return ccr_parse_text("inline.c", text, strlen(text), args, 1, a);
}

static void test_inline(void)
{
    ccr_asm_parts p;
    const char *st = "asm volatile goto (\"jmp %l0\" : : \"r\"(x), [y] \"m\"(f(1, 2)) : \"memory\" : out)";
    CHECK(ccr_asm_parts_of(st, (uint32_t)strlen(st), &p), "parts");
    CHECK(p.sections == 4 && p.op_count == 2 && p.op_section[1] == 1, "%d sections, %d operands", p.sections,
          p.op_count);
    CHECK(strncmp(st + p.ops[1].cb, "[y] \"m\"", p.ops[1].ce - p.ops[1].cb) == 0 &&
          strncmp(st + p.ops[1].eb, "f(1, 2)", p.ops[1].ee - p.ops[1].eb) == 0, "named operand");
    CHECK(strncmp(st + p.quals_b, "volatile goto", p.quals_e - p.quals_b) == 0, "qualifiers");

    ccr_ast a;
    CHECK(parse_c(g_inline, &a), "C with asm() does not read");
    const ccr_node_id as = find(&a, CCR_NODE_ASM, NULL, 0);
    CHECK(as != CCR_NONE && N(&a, kid(&a, as, 0))->kind == CCR_NODE_BLOCK, "asm() has its Block first");
    const ccr_node_id blk = as != CCR_NONE ? kid(&a, as, 0) : CCR_NONE;
    CHECK(blk != CCR_NONE && count_kids(&a, blk) == 2, "two instructions");
    CHECK(count_kids(&a, as) == 4, "and three operands after it: %d kids", count_kids(&a, as));
    const ccr_node_id movl = blk != CCR_NONE ? kid(&a, blk, 0) : CCR_NONE;
    CHECK(movl != CCR_NONE && strcmp(S(&a, N(&a, movl)->name), "movl") == 0 && (N(&a, movl)->flags & CCR_FLAG_ASM),
          "movl %%1, %%0");
    char *t = gen(&a);
    CHECK(t && strcmp(t, g_inline) == 0, "untouched:\n%s", t);
    free(t);
    if (blk == CCR_NONE) {
        ccr_ast_free(&a);
        return;
    }

    /* swapped, and one more: the strings are written again */
    const ccr_node_id addl = kid(&a, blk, 1);
    ccr_ast_unlink(&a, addl);
    ccr_ast_insert_first(&a, blk, addl);
    ccr_ast_append(&a, blk, ccr_asm_build(&a, CCR_NONE, "incl %0", CCR_ASM_ATT));
    dirty(&a, blk);
    t = gen(&a);
    CHECK(t && strstr(t, "    asm(\"addl %2, %0\\n\\t\"\n        \"movl %1, %0\\n\\t\"\n        \"incl %0\"\n"
                         "        : \"=r\"(r)\n"),
          "rewritten:\n%s", t);
    free(t);

    /* one taken out, among the C statements: an asm of its own */
    const ccr_node_id fn_body = kid(&a, find(&a, CCR_NODE_FUNCTION, "add", 0), 2);
    ccr_node_id body = CCR_NONE;
    for (ccr_node_id c = N(&a, find(&a, CCR_NODE_FUNCTION, "add", 0))->first_child; c != CCR_NONE;
         c = N(&a, c)->next_sibling)
        if (N(&a, c)->kind == CCR_NODE_BLOCK)
            body = c;
    (void)fn_body;
    CHECK(body != CCR_NONE, "add's body");
    if (body != CCR_NONE) {
        ccr_ast_unlink(&a, movl);
        ccr_ast_append(&a, body, movl);
        dirty(&a, body);
        t = gen(&a);
        CHECK(t && strstr(t, "    return r;\n    __asm__ volatile (\"movl %1, %0\");\n}"),
              "an instruction on its own in C:\n%s", t);
        free(t);
        /* and a new asm block from the palette */
        ccr_node_id nu = ccr_asm_build_inline(&a, CCR_NONE);
        ccr_ast_append(&a, kid(&a, nu, 0), ccr_asm_build(&a, CCR_NONE, "pause", CCR_ASM_ATT));
        a.nodes[kid(&a, nu, 3)].name = ccr_ast_intern(&a, "\"memory\"");
        ccr_ast_insert_first(&a, body, nu);
        t = gen(&a);
        CHECK(t && strstr(t, "{\n    __asm__ volatile (\"pause\" : : : \"memory\");\n    int r;"),
              "a new asm():\n%s", t);
        ccr_ast b;
        CHECK(t && parse_c(t, &b) && find(&b, CCR_NODE_ASM, NULL, 3) != CCR_NONE, "what is saved reads again");
        ccr_ast_free(&b);
        free(t);
    }
    ccr_ast_free(&a);
}

/* ------------------------------------------------------- a corpus */

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    fclose(f);
    if (s) {
        s[n] = '\0';
        *len = (size_t)n;
    }
    return s;
}

/* The bytes an assembler makes of `path`, as objdump prints them. */
static char *assembled(const char *path, ccr_asm_syntax syn, const char *tmp)
{
    char cmd[2048], obj[600], dump[640];
    snprintf(obj, sizeof obj, "%s.o", tmp);
    snprintf(dump, sizeof dump, "%s.dump", tmp);
    const bool arm = CCR_ASM_IS_ARM(syn);
    if (syn == CCR_ASM_NASM)
        snprintf(cmd, sizeof cmd, "nasm -f elf64 -o '%s' '%s' 2>/dev/null", obj, path);
    else if (arm)   /* clang assembles for any target, no cross binutils needed */
        snprintf(cmd, sizeof cmd, "clang --target=%s -c -x assembler -o '%s' '%s' 2>/dev/null",
                 syn == CCR_ASM_ARM64 ? "aarch64-linux-gnu" : "armv7a-linux-gnueabihf", obj, path);
    else
        snprintf(cmd, sizeof cmd, "cc -c -x assembler -o '%s' '%s' 2>/dev/null", obj, path);
    if (system(cmd) != 0)
        return NULL;
    snprintf(cmd, sizeof cmd, "%s -s -d '%s' | tail -n +3 > '%s' 2>/dev/null", arm ? "llvm-objdump" : "objdump", obj,
             dump);
    char *out = NULL;
    size_t n;
    if (system(cmd) == 0)
        out = slurp(dump, &n);
    remove(obj);
    remove(dump);
    return out;
}

static void corpus_file(const char *path)
{
    size_t len = 0;
    char *text = slurp(path, &len);
    if (!text) {
        CHECK(0, "cannot read %s", path);
        return;
    }
    const ccr_asm_syntax syn = ccr_asm_syntax_of(path, text, len);
    ccr_ast a;
    ccr_ast_init(&a);
    const size_t pl = strlen(path);
    CHECK(ccr_asm_parse_text(text, len, "x", syn, pl > 2 && strcmp(path + pl - 2, ".S") == 0, &a),
          "%s does not read", path);
    char *t = gen(&a);
    CHECK(t && strlen(t) == len && memcmp(t, text, len) == 0, "%s: not the same saved untouched", path);
    free(t);
    for (uint32_t i = 0; i < a.node_count; ++i)
        if (N(&a, i)->kind == CCR_NODE_BLOCK)
            dirty(&a, i);
    t = gen(&a);
    CHECK(t != NULL, "%s: not saved rebuilt", path);
    if (t && (strlen(t) != len || memcmp(t, text, len) != 0)) {
        /* spaced differently is fine; it must still be the same program */
        char tmp[512], rebuilt[600];
        snprintf(tmp, sizeof tmp, "/tmp/ccratch_test_asm_%d", (int)getpid());
        snprintf(rebuilt, sizeof rebuilt, "%s%s", tmp, syn == CCR_ASM_NASM ? ".asm" : ".s");
        FILE *f = fopen(rebuilt, "w");
        if (f) {
            fputs(t, f);
            fclose(f);
        }
        char *want = assembled(path, syn, tmp);
        if (want) {
            char *got = assembled(rebuilt, syn, tmp);
            CHECK(got && strcmp(got, want) == 0, "%s: rebuilt, it assembles to something else (see %s)", path,
                  rebuilt);
            if (got && strcmp(got, want) == 0)
                remove(rebuilt);
            free(got);
            free(want);
        } else {
            remove(rebuilt);
        }
    }
    free(t);

    /* every instruction, label and jump printed anew from its parts (as the
     * palette's are): spaced the way Ccratch spaces, the same program */
    for (uint32_t i = 0; i < a.node_count; ++i)
        if ((N(&a, i)->flags & CCR_FLAG_ASM) &&
            (N(&a, i)->kind == CCR_NODE_CALL || N(&a, i)->kind == CCR_NODE_GOTO || N(&a, i)->kind == CCR_NODE_LABEL))
            a.nodes[i].flags |= CCR_FLAG_NEW;
    t = gen(&a);
    CHECK(t != NULL, "%s: not saved reprinted", path);
    if (t) {
        char tmp[512], again[600];
        snprintf(tmp, sizeof tmp, "/tmp/ccratch_test_asm_new_%d", (int)getpid());
        snprintf(again, sizeof again, "%s%s", tmp, syn == CCR_ASM_NASM ? ".asm" : ".s");
        FILE *f = fopen(again, "w");
        if (f) {
            fputs(t, f);
            fclose(f);
        }
        char *want = assembled(path, syn, tmp);
        if (want) {
            char *got = assembled(again, syn, tmp);
            CHECK(got && strcmp(got, want) == 0, "%s: reprinted, it assembles to something else (see %s)", path,
                  again);
            if (got && strcmp(got, want) == 0)
                remove(again);
            free(got);
            free(want);
        } else {
            remove(again);
        }
    }
    free(t);
    ccr_ast_free(&a);
    free(text);
}

/* clang's assembly for another machine (aarch64-linux-gnu...), -O0 and -O2. */
static void corpus_for_target(const char *target, const char *c_file)
{
    char out[512], cmd[2048];
    for (int o = 0; o < 2; ++o) {
        snprintf(out, sizeof out, "/tmp/ccratch_test_asm_%d_%s_%d.s", (int)getpid(), target, o);
        snprintf(cmd, sizeof cmd, "clang --target=%s -S -O%d -w -o '%s' '%s' 2>/dev/null", target, o * 2, out, c_file);
        if (system(cmd) != 0) {
            fprintf(stderr, "(no clang for %s: skipped)\n", target);
            return;
        }
        size_t len = 0;
        char *text = slurp(out, &len);
        CHECK(text && CCR_ASM_IS_ARM(ccr_asm_syntax_of(out, text, len)) == (strncmp(target, "x86", 3) != 0),
              "%s: %s taken for %s", out, target, text ? ccr_asm_syntax_name(ccr_asm_syntax_of(out, text, len)) : "?");
        free(text);
        corpus_file(out);
        remove(out);
    }
}

static void corpus_from_c(const char *c_file, const char *flags)
{
    char out[512], cmd[2048];
    for (int intel = 0; intel < 2; ++intel) {
        snprintf(out, sizeof out, "/tmp/ccratch_test_asm_%d_%d.s", (int)getpid(), intel);
        snprintf(cmd, sizeof cmd, "cc -S -O2 %s %s -o '%s' '%s' 2>/dev/null", intel ? "-masm=intel" : "",
                 flags, out, c_file);
        if (system(cmd) != 0) {
            fprintf(stderr, "(no cc -S for %s: skipped)\n", c_file);
            return;
        }
        corpus_file(out);
        remove(out);
    }
}

int main(int argc, char **argv)
{
    test_structure();
    test_nasm();
    test_arm();
    test_inline();
    const char *flags = "";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc) {
            flags = argv[++i];
        } else if (strcmp(argv[i], "--from-c") == 0 && i + 1 < argc) {
            corpus_from_c(argv[++i], flags);
        } else if (strcmp(argv[i], "--target") == 0 && i + 2 < argc) {
            corpus_for_target(argv[i + 1], argv[i + 2]);
            i += 2;
        } else {
            corpus_file(argv[i]);
        }
    }
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("assembly as blocks: ok");
    return 0;
}
