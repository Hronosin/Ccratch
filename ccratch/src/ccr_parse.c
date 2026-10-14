/* dladdr, realpath, glob: POSIX/GNU, not plain C11 */
#define _GNU_SOURCE

#include "ccr_parse.h"

#include <clang-c/Index.h>

#include <dlfcn.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CCR_HAVE_OPERATOR_KIND_API \
    (CINDEX_VERSION >= CINDEX_VERSION_ENCODE(0, 64))

typedef struct visit_ctx {
    ccr_ast          *ast;
    CXTranslationUnit tu;
    CXFile            main_file;
    ccr_node_id       parent;
    ccr_node_id       callee_pending;  /* Call whose callee is not seen yet */

    /* Which child of the Var being visited is its initializer */
    ccr_node_id       init_var;
    CXCursor          init_cursor;
    bool              init_hit;

    /* `int x = 1;` is a DeclStmt holding one VarDecl. The Var stands for
     * the whole statement, so it takes the DeclStmt's range (type, ';'). */
    bool              decl_pending;
    ccr_node_id       decl_owner;
    uint32_t          decl_b, decl_e;
} visit_ctx;

/* Interns a CXString and disposes it. */
static ccr_str take(ccr_ast *ast, CXString s)
{
    ccr_str id = ccr_ast_intern(ast, clang_getCString(s));
    clang_disposeString(s);
    return id;
}

/* First token of a source range, or 0 if the range has none. */
static ccr_str first_token_in(visit_ctx *ctx, CXSourceRange range)
{
    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(ctx->tu, range, &toks, &n);
    ccr_str s = 0;
    if (n > 0)
        s = take(ctx->ast, clang_getTokenSpelling(ctx->tu, toks[0]));
    clang_disposeTokens(ctx->tu, toks, n);
    return s;
}

/*
 * First token of the cursor's extent: literal text, operator fallback.
 * A literal that came out of a macro (`RL_QUADS` -> 0x0007) has no tokens
 * of its own; then we show what was written at the use site, which is the
 * macro name - more useful than the number anyway.
 */
/*
 * What a literal should be called.
 *
 * libclang reports the same location for `4`, for `ARRAY_LEN` that expands
 * to 4, and for the `0` inside `MAX(a[0], b)`, so locations cannot tell
 * them apart. The token at the use site can:
 *   - a number/char/string there: a plain literal, show it;
 *   - an object-like macro there (`ARRAY_LEN`, `RL_QUADS`): the literal
 *     is that macro, show its name;
 *   - a function-like macro there (`MAX`): the literal is one of its
 *     arguments, written in this file - show the literal's own token.
 */
static ccr_str first_token(visit_ctx *ctx, CXCursor c)
{
    ccr_str own = first_token_in(ctx, clang_getCursorExtent(c));

    CXFile file = NULL;
    unsigned line = 0, col = 0;
    clang_getExpansionLocation(clang_getCursorLocation(c), &file, &line, &col, NULL);
    if (!file)
        return own;
    CXSourceLocation at = clang_getLocation(ctx->tu, file, line, col);
    CXSourceLocation next = clang_getLocation(ctx->tu, file, line, col + 1);
    ccr_str use = first_token_in(ctx, clang_getRange(at, next));
    if (use == 0)
        return own;

    const char *u = ccr_ast_str(ctx->ast, use);
    bool identifier = (u[0] >= 'a' && u[0] <= 'z') || (u[0] >= 'A' && u[0] <= 'Z') || u[0] == '_';
    if (!identifier)
        return use;

    CXCursor at_cursor = clang_getCursor(ctx->tu, at);
    if (clang_getCursorKind(at_cursor) == CXCursor_MacroExpansion) {
        CXCursor def = clang_getCursorReferenced(at_cursor);
        if (clang_Cursor_isNull(def) || !clang_Cursor_isMacroFunctionLike(def))
            return use;   /* object-like macro: show its name */
    }
    if (own != 0)
        return own;

    /* A literal from the body of a function-like macro defined in some
     * header has no token in this file: let clang tell us its value. */
    CXEvalResult ev = clang_Cursor_Evaluate(c);
    ccr_str value = 0;
    if (ev) {
        char buf[64] = { 0 };
        switch (clang_EvalResult_getKind(ev)) {
        case CXEval_Int:
            snprintf(buf, sizeof buf, "%lld", clang_EvalResult_getAsLongLong(ev));
            break;
        case CXEval_Float:
            snprintf(buf, sizeof buf, "%g", clang_EvalResult_getAsDouble(ev));
            break;
        default:
            break;
        }
        clang_EvalResult_dispose(ev);
        value = ccr_ast_intern(ctx->ast, buf);
    }
    return value != 0 ? value : use;
}

static bool is_word_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/* Source text of a cursor, rebuilt from tokens, at most ~80 chars. */
static ccr_str extent_text(visit_ctx *ctx, CXCursor c)
{
    enum { MAX_LEN = 80 };
    char buf[MAX_LEN + 4];
    size_t len = 0;

    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
    for (unsigned i = 0; i < n && len < MAX_LEN; ++i) {
        CXString s = clang_getTokenSpelling(ctx->tu, toks[i]);
        const char *t = clang_getCString(s);
        size_t tl = t ? strlen(t) : 0;
        /* a space only where two words would otherwise glue together */
        if (len > 0 && tl > 0 && is_word_char(buf[len - 1]) && is_word_char(t[0]))
            buf[len++] = ' ';
        for (size_t k = 0; k < tl && len < MAX_LEN; ++k)
            buf[len++] = t[k];
        clang_disposeString(s);
    }
    clang_disposeTokens(ctx->tu, toks, n);

    if (len >= MAX_LEN) {
        memcpy(buf + MAX_LEN, "...", 3);
        len = MAX_LEN + 3;
    }
    buf[len] = '\0';
    return ccr_ast_intern(ctx->ast, buf);
}

/* `p->x` or `p.x`: the last member operator in the expression decides. */
static bool member_is_arrow(visit_ctx *ctx, CXCursor c)
{
    bool arrow = false;
    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
    for (unsigned i = n; i-- > 0;) {
        if (clang_getTokenKind(toks[i]) != CXToken_Punctuation)
            continue;
        CXString s = clang_getTokenSpelling(ctx->tu, toks[i]);
        const char *t = clang_getCString(s);
        bool found = t && (strcmp(t, "->") == 0 || strcmp(t, ".") == 0);
        arrow = found && t[0] == '-';
        clang_disposeString(s);
        if (found)
            break;
    }
    clang_disposeTokens(ctx->tu, toks, n);
    return arrow;
}

typedef enum node_policy {
    POLICY_NODE,         /* becomes a ccr_node */
    POLICY_TRANSPARENT,  /* skipped, children attach to our parent */
    POLICY_SKIP          /* dropped with its whole subtree */
} node_policy;

static node_policy classify(enum CXCursorKind k, ccr_node_kind *out)
{
    switch (k) {
    /* --- noise and wrappers --- */
    case CXCursor_UnexposedExpr:     /* implicit casts and friends */
    case CXCursor_UnexposedStmt:
    case CXCursor_ParenExpr:         /* the tree already encodes precedence */
    case CXCursor_DeclStmt:          /* `int x;` inside a body -> just Var */
        return POLICY_TRANSPARENT;
    case CXCursor_TypeRef:
    case CXCursor_UnexposedAttr:
        return POLICY_SKIP;

    /* --- preprocessor --- */
    case CXCursor_InclusionDirective: *out = CCR_NODE_INCLUDE;      return POLICY_NODE;
    case CXCursor_MacroDefinition:    *out = CCR_NODE_MACRO_DEFINE; return POLICY_NODE;
    case CXCursor_MacroExpansion:     *out = CCR_NODE_MACRO_USE;    return POLICY_NODE;

    /* --- declarations --- */
    case CXCursor_FunctionDecl:     *out = CCR_NODE_FUNCTION;   return POLICY_NODE;
    case CXCursor_ParmDecl:         *out = CCR_NODE_PARAM;      return POLICY_NODE;
    case CXCursor_VarDecl:          *out = CCR_NODE_VAR;        return POLICY_NODE;
    case CXCursor_StructDecl:       *out = CCR_NODE_STRUCT;     return POLICY_NODE;
    case CXCursor_UnionDecl:        *out = CCR_NODE_UNION;      return POLICY_NODE;
    case CXCursor_FieldDecl:        *out = CCR_NODE_FIELD;      return POLICY_NODE;
    case CXCursor_TypedefDecl:      *out = CCR_NODE_TYPEDEF;    return POLICY_NODE;
    case CXCursor_EnumDecl:         *out = CCR_NODE_ENUM;       return POLICY_NODE;
    case CXCursor_EnumConstantDecl: *out = CCR_NODE_ENUM_CONST; return POLICY_NODE;

    /* --- statements --- */
    case CXCursor_CompoundStmt: *out = CCR_NODE_BLOCK;    return POLICY_NODE;
    case CXCursor_IfStmt:       *out = CCR_NODE_IF;       return POLICY_NODE;
    case CXCursor_WhileStmt:    *out = CCR_NODE_WHILE;    return POLICY_NODE;
    case CXCursor_DoStmt:       *out = CCR_NODE_DO_WHILE; return POLICY_NODE;
    case CXCursor_ForStmt:      *out = CCR_NODE_FOR;      return POLICY_NODE;
    case CXCursor_SwitchStmt:   *out = CCR_NODE_SWITCH;   return POLICY_NODE;
    case CXCursor_CaseStmt:     *out = CCR_NODE_CASE;     return POLICY_NODE;
    case CXCursor_DefaultStmt:  *out = CCR_NODE_DEFAULT;  return POLICY_NODE;
    case CXCursor_BreakStmt:    *out = CCR_NODE_BREAK;    return POLICY_NODE;
    case CXCursor_ContinueStmt: *out = CCR_NODE_CONTINUE; return POLICY_NODE;
    case CXCursor_ReturnStmt:   *out = CCR_NODE_RETURN;   return POLICY_NODE;
    case CXCursor_GotoStmt:     *out = CCR_NODE_GOTO;     return POLICY_NODE;
    case CXCursor_LabelStmt:    *out = CCR_NODE_LABEL;    return POLICY_NODE;
    case CXCursor_NullStmt:     *out = CCR_NODE_EMPTY;    return POLICY_NODE;

    /* --- expressions --- */
    case CXCursor_CallExpr:               *out = CCR_NODE_CALL;       return POLICY_NODE;
    case CXCursor_BinaryOperator:
    case CXCursor_CompoundAssignOperator: *out = CCR_NODE_BINARY_OP;  return POLICY_NODE;
    case CXCursor_UnaryOperator:          *out = CCR_NODE_UNARY_OP;   return POLICY_NODE;
    case CXCursor_ConditionalOperator:    *out = CCR_NODE_TERNARY;    return POLICY_NODE;
    case CXCursor_DeclRefExpr:            *out = CCR_NODE_REF;        return POLICY_NODE;
    case CXCursor_MemberRefExpr:          *out = CCR_NODE_MEMBER;     return POLICY_NODE;
    case CXCursor_ArraySubscriptExpr:     *out = CCR_NODE_INDEX;      return POLICY_NODE;
    case CXCursor_CStyleCastExpr:         *out = CCR_NODE_CAST;       return POLICY_NODE;
    case CXCursor_UnaryExpr:              *out = CCR_NODE_SIZEOF;     return POLICY_NODE;
    case CXCursor_InitListExpr:           *out = CCR_NODE_INIT_LIST;  return POLICY_NODE;
    case CXCursor_CompoundLiteralExpr:    *out = CCR_NODE_COMPOUND_LIT; return POLICY_NODE;
    case CXCursor_MemberRef:              *out = CCR_NODE_DESIGNATOR; return POLICY_NODE; /* {.x = 1} */
    case CXCursor_IntegerLiteral:         *out = CCR_NODE_INT_LIT;    return POLICY_NODE;
    case CXCursor_FloatingLiteral:        *out = CCR_NODE_FLOAT_LIT;  return POLICY_NODE;
    case CXCursor_CharacterLiteral:       *out = CCR_NODE_CHAR_LIT;   return POLICY_NODE;
    case CXCursor_StringLiteral:          *out = CCR_NODE_STRING_LIT; return POLICY_NODE;

    default:
        *out = CCR_NODE_OTHER;
        return POLICY_NODE;
    }
}

static bool is_block_like_decl(ccr_node_kind k)
{
    return k == CCR_NODE_FUNCTION || k == CCR_NODE_STRUCT ||
           k == CCR_NODE_UNION    || k == CCR_NODE_ENUM;
}

static void fill_unary(visit_ctx *ctx, CXCursor c, ccr_node_id id)
{
#if CCR_HAVE_OPERATOR_KIND_API
    enum CXUnaryOperatorKind k = clang_getCursorUnaryOperatorKind(c);
    ccr_str op = take(ctx->ast, clang_getUnaryOperatorKindSpelling(k));
    ctx->ast->nodes[id].op = op;
    if (k == CXUnaryOperator_PostInc || k == CXUnaryOperator_PostDec)
        ctx->ast->nodes[id].flags |= CCR_FLAG_POSTFIX;
#else
    ctx->ast->nodes[id].op = first_token(ctx, c);
#endif
}

static void fill_binary(visit_ctx *ctx, CXCursor c, ccr_node_id id)
{
#if CCR_HAVE_OPERATOR_KIND_API
    enum CXBinaryOperatorKind k = clang_getCursorBinaryOperatorKind(c);
    ctx->ast->nodes[id].op = take(ctx->ast, clang_getBinaryOperatorKindSpelling(k));
#else
    (void)c;
    ctx->ast->nodes[id].op = ccr_ast_intern(ctx->ast, "?");
#endif
}

static void fill_include(visit_ctx *ctx, CXCursor c, ccr_node_id id)
{
    ctx->ast->nodes[id].name = take(ctx->ast, clang_getCursorSpelling(c));

    /* angled vs quoted: look at the tokens of the directive */
    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
    for (unsigned i = 0; i < n; ++i) {
        CXString s = clang_getTokenSpelling(ctx->tu, toks[i]);
        const char *t = clang_getCString(s);
        bool angled = t && t[0] == '<';
        bool quoted = t && t[0] == '"';
        clang_disposeString(s);
        if (angled)
            ctx->ast->nodes[id].flags |= CCR_FLAG_ANGLED;
        if (angled || quoted)
            break;
    }
    clang_disposeTokens(ctx->tu, toks, n);
}

static void fill_node(visit_ctx *ctx, CXCursor c, enum CXCursorKind ck, ccr_node_id id)
{
    ccr_ast *ast = ctx->ast;
    ccr_node_kind kind = ast->nodes[id].kind;

    /* location (expansion location, so macro uses point at the use site) */
    unsigned line = 0, col = 0;
    clang_getExpansionLocation(clang_getCursorLocation(c), NULL, &line, &col, NULL);
    ast->nodes[id].line = line;
    ast->nodes[id].column = col;

    /* byte range in the file; for code from macros, the macro call itself */
    CXSourceRange ext = clang_getCursorExtent(c);
    unsigned b = 0, e = 0;
    clang_getExpansionLocation(clang_getRangeStart(ext), NULL, NULL, NULL, &b);
    clang_getExpansionLocation(clang_getRangeEnd(ext), NULL, NULL, NULL, &e);
    ast->nodes[id].src_b = b;
    ast->nodes[id].src_e = e > b ? e : b;

    switch (kind) {
    case CCR_NODE_INCLUDE:
        fill_include(ctx, c, id);
        return;
    case CCR_NODE_BINARY_OP:
        fill_binary(ctx, c, id);
        return;
    case CCR_NODE_UNARY_OP:
        fill_unary(ctx, c, id);
        return;
    case CCR_NODE_INT_LIT:
    case CCR_NODE_FLOAT_LIT:
    case CCR_NODE_CHAR_LIT: {
        ccr_str s = first_token(ctx, c);
        ast->nodes[id].name = s;
        return;
    }
    case CCR_NODE_CAST:
    case CCR_NODE_COMPOUND_LIT: {
        ccr_str t = take(ast, clang_getTypeSpelling(clang_getCursorType(c)));
        ast->nodes[id].type = t;
        return;
    }
    case CCR_NODE_SIZEOF: {
        /* sizeof(type) has no child expression; keep the source instead */
        ccr_str text = extent_text(ctx, c);
        ast->nodes[id].aux = text;
        return;
    }
    case CCR_NODE_MEMBER:
        if (member_is_arrow(ctx, c))
            ast->nodes[id].flags |= CCR_FLAG_ARROW;
        break;
    case CCR_NODE_OTHER: {
        ccr_str raw = take(ast, clang_getCursorKindSpelling(ck));
        ast->nodes[id].raw = raw;
        ccr_str text = extent_text(ctx, c);
        ast->nodes[id].aux = text;
        break;
    }
    default:
        break;
    }

    ccr_str name = take(ast, clang_getCursorSpelling(c));
    ast->nodes[id].name = name;

    if (kind == CCR_NODE_FUNCTION || kind == CCR_NODE_PARAM || kind == CCR_NODE_VAR ||
        kind == CCR_NODE_FIELD || kind == CCR_NODE_TYPEDEF) {
        CXType t = (kind == CCR_NODE_TYPEDEF)
                 ? clang_getTypedefDeclUnderlyingType(c)
                 : clang_getCursorType(c);
        ccr_str ts = take(ast, clang_getTypeSpelling(t));
        ast->nodes[id].type = ts;
    }

    if (kind == CCR_NODE_FUNCTION) {
        CXType rt = clang_getResultType(clang_getCursorType(c));
        ccr_str rs = take(ast, clang_getTypeSpelling(rt));
        ast->nodes[id].aux = rs;
    }

    if (is_block_like_decl(kind) && clang_isCursorDefinition(c))
        ast->nodes[id].flags |= CCR_FLAG_DEFINITION;
}

/*
 * Keep only what was written in this file, not in its headers.
 * The *expansion* location decides: the body of a macro used here
 * belongs here, even though its spelling lives in the #define.
 */
static bool from_main_file(const visit_ctx *ctx, CXCursor c)
{
    CXFile file = NULL;
    clang_getExpansionLocation(clang_getCursorLocation(c), &file, NULL, NULL, NULL);
    return file && clang_File_isEqual(file, ctx->main_file);
}

/* Context-dependent cleanup that classify() cannot do on its own. */
static bool absorb_into_parent(visit_ctx *ctx, CXCursor c, enum CXCursorKind ck)
{
    if (ctx->parent == CCR_NONE)
        return false;
    ccr_node *p = &ctx->ast->nodes[ctx->parent];

    /* `foo(x)` -> the callee is the first real child; it is already the
     * Call's name. Only a plain name is absorbed: `obj->fn(x)` keeps it. */
    if (ctx->callee_pending == ctx->parent) {
        ctx->callee_pending = CCR_NONE;
        if (ck == CXCursor_DeclRefExpr)
            return true;
    }

    /* `goto done;` -> the label name goes on the Goto itself */
    if (p->kind == CCR_NODE_GOTO && ck == CXCursor_LabelRef) {
        ccr_str s = take(ctx->ast, clang_getCursorSpelling(c));
        ctx->ast->nodes[ctx->parent].name = s;
        return true;
    }

    /* `typedef struct point {...} point;` -> the struct is already a sibling */
    if (p->kind == CCR_NODE_TYPEDEF &&
        (ck == CXCursor_StructDecl || ck == CXCursor_UnionDecl || ck == CXCursor_EnumDecl))
        return true;

    return false;
}

/*
 * An expression that is entirely the expansion of one object-like macro
 * (`NULL`, `true`, `DEG2RAD` -> (PI/180.0f)) becomes a single MacroValue
 * node named after the macro, instead of whatever it expands to.
 * Function-like macros (`MAX(a, b)`) are left expanded: their arguments
 * are real code written here.
 */
static bool object_macro_value(visit_ctx *ctx, CXCursor c, ccr_str *name)
{
    CXSourceRange ext = clang_getCursorExtent(c);
    CXFile f1 = NULL, f2 = NULL;
    unsigned l1 = 0, c1 = 0, l2 = 0, c2 = 0;
    clang_getExpansionLocation(clang_getRangeStart(ext), &f1, &l1, &c1, NULL);
    clang_getExpansionLocation(clang_getRangeEnd(ext), &f2, &l2, &c2, NULL);
    if (!f1 || !f2 || l1 != l2 || !clang_File_isEqual(f1, f2))
        return false;

    CXCursor at = clang_getCursor(ctx->tu, clang_getLocation(ctx->tu, f1, l1, c1));
    if (clang_getCursorKind(at) != CXCursor_MacroExpansion)
        return false;

    /* the macro name must cover the whole expression, nothing after it */
    CXString spelled = clang_getCursorSpelling(at);
    const char *s = clang_getCString(spelled);
    size_t len = s ? strlen(s) : 0;
    bool covers = len > 0 && (c2 <= c1 + (unsigned)len);
    clang_disposeString(spelled);
    if (!covers)
        return false;

    CXCursor def = clang_getCursorReferenced(at);
    if (!clang_Cursor_isNull(def) && clang_Cursor_isMacroFunctionLike(def))
        return false;

    *name = take(ctx->ast, clang_getCursorSpelling(at));
    return true;
}

static enum CXChildVisitResult count_child(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)c;
    (void)parent;
    ++*(unsigned *)data;
    return CXChildVisit_Continue;
}

static enum CXChildVisitResult visit(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)parent;
    visit_ctx *ctx = data;

    if (!from_main_file(ctx, c))
        return CXChildVisit_Continue;

    enum CXCursorKind ck = clang_getCursorKind(c);
    ccr_node_kind kind = CCR_NODE_OTHER;
    node_policy policy = classify(ck, &kind);

    /* `int a = 1, b = 2;` stays one block: its parts cannot move apart */
    bool single_decl = false;
    if (ck == CXCursor_DeclStmt) {
        unsigned kids = 0;
        clang_visitChildren(c, count_child, &kids);
        if (kids > 1) {
            kind = CCR_NODE_DECL_GROUP;
            policy = POLICY_NODE;
        } else {
            single_decl = true;
        }
    }

    ccr_str macro_name = 0;
    bool leaf = false;
    if (clang_isExpression(ck) && policy != POLICY_SKIP && ck != CXCursor_CallExpr &&
        object_macro_value(ctx, c, &macro_name)) {
        kind = CCR_NODE_MACRO_VALUE;
        policy = POLICY_NODE;
        leaf = true;
    }

    /* Is this the initializer of the Var we are inside? It may be wrapped
     * in a transparent implicit cast; the flag then goes to the first real
     * node underneath. */
    if (ctx->init_var != CCR_NONE && ctx->parent == ctx->init_var &&
        clang_equalCursors(c, ctx->init_cursor))
        ctx->init_hit = true;

    /* Transparent wrappers (implicit casts around the callee) are looked
     * through without consuming the pending callee. */
    if (policy != POLICY_TRANSPARENT &&
        (policy == POLICY_SKIP || absorb_into_parent(ctx, c, ck)))
        return CXChildVisit_Continue;

    ccr_node_id saved_parent = ctx->parent;
    ccr_node_id saved_init_var = ctx->init_var;
    CXCursor    saved_init_cursor = ctx->init_cursor;
    const bool  saved_decl_pending = ctx->decl_pending;

    if (single_decl) {
        unsigned b = 0, e = 0;
        CXSourceRange ext = clang_getCursorExtent(c);
        clang_getExpansionLocation(clang_getRangeStart(ext), NULL, NULL, NULL, &b);
        clang_getExpansionLocation(clang_getRangeEnd(ext), NULL, NULL, NULL, &e);
        ctx->decl_pending = true;
        ctx->decl_owner = ctx->parent;
        ctx->decl_b = b;
        ctx->decl_e = e > b ? e : b;
    }

    if (policy == POLICY_NODE) {
        ccr_node_id id = ccr_ast_add(ctx->ast, kind, ctx->parent);
        if (id == CCR_NONE)
            return CXChildVisit_Break; /* out of memory: stop politely */
        fill_node(ctx, c, ck, id);
        if (kind == CCR_NODE_MACRO_VALUE)
            ctx->ast->nodes[id].name = macro_name;
        if (ctx->decl_pending && ctx->decl_owner == ctx->parent) {
            ctx->ast->nodes[id].src_b = ctx->decl_b;
            ctx->ast->nodes[id].src_e = ctx->decl_e;
            ctx->decl_pending = false;
        }

        if (ctx->init_hit && ctx->parent == ctx->init_var) {
            ctx->ast->nodes[id].flags |= CCR_FLAG_INIT;
            ctx->init_hit = false;
        }

        ctx->parent = id;
        if (kind == CCR_NODE_CALL)
            ctx->callee_pending = id;

        if (kind == CCR_NODE_VAR) {
#if CCR_HAVE_OPERATOR_KIND_API   /* same LLVM 17 release added this */
            ctx->init_var = id;
            ctx->init_cursor = clang_Cursor_getVarDeclInitializer(c);
            ctx->init_hit = false;
#endif
        }
    }

    if (!leaf)
        clang_visitChildren(c, visit, ctx);

#if !CCR_HAVE_OPERATOR_KIND_API
    /* Older libclang: the initializer is the last child, if any. */
    if (policy == POLICY_NODE && kind == CCR_NODE_VAR) {
        ccr_node_id last = ctx->ast->nodes[ctx->parent].last_child;
        if (last != CCR_NONE)
            ctx->ast->nodes[last].flags |= CCR_FLAG_INIT;
    }
#endif

    ctx->parent = saved_parent;
    ctx->init_var = saved_init_var;
    ctx->init_cursor = saved_init_cursor;
    if (single_decl)
        ctx->decl_pending = saved_decl_pending;
    return CXChildVisit_Continue;
}

static bool has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static ccr_diag_severity map_severity(enum CXDiagnosticSeverity s)
{
    switch (s) {
    case CXDiagnostic_Warning: return CCR_DIAG_WARNING;
    case CXDiagnostic_Error:   return CCR_DIAG_ERROR;
    case CXDiagnostic_Fatal:   return CCR_DIAG_FATAL;
    case CXDiagnostic_Ignored:
    case CXDiagnostic_Note:
    default:                   return CCR_DIAG_NOTE;
    }
}

static void collect_diags(CXTranslationUnit tu, ccr_ast *ast)
{
    unsigned n = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < n; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        enum CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(d);
        if (sev == CXDiagnostic_Ignored) {
            clang_disposeDiagnostic(d);
            continue;
        }

        CXFile file = NULL;
        unsigned line = 0, col = 0;
        clang_getSpellingLocation(clang_getDiagnosticLocation(d), &file, &line, &col, NULL);

        ccr_diag out = {
            .severity = map_severity(sev),
            .file     = file ? take(ast, clang_getFileName(file)) : 0,
            .line     = line,
            .column   = col,
            .message  = take(ast, clang_getDiagnosticSpelling(d)),
        };
        ccr_ast_add_diag(ast, out);
        clang_disposeDiagnostic(d);
    }
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* Is `dir` a clang resource dir, i.e. does it hold the builtin headers? */
static bool is_resource_dir(const char *dir)
{
    char probe[4096];
    int w = snprintf(probe, sizeof probe, "%s/include/stddef.h", dir);
    return w > 0 && (size_t)w < sizeof probe && file_exists(probe);
}

void ccr_parse_clang_version(char *out, unsigned int cap)
{
    CXString v = clang_getClangVersion();
    snprintf(out, cap, "%s", clang_getCString(v));
    clang_disposeString(v);
}

/* Major version of the libclang actually loaded: "... clang version 22.1.8" -> 22 */
static int libclang_major(void)
{
    char v[256];
    ccr_parse_clang_version(v, sizeof v);
    const char *p = strstr(v, "version ");
    return p ? atoi(p + 8) : 0;
}

/* Directory the loaded libclang .so lives in, symlinks resolved. */
static bool libclang_dir(char *out, size_t cap)
{
    /* ISO C has no function->object pointer cast; a union does it legally */
    union { CXString (*fn)(void); void *ptr; } addr = { .fn = clang_getClangVersion };
    Dl_info info;
    if (!dladdr(addr.ptr, &info) || !info.dli_fname)
        return false;

    char real[PATH_MAX];
    if (!realpath(info.dli_fname, real))
        return false;
    char *slash = strrchr(real, '/');
    if (!slash)
        return false;
    *slash = '\0';
    return snprintf(out, cap, "%s", real) < (int)cap;
}

/* ".../clang/22/include/stddef.h" -> 22 */
static int version_of_match(const char *path)
{
    const char *clang = strstr(path, "/clang/");
    const char *last = NULL;
    while (clang) {
        last = clang;
        clang = strstr(clang + 1, "/clang/");
    }
    return last ? atoi(last + 7) : 0;
}

/*
 * Looks for clang's builtin headers relative to where libclang is, then in
 * the usual system places. Prefers the version matching the loaded
 * libclang; with want_major = 0 takes the newest of any version.
 */
static bool discover(int want_major, char *out, size_t cap)
{
    char libdir[PATH_MAX] = "";
    libclang_dir(libdir, sizeof libdir);

    char bases[8][PATH_MAX + 32];
    int nb = 0;
    if (libdir[0]) {
        snprintf(bases[nb++], sizeof bases[0], "%s/clang", libdir);          /* Debian, LLVM tarballs */
        snprintf(bases[nb++], sizeof bases[0], "%s/../lib/clang", libdir);   /* Fedora: lib64 vs lib */
        snprintf(bases[nb++], sizeof bases[0], "%s/../lib64/clang", libdir);
    }
    snprintf(bases[nb++], sizeof bases[0], "/usr/lib/clang");
    snprintf(bases[nb++], sizeof bases[0], "/usr/lib64/clang");
    snprintf(bases[nb++], sizeof bases[0], "/usr/local/lib/clang");
    if (want_major > 0)
        snprintf(bases[nb++], sizeof bases[0], "/usr/lib/llvm-%d/lib/clang", want_major);

    char best[PATH_MAX] = "";
    int best_major = -1;

    for (int b = 0; b < nb; ++b) {
        char pattern[PATH_MAX + 64];
        snprintf(pattern, sizeof pattern, "%s/*/include/stddef.h", bases[b]);
        glob_t g;
        if (glob(pattern, 0, NULL, &g) != 0)
            continue;
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            int major = version_of_match(g.gl_pathv[i]);
            bool take_it = want_major > 0
                ? (major == want_major && best_major != want_major)  /* first exact match wins */
                : (major > best_major);                              /* otherwise newest */
            if (!take_it)
                continue;
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s", g.gl_pathv[i]);
            char *cut = strstr(dir, "/include/stddef.h");
            if (cut)
                *cut = '\0';
            if (realpath(dir, best))
                best_major = major;
        }
        globfree(&g);
    }

    if (best_major < 0 || (want_major > 0 && best_major != want_major))
        return false;
    return snprintf(out, cap, "%s", best) < (int)cap;
}

const char *ccr_parse_resource_dir(const char **how)
{
    static char found[PATH_MAX];
    const char *dummy;
    if (!how)
        how = &dummy;

    const char *env = getenv("CCRATCH_CLANG_RESOURCE_DIR");
    if (env && *env && is_resource_dir(env)) {
        *how = "from CCRATCH_CLANG_RESOURCE_DIR";
        return env;
    }
    int major = libclang_major();
    if (major > 0 && discover(major, found, sizeof found)) {
        *how = "found at run time, matches the loaded libclang";
        return found;
    }
#ifdef CCR_CLANG_RESOURCE_DIR
    if (is_resource_dir(CCR_CLANG_RESOURCE_DIR)) {
        *how = "remembered from build time";
        return CCR_CLANG_RESOURCE_DIR;
    }
#endif
    if (discover(0, found, sizeof found)) {
        *how = "newest found, version differs from libclang";
        return found;
    }
    *how = "not found, libclang will guess";
    return NULL;
}

bool ccr_parse_file(const char *path, const char *const *args, int arg_count, ccr_ast *out)
{
    static const char *const c_defaults[]   = { "-std=c11" };
    static const char *const cpp_defaults[] = { "-x", "c++", "-std=c++20" };

    if (!args || arg_count <= 0) {
        bool cpp = has_suffix(path, ".cpp") || has_suffix(path, ".cc") ||
                   has_suffix(path, ".cxx") || has_suffix(path, ".hpp");
        args      = cpp ? cpp_defaults : c_defaults;
        arg_count = cpp ? 3 : 1;
    }

    /* Tell libclang where its builtin headers are, unless the user did. */
    bool user_set = false;
    for (int i = 0; i < arg_count; ++i)
        if (strncmp(args[i], "-resource-dir", 13) == 0)
            user_set = true;
    const char *resource = user_set ? NULL : ccr_parse_resource_dir(NULL);

    const char **all = malloc(sizeof(char *) * (size_t)(arg_count + 2));
    if (!all)
        return false;
    int all_count = 0;
    for (int i = 0; i < arg_count; ++i)
        all[all_count++] = args[i];
    if (resource) {
        all[all_count++] = "-resource-dir";
        all[all_count++] = resource;
    }

    CXIndex index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0,
                                      /*displayDiagnostics=*/0);
    if (!index) {
        free(all);
        return false;
    }

    unsigned flags = CXTranslationUnit_DetailedPreprocessingRecord
                   | CXTranslationUnit_KeepGoing;

    CXTranslationUnit tu = NULL;
    enum CXErrorCode err = clang_parseTranslationUnit2(
        index, path, all, all_count, NULL, 0, flags, &tu);
    free(all);

    if (err != CXError_Success || !tu) {
        clang_disposeIndex(index);
        return false;
    }

    out->source_path = ccr_ast_intern(out, path);

    visit_ctx ctx = {
        .ast = out,
        .tu = tu,
        .main_file = clang_getFile(tu, path),
        .parent = CCR_NONE,
        .callee_pending = CCR_NONE,
        .init_var = CCR_NONE,
        .init_cursor = clang_getNullCursor(),
        .init_hit = false,
    };
    ccr_node_id root = ccr_ast_add(out, CCR_NODE_TRANSLATION_UNIT, CCR_NONE);
    if (root != CCR_NONE) {
        out->nodes[root].name = out->source_path;
        ctx.parent = root;
        clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &ctx);
    }

    /* keep the file as clang read it: saving splices from it */
    size_t size = 0;
    const char *contents = clang_getFileContents(tu, ctx.main_file, &size);
    if (contents && size < UINT32_MAX) {
        out->source = malloc(size + 1);
        if (out->source) {
            memcpy(out->source, contents, size);
            out->source[size] = '\0';
            out->source_len = (uint32_t)size;
        }
    }
    if (root != CCR_NONE) {
        out->nodes[root].src_b = 0;
        out->nodes[root].src_e = out->source_len;
    }

    ccr_ast_wrap_bodies(out);
    ccr_ast_finish_ranges(out);
    collect_diags(tu, out);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return root != CCR_NONE;
}
