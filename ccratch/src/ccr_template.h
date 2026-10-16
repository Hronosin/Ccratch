/*
 * ccr_template - what the palette offers, as syntax trees.
 *
 * A template builds real nodes (an If with a condition slot and a body, a
 * BinaryOp '+' with two slots...), flagged CCR_FLAG_NEW: they have no
 * source text, saving prints them. Slots are empty Text nodes; reporters
 * drop into them, or text is typed in. Names and types a block needs (the
 * `x` of `int x = ...`, the `f` of `f(...)`) are Text children flagged
 * CCR_FLAG_WORD_*: typed into like any field, kept in their parent's
 * name/type, and nothing can be dropped into them.
 *
 * Knows nothing about the editor or the window, so the palette and the
 * editor build the very same trees.
 */
#ifndef CCR_TEMPLATE_H
#define CCR_TEMPLATE_H

#include "ccr_ast.h"

/* Palette sections, top to bottom. */
typedef enum ccr_tcat {
    CCR_TCAT_CONTROL,
    CCR_TCAT_OPERATORS,
    CCR_TCAT_VARIABLES,
    CCR_TCAT_POINTERS,
    CCR_TCAT_FUNCTIONS,
    CCR_TCAT_TYPES,
    CCR_TCAT_DECLS,          /* the file's own: functions, structs, #include */
    CCR_TCAT_COUNT
} ccr_tcat;

/* X(ID, category, label) - the label is for tests and logs; the palette
 * shows the block itself. */
#define CCR_TEMPLATES(X)                                   \
    X(CODE,        FUNCTIONS, "code line")                 \
    X(IF,          CONTROL,   "if")                        \
    X(IF_ELSE,     CONTROL,   "if / else")                 \
    X(WHILE,       CONTROL,   "while")                     \
    X(FOR,         CONTROL,   "for")                       \
    X(DO_WHILE,    CONTROL,   "do / while")                \
    X(RETURN,      CONTROL,   "return")                    \
    X(BREAK,       CONTROL,   "break")                     \
    X(CONTINUE,    CONTROL,   "continue")                  \
    X(SWITCH,      CONTROL,   "switch")                    \
    X(CASE,        CONTROL,   "case")                      \
    X(DEFAULT,     CONTROL,   "default")                   \
    X(GOTO,        CONTROL,   "goto")                      \
    X(LABEL,       CONTROL,   "label")                     \
    X(ADD,         OPERATORS, "a + b")                     \
    X(SUB,         OPERATORS, "a - b")                     \
    X(MUL,         OPERATORS, "a * b")                     \
    X(DIV,         OPERATORS, "a / b")                     \
    X(MOD,         OPERATORS, "a % b")                     \
    X(LT,          OPERATORS, "a < b")                     \
    X(GT,          OPERATORS, "a > b")                     \
    X(LE,          OPERATORS, "a <= b")                    \
    X(GE,          OPERATORS, "a >= b")                    \
    X(EQ,          OPERATORS, "a == b")                    \
    X(NE,          OPERATORS, "a != b")                    \
    X(AND,         OPERATORS, "a && b")                    \
    X(OR,          OPERATORS, "a || b")                    \
    X(NOT,         OPERATORS, "!a")                        \
    X(BIT_AND,     OPERATORS, "a & b")                     \
    X(BIT_OR,      OPERATORS, "a | b")                     \
    X(BIT_XOR,     OPERATORS, "a ^ b")                     \
    X(BIT_NOT,     OPERATORS, "~a")                        \
    X(SHL,         OPERATORS, "a << b")                    \
    X(SHR,         OPERATORS, "a >> b")                    \
    X(TERNARY,     OPERATORS, "c ? a : b")                 \
    X(DECL_INIT,   VARIABLES, "int x = v")                 \
    X(DECL,        VARIABLES, "int x")                     \
    X(ASSIGN,      VARIABLES, "a = b")                     \
    X(ADD_ASSIGN,  VARIABLES, "a += b")                    \
    X(SUB_ASSIGN,  VARIABLES, "a -= b")                    \
    X(MUL_ASSIGN,  VARIABLES, "a *= b")                    \
    X(INC,         VARIABLES, "a++")                       \
    X(DEC,         VARIABLES, "a--")                       \
    X(ADDR,        POINTERS,  "&a")                        \
    X(DEREF,       POINTERS,  "*p")                        \
    X(INDEX,       POINTERS,  "a[i]")                      \
    X(MEMBER,      POINTERS,  "s.f")                       \
    X(ARROW,       POINTERS,  "p->f")                      \
    X(NULL_PTR,    POINTERS,  "NULL")                      \
    X(CALL_STMT,   FUNCTIONS, "f(...);")                   \
    X(CALL_EXPR,   FUNCTIONS, "f(...)")                    \
    X(PRINTF,      FUNCTIONS, "printf")                    \
    X(MALLOC,      FUNCTIONS, "malloc")                    \
    X(FREE,        FUNCTIONS, "free")                      \
    X(CAST,        TYPES,     "(T) a")                     \
    X(SIZEOF_TYPE, TYPES,     "sizeof(T)")                 \
    X(SIZEOF_EXPR, TYPES,     "sizeof a")                  \
    X(FUNC_DEF,    DECLS,     "int f(void) { }")           \
    X(FUNC_DECL,   DECLS,     "int f(void);")              \
    X(STRUCT_DEF,  DECLS,     "struct s { };")             \
    X(ENUM_DEF,    DECLS,     "enum e { A, B };")          \
    X(TYPEDEF,     DECLS,     "typedef T name;")           \
    X(GLOBAL_VAR,  DECLS,     "int x = 0; (global)")       \
    X(INCLUDE_SYS, DECLS,     "#include <...>")            \
    X(INCLUDE_LOC, DECLS,     "#include \"...\"")          \
    X(DEFINE,      DECLS,     "#define NAME value")

typedef enum ccr_template {
#define X(id, cat, label) CCR_TEMPLATE_##id,
    CCR_TEMPLATES(X)
#undef X
    CCR_TEMPLATE_COUNT
} ccr_template;

const char *ccr_template_label(ccr_template t);
ccr_tcat    ccr_template_category(ccr_template t);

/* An expression (a reporter: goes into slots) rather than a statement. */
bool        ccr_template_is_reporter(ccr_template t);
/* A declaration for the file itself, not a statement in a function: it
 * goes between the file's other declarations. */
bool        ccr_template_is_toplevel(ccr_template t);

/* Builds template `t` as the last child of `parent` (a Block: a stack or a
 * reporter's root). Returns the node made, or CCR_NONE if out of memory. */
ccr_node_id ccr_template_build(ccr_ast *ast, ccr_template t, ccr_node_id parent);

/* The operators `op` can be swapped for, itself included (+ for - * / %,
 * == for != < > && ||...); NULL if it is not one to swap (&x, *p). */
const char *const *ccr_op_choices(const char *op, bool unary, int *count);

/* A new node flagged CCR_FLAG_NEW (Blocks also DIRTY, with a 4-space
 * indent: printed with relative indents). */
ccr_node_id ccr_template_node(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent);
/* A new Text with `text` in it. */
ccr_node_id ccr_template_text(ccr_ast *ast, ccr_node_id parent, const char *text);

#endif /* CCR_TEMPLATE_H */
