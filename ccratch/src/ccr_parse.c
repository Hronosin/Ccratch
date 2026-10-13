#include "ccr_parse.h"

#include <clang-c/Index.h>

#include <string.h>

#define CCR_HAVE_OPERATOR_KIND_API \
    (CINDEX_VERSION >= CINDEX_VERSION_ENCODE(0, 64))

typedef struct visit_ctx {
    ccr_ast          *ast;
    CXTranslationUnit tu;
    CXFile            main_file;
    ccr_node_id       parent;
    ccr_node_id       callee_pending;  /* Call whose callee is not seen yet */
} visit_ctx;

/* Interns a CXString and disposes it. */
static ccr_str take(ccr_ast *ast, CXString s)
{
    ccr_str id = ccr_ast_intern(ast, clang_getCString(s));
    clang_disposeString(s);
    return id;
}

/* First token of the cursor's extent: literal text, operator fallback. */
static ccr_str first_token(visit_ctx *ctx, CXCursor c)
{
    CXToken *toks = NULL;
    unsigned n = 0;
    clang_tokenize(ctx->tu, clang_getCursorExtent(c), &toks, &n);
    ccr_str s = 0;
    if (n > 0)
        s = take(ctx->ast, clang_getTokenSpelling(ctx->tu, toks[0]));
    clang_disposeTokens(ctx->tu, toks, n);
    return s;
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
    case CCR_NODE_CAST: {
        ccr_str t = take(ast, clang_getTypeSpelling(clang_getCursorType(c)));
        ast->nodes[id].type = t;
        return;
    }
    case CCR_NODE_OTHER: {
        ccr_str raw = take(ast, clang_getCursorKindSpelling(ck));
        ast->nodes[id].raw = raw;
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

static enum CXChildVisitResult visit(CXCursor c, CXCursor parent, CXClientData data)
{
    (void)parent;
    visit_ctx *ctx = data;

    if (!from_main_file(ctx, c))
        return CXChildVisit_Continue;

    enum CXCursorKind ck = clang_getCursorKind(c);
    ccr_node_kind kind = CCR_NODE_OTHER;
    node_policy policy = classify(ck, &kind);

    /* Transparent wrappers (implicit casts around the callee) are looked
     * through without consuming the pending callee. */
    if (policy != POLICY_TRANSPARENT &&
        (policy == POLICY_SKIP || absorb_into_parent(ctx, c, ck)))
        return CXChildVisit_Continue;

    ccr_node_id saved = ctx->parent;
    if (policy == POLICY_NODE) {
        ccr_node_id id = ccr_ast_add(ctx->ast, kind, ctx->parent);
        if (id == CCR_NONE)
            return CXChildVisit_Break; /* out of memory: stop politely */
        fill_node(ctx, c, ck, id);
        ctx->parent = id;
        if (kind == CCR_NODE_CALL)
            ctx->callee_pending = id;
    }

    clang_visitChildren(c, visit, ctx);
    ctx->parent = saved;
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

    CXIndex index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0,
                                      /*displayDiagnostics=*/0);
    if (!index)
        return false;

    unsigned flags = CXTranslationUnit_DetailedPreprocessingRecord
                   | CXTranslationUnit_KeepGoing;

    CXTranslationUnit tu = NULL;
    enum CXErrorCode err = clang_parseTranslationUnit2(
        index, path, args, arg_count, NULL, 0, flags, &tu);

    if (err != CXError_Success || !tu) {
        clang_disposeIndex(index);
        return false;
    }

    out->source_path = ccr_ast_intern(out, path);

    visit_ctx ctx = { out, tu, clang_getFile(tu, path), CCR_NONE, CCR_NONE };
    ccr_node_id root = ccr_ast_add(out, CCR_NODE_TRANSLATION_UNIT, CCR_NONE);
    if (root != CCR_NONE) {
        out->nodes[root].name = out->source_path;
        ctx.parent = root;
        clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &ctx);
    }

    collect_diags(tu, out);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return root != CCR_NONE;
}
