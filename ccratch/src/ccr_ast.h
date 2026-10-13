/*
 * ccr_ast - Ccratch's own syntax tree.
 *
 * This is what blocks will be built from. It knows nothing about libclang:
 * the parser fills it, and everything else (canvas, codegen, plugins) only
 * ever sees this.
 *
 * Data-oriented on purpose: all nodes live in one flat array and refer to
 * each other by index, all strings live in one arena. No per-node malloc,
 * no pointer chasing, trivially serializable.
 *
 * WARNING: adding nodes or strings may reallocate. Never hold a ccr_node*
 * or a string pointer across ccr_ast_add / ccr_ast_intern - hold ids.
 */
#ifndef CCR_AST_H
#define CCR_AST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CCR_NONE UINT32_MAX

typedef uint32_t ccr_node_id;
typedef uint32_t ccr_str;   /* offset into the string arena; 0 is "" */

/* X-macro: kind, printable name. Order defines the enum values. */
#define CCR_NODE_KINDS(X)                     \
    X(TRANSLATION_UNIT, "TranslationUnit")    \
    /* preprocessor */                        \
    X(INCLUDE,          "Include")            \
    X(MACRO_DEFINE,     "MacroDefine")        \
    X(MACRO_USE,        "MacroUse")           \
    /* declarations */                        \
    X(FUNCTION,         "Function")           \
    X(PARAM,            "Param")              \
    X(VAR,              "Var")                \
    X(STRUCT,           "Struct")             \
    X(UNION,            "Union")              \
    X(FIELD,            "Field")              \
    X(TYPEDEF,          "Typedef")            \
    X(ENUM,             "Enum")               \
    X(ENUM_CONST,       "EnumConst")          \
    /* statements */                          \
    X(BLOCK,            "Block")              \
    X(IF,               "If")                 \
    X(WHILE,            "While")              \
    X(DO_WHILE,         "DoWhile")            \
    X(FOR,              "For")                \
    X(SWITCH,           "Switch")             \
    X(CASE,             "Case")               \
    X(DEFAULT,          "Default")            \
    X(BREAK,            "Break")              \
    X(CONTINUE,         "Continue")           \
    X(RETURN,           "Return")             \
    X(GOTO,             "Goto")               \
    X(LABEL,            "Label")              \
    X(EMPTY,            "Empty")              \
    /* expressions */                         \
    X(CALL,             "Call")               \
    X(BINARY_OP,        "BinaryOp")           \
    X(UNARY_OP,         "UnaryOp")            \
    X(TERNARY,          "Ternary")            \
    X(REF,              "Ref")                \
    X(MEMBER,           "Member")             \
    X(INDEX,            "Index")              \
    X(CAST,             "Cast")               \
    X(SIZEOF,           "Sizeof")             \
    X(INIT_LIST,        "InitList")           \
    X(INT_LIT,          "Int")                \
    X(FLOAT_LIT,        "Float")              \
    X(CHAR_LIT,         "Char")               \
    X(STRING_LIT,       "String")             \
    /* anything we do not model yet ("here be dragons") */ \
    X(OTHER,            "Other")

typedef enum ccr_node_kind {
#define X(k, name) CCR_NODE_##k,
    CCR_NODE_KINDS(X)
#undef X
    CCR_NODE_KIND_COUNT
} ccr_node_kind;

typedef struct ccr_node {
    ccr_node_kind kind;
    ccr_node_id   parent;
    ccr_node_id   first_child;
    ccr_node_id   last_child;
    ccr_node_id   next_sibling;

    ccr_str  name;    /* identifier, literal text, include path...  */
    ccr_str  type;    /* C type spelling, when it means something   */
    ccr_str  op;      /* operator for BinaryOp/UnaryOp              */
    ccr_str  raw;     /* original clang kind, for OTHER nodes       */

    uint32_t line;
    uint32_t column;
    uint32_t flags;
} ccr_node;

/* ccr_node.flags */
#define CCR_FLAG_DEFINITION  (1u << 0)   /* function/struct has a body */
#define CCR_FLAG_ANGLED      (1u << 1)   /* #include <...> */
#define CCR_FLAG_POSTFIX     (1u << 2)   /* x++ rather than ++x */

typedef enum ccr_diag_severity {
    CCR_DIAG_NOTE,
    CCR_DIAG_WARNING,
    CCR_DIAG_ERROR,
    CCR_DIAG_FATAL
} ccr_diag_severity;

typedef struct ccr_diag {
    ccr_diag_severity severity;
    ccr_str  file;
    uint32_t line;
    uint32_t column;
    ccr_str  message;
} ccr_diag;

typedef struct ccr_ast {
    ccr_node *nodes;
    uint32_t  node_count, node_cap;

    char     *strings;
    uint32_t  str_len, str_cap;

    ccr_diag *diags;
    uint32_t  diag_count, diag_cap;

    ccr_str   source_path;
} ccr_ast;

bool        ccr_ast_init(ccr_ast *ast);
void        ccr_ast_free(ccr_ast *ast);

/* Appends a node as the last child of `parent` (CCR_NONE for a root). */
ccr_node_id ccr_ast_add(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent);
ccr_str     ccr_ast_intern(ccr_ast *ast, const char *s);
const char *ccr_ast_str(const ccr_ast *ast, ccr_str s);
bool        ccr_ast_add_diag(ccr_ast *ast, ccr_diag d);

uint32_t    ccr_ast_count_diags(const ccr_ast *ast, ccr_diag_severity at_least);
const char *ccr_node_kind_name(ccr_node_kind kind);

/* Pretty tree, then diagnostics. */
void        ccr_ast_dump(const ccr_ast *ast, FILE *out);

#endif /* CCR_AST_H */
