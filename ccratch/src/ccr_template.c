#include "ccr_template.h"
#include "ccr_lib.h"

#include <string.h>

static const char *const g_labels[CCR_TEMPLATE_COUNT] = {
#define X(id, cat, label) label,
    CCR_TEMPLATES(X)
#undef X
};

static const ccr_tcat g_cats[CCR_TEMPLATE_COUNT] = {
#define X(id, cat, label) CCR_TCAT_##cat,
    CCR_TEMPLATES(X)
#undef X
};

int ccr_template_total(void)
{
    return CCR_TEMPLATE_COUNT + ccr_lib_block_count();
}

/* The pack block a template stands for, or NULL. */
static const ccr_lib_block *lib_block(ccr_template t)
{
    return (int)t >= CCR_TEMPLATE_COUNT ? ccr_lib_block_at((int)t - CCR_TEMPLATE_COUNT) : NULL;
}

const char *ccr_template_label(ccr_template t)
{
    if (lib_block(t))
        return lib_block(t)->call;
    return (unsigned)t < CCR_TEMPLATE_COUNT ? g_labels[t] : "?";
}

ccr_tcat ccr_template_category(ccr_template t)
{
    return (unsigned)t < CCR_TEMPLATE_COUNT ? g_cats[t] : CCR_TCAT_COUNT;   /* a pack's */
}

ccr_node_id ccr_template_node(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent)
{
    ccr_node_id id = ccr_ast_add(ast, kind, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ast->nodes[id].flags |= CCR_FLAG_NEW;
    if (kind == CCR_NODE_BLOCK) {
        /* printed with relative indents; the block it lands in shifts it */
        ast->nodes[id].indent = ccr_ast_intern(ast, "    ");
        ast->nodes[id].flags |= CCR_FLAG_DIRTY;
    }
    if (parent != CCR_NONE)
        ccr_ast_append(ast, parent, id);
    return id;
}

ccr_node_id ccr_template_text(ccr_ast *ast, ccr_node_id parent, const char *text)
{
    ccr_node_id id = ccr_template_node(ast, CCR_NODE_TEXT, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ast->nodes[id].name = ccr_ast_intern(ast, text);
    if (parent != CCR_NONE)
        ccr_ast_append(ast, parent, id);
    return id;
}

/* A name or type field: the text lives in the parent too. */
static void word(ccr_ast *ast, ccr_node_id parent, uint32_t which, const char *text)
{
    ccr_node_id w = ccr_template_text(ast, parent, text);
    if (w == CCR_NONE || parent == CCR_NONE)
        return;
    ast->nodes[w].flags |= which;
    if (which == CCR_FLAG_WORD_NAME)
        ast->nodes[parent].name = ast->nodes[w].name;
    else
        ast->nodes[parent].type = ast->nodes[w].name;
}

static ccr_node_id op_node(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent, const char *op,
                           int slots)
{
    ccr_node_id n = ccr_template_node(ast, kind, parent);
    if (n == CCR_NONE)
        return CCR_NONE;
    ast->nodes[n].op = ccr_ast_intern(ast, op);
    for (int i = 0; i < slots; ++i)
        ccr_template_text(ast, n, "");
    return n;
}

static ccr_node_id call(ccr_ast *ast, ccr_node_id parent, const char *name, const char *args)
{
    ccr_node_id n = ccr_template_node(ast, CCR_NODE_CALL, parent);
    if (n == CCR_NONE)
        return CCR_NONE;
    word(ast, n, CCR_FLAG_WORD_NAME, name);
    ccr_template_text(ast, n, args);
    return n;
}

const char *const *ccr_op_choices(const char *op, bool unary, int *count)
{
    static const char *const arith[]  = { "+", "-", "*", "/", "%" };
    static const char *const test[]   = { "==", "!=", "<", ">", "<=", ">=", "&&", "||" };
    static const char *const bits[]   = { "&", "|", "^", "<<", ">>" };
    static const char *const assign[] = { "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                                          "<<=", ">>=" };
    static const char *const prefix[] = { "!", "~", "-", "+" };
    static const char *const step[]   = { "++", "--" };
    static const struct { const char *const *v; int n; bool unary; } groups[] = {
        { arith, 5, false }, { test, 8, false }, { bits, 5, false }, { assign, 11, false },
        { prefix, 4, true }, { step, 2, true },
    };
    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; ++g) {
        if (groups[g].unary != unary)
            continue;
        for (int i = 0; i < groups[g].n; ++i)
            if (strcmp(groups[g].v[i], op) == 0) {
                *count = groups[g].n;
                return groups[g].v;
            }
    }
    *count = 0;
    return NULL;
}

bool ccr_template_is_toplevel(ccr_template t)
{
    return ccr_template_category(t) == CCR_TCAT_DECLS;
}

bool ccr_template_is_reporter(ccr_template t)
{
    if (lib_block(t))
        return strcmp(lib_block(t)->returns, "void") != 0;
    switch (t) {
    case CCR_TEMPLATE_ADD: case CCR_TEMPLATE_SUB: case CCR_TEMPLATE_MUL:
    case CCR_TEMPLATE_DIV: case CCR_TEMPLATE_MOD:
    case CCR_TEMPLATE_LT: case CCR_TEMPLATE_GT: case CCR_TEMPLATE_LE:
    case CCR_TEMPLATE_GE: case CCR_TEMPLATE_EQ: case CCR_TEMPLATE_NE:
    case CCR_TEMPLATE_AND: case CCR_TEMPLATE_OR: case CCR_TEMPLATE_NOT:
    case CCR_TEMPLATE_BIT_AND: case CCR_TEMPLATE_BIT_OR: case CCR_TEMPLATE_BIT_XOR:
    case CCR_TEMPLATE_BIT_NOT: case CCR_TEMPLATE_SHL: case CCR_TEMPLATE_SHR:
    case CCR_TEMPLATE_TERNARY:
    case CCR_TEMPLATE_ADDR: case CCR_TEMPLATE_DEREF: case CCR_TEMPLATE_INDEX:
    case CCR_TEMPLATE_MEMBER: case CCR_TEMPLATE_ARROW: case CCR_TEMPLATE_NULL_PTR:
    case CCR_TEMPLATE_CALL_EXPR: case CCR_TEMPLATE_MALLOC:
    case CCR_TEMPLATE_CAST: case CCR_TEMPLATE_SIZEOF_TYPE: case CCR_TEMPLATE_SIZEOF_EXPR:
        return true;
    default:
        return false;
    }
}

ccr_node_id ccr_template_build(ccr_ast *ast, ccr_template t, ccr_node_id parent)
{
    ccr_node_id s = CCR_NONE;
    const ccr_lib_block *lb = lib_block(t);
    if (lb) {
        /* a library call: its name fixed, its arguments slots with defaults */
        s = ccr_template_node(ast, CCR_NODE_CALL, parent);
        if (s == CCR_NONE)
            return CCR_NONE;
        ast->nodes[s].name = ccr_ast_intern(ast, lb->call);
        for (int i = 0; i < lb->nparams; ++i)
            ccr_template_text(ast, s, lb->defaults[i]);
        return s;
    }
    switch (t) {
    case CCR_TEMPLATE_CODE:
        s = ccr_template_text(ast, parent, "");
        break;
    case CCR_TEMPLATE_IF:
    case CCR_TEMPLATE_IF_ELSE:
        s = ccr_template_node(ast, CCR_NODE_IF, parent);
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        if (t == CCR_TEMPLATE_IF_ELSE)
            ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_WHILE:
    case CCR_TEMPLATE_SWITCH:
        s = ccr_template_node(ast, t == CCR_TEMPLATE_WHILE ? CCR_NODE_WHILE : CCR_NODE_SWITCH,
                              parent);
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_FOR:
        s = ccr_template_node(ast, CCR_NODE_FOR, parent);
        ccr_template_text(ast, s, "int i = 0");
        ccr_template_text(ast, s, "i < n");
        ccr_template_text(ast, s, "i++");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_DO_WHILE:
        s = ccr_template_node(ast, CCR_NODE_DO_WHILE, parent);
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_RETURN:
        s = ccr_template_node(ast, CCR_NODE_RETURN, parent);
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_BREAK:
        s = ccr_template_node(ast, CCR_NODE_BREAK, parent);
        break;
    case CCR_TEMPLATE_CONTINUE:
        s = ccr_template_node(ast, CCR_NODE_CONTINUE, parent);
        break;
    case CCR_TEMPLATE_CASE:
        s = ccr_template_node(ast, CCR_NODE_CASE, parent);
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_DEFAULT:
        s = ccr_template_node(ast, CCR_NODE_DEFAULT, parent);
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_GOTO:
    case CCR_TEMPLATE_LABEL:
        /* the same name in both, so a fresh pair is already an arrow */
        s = ccr_template_node(ast, t == CCR_TEMPLATE_GOTO ? CCR_NODE_GOTO : CCR_NODE_LABEL, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "done");
        break;

    case CCR_TEMPLATE_ADD:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "+", 2);  break;
    case CCR_TEMPLATE_SUB:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "-", 2);  break;
    case CCR_TEMPLATE_MUL:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "*", 2);  break;
    case CCR_TEMPLATE_DIV:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "/", 2);  break;
    case CCR_TEMPLATE_MOD:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "%", 2);  break;
    case CCR_TEMPLATE_LT:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "<", 2);  break;
    case CCR_TEMPLATE_GT:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, ">", 2);  break;
    case CCR_TEMPLATE_LE:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "<=", 2); break;
    case CCR_TEMPLATE_GE:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, ">=", 2); break;
    case CCR_TEMPLATE_EQ:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "==", 2); break;
    case CCR_TEMPLATE_NE:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "!=", 2); break;
    case CCR_TEMPLATE_AND:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "&&", 2); break;
    case CCR_TEMPLATE_OR:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "||", 2); break;
    case CCR_TEMPLATE_NOT:     s = op_node(ast, CCR_NODE_UNARY_OP, parent, "!", 1);   break;
    case CCR_TEMPLATE_BIT_AND: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "&", 2);  break;
    case CCR_TEMPLATE_BIT_OR:  s = op_node(ast, CCR_NODE_BINARY_OP, parent, "|", 2);  break;
    case CCR_TEMPLATE_BIT_XOR: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "^", 2);  break;
    case CCR_TEMPLATE_BIT_NOT: s = op_node(ast, CCR_NODE_UNARY_OP, parent, "~", 1);   break;
    case CCR_TEMPLATE_SHL:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "<<", 2); break;
    case CCR_TEMPLATE_SHR:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, ">>", 2); break;
    case CCR_TEMPLATE_TERNARY: s = op_node(ast, CCR_NODE_TERNARY, parent, "", 3);     break;

    case CCR_TEMPLATE_DECL_INIT:
    case CCR_TEMPLATE_DECL:
        s = ccr_template_node(ast, CCR_NODE_VAR, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        word(ast, s, CCR_FLAG_WORD_NAME, "x");
        if (t == CCR_TEMPLATE_DECL_INIT) {
            ccr_node_id v = ccr_template_text(ast, s, "0");
            if (v != CCR_NONE)
                ast->nodes[v].flags |= CCR_FLAG_INIT;
        }
        break;
    case CCR_TEMPLATE_ASSIGN:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "=", 2);  break;
    case CCR_TEMPLATE_ADD_ASSIGN: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "+=", 2); break;
    case CCR_TEMPLATE_SUB_ASSIGN: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "-=", 2); break;
    case CCR_TEMPLATE_MUL_ASSIGN: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "*=", 2); break;
    case CCR_TEMPLATE_INC:
    case CCR_TEMPLATE_DEC:
        s = op_node(ast, CCR_NODE_UNARY_OP, parent, t == CCR_TEMPLATE_INC ? "++" : "--", 1);
        if (s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_POSTFIX;
        break;

    case CCR_TEMPLATE_ADDR:  s = op_node(ast, CCR_NODE_UNARY_OP, parent, "&", 1); break;
    case CCR_TEMPLATE_DEREF: s = op_node(ast, CCR_NODE_UNARY_OP, parent, "*", 1); break;
    case CCR_TEMPLATE_INDEX: s = op_node(ast, CCR_NODE_INDEX, parent, "", 2);     break;
    case CCR_TEMPLATE_MEMBER:
    case CCR_TEMPLATE_ARROW:
        s = ccr_template_node(ast, CCR_NODE_MEMBER, parent);
        ccr_template_text(ast, s, "");
        word(ast, s, CCR_FLAG_WORD_NAME, "field");
        if (t == CCR_TEMPLATE_ARROW && s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_ARROW;
        break;
    case CCR_TEMPLATE_NULL_PTR:
        s = ccr_template_node(ast, CCR_NODE_REF, parent);
        if (s != CCR_NONE)
            ast->nodes[s].name = ccr_ast_intern(ast, "NULL");
        break;

    case CCR_TEMPLATE_CALL_STMT:
    case CCR_TEMPLATE_CALL_EXPR: s = call(ast, parent, "f", "");                       break;
    case CCR_TEMPLATE_PRINTF:    s = call(ast, parent, "printf", "\"Hello, world!\\n\""); break;
    case CCR_TEMPLATE_MALLOC:    s = call(ast, parent, "malloc", "");                  break;
    case CCR_TEMPLATE_FREE:      s = call(ast, parent, "free", "");                    break;

    case CCR_TEMPLATE_CAST:
        s = ccr_template_node(ast, CCR_NODE_CAST, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_SIZEOF_TYPE:
        s = ccr_template_node(ast, CCR_NODE_SIZEOF, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        break;
    case CCR_TEMPLATE_SIZEOF_EXPR:
        s = ccr_template_node(ast, CCR_NODE_SIZEOF, parent);
        ccr_template_text(ast, s, "");
        break;

    /* the file's own declarations; a function's return type is its `type`,
     * its parameters one text slot ("int a, char *b") */
    case CCR_TEMPLATE_FUNC_DEF:
    case CCR_TEMPLATE_FUNC_DECL:
        s = ccr_template_node(ast, CCR_NODE_FUNCTION, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        word(ast, s, CCR_FLAG_WORD_NAME, "name");
        ccr_template_text(ast, s, "void");
        if (t == CCR_TEMPLATE_FUNC_DEF && s != CCR_NONE) {
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
            ccr_node_id body = ccr_template_node(ast, CCR_NODE_BLOCK, s);
            if (body != CCR_NONE)
                ast->nodes[body].close_indent = ccr_ast_intern(ast, "");
        }
        break;
    case CCR_TEMPLATE_STRUCT_DEF:
        s = ccr_template_node(ast, CCR_NODE_STRUCT, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "point");
        if (s != CCR_NONE) {
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
            ccr_template_node(ast, CCR_NODE_BLOCK, s);   /* fields: declarations go in */
        }
        break;
    case CCR_TEMPLATE_ENUM_DEF:
        s = ccr_template_node(ast, CCR_NODE_ENUM, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "mood");
        ccr_template_text(ast, s, "CALM, ANGRY");
        if (s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
        break;
    case CCR_TEMPLATE_TYPEDEF:
        s = ccr_template_node(ast, CCR_NODE_TYPEDEF, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "struct point");
        word(ast, s, CCR_FLAG_WORD_NAME, "point");
        break;
    case CCR_TEMPLATE_GLOBAL_VAR:
        s = ccr_template_node(ast, CCR_NODE_VAR, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "static int");
        word(ast, s, CCR_FLAG_WORD_NAME, "counter");
        if (s != CCR_NONE) {
            ccr_node_id v = ccr_template_text(ast, s, "0");
            if (v != CCR_NONE)
                ast->nodes[v].flags |= CCR_FLAG_INIT;
        }
        break;
    case CCR_TEMPLATE_INCLUDE_SYS:
    case CCR_TEMPLATE_INCLUDE_LOC:
        s = ccr_template_node(ast, CCR_NODE_INCLUDE, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, t == CCR_TEMPLATE_INCLUDE_SYS ? "stdio.h" : "util.h");
        if (t == CCR_TEMPLATE_INCLUDE_SYS && s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_ANGLED;
        break;
    case CCR_TEMPLATE_DEFINE:
        s = ccr_template_node(ast, CCR_NODE_MACRO_DEFINE, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "SIZE");
        ccr_template_text(ast, s, "64");
        break;

    case CCR_TEMPLATE_COUNT:
        break;
    }
    return s;
}
