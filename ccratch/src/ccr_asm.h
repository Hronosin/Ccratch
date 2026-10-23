/*
 * ccr_asm - assembly as blocks: .s/.S/.asm files, and asm() in C.
 *
 * An assembly file is read into the same ccr_ast C is read into, with
 * ast.lang = CCR_LANG_ASM:
 *
 *     TranslationUnit
 *       Function "hello.s"     a hat for what comes before anything else
 *       Function ".rodata"     a hat per section (.data, section .bss...)
 *       Function "main"        a hat per global label (.globl main / global
 *         Block                main): the code that runs from there
 *           Call "mov"         an instruction or a directive; each operand a
 *             Text "rax"       Text slot (`msg db "hi", 0`: the label goes in
 *             Text "1"         the Call's type, `x = 1` too)
 *           Label ".L2"        a label of its own (not a hat)
 *           Goto ".L2"         jne .L2: a jump to a label of this file (its
 *                              mnemonic in op), drawn with its arrow
 *           Scope ".macro"     .macro/.rept/%macro/%rep ... the end of it
 *           If ".if"           .if/%if ... .else ... .endif
 *
 * Every statement is flagged CCR_FLAG_ASM: blocks are drawn and saved as
 * assembly wherever they are, so one dragged into C becomes
 * __asm__ volatile("...");.
 *
 * In C, asm("..." : out : in : clobbers) is an Asm node: its first child a
 * Block of the instructions read out of the string, then the operands'
 * expressions. Saving an edited one writes the string again.
 *
 * Saving copies text that was not touched, like C: comments, spacing,
 * everything.
 */
#ifndef CCR_ASM_H
#define CCR_ASM_H

#include "ccr_ast.h"

#include <stdbool.h>
#include <stddef.h>

/* .s, .S (with the C preprocessor), .asm and .nasm (NASM). */
bool ccr_asm_is_asm(const char *path);
/* NASM for .asm/.nasm; GNU as otherwise, Intel after .intel_syntax. */
ccr_asm_syntax ccr_asm_syntax_of(const char *path, const char *text, size_t len);
const char *ccr_asm_syntax_name(ccr_asm_syntax s);   /* "att", "intel", "nasm" */

bool ccr_asm_parse_file(const char *path, ccr_ast *out);
/* `name` is what the first hat says; `cpp`: #include and friends are the
 * C preprocessor's (a .S file), not comments. */
bool ccr_asm_parse_text(const char *text, size_t len, const char *name, ccr_asm_syntax syntax, bool cpp,
                        ccr_ast *out);

/* The statements of ast->source[b, e) (assembly), appended to `block`.
 * False if out of memory. */
bool ccr_asm_parse_into(ccr_ast *ast, ccr_node_id block, uint32_t b, uint32_t e, ccr_asm_syntax syntax);

/* A block's colour (a ccr_theme_cat) for a mnemonic or directive. */
int  ccr_asm_category(const char *mnemonic);
/* jmp, ret, ud2...: nothing runs after it (no tab under the block). */
bool ccr_asm_ends_flow(const char *mnemonic);
/* jmp, jne, loop...: jumps to its one operand. */
bool ccr_asm_is_jump(const char *mnemonic);
/* "# " for GNU as, "; " for NASM. */
const char *ccr_asm_comment(ccr_asm_syntax s);
/* .endm for .macro, %endif for %if... */
const char *ccr_asm_end_word(const char *head);
/* .else, %else for an If's head */
const char *ccr_asm_else_word(const char *head);

/* A new statement (flagged NEW and ASM) from one line of assembly:
 * "mov rax, 1", "msg db 'hi'", "#include <x.h>". Appended to `parent`. */
ccr_node_id ccr_asm_build(ccr_ast *ast, ccr_node_id parent, const char *line, ccr_asm_syntax syntax);
/* .macro / .rept / .if ...: `head` the directive, `args` its slot. */
ccr_node_id ccr_asm_build_scope(ccr_ast *ast, ccr_node_id parent, const char *head, const char *args,
                                bool with_else);
/* A new jump (jne + a label to type) or label. */
ccr_node_id ccr_asm_build_jump(ccr_ast *ast, ccr_node_id parent, const char *mnemonic);
ccr_node_id ccr_asm_build_label(ccr_ast *ast, ccr_node_id parent);
/* A new asm volatile("") for C, with slots for outputs, inputs, clobbers. */
ccr_node_id ccr_asm_build_inline(ccr_ast *ast, ccr_node_id parent);

/* ------------------------------------------------------- asm() in C */

/* Where the parts of `asm volatile ("..." : "=r"(x) : "r"(y) : "cc")` are,
 * as offsets into its text. */
#define CCR_ASM_MAX_OPERANDS 30
typedef struct ccr_asm_operand {
    uint32_t cb, ce;   /* the constraint, quotes and all ("=r", [out] "=r") */
    uint32_t eb, ee;   /* the expression in its ( ) */
} ccr_asm_operand;

typedef struct ccr_asm_parts {
    uint32_t keyword_e;          /* asm / __asm__ */
    uint32_t quals_b, quals_e;   /* volatile, goto, inline */
    uint32_t tmpl_b, tmpl_e;     /* the string(s), quotes and all */
    int      sections;           /* how many ':' parts follow (0..4) */
    uint32_t sec_b[4], sec_e[4]; /* each, after its ':' */
    ccr_asm_operand ops[CCR_ASM_MAX_OPERANDS];
    int      op_section[CCR_ASM_MAX_OPERANDS];   /* 0 outputs, 1 inputs */
    int      op_count;
} ccr_asm_parts;

/* False if it is not an asm statement Ccratch can take apart. */
bool ccr_asm_parts_of(const char *text, uint32_t len, ccr_asm_parts *out);

/* The string(s) of a C asm statement decoded ("mov %1, %0\n\t..." with
 * real newlines). Returns the length written, or -1 if it does not fit
 * or is not a string Ccratch can read. */
int  ccr_asm_decode(const char *text, uint32_t b, uint32_t e, char *out, size_t cap);
/* And back: one line of assembly as the inside of a C string. */
void ccr_asm_escape(const char *line, size_t n, char *out, size_t cap);

/* After clang: every Asm node gets its Block of instructions. */
void ccr_asm_split_inline(ccr_ast *ast);

#endif /* CCR_ASM_H */
