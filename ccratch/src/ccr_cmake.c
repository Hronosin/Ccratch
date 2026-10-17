#include "ccr_cmake.h"
#include "ccr_template.h"
#include "ccr_theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool ccr_cmake_is_cmake(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const size_t n = strlen(base);
    return strcmp(base, "CMakeLists.txt") == 0 || (n > 6 && strcasecmp(base + n - 6, ".cmake") == 0);
}

/* ------------------------------------------------------------ words */

bool ccr_cmake_is_keyword(const char *w)
{
    static const char *const words[] = {
        "AFTER", "ALIAS", "ALL", "APPEND", "ARCHIVE", "BEFORE", "BOOL", "CACHE", "COMMAND",
        "COMMENT", "COMPONENTS", "CONFIG", "CONFIGURE_DEPENDS", "DEPENDS", "DESCRIPTION",
        "DESTINATION", "DIRECTORY", "EXCLUDE_FROM_ALL", "FATAL_ERROR", "FILEPATH", "FILES", "FORCE",
        "GLOB", "GLOB_RECURSE", "GLOBAL", "HOMEPAGE_URL", "IMPORTED", "IN", "INTERFACE", "INTERNAL",
        "ITEMS", "LANGUAGES", "LIBRARY", "LISTS", "MODULE", "NAME", "NAMES", "NO_DEFAULT_PATH",
        "OBJECT", "OPTIONAL", "OPTIONAL_COMPONENTS", "PARENT_SCOPE", "PATH", "PATHS", "PRIVATE",
        "PROPERTIES", "PROPERTY", "PUBLIC", "QUIET", "RANGE", "REMOVE_ITEM", "REQUIRED", "RUNTIME",
        "SEND_ERROR", "SHARED", "STATIC", "STATUS", "STRING", "SYSTEM", "TARGET", "TARGETS",
        "VERBOSE", "VERSION", "WARNING", "WORKING_DIRECTORY", "ZIP_LISTS", "AUTHOR_WARNING",
        "DEPRECATION", "NOTICE", "DEBUG", "TRACE", "CHECK_START", "CHECK_PASS", "CHECK_FAIL",
        "GIT_REPOSITORY", "GIT_TAG", "URL", "URL_HASH", "SOURCE_DIR", "BINARY_DIR", "SOURCES",
        "WIN32", "MACOSX_BUNDLE",
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
        if (strcmp(w, words[i]) == 0)
            return true;
    return false;
}

int ccr_cmake_category(const char *c)
{
    static const struct { const char *name; int cat; } map[] = {
        { "if", CCR_CAT_CONTROL }, { "elseif", CCR_CAT_CONTROL }, { "else", CCR_CAT_CONTROL },
        { "foreach", CCR_CAT_CONTROL }, { "while", CCR_CAT_CONTROL }, { "function", CCR_CAT_CONTROL },
        { "macro", CCR_CAT_CONTROL }, { "block", CCR_CAT_CONTROL }, { "return", CCR_CAT_CONTROL },
        { "break", CCR_CAT_CONTROL }, { "continue", CCR_CAT_CONTROL }, { "include_guard", CCR_CAT_CONTROL },
        { "cmake_minimum_required", CCR_CAT_TYPE }, { "project", CCR_CAT_TYPE },
        { "add_executable", CCR_CAT_TYPE }, { "add_library", CCR_CAT_TYPE },
        { "add_subdirectory", CCR_CAT_TYPE }, { "add_custom_target", CCR_CAT_TYPE },
        { "add_custom_command", CCR_CAT_TYPE }, { "install", CCR_CAT_TYPE },
        { "enable_testing", CCR_CAT_TYPE }, { "add_test", CCR_CAT_TYPE },
        { "set", CCR_CAT_VARIABLE }, { "unset", CCR_CAT_VARIABLE }, { "option", CCR_CAT_VARIABLE },
        { "list", CCR_CAT_VARIABLE }, { "string", CCR_CAT_VARIABLE }, { "math", CCR_CAT_VARIABLE },
        { "file", CCR_CAT_VARIABLE }, { "get_filename_component", CCR_CAT_VARIABLE },
        { "set_property", CCR_CAT_VARIABLE }, { "get_property", CCR_CAT_VARIABLE },
        { "set_target_properties", CCR_CAT_PREPROC }, { "include_directories", CCR_CAT_PREPROC },
        { "add_compile_options", CCR_CAT_PREPROC }, { "add_compile_definitions", CCR_CAT_PREPROC },
        { "add_definitions", CCR_CAT_PREPROC }, { "add_link_options", CCR_CAT_PREPROC },
        { "find_package", CCR_CAT_LIBRARY }, { "find_library", CCR_CAT_LIBRARY },
        { "find_path", CCR_CAT_LIBRARY }, { "link_libraries", CCR_CAT_LIBRARY },
        { "target_link_libraries", CCR_CAT_LIBRARY }, { "pkg_check_modules", CCR_CAT_LIBRARY },
        { "fetchcontent_declare", CCR_CAT_LIBRARY }, { "fetchcontent_makeavailable", CCR_CAT_LIBRARY },
        { "include", CCR_CAT_LIBRARY },
    };
    char low[64];
    size_t n = 0;
    for (; c[n] && n + 1 < sizeof low; ++n)
        low[n] = (char)tolower((unsigned char)c[n]);
    low[n] = '\0';
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (strcmp(low, map[i].name) == 0)
            return map[i].cat;
    if (strncmp(low, "target_", 7) == 0)
        return CCR_CAT_PREPROC;   /* the settings of a target */
    return CCR_CAT_CALL;
}

/* ------------------------------------------------------------ lexing */

typedef struct span { uint32_t b, e; } span;

typedef struct cmd {
    span     name;
    uint32_t open, close;          /* '(' and ')' */
    uint32_t line;
    uint32_t arg0, nargs;          /* into lex.args */
} cmd;

typedef struct lex {
    const char *s;
    uint32_t    n, i, line;
    cmd        *cmds;
    uint32_t    ncmd, cmd_cap;
    span       *args;
    uint32_t    narg, arg_cap;
    char        err[200];
    uint32_t    err_line;
} lex;

static bool grow(void **p, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need)
        c *= 2;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q)
        return false;
    *p = q;
    *cap = c;
    return true;
}

static void bad(lex *L, const char *msg)
{
    if (!L->err[0]) {
        snprintf(L->err, sizeof L->err, "%s", msg);
        L->err_line = L->line;
    }
}

static void step(lex *L)
{
    if (L->s[L->i] == '\n')
        ++L->line;
    ++L->i;
}

/* [=*[ at i: how many '='; -1 if it is not a bracket opening */
static int bracket_open(const lex *L, uint32_t i)
{
    if (i >= L->n || L->s[i] != '[')
        return -1;
    int eq = 0;
    uint32_t k = i + 1;
    while (k < L->n && L->s[k] == '=') {
        ++eq;
        ++k;
    }
    return k < L->n && L->s[k] == '[' ? eq : -1;
}

/* past ]=*] for a bracket with `eq` '=' opened at i */
static void skip_bracket(lex *L, int eq)
{
    L->i += (uint32_t)eq + 2;
    while (L->i < L->n) {
        if (L->s[L->i] == ']') {
            uint32_t k = L->i + 1;
            int e = 0;
            while (k < L->n && L->s[k] == '=') {
                ++e;
                ++k;
            }
            if (e == eq && k < L->n && L->s[k] == ']') {
                L->i = k + 1;
                return;
            }
        }
        step(L);
    }
    bad(L, "a [[bracket]] that never closes");
}

/* # to the end of the line, or #[[ ... ]] */
static void skip_comment(lex *L)
{
    const int eq = bracket_open(L, L->i + 1);
    if (eq >= 0) {
        ++L->i;
        skip_bracket(L, eq);
        return;
    }
    while (L->i < L->n && L->s[L->i] != '\n')
        ++L->i;
}

static void skip_quoted(lex *L)
{
    step(L);
    while (L->i < L->n && L->s[L->i] != '"') {
        if (L->s[L->i] == '\\' && L->i + 1 < L->n)
            step(L);
        step(L);
    }
    if (L->i >= L->n)
        bad(L, "a \"string\" that never closes");
    else
        ++L->i;
}

static bool push_arg(lex *L, uint32_t b, uint32_t e)
{
    if (!grow((void **)&L->args, &L->arg_cap, L->narg + 1, sizeof *L->args))
        return false;
    L->args[L->narg++] = (span){ b, e };
    return true;
}

/* One argument starting at L->i (not a space, '#' or ')'). */
static void one_arg(lex *L)
{
    const uint32_t b = L->i;
    const char c = L->s[L->i];
    int eq;
    if (c == '"') {
        skip_quoted(L);
    } else if ((eq = bracket_open(L, L->i)) >= 0) {
        skip_bracket(L, eq);
    } else if (c == '(') {
        /* (A OR B) in a condition: one argument, parentheses and all */
        int depth = 0;
        while (L->i < L->n) {
            const char k = L->s[L->i];
            if (k == '"') {
                skip_quoted(L);
                continue;
            }
            if (k == '(') ++depth;
            if (k == ')' && --depth == 0) {
                ++L->i;
                break;
            }
            step(L);
        }
    } else {
        while (L->i < L->n) {
            const char k = L->s[L->i];
            if (k == ' ' || k == '\t' || k == '\n' || k == '\r' || k == '(' || k == ')')
                break;
            if (k == '\\' && L->i + 1 < L->n) {
                L->i += 2;
                continue;
            }
            if (k == '"') {   /* -DNAME="a b": one word */
                skip_quoted(L);
                continue;
            }
            ++L->i;
        }
    }
    if (L->i > b)
        push_arg(L, b, L->i);
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident(char c)       { return isalnum((unsigned char)c) || c == '_'; }

static bool lex_all(lex *L)
{
    L->line = 1;
    while (L->i < L->n && !L->err[0]) {
        const char c = L->s[L->i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            step(L);
            continue;
        }
        if (c == '#') {
            skip_comment(L);
            continue;
        }
        if (!is_ident_start(c)) {
            bad(L, "not a command here");
            break;
        }
        cmd k = { .line = L->line, .arg0 = L->narg };
        k.name.b = L->i;
        while (L->i < L->n && is_ident(L->s[L->i]))
            ++L->i;
        k.name.e = L->i;
        while (L->i < L->n && (L->s[L->i] == ' ' || L->s[L->i] == '\t'))
            ++L->i;
        if (L->i >= L->n || L->s[L->i] != '(') {
            bad(L, "a command without its ( )");
            break;
        }
        k.open = L->i++;
        for (;;) {
            if (L->i >= L->n) {
                bad(L, "a command whose ( never closes");
                break;
            }
            const char a = L->s[L->i];
            if (a == ' ' || a == '\t' || a == '\r' || a == '\n') {
                step(L);
            } else if (a == '#') {
                skip_comment(L);
            } else if (a == ')') {
                k.close = L->i++;
                break;
            } else {
                one_arg(L);
            }
            if (L->err[0])
                break;
        }
        if (L->err[0])
            break;
        k.nargs = L->narg - k.arg0;
        if (!grow((void **)&L->cmds, &L->cmd_cap, L->ncmd + 1, sizeof *L->cmds)) {
            bad(L, "out of memory");
            break;
        }
        L->cmds[L->ncmd++] = k;
    }
    return !L->err[0];
}

/* ----------------------------------------------------------- the tree */

typedef struct frame {
    int         kind;        /* 0 if, 1 scope */
    char        end[24];     /* endif, endforeach... */
    ccr_node_id node;        /* the If (the outermost) or the Scope */
    ccr_node_id cur;         /* the If whose branch is open (elseif) */
    ccr_node_id body;        /* the Block being filled */
    ccr_node_id chain[64];   /* elseif wrappers and Ifs, closed at endif */
    int         nchain;
} frame;

typedef struct build {
    ccr_ast  *ast;
    lex      *L;
    frame     st[128];
    int       depth;
    ccr_node_id top;   /* the hat's Block */
} build;

static ccr_node_id node(build *B, ccr_node_kind kind, ccr_node_id parent, uint32_t b, uint32_t e,
                        uint32_t line)
{
    ccr_node_id id = ccr_ast_add(B->ast, kind, parent);
    if (id == CCR_NONE)
        return CCR_NONE;
    ccr_node *n = &B->ast->nodes[id];
    n->src_b = n->place_b = n->ext_b = b;
    n->src_e = n->place_e = n->ext_e = e;
    n->line = line;
    return id;
}

static ccr_str sub(build *B, uint32_t b, uint32_t e)
{
    char buf[2048];
    uint32_t n = e - b < sizeof buf - 1 ? e - b : (uint32_t)sizeof buf - 1;
    memcpy(buf, B->L->s + b, n);
    buf[n] = '\0';
    return ccr_ast_intern(B->ast, buf);
}

static ccr_node_id text(build *B, ccr_node_id parent, uint32_t b, uint32_t e, uint32_t line)
{
    ccr_node_id t = node(B, CCR_NODE_TEXT, parent, b, e, line);
    if (t != CCR_NONE)
        B->ast->nodes[t].name = sub(B, b, e);
    return t;
}

/* What is inside a command's ( ), spaces trimmed. */
static void inner(const lex *L, const cmd *k, uint32_t *b, uint32_t *e)
{
    *b = k->open + 1;
    *e = k->close;
    while (*b < *e && isspace((unsigned char)L->s[*b]))
        ++*b;
    while (*e > *b && isspace((unsigned char)L->s[*e - 1]))
        --*e;
}

/* Where a block after a command begins: the end of its line (a comment
 * there stays with the command). */
static uint32_t line_end_after(const lex *L, uint32_t at)
{
    uint32_t i = at;
    while (i < L->n && (L->s[i] == ' ' || L->s[i] == '\t'))
        ++i;
    if (i < L->n && L->s[i] == '#') {
        lex tmp = *L;
        tmp.i = i;
        tmp.err[0] = '\0';
        skip_comment(&tmp);
        i = tmp.i;
        while (i < L->n && (L->s[i] == ' ' || L->s[i] == '\t'))
            ++i;
    }
    if (i < L->n && (L->s[i] == '\n' || L->s[i] == '\r'))
        return i;
    return at;   /* more on the same line: if(A) x() endif() */
}

static bool lower_is(const lex *L, span s, const char *word)
{
    const size_t n = strlen(word);
    return s.e - s.b == n && strncasecmp(L->s + s.b, word, n) == 0;
}

static ccr_node_id open_block(build *B, ccr_node_id parent, const cmd *k)
{
    const uint32_t at = line_end_after(B->L, k->close + 1);
    return node(B, CCR_NODE_BLOCK, parent, at, at, k->line);
}

static void close_block(build *B, ccr_node_id blk, const cmd *end)
{
    B->ast->nodes[blk].src_e = B->ast->nodes[blk].place_e = B->ast->nodes[blk].ext_e = end->name.b;
}

static bool one_command(build *B, const cmd *k)
{
    lex *L = B->L;
    frame *f = B->depth ? &B->st[B->depth - 1] : NULL;
    const ccr_node_id parent = f ? f->body : B->top;
    static const char *const scopes[] = { "foreach", "while", "function", "macro", "block" };

    if (lower_is(L, k->name, "if")) {
        if (B->depth == 128)
            return false;
        ccr_node_id s = node(B, CCR_NODE_IF, parent, k->name.b, k->close + 1, k->line);
        uint32_t b, e;
        inner(L, k, &b, &e);
        text(B, s, b, e, k->line);
        frame *nf = &B->st[B->depth++];
        memset(nf, 0, sizeof *nf);
        snprintf(nf->end, sizeof nf->end, "endif");
        nf->node = nf->cur = s;
        nf->body = open_block(B, s, k);
        return true;
    }
    if (lower_is(L, k->name, "elseif") || lower_is(L, k->name, "else")) {
        if (!f || f->kind != 0 || f->body == CCR_NONE)
            return false;
        const ccr_node_id cur = f->cur;
        if (cur == CCR_NONE || B->ast->nodes[cur].last_child != f->body)
            return false;   /* else() after else() */
        close_block(B, f->body, k);
        if (lower_is(L, k->name, "else")) {
            /* else(): a Block, as the third child of the If it belongs to */
            f->body = open_block(B, cur, k);
            f->cur = CCR_NONE;   /* nothing more but endif */
            return true;
        }
        if (f->nchain + 2 > 64)
            return false;
        ccr_node_id wrap = node(B, CCR_NODE_BLOCK, cur, k->name.b, k->name.b, k->line);
        ccr_node_id s = node(B, CCR_NODE_IF, wrap, k->name.b, k->name.b, k->line);
        B->ast->nodes[s].flags |= CCR_FLAG_ELSEIF;
        uint32_t b, e;
        inner(L, k, &b, &e);
        text(B, s, b, e, k->line);
        f->chain[f->nchain++] = wrap;
        f->chain[f->nchain++] = s;
        f->cur = s;
        f->body = open_block(B, s, k);
        return true;
    }
    for (size_t i = 0; i < sizeof scopes / sizeof scopes[0]; ++i)
        if (lower_is(L, k->name, scopes[i])) {
            if (B->depth == 128)
                return false;
            ccr_node_id s = node(B, CCR_NODE_SCOPE, parent, k->name.b, k->close + 1, k->line);
            B->ast->nodes[s].name = sub(B, k->name.b, k->name.e);
            uint32_t b, e;
            inner(L, k, &b, &e);
            text(B, s, b, e, k->line);
            frame *nf = &B->st[B->depth++];
            memset(nf, 0, sizeof *nf);
            nf->kind = 1;
            snprintf(nf->end, sizeof nf->end, "end%s", scopes[i]);
            nf->node = s;
            nf->body = open_block(B, s, k);
            return true;
        }
    if (k->name.e - k->name.b > 3 && strncasecmp(L->s + k->name.b, "end", 3) == 0 &&
        (k->name.e - k->name.b < 24)) {
        char word[24];
        const uint32_t n = k->name.e - k->name.b;
        for (uint32_t i = 0; i < n; ++i)
            word[i] = (char)tolower((unsigned char)L->s[k->name.b + i]);
        word[n] = '\0';
        if (strcmp(word, "endif") == 0 || strcmp(word, "endforeach") == 0 ||
            strcmp(word, "endwhile") == 0 || strcmp(word, "endfunction") == 0 ||
            strcmp(word, "endmacro") == 0 || strcmp(word, "endblock") == 0) {
            if (!f || strcmp(f->end, word) != 0)
                return false;
            close_block(B, f->body, k);
            for (int i = 0; i < f->nchain; ++i) {   /* the elseif()s end here too */
                ccr_node *c = &B->ast->nodes[f->chain[i]];
                c->src_e = c->place_e = c->ext_e = k->name.b;
            }
            ccr_node *s = &B->ast->nodes[f->node];
            s->src_e = s->place_e = s->ext_e = k->close + 1;
            --B->depth;
            return true;
        }
    }

    /* any other command: its name, and its arguments as slots */
    ccr_node_id c = node(B, CCR_NODE_CALL, parent, k->name.b, k->close + 1, k->line);
    if (c == CCR_NONE)
        return false;
    B->ast->nodes[c].name = sub(B, k->name.b, k->name.e);
    for (uint32_t a = 0; a < k->nargs; ++a)
        text(B, c, L->args[k->arg0 + a].b, L->args[k->arg0 + a].e, k->line);
    return true;
}

/* ------------------------------------------------- ranges and indents */

static ccr_str indent_of(build *B, uint32_t at)
{
    const char *s = B->L->s;
    uint32_t b = at;
    while (b > 0 && s[b - 1] != '\n')
        --b;
    for (uint32_t i = b; i < at; ++i)
        if (s[i] != ' ' && s[i] != '\t')
            return 0;   /* something else first on that line */
    return sub(B, b, at);
}

/* The rest of a command's line, if it is a comment. */
static uint32_t trailing_comment(const lex *L, uint32_t at)
{
    uint32_t i = at;
    while (i < L->n && (L->s[i] == ' ' || L->s[i] == '\t'))
        ++i;
    if (i >= L->n || L->s[i] != '#')
        return at;
    lex tmp = *L;
    tmp.i = i;
    tmp.err[0] = '\0';
    skip_comment(&tmp);
    return tmp.i;
}

static void finish_block(build *B, ccr_node_id blk, bool top, const char *unit)
{
    ccr_ast *a = B->ast;
    const char *s = B->L->s;
    uint32_t prev = top ? 0 : a->nodes[blk].src_b;
    bool first = true;
    for (ccr_node_id c = a->nodes[blk].first_child; c != CCR_NONE; c = a->nodes[c].next_sibling) {
        ccr_node *k = &a->nodes[c];
        uint32_t j = prev, nl = 0;
        while (j < k->src_b && isspace((unsigned char)s[j])) {
            nl += s[j] == '\n';
            ++j;
        }
        k->ext_b = j;   /* comments above it, up to the command before */
        if (!first && nl >= 2)
            k->flags |= CCR_FLAG_BLANK_BEFORE;
        k->ext_e = trailing_comment(B->L, k->src_e);
        prev = k->ext_e;
        first = false;
    }
    ccr_node *b = &a->nodes[blk];
    if (top) {
        if (b->first_child != CCR_NONE) {
            b->src_b = b->place_b = a->nodes[b->first_child].ext_b;
            b->src_e = b->place_e = a->nodes[b->last_child].ext_e;
        } else {
            b->src_b = b->src_e = b->place_b = b->place_e = B->L->n;
        }
        b->ext_b = b->src_b;
        b->ext_e = b->src_e;
        b->indent = b->close_indent = 0;
    } else {
        b->close_indent = indent_of(B, b->src_e);
        /* what lies between the last command and end*(): comments */
        uint32_t t0 = prev, t1 = b->src_e;
        while (t0 < t1 && isspace((unsigned char)s[t0])) ++t0;
        while (t1 > t0 && isspace((unsigned char)s[t1 - 1])) --t1;
        if (t1 > t0)
            a->nodes[blk].aux = sub(B, t0, t1);
        b = &a->nodes[blk];
        if (b->first_child != CCR_NONE) {
            b->indent = indent_of(B, a->nodes[b->first_child].src_b);
        } else if (t1 > t0 && indent_of(B, t0)) {
            b->indent = indent_of(B, t0);   /* only comments in it: theirs */
        } else {
            char ind[256];
            snprintf(ind, sizeof ind, "%s%s", ccr_ast_str(a, b->close_indent), unit);
            b->indent = ccr_ast_intern(a, ind);
        }
    }
    /* deeper than the others: kept that way */
    /* copies: interning may move the string arena under a pointer */
    char base[256], mine[256];
    snprintf(base, sizeof base, "%s", ccr_ast_str(a, a->nodes[blk].indent));
    const size_t bl = strlen(base);
    for (ccr_node_id c = a->nodes[blk].first_child; c != CCR_NONE; c = a->nodes[c].next_sibling) {
        const ccr_str ind = indent_of(B, a->nodes[c].src_b);
        snprintf(mine, sizeof mine, "%s", ccr_ast_str(a, ind));
        if (strlen(mine) > bl && strncmp(mine, base, bl) == 0)
            a->nodes[c].extra_indent = ccr_ast_intern(a, mine + bl);
    }
    for (ccr_node_id c = a->nodes[blk].first_child; c != CCR_NONE; c = a->nodes[c].next_sibling)
        for (ccr_node_id k = a->nodes[c].first_child; k != CCR_NONE; k = a->nodes[k].next_sibling)
            if (a->nodes[k].kind == CCR_NODE_BLOCK)   /* bodies; an elseif()'s too */
                finish_block(B, k, false, unit);
}

/* Two spaces or four: what the file uses, from the first command that is
 * inside something (comments do not count: they hold ASCII art). */
static void indent_unit(const build *B, char *out, size_t cap)
{
    const lex *L = B->L;
    for (uint32_t i = 0; i < B->ast->node_count; ++i) {
        const ccr_node *n = &B->ast->nodes[i];
        if (n->kind == CCR_NODE_TRANSLATION_UNIT || n->kind == CCR_NODE_FUNCTION ||
            n->kind == CCR_NODE_BLOCK || n->kind == CCR_NODE_TEXT || n->parent == CCR_NONE)
            continue;
        uint32_t b = n->src_b, depth = 0;
        for (ccr_node_id p = n->parent; p != CCR_NONE; p = B->ast->nodes[p].parent)
            depth += B->ast->nodes[p].kind == CCR_NODE_BLOCK;
        if (depth < 2)
            continue;   /* the file's own commands */
        uint32_t ls = b;
        while (ls > 0 && L->s[ls - 1] != '\n')
            --ls;
        uint32_t w = b - ls;
        if (w == 0)
            continue;
        const uint32_t per = w / (depth - 1);
        if (per >= 1 && per <= 8 && per * (depth - 1) == w) {
            snprintf(out, cap, "%.*s", (int)per, L->s[ls] == '\t' ? "\t\t\t\t\t\t\t\t" : "        ");
            return;
        }
    }
    snprintf(out, cap, "    ");
}

static bool fail_diag(ccr_ast *out, const char *msg, uint32_t line)
{
    ccr_diag d = { .severity = CCR_DIAG_ERROR, .file = out->source_path, .line = line, .column = 1,
                   .message = ccr_ast_intern(out, msg) };
    ccr_ast_add_diag(out, d);
    return false;
}

bool ccr_cmake_parse_text(const char *src, size_t len, const char *name, ccr_ast *out)
{
    out->lang = CCR_LANG_CMAKE;
    if (len >= UINT32_MAX / 2)
        return fail_diag(out, "file too big", 0);
    out->source = malloc(len + 1);
    if (!out->source)
        return false;
    memcpy(out->source, src, len);
    out->source[len] = '\0';
    out->source_len = (uint32_t)len;

    lex L = { .s = out->source, .n = (uint32_t)len };
    build B = { .ast = out, .L = &L };
    bool ok = lex_all(&L);
    if (!ok) {
        fail_diag(out, L.err, L.err_line);
        goto done;
    }

    const ccr_node_id tu = node(&B, CCR_NODE_TRANSLATION_UNIT, CCR_NONE, 0, L.n, 1);
    const ccr_node_id hat = node(&B, CCR_NODE_FUNCTION, tu, 0, L.n, 1);
    if (tu == CCR_NONE || hat == CCR_NONE) {
        ok = false;
        goto done;
    }
    out->nodes[hat].flags |= CCR_FLAG_DEFINITION;
    out->nodes[hat].name = ccr_ast_intern(out, name);
    B.top = node(&B, CCR_NODE_BLOCK, hat, 0, 0, 1);

    for (uint32_t i = 0; i < L.ncmd && ok; ++i) {
        if (!one_command(&B, &L.cmds[i])) {
            char msg[160];
            snprintf(msg, sizeof msg, "%.*s() does not fit here (an end or else without its start?)",
                     (int)(L.cmds[i].name.e - L.cmds[i].name.b), L.s + L.cmds[i].name.b);
            ok = fail_diag(out, msg, L.cmds[i].line);
        }
    }
    if (ok && B.depth > 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s() is missing", B.st[B.depth - 1].end);
        ok = fail_diag(out, msg, out->nodes[B.st[B.depth - 1].node].line);
    }
    if (ok) {
        char unit[16];
        indent_unit(&B, unit, sizeof unit);
        finish_block(&B, B.top, true, unit);
    }
done:
    free(L.cmds);
    free(L.args);
    return ok;
}

bool ccr_cmake_parse_file(const char *path, ccr_ast *out)
{
    out->lang = CCR_LANG_CMAKE;
    out->source_path = ccr_ast_intern(out, path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail_diag(out, "cannot open the file", 0);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 16384;
            char *b = realloc(buf, cap);
            if (!b) {
                free(buf);
                fclose(f);
                return false;
            }
            buf = b;
        }
        const size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0)
            break;
    }
    fclose(f);
    const char *base = strrchr(path, '/');
    const bool ok = ccr_cmake_parse_text(buf ? buf : "", len, base ? base + 1 : path, out);
    free(buf);
    return ok;
}

/* ------------------------------------------------------- new blocks */

ccr_node_id ccr_cmake_build_command(ccr_ast *ast, ccr_node_id parent, const char *spec)
{
    lex L = { .s = spec, .n = (uint32_t)strlen(spec) };
    if (!lex_all(&L) || L.ncmd != 1) {
        free(L.cmds);
        free(L.args);
        return CCR_NONE;
    }
    const cmd *k = &L.cmds[0];
    ccr_node_id c = ccr_template_node(ast, CCR_NODE_CALL, parent);
    if (c != CCR_NONE) {
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(k->name.e - k->name.b), spec + k->name.b);
        ast->nodes[c].name = ccr_ast_intern(ast, buf);
        for (uint32_t a = 0; a < k->nargs; ++a) {
            const span sp = L.args[k->arg0 + a];
            snprintf(buf, sizeof buf, "%.*s", (int)(sp.e - sp.b), spec + sp.b);
            ccr_template_text(ast, c, buf);
        }
    }
    free(L.cmds);
    free(L.args);
    return c;
}

ccr_node_id ccr_cmake_build_scope(ccr_ast *ast, ccr_node_id parent, const char *head,
                                  const char *args, bool with_else)
{
    const bool is_if = strcmp(head, "if") == 0;
    ccr_node_id s = ccr_template_node(ast, is_if ? CCR_NODE_IF : CCR_NODE_SCOPE, parent);
    if (s == CCR_NONE)
        return CCR_NONE;
    ast->nodes[s].name = ccr_ast_intern(ast, head);
    ccr_template_text(ast, s, args);
    ccr_node_id b = ccr_template_node(ast, CCR_NODE_BLOCK, s);
    if (b != CCR_NONE)
        ast->nodes[b].close_indent = 0;
    if (is_if && with_else) {
        b = ccr_template_node(ast, CCR_NODE_BLOCK, s);
        if (b != CCR_NONE)
            ast->nodes[b].close_indent = 0;
    }
    return s;
}
