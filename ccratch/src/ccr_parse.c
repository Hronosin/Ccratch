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
    /* C++: parts of a name or a type, already in its spelling */
    case CXCursor_NamespaceRef:
    case CXCursor_TemplateRef:
    case CXCursor_TemplateTypeParameter:
    case CXCursor_NonTypeTemplateParameter:
    case CXCursor_TemplateTemplateParameter:
    case CXCursor_CXXBaseSpecifier:
    case CXCursor_CXXFinalAttr:
    case CXCursor_CXXOverrideAttr:
        return POLICY_SKIP;

    /* --- C++ --- */
    case CXCursor_Namespace:          *out = CCR_NODE_NAMESPACE; return POLICY_NODE;
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
                                      *out = CCR_NODE_STRUCT;    return POLICY_NODE;
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_ConversionFunction:
    case CXCursor_FunctionTemplate:   *out = CCR_NODE_FUNCTION;  return POLICY_NODE;
    case CXCursor_CXXAccessSpecifier: *out = CCR_NODE_ACCESS;    return POLICY_NODE;
    case CXCursor_UsingDirective:
    case CXCursor_UsingDeclaration:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypeAliasTemplateDecl:
    case CXCursor_NamespaceAlias:     *out = CCR_NODE_USING;     return POLICY_NODE;
    case CXCursor_CXXForRangeStmt:    *out = CCR_NODE_FOR_RANGE; return POLICY_NODE;
    case CXCursor_CXXNewExpr:         *out = CCR_NODE_NEW;       return POLICY_NODE;
    case CXCursor_CXXDeleteExpr:      *out = CCR_NODE_DELETE;    return POLICY_NODE;
    case CXCursor_CXXThisExpr:
    case CXCursor_CXXNullPtrLiteralExpr: *out = CCR_NODE_REF;    return POLICY_NODE;
    case CXCursor_CXXBoolLiteralExpr: *out = CCR_NODE_INT_LIT;   return POLICY_NODE;
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXDynamicCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXFunctionalCastExpr: *out = CCR_NODE_CAST;   return POLICY_NODE;

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

/* Is the first of class / struct / union in `c`'s tokens `word`? (After
 * a template <...> header.) */
static bool first_keyword_is(visit_ctx *ctx, CXCursor c, const char *word)
{
    CXToken *toks = NULL;
    unsigned n = 0;
    bool is = false;
    clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
    for (unsigned i = 0; i < n; ++i) {
        CXString s = clang_getTokenSpelling(ctx->tu, toks[i]);
        const char *t = clang_getCString(s);
        const bool kw = t && (strcmp(t, "class") == 0 || strcmp(t, "struct") == 0 || strcmp(t, "union") == 0);
        is = kw && strcmp(t, word) == 0;
        clang_disposeString(s);
        if (kw)
            break;
    }
    clang_disposeTokens(ctx->tu, toks, n);
    return is;
}

/* The tokens of a declaration before its name: its type as written. */
static void written_type(visit_ctx *ctx, CXCursor c, ccr_node_id id)
{
    const char *name = ccr_ast_str(ctx->ast, ctx->ast->nodes[id].name);
    if (!*name)
        return;
    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
    char buf[200];
    size_t len = 0;
    bool found = false;
    for (unsigned i = 0; i < n; ++i) {
        CXString s = clang_getTokenSpelling(ctx->tu, toks[i]);
        const char *t = clang_getCString(s);
        if (!t)
            t = "";
        if (clang_getTokenKind(toks[i]) == CXToken_Identifier && strcmp(t, name) == 0) {
            found = true;
            clang_disposeString(s);
            break;
        }
        const size_t tl = strlen(t);
        if (len > 0 && tl > 0 && (is_word_char(buf[len - 1]) || buf[len - 1] == '>' || buf[len - 1] == '&' ||
                                  buf[len - 1] == '*') &&
            (is_word_char(t[0]) || t[0] == '*' || t[0] == '&') && len < sizeof buf - 1)
            buf[len++] = ' ';
        for (size_t k = 0; k < tl && len < sizeof buf - 1; ++k)
            buf[len++] = t[k];
        clang_disposeString(s);
    }
    clang_disposeTokens(ctx->tu, toks, n);
    buf[len] = '\0';
    if (found && len > 0)
        ctx->ast->nodes[id].type = ccr_ast_intern(ctx->ast, buf);
}

/* `operator<<`, `operator[]`: a C++ operator, called like a function. */
static bool is_operator_name(const char *s)
{
    return strncmp(s, "operator", 8) == 0 && s[8] && !is_word_char(s[8]) && s[8] != ' ';
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
    case CCR_NODE_ACCESS: {
        const enum CX_CXXAccessSpecifier a = clang_getCXXAccessSpecifier(c);
        ast->nodes[id].name = ccr_ast_intern(ast, a == CX_CXXPublic ? "public" : a == CX_CXXProtected ? "protected"
                                                                                                      : "private");
        return;
    }
    case CCR_NODE_USING:
        ast->nodes[id].name = take(ast, clang_getCursorSpelling(c));
        ast->nodes[id].aux = extent_text(ctx, c);
        return;
    case CCR_NODE_NEW:
    case CCR_NODE_DELETE: {
        /* new T(args) / new T[n]: T as written; delete[] p */
        CXToken *toks = NULL;
        unsigned nt = 0;
        clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &nt);
        char type[160];
        size_t tl = 0;
        int depth = 0;
        bool after_new = false, array = false;
        for (unsigned i = 0; i < nt; ++i) {
            CXString ts = clang_getTokenSpelling(ctx->tu, toks[i]);
            const char *t = clang_getCString(ts);
            if (!t) t = "";
            if (kind == CCR_NODE_DELETE) {
                if (strcmp(t, "[") == 0 && i == 1)
                    array = true;
            } else if (!after_new) {
                after_new = strcmp(t, "new") == 0;
            } else if (depth == 0 && (strcmp(t, "(") == 0 || strcmp(t, "{") == 0 || strcmp(t, "[") == 0)) {
                array = t[0] == '[';
                clang_disposeString(ts);
                break;
            } else {
                depth += strcmp(t, "<") == 0 ? 1 : strcmp(t, ">") == 0 ? -1 : 0;
                const size_t l = strlen(t);
                if (tl > 0 && l > 0 && is_word_char(type[tl - 1]) && is_word_char(t[0]) && tl < sizeof type - 1)
                    type[tl++] = ' ';
                for (size_t k = 0; k < l && tl < sizeof type - 1; ++k)
                    type[tl++] = t[k];
            }
            clang_disposeString(ts);
        }
        clang_disposeTokens(ctx->tu, toks, nt);
        type[tl] = '\0';
        if (kind == CCR_NODE_NEW)
            ast->nodes[id].type = ccr_ast_intern(ast, type);
        if (array)
            ast->nodes[id].flags |= CCR_FLAG_ARRAY_FORM;
        return;
    }
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
        /* C++'s own casts: static_cast<T>(x), T(x) */
        const char *how = ck == CXCursor_CXXStaticCastExpr      ? "static_cast"
                        : ck == CXCursor_CXXDynamicCastExpr     ? "dynamic_cast"
                        : ck == CXCursor_CXXReinterpretCastExpr ? "reinterpret_cast"
                        : ck == CXCursor_CXXConstCastExpr       ? "const_cast"
                        : ck == CXCursor_CXXFunctionalCastExpr  ? "functional" : NULL;
        if (how)
            ast->nodes[id].raw = ccr_ast_intern(ast, how);
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
    if (ck == CXCursor_CXXThisExpr)
        ast->nodes[id].name = ccr_ast_intern(ast, "this");
    else if (ck == CXCursor_CXXNullPtrLiteralExpr)
        ast->nodes[id].name = ccr_ast_intern(ast, "nullptr");
    if (kind == CCR_NODE_STRUCT && ast->cpp && first_keyword_is(ctx, c, "class"))
        ast->nodes[id].flags |= CCR_FLAG_CLASS;
    if (kind == CCR_NODE_CALL) {   /* a constructor: implicit ones go (cpp_cleanup) */
        const CXCursor to = clang_getCursorReferenced(c);
        if (!clang_Cursor_isNull(to) && clang_getCursorKind(to) == CXCursor_Constructor)
            ast->nodes[id].flags |= CCR_FLAG_CTOR_CALL;
    }

    if (kind == CCR_NODE_REF && ast->cpp && ck == CXCursor_DeclRefExpr) {
        const ccr_str q = extent_text(ctx, c);   /* std::cout, not cout */
        if (strstr(ccr_ast_str(ast, q), "::"))
            ast->nodes[id].name = q;
    }

    if (kind == CCR_NODE_REF || kind == CCR_NODE_MEMBER) {
        /* what it is: an enum's values make a list for the slot next to it */
        ccr_str ts = take(ast, clang_getTypeSpelling(clang_getCursorType(c)));
        ast->nodes[id].raw = ts;
    }

    if (kind == CCR_NODE_FUNCTION || kind == CCR_NODE_PARAM || kind == CCR_NODE_VAR ||
        kind == CCR_NODE_FIELD || kind == CCR_NODE_TYPEDEF) {
        CXType t = (kind == CCR_NODE_TYPEDEF)
                 ? clang_getTypedefDeclUnderlyingType(c)
                 : clang_getCursorType(c);
        ccr_str ts = take(ast, clang_getTypeSpelling(t));
        ast->nodes[id].type = ts;
        /* C++: the type as written (auto stays auto, not what it became) */
        if (ast->cpp && (kind == CCR_NODE_VAR || kind == CCR_NODE_PARAM || kind == CCR_NODE_FIELD) &&
            !strchr(ccr_ast_str(ast, ts), '['))
            written_type(ctx, c, id);
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
        if (ck == CXCursor_DeclRefExpr || ck == CXCursor_OverloadedDeclRef) {
            if (ctx->ast->cpp) {   /* std::sort, game::clamp: as written */
                const ccr_str q = extent_text(ctx, c);
                if (strstr(ccr_ast_str(ctx->ast, q), "::"))
                    ctx->ast->nodes[ctx->parent].name = q;
            }
            return true;
        }
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
        if (kind == CCR_NODE_CALL && !is_operator_name(ccr_ast_str(ctx->ast, ctx->ast->nodes[id].name)))
            ctx->callee_pending = id;   /* `a << b`: `a` is an operand, not the callee */

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

static bool parse_any(const char *path, const char *text, size_t text_len, const char *const *args,
                      int arg_count, ccr_ast *out);
/* ------------------------------------------------------ C++ cleanup
 *
 * libclang shows C++ the way the compiler sees it: `a << b` is a call to
 * operator<<, `std::string s = "x"` hides a call to basic_string's
 * constructor, `for (auto &p : v)` holds the iterator's operator* the
 * compiler wrote. Blocks want what was written: the operators become
 * operators again, and what nobody wrote goes.
 */
static void kill(ccr_ast *ast, ccr_node_id id)
{
    ccr_ast_unlink(ast, id);
    ast->nodes[id].flags |= CCR_FLAG_DEAD | CCR_FLAG_LOOSE;
}

/* `id` goes, its children take its place (in order). */
static void hoist_children(ccr_ast *ast, ccr_node_id id)
{
    ccr_node_id at = id;
    const uint32_t keep = ast->nodes[id].flags & CCR_FLAG_INIT;
    for (ccr_node_id c = ast->nodes[id].first_child; c != CCR_NONE;) {
        const ccr_node_id next = ast->nodes[c].next_sibling;
        ccr_ast_insert_after(ast, at, c);
        ast->nodes[c].flags |= keep;
        at = c;
        c = next;
    }
    kill(ast, id);
}

/* Was `name` written in the node's text? (No: the compiler made it.) */
static bool written(const ccr_ast *ast, ccr_node_id id, const char *name)
{
    const ccr_node *n = &ast->nodes[id];
    const size_t len = strlen(name);
    if (!ast->source || n->src_e <= n->src_b || n->src_e > ast->source_len || len == 0)
        return false;
    for (uint32_t i = n->src_b; i + len <= n->src_e; ++i)
        if (memcmp(ast->source + i, name, len) == 0)
            return true;
    return false;
}

static uint32_t kids(const ccr_ast *ast, ccr_node_id id)
{
    uint32_t k = 0;
    for (ccr_node_id c = ast->nodes[id].first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling)
        ++k;
    return k;
}

static void cpp_cleanup(ccr_ast *ast)
{
    const uint32_t count = ast->node_count;
    /* operators first: their operand may be an implicit constructor */
    for (ccr_node_id id = 0; id < count; ++id) {
        ccr_node *n = &ast->nodes[id];
        if (n->kind != CCR_NODE_CALL || (n->flags & CCR_FLAG_DEAD))
            continue;
        const char *name = ccr_ast_str(ast, n->name);
        if (!is_operator_name(name))
            continue;
        if (strcmp(name + 8, "()") == 0) {   /* twice(21): the object is the callee */
            for (ccr_node_id c = n->first_child; c != CCR_NONE;) {   /* the operator's own Ref */
                const ccr_node_id next = ast->nodes[c].next_sibling;
                if (ast->nodes[c].kind == CCR_NODE_REF &&
                    strcmp(ccr_ast_str(ast, ast->nodes[c].name), "operator()") == 0)
                    kill(ast, c);
                c = next;
            }
            const ccr_node_id obj = ast->nodes[id].first_child;
            if (obj != CCR_NONE && ast->nodes[obj].kind == CCR_NODE_REF) {
                ast->nodes[id].name = ast->nodes[obj].name;
                kill(ast, obj);
            }
            continue;
        }
        /* the operator's own Ref: the callee, between (or before) the operands */
        ccr_node_id ref = CCR_NONE;
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling)
            if (ast->nodes[c].kind == CCR_NODE_REF && strcmp(ccr_ast_str(ast, ast->nodes[c].name), name) == 0) {
                ref = c;
                break;
            }
        if (ref == CCR_NONE)
            continue;
        const char *sym = name + 8;
        const uint32_t ref_b = ast->nodes[ref].src_b;
        kill(ast, ref);
        n = &ast->nodes[id];
        const uint32_t k = kids(ast, id);
        if (strcmp(sym, "()") == 0 || k == 0) {
            continue;   /* f(x) on an object; or one the compiler wrote */
        } else if (strcmp(sym, "[]") == 0 && k == 2) {
            n->kind = CCR_NODE_INDEX;
        } else if (k == 2) {
            n->kind = CCR_NODE_BINARY_OP;
            n->op = ccr_ast_intern(ast, sym);
            n = &ast->nodes[id];
        } else if (k == 1) {
            n->kind = CCR_NODE_UNARY_OP;
            n->op = ccr_ast_intern(ast, sym);
            n = &ast->nodes[id];
            if (ast->nodes[n->first_child].src_b < ref_b)
                n->flags |= CCR_FLAG_POSTFIX;   /* it++: the operand comes first */
        } else {
            continue;
        }
        n->name = 0;
    }
    for (ccr_node_id id = 0; id < count; ++id) {
        ccr_node *n = &ast->nodes[id];
        if (n->flags & CCR_FLAG_DEAD)
            continue;
        if (n->kind == CCR_NODE_CALL && (n->flags & CCR_FLAG_CTOR_CALL) &&
            !written(ast, id, ccr_ast_str(ast, n->name))) {
            /* `std::string s = "hi"`: the string, not basic_string("hi");
             * `std::vector<int> v;`: nothing at all */
            const uint32_t k = kids(ast, id);
            if (k <= 1)
                hoist_children(ast, id);
        }
    }
    for (ccr_node_id id = 0; id < count; ++id) {
        const ccr_node *n = &ast->nodes[id];
        if (n->flags & CCR_FLAG_DEAD)
            continue;
        if (n->kind == CCR_NODE_FOR_RANGE) {
            /* the loop variable starts as *__begin: the compiler's, not shown */
            const ccr_node_id var = n->first_child;
            if (var != CCR_NONE && ast->nodes[var].kind == CCR_NODE_VAR)
                for (ccr_node_id c = ast->nodes[var].first_child; c != CCR_NONE;) {
                    const ccr_node_id next = ast->nodes[c].next_sibling;
                    if (ast->nodes[c].flags & CCR_FLAG_INIT)
                        kill(ast, c);
                    c = next;
                }
        } else if (n->kind == CCR_NODE_NEW) {
            /* new Player(a, b): the constructor's arguments are the New's */
            for (ccr_node_id c = n->first_child; c != CCR_NONE;) {
                const ccr_node_id next = ast->nodes[c].next_sibling;
                if (ast->nodes[c].kind == CCR_NODE_CALL && (ast->nodes[c].flags & CCR_FLAG_CTOR_CALL))
                    hoist_children(ast, c);
                c = next;
            }
        }
    }
}


bool ccr_parse_file(const char *path, const char *const *args, int arg_count, ccr_ast *out)
{
    return parse_any(path, NULL, 0, args, arg_count, out);
}

bool ccr_parse_text(const char *path, const char *text, size_t len, const char *const *args, int arg_count,
                    ccr_ast *out)
{
    return parse_any(path, text, len, args, arg_count, out);
}

static bool parse_any(const char *path, const char *text, size_t text_len, const char *const *args,
                      int arg_count, ccr_ast *out)
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
    struct CXUnsavedFile unsaved = { path, text, (unsigned long)text_len };
    enum CXErrorCode err = clang_parseTranslationUnit2(
        index, path, all, all_count, text ? &unsaved : NULL, text ? 1u : 0u, flags, &tu);
    free(all);

    if (err != CXError_Success || !tu) {
        clang_disposeIndex(index);
        return false;
    }

    out->source_path = ccr_ast_intern(out, path);
    out->cpp = false;
    for (int i = 0; i + 1 < arg_count; ++i)
        if (strcmp(args[i], "-x") == 0 && strncmp(args[i + 1], "c++", 3) == 0)
            out->cpp = true;
    if (has_suffix(path, ".cpp") || has_suffix(path, ".cc") || has_suffix(path, ".cxx") ||
        has_suffix(path, ".hpp") || has_suffix(path, ".hh") || has_suffix(path, ".hxx") || has_suffix(path, ".C"))
        out->cpp = true;

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

    if (out->cpp)
        cpp_cleanup(out);
    ccr_ast_wrap_bodies(out);
    ccr_ast_finish_ranges(out);
    collect_diags(tu, out);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return root != CCR_NONE;
}

/* ------------------------------------------------------- library API */

typedef struct api_ctx {
    ccr_api *api;
    char     dir[PATH_MAX];   /* the library's own headers live under here */
    size_t   dir_len;
    /* anonymous enums, named by a typedef that comes after them */
    unsigned pending_hash;
    int      pending_enum;
    bool     main_only;
    bool     no_static;   /* a project's own file: what is static is its own */
} api_ctx;

static void copy_str(char *dst, size_t cap, CXString s)
{
    const char *c = clang_getCString(s);
    snprintf(dst, cap, "%s", c ? c : "");
    clang_disposeString(s);
}

static void type_name(char *dst, size_t cap, CXType t)
{
    copy_str(dst, cap, clang_getTypeSpelling(t));
    if (strcmp(dst, "_Bool") == 0)
        snprintf(dst, cap, "bool");
}

/* Declared in one of the library's own headers? */
static bool in_library(const api_ctx *ctx, CXCursor c)
{
    CXFile file;
    clang_getSpellingLocation(clang_getCursorLocation(c), &file, NULL, NULL, NULL);
    if (!file)
        return false;
    char path[PATH_MAX], real[PATH_MAX];
    copy_str(path, sizeof path, clang_getFileName(file));
    if (!realpath(path, real))
        return false;
    if (ctx->main_only)   /* a header right in /usr/include: its neighbours are libc */
        return strcmp(real, ctx->api->header) == 0;
    return strncmp(real, ctx->dir, ctx->dir_len) == 0 && real[ctx->dir_len] == '/';
}

static enum CXChildVisitResult api_enum_value(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)parent;
    if (clang_getCursorKind(c) == CXCursor_EnumConstantDecl) {
        char name[128];
        copy_str(name, sizeof name, clang_getCursorSpelling(c));
        ccr_api_enum_value((ccr_api_enum *)data, name);
    }
    return CXChildVisit_Continue;
}

/* Bodies are skipped (faster), so clang does not call these definitions:
 * a `{` in the declaration's text says it has one. */
static bool has_body(CXCursor c)
{
    if (clang_isCursorDefinition(c) || clang_Cursor_isFunctionInlined(c))
        return true;
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(c);
    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(tu, clang_getCursorExtent(c), &toks, &n);
    bool body = false;
    for (unsigned i = 0; i < n && !body; ++i)
        if (clang_getTokenKind(toks[i]) == CXToken_Punctuation) {
            CXString sp = clang_getTokenSpelling(tu, toks[i]);
            body = strcmp(clang_getCString(sp), "{") == 0;
            clang_disposeString(sp);
        }
    clang_disposeTokens(tu, toks, n);
    return body;
}

static enum CXChildVisitResult api_visit(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)parent;
    api_ctx *ctx = data;
    const enum CXCursorKind kind = clang_getCursorKind(c);
    if (kind == CXCursor_LinkageSpec)   /* extern "C" { ... } */
        return CXChildVisit_Recurse;
    if (!in_library(ctx, c))
        return CXChildVisit_Continue;

    if (kind == CXCursor_FunctionDecl) {
        char name[128];
        copy_str(name, sizeof name, clang_getCursorSpelling(c));
        const bool body = has_body(c);
        const bool is_static = clang_Cursor_getStorageClass(c) == CX_SC_Static;
        if (!name[0] || name[0] == '_' || (is_static && (!body || ctx->no_static)))
            return CXChildVisit_Continue;
        ccr_api_func *f = ccr_api_add_func(ctx->api, name);
        if (!f)
            return CXChildVisit_Break;
        const CXType t = clang_getCursorType(c);
        type_name(f->ret, sizeof f->ret, clang_getResultType(t));
        const int n = clang_Cursor_getNumArguments(c);
        f->nparams = 0;
        for (int i = 0; i < n && i < CCR_API_MAX_PARAMS; ++i) {
            CXCursor a = clang_Cursor_getArgument(c, (unsigned)i);
            ccr_api_param *p = &f->params[f->nparams++];
            copy_str(p->name, sizeof p->name, clang_getCursorSpelling(a));
            type_name(p->type, sizeof p->type, clang_getCursorType(a));
        }
        f->variadic = clang_isFunctionTypeVariadic(t) != 0;
        f->inline_def = f->inline_def || body;
        if (!f->doc[0])
            copy_str(f->doc, sizeof f->doc, clang_Cursor_getRawCommentText(c));
        return CXChildVisit_Continue;
    }
    if (kind == CXCursor_EnumDecl && clang_isCursorDefinition(c)) {
        char name[128];
        copy_str(name, sizeof name, clang_getCursorSpelling(c));
        const bool anonymous = !name[0] || strchr(name, '(') || clang_Cursor_isAnonymous(c);
        ccr_api_enum *e = ccr_api_add_enum(ctx->api, anonymous ? "" : name);
        if (!e)
            return CXChildVisit_Break;
        clang_visitChildren(c, api_enum_value, e);
        ctx->pending_hash = clang_hashCursor(c);
        ctx->pending_enum = ctx->api->enum_count - 1;
        return CXChildVisit_Continue;
    }
    if (kind == CXCursor_TypedefDecl && ctx->pending_enum >= 0) {
        /* typedef enum { ... } Name; */
        CXType under = clang_getTypedefDeclUnderlyingType(c);
        CXCursor decl = clang_getTypeDeclaration(clang_Type_getNamedType(under).kind != CXType_Invalid
                                                 ? clang_Type_getNamedType(under) : under);
        if (clang_hashCursor(decl) == ctx->pending_hash) {
            ccr_api_enum *e = &ctx->api->enums[ctx->pending_enum];
            if (!e->name[0])
                copy_str(e->name, sizeof e->name, clang_getCursorSpelling(c));
        }
    }
    return CXChildVisit_Continue;
}

static bool parse_api(const char *header, const char *const *args, int arg_count, ccr_api *out, bool own_file)
{
    const char *base[CCR_API_MAX_PARAMS * 4 + 8];
    int n = 0;
    base[n++] = "-x";
    base[n++] = "c";
    base[n++] = "-std=c11";
    base[n++] = "-fparse-all-comments";
    for (int i = 0; i < arg_count && n < (int)(sizeof base / sizeof base[0]) - 2; ++i)
        base[n++] = args[i];
    const char *resource = ccr_parse_resource_dir(NULL);
    if (resource) {
        base[n++] = "-resource-dir";
        base[n++] = resource;
    }

    CXIndex index = clang_createIndex(0, 0);
    if (!index)
        return false;
    CXTranslationUnit tu = NULL;
    enum CXErrorCode err = clang_parseTranslationUnit2(
        index, header, base, n, NULL, 0,
        CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing, &tu);
    if (err != CXError_Success || !tu) {
        clang_disposeIndex(index);
        return false;
    }

    api_ctx ctx = { .api = out, .pending_enum = -1 };
    char real[PATH_MAX];
    if (!realpath(header, real))
        snprintf(real, sizeof real, "%s", header);
    snprintf(out->header, sizeof out->header, "%s", real);
    char *slash = strrchr(real, '/');
    if (slash)
        *slash = '\0';
    snprintf(ctx.dir, sizeof ctx.dir, "%s", real);
    ctx.dir_len = strlen(ctx.dir);
    ctx.main_only = own_file || strcmp(ctx.dir, "/usr/include") == 0 || strcmp(ctx.dir, "/usr/local/include") == 0;
    ctx.no_static = own_file;
    snprintf(out->dir, sizeof out->dir, "%s", ctx.dir);

    clang_visitChildren(clang_getTranslationUnitCursor(tu), api_visit, &ctx);
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return true;
}

bool ccr_parse_api(const char *header, const char *const *args, int arg_count, ccr_api *out)
{
    return parse_api(header, args, arg_count, out, false);
}

bool ccr_parse_file_api(const char *path, const char *const *args, int arg_count, ccr_api *out)
{
    return parse_api(path, args, arg_count, out, true);
}
