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
    X(MACRO_VALUE,      "MacroValue")         \
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
    X(DECL_GROUP,       "DeclGroup")          \
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
    /* CMake: foreach / while / function / macro / block ... end*() */ \
    X(SCOPE,            "Scope")              \
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
    X(COMPOUND_LIT,     "CompoundLiteral")    \
    X(DESIGNATOR,       "Designator")         \
    X(INT_LIT,          "Int")                \
    X(FLOAT_LIT,        "Float")              \
    X(CHAR_LIT,         "Char")               \
    X(STRING_LIT,       "String")             \
    /* code typed into a block from the palette, kept as text */ \
    X(TEXT,             "Text")               \
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
    ccr_str  aux;     /* return type of a Function; source text of
                         Other and type-only sizeof                 */

    uint32_t line;
    uint32_t column;
    uint32_t flags;

    /* Where the node is in ccr_ast.source, as byte offsets [b, e). For a
     * statement, e includes its ';'. ext_b/ext_e also take in comments
     * that belong to it (lines right above, rest of its line). Saving
     * copies these ranges, so text that was not changed stays as written. */
    uint32_t src_b, src_e;
    uint32_t ext_b, ext_e;

    /* The bytes of its parent's text this node stands in for: where it was
     * parsed, src, until an expression is moved into another slot. Saving
     * an edited parent copies the text around its children's places and
     * fills each place with that child's own text. */
    uint32_t place_b, place_e;

    /* Blocks only: indentation of the statements inside and of the
     * closing brace, as found in the source. */
    ccr_str  indent;
    ccr_str  close_indent;

    /* Statements: how much deeper than its block it was indented, so a
     * deliberate visual indent (rlBegin/rlEnd...) survives a rebuild. */
    ccr_str  extra_indent;
} ccr_node;

/* ccr_node.flags */
#define CCR_FLAG_DEFINITION  (1u << 0)   /* function/struct has a body */
#define CCR_FLAG_ANGLED      (1u << 1)   /* #include <...> */
#define CCR_FLAG_POSTFIX     (1u << 2)   /* x++ rather than ++x */
#define CCR_FLAG_ARROW       (1u << 3)   /* p->x rather than p.x */
#define CCR_FLAG_INIT        (1u << 4)   /* this expression initializes its Var */
#define CCR_FLAG_IMPLICIT    (1u << 5)   /* Block added around a braceless body */
#define CCR_FLAG_LOOSE       (1u << 6)   /* root Block of blocks lying on the
                                            canvas: not part of the program */
#define CCR_FLAG_DEAD        (1u << 7)   /* emptied loose root, ignore */
#define CCR_FLAG_DIRTY       (1u << 8)   /* Block whose statements were moved */
#define CCR_FLAG_UNSAFE_GAP  (1u << 9)   /* Block with something other than
                                            comments between its statements
                                            (#if, #define...): cannot be
                                            rebuilt safely */
#define CCR_FLAG_COMMENTED   (1u << 12)  /* statement switched off: drawn grey,
                                            saved as // lines */
#define CCR_FLAG_NEW         (1u << 11)  /* made in the editor (palette): has
                                            no source text, saving prints it */
#define CCR_FLAG_BLANK_BEFORE (1u << 10) /* statement had an empty line above
                                            it: the paragraph break moves
                                            with it */
#define CCR_FLAG_REPORTER    (1u << 13)  /* loose root holding one expression
                                            (a Scratch reporter), not a stack */
#define CCR_FLAG_MOVED       (1u << 14)  /* expression dropped into another
                                            slot: its place is not its src */
#define CCR_FLAG_WORD_NAME   (1u << 15)  /* Text holding its parent's name (a
                                            new block's variable, function,
                                            label, field): no reporter goes in */
#define CCR_FLAG_WORD_TYPE   (1u << 16)  /* the same for its parent's type */
#define CCR_FLAG_WORD        (CCR_FLAG_WORD_NAME | CCR_FLAG_WORD_TYPE)
#define CCR_FLAG_COLLAPSED   (1u << 18)  /* folded to one line on the canvas:
                                            a view, not an edit (undo keeps it) */
#define CCR_FLAG_ELSEIF      (1u << 19)  /* CMake: an If that is the elseif()
                                            of the If around it */
#define CCR_FLAG_TOPLEVEL    (1u << 17)  /* loose root holding one declaration
                                            for the file itself (a function,
                                            a struct, an #include...) */

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

/* A node's place in the tree, for undo. */
typedef struct ccr_node_links {
    ccr_node_id id;
    ccr_node_id parent, first_child, last_child, next_sibling;
    uint32_t    flags;
    ccr_str     name;   /* text typed into a palette block's slot */
    ccr_str     type;   /* and a new declaration's type */
    ccr_str     op;     /* an operator picked from the dropdown */
    uint32_t    place_b, place_e;
} ccr_node_links;

/*
 * While a journal is attached to a ccr_ast, every edit records the old
 * links and flags of each node it touches, once per node. That is all
 * undo needs: put those back.
 */
typedef struct ccr_journal {
    ccr_node_links *old;
    uint32_t        count, cap;
    bool            oom;
} ccr_journal;

/* What the tree was read from: saving and drawing speak its language. */
typedef enum ccr_lang_kind { CCR_LANG_C, CCR_LANG_CMAKE } ccr_lang_kind;

typedef struct ccr_ast {
    ccr_lang_kind lang;
    ccr_node *nodes;
    uint32_t  node_count, node_cap;

    char     *strings;
    uint32_t  str_len, str_cap;

    ccr_diag *diags;
    uint32_t  diag_count, diag_cap;

    ccr_str   source_path;

    /* The file as it was parsed. Saving splices from this. */
    char     *source;
    uint32_t  source_len;

    ccr_journal *journal;   /* NULL unless an undoable edit is in progress */
} ccr_ast;

bool        ccr_ast_init(ccr_ast *ast);
void        ccr_ast_free(ccr_ast *ast);

/* Appends a node as the last child of `parent` (CCR_NONE for a root). */
ccr_node_id ccr_ast_add(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent);
ccr_str     ccr_ast_intern(ccr_ast *ast, const char *s);

/* Moving nodes around. A node is detached before it is attached again,
 * so these can move nodes between parents. A node with parent CCR_NONE
 * is a root. */
void        ccr_ast_unlink(ccr_ast *ast, ccr_node_id id);

/* Call before changing a node's flags by hand, so undo can restore them.
 * The edit functions here call it themselves. */
void        ccr_ast_touch(ccr_ast *ast, ccr_node_id id);
ccr_node_links ccr_ast_links(const ccr_ast *ast, ccr_node_id id);
void        ccr_ast_set_links(ccr_ast *ast, const ccr_node_links *l);
void        ccr_ast_append(ccr_ast *ast, ccr_node_id parent, ccr_node_id id);
void        ccr_ast_insert_first(ccr_ast *ast, ccr_node_id parent, ccr_node_id id);
void        ccr_ast_insert_after(ccr_ast *ast, ccr_node_id ref, ccr_node_id id);

/* `if (x) y;` -> `if (x) { y; }`, same for else, while, for, do: every
 * body becomes a Block, so it always has a stack to drop blocks into. */
void        ccr_ast_wrap_bodies(ccr_ast *ast);

/* Needs ast->source and the src ranges from the parser: gives statements
 * their ';' and comments, blocks their indents, and flags blocks that
 * cannot be rebuilt safely. Call after ccr_ast_wrap_bodies. */
void        ccr_ast_finish_ranges(ccr_ast *ast);
const char *ccr_ast_str(const ccr_ast *ast, ccr_str s);
bool        ccr_ast_add_diag(ccr_ast *ast, ccr_diag d);

uint32_t    ccr_ast_count_diags(const ccr_ast *ast, ccr_diag_severity at_least);
const char *ccr_node_kind_name(ccr_node_kind kind);

/* Pretty tree, then diagnostics. */
void        ccr_ast_dump(const ccr_ast *ast, FILE *out);
/* Diagnostics only, one "file:line:col: severity: message" per line. */
void        ccr_ast_dump_diags(const ccr_ast *ast, FILE *out);

#endif /* CCR_AST_H */
