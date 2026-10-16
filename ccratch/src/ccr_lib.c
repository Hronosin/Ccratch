#include "ccr_lib.h"
#include "ccr_ast.h"
#include "ccr_config.h"
#include "ccr_lang.h"
#include "ccr_parse.h"
#include "ccr_theme.h"
#include "ccr_yaml.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

static ccr_lib_pack   g_packs[CCR_LIB_MAX_PACKS];
static int            g_pack_count;
static ccr_lib_block *g_blocks;
static int            g_block_count, g_block_cap;
static int           *g_sorted;   /* block indexes by (call, nparams) */

int                  ccr_lib_pack_count(void) { return g_pack_count; }
const ccr_lib_pack  *ccr_lib_pack_at(int i)   { return (i >= 0 && i < g_pack_count) ? &g_packs[i] : NULL; }
int                  ccr_lib_block_count(void) { return g_block_count; }
const ccr_lib_block *ccr_lib_block_at(int i)  { return (i >= 0 && i < g_block_count) ? &g_blocks[i] : NULL; }

int ccr_lib_index(const ccr_lib_block *b)
{
    return (b && g_blocks && b >= g_blocks && b < g_blocks + g_block_count) ? (int)(b - g_blocks) : -1;
}

/* ------------------------------------------------------------ reading */

static bool is_ident(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_'))
        return false;
    for (; *s; ++s)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return false;
    return true;
}

/* `key.<language in use>` if the pack has it, else `key`. */
static const char *local(const ccr_yaml_node *map, const char *key, const char *fallback)
{
    char k[64];
    snprintf(k, sizeof k, "%s.%s", key, ccr_lang_code());
    const char *v = ccr_yaml_str(map, k, NULL);
    return v ? v : ccr_yaml_str(map, key, fallback);
}

#define PROBLEM(...)                                        \
    do {                                                    \
        ++problems;                                         \
        if (log) {                                          \
            fprintf(log, "%s:", path);                      \
            fprintf(log, __VA_ARGS__);                      \
            fputc('\n', log);                               \
        }                                                   \
    } while (0)

/* Reads one block of a pack into `b`; the problems found. */
static int read_block(const char *path, const ccr_yaml_node *m, ccr_lib_block *b, FILE *log)
{
    int problems = 0;
    memset(b, 0, sizeof *b);
    if (!m || m->kind != CCR_YAML_MAP) {
        PROBLEM("%d: a block is a mapping (call: ..., text: ...)", m ? m->line : 0);
        return problems;
    }
    static const char *const known[] = { "call", "text", "params", "defaults", "returns" };
    for (const ccr_yaml_node *k = m->first; k; k = k->next) {
        bool ok = strncmp(k->key, "text.", 5) == 0;
        for (size_t i = 0; i < sizeof known / sizeof known[0] && !ok; ++i)
            ok = strcmp(k->key, known[i]) == 0;
        if (!ok)
            PROBLEM("%d: unknown key '%s'", k->line, k->key);
    }

    const char *call = ccr_yaml_str(m, "call", "");
    if (!is_ident(call) || strlen(call) >= sizeof b->call) {
        PROBLEM("%d: 'call' must be a C function name, not '%s'", m->line, call);
        return problems;
    }
    snprintf(b->call, sizeof b->call, "%s", call);
    snprintf(b->returns, sizeof b->returns, "%s", ccr_yaml_str(m, "returns", "void"));

    const ccr_yaml_node *params = ccr_yaml_get(m, "params");
    if (params && params->kind == CCR_YAML_SEQ) {
        for (const ccr_yaml_node *p = params->first; p; p = p->next) {
            if (b->nparams == CCR_LIB_MAX_PARAMS) {
                PROBLEM("%d: %s: more than %d parameters", p->line, call, CCR_LIB_MAX_PARAMS);
                break;
            }
            if (p->kind != CCR_YAML_SCALAR || !is_ident(p->value) || strlen(p->value) >= 32) {
                PROBLEM("%d: %s: a parameter must be a name", p->line, call);
                continue;
            }
            snprintf(b->params[b->nparams++], 32, "%s", p->value);
        }
    } else if (params && params->kind != CCR_YAML_NULL) {
        PROBLEM("%d: %s: 'params' is a list: [a, b]", params->line, call);
    }

    const ccr_yaml_node *defs = ccr_yaml_get(m, "defaults");
    if (defs && defs->kind == CCR_YAML_MAP) {
        for (const ccr_yaml_node *d = defs->first; d; d = d->next) {
            int i = 0;
            while (i < b->nparams && strcmp(b->params[i], d->key) != 0)
                ++i;
            if (i == b->nparams)
                PROBLEM("%d: %s: a default for '%s', which is no parameter", d->line, call, d->key);
            else if (d->kind == CCR_YAML_SCALAR)
                snprintf(b->defaults[i], sizeof b->defaults[i], "%s", d->value);
        }
    } else if (defs && defs->kind != CCR_YAML_NULL) {
        PROBLEM("%d: %s: 'defaults' is a mapping: {a: 1}", defs->line, call);
    }

    /* the text: every {name} a parameter, every parameter somewhere */
    char text[sizeof b->text];
    snprintf(text, sizeof text, "%s", local(m, "text", ""));
    if (!*text)
        snprintf(text, sizeof text, "%s", call);
    bool used[CCR_LIB_MAX_PARAMS] = { false };
    for (const char *c = text; *c; ++c) {
        if (*c != '{')
            continue;
        const char *e = strchr(c, '}');
        if (!e) {
            PROBLEM("%d: %s: a '{' with no '}' in the text", m->line, call);
            break;
        }
        int i = 0;
        while (i < b->nparams && !(strlen(b->params[i]) == (size_t)(e - c - 1) &&
                                   strncmp(b->params[i], c + 1, (size_t)(e - c - 1)) == 0))
            ++i;
        if (i == b->nparams)
            PROBLEM("%d: %s: {%.*s} in the text is no parameter", m->line, call, (int)(e - c - 1), c + 1);
        else
            used[i] = true;
        c = e;
    }
    size_t len = strlen(text);
    for (int i = 0; i < b->nparams; ++i)   /* left out of the text: at the end */
        if (!used[i] && len + strlen(b->params[i]) + 4 < sizeof text)
            len += (size_t)snprintf(text + len, sizeof text - len, " {%s}", b->params[i]);
    snprintf(b->text, sizeof b->text, "%s", text);
    return problems;
}

static bool push_block(const ccr_lib_block *b)
{
    if (g_block_count == g_block_cap) {
        int cap = g_block_cap ? g_block_cap * 2 : 128;
        ccr_lib_block *v = realloc(g_blocks, (size_t)cap * sizeof *v);
        if (!v)
            return false;
        g_blocks = v;
        g_block_cap = cap;
    }
    g_blocks[g_block_count++] = *b;
    return true;
}

/* Reads a pack; with `keep`, into the registry. The problems found. */
static int read_pack(const char *path, bool keep, FILE *log)
{
    int problems = 0;
    ccr_yaml y;
    if (!ccr_yaml_load(path, &y)) {
        PROBLEM("%s", y.error);
        return problems;
    }
    const ccr_yaml_node *r = y.root;
    if (!r || r->kind != CCR_YAML_MAP) {
        PROBLEM("1: a pack is a mapping: name, header, blocks");
        ccr_yaml_free(&y);
        return problems;
    }
    ccr_lib_pack pack;
    memset(&pack, 0, sizeof pack);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(pack.code, sizeof pack.code, "%.*s", (int)(strcspn(base, ".")), base);
    snprintf(pack.name, sizeof pack.name, "%s", local(r, "name", pack.code));
    snprintf(pack.header, sizeof pack.header, "%s", ccr_yaml_str(r, "header", ""));
    pack.category = ccr_theme_cat_from_key(ccr_yaml_str(r, "color", "library"));
    if (pack.category < 0) {
        PROBLEM("%d: color '%s' is no block category (library, call, control, operator...)",
                ccr_yaml_get(r, "color")->line, ccr_yaml_str(r, "color", ""));
        pack.category = CCR_CAT_LIBRARY;
    }
    snprintf(pack.path, sizeof pack.path, "%s", path);
    pack.first = g_block_count;

    const ccr_yaml_node *blocks = ccr_yaml_get(r, "blocks");
    if (!blocks || blocks->kind != CCR_YAML_SEQ) {
        PROBLEM("%d: 'blocks' is a list of blocks", blocks ? blocks->line : 1);
    } else {
        for (const ccr_yaml_node *m = blocks->first; m; m = m->next) {
            ccr_lib_block b;
            int bad = read_block(path, m, &b, log);
            problems += bad;
            if (b.call[0] && keep) {
                b.pack = g_pack_count;
                push_block(&b);
            }
        }
    }
    pack.count = g_block_count - pack.first;
    if (keep && g_pack_count < CCR_LIB_MAX_PACKS)
        g_packs[g_pack_count++] = pack;
    ccr_yaml_free(&y);
    return problems;
}

static int compare_sorted(const void *a, const void *b)
{
    const ccr_lib_block *x = &g_blocks[*(const int *)a], *y = &g_blocks[*(const int *)b];
    int c = strcmp(x->call, y->call);
    return c ? c : x->nparams - y->nparams;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void ccr_lib_unload(void)
{
    free(g_blocks);
    free(g_sorted);
    g_blocks = NULL;
    g_sorted = NULL;
    g_block_count = g_block_cap = g_pack_count = 0;
}

void ccr_lib_load(FILE *log)
{
    ccr_lib_unload();
    char dirs[4][1024];
    int nd = ccr_config_data_dirs("blocks", "CCRATCH_BLOCKS_DIR", dirs, 4);
    for (int d = 0; d < nd; ++d) {
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        /* sorted by name, so packs keep their order */
        char names[CCR_LIB_MAX_PACKS * 2][256];
        int n = 0;
        for (struct dirent *e = readdir(dir); e && n < CCR_LIB_MAX_PACKS * 2; e = readdir(dir)) {
            size_t len = strlen(e->d_name);
            if (len > 5 && len < 256 && strcmp(e->d_name + len - 5, ".yaml") == 0)
                memcpy(names[n++], e->d_name, len + 1);
        }
        closedir(dir);
        qsort(names, (size_t)n, sizeof names[0], compare_names);
        for (int i = 0; i < n && g_pack_count < CCR_LIB_MAX_PACKS; ++i) {
            char code[256];
            snprintf(code, sizeof code, "%.*s", (int)(strlen(names[i]) - 5), names[i]);
            bool seen = false;   /* the same pack in two places: the first wins */
            for (int k = 0; k < g_pack_count && !seen; ++k)
                seen = strcmp(g_packs[k].code, code) == 0;
            if (seen)
                continue;
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", dirs[d], names[i]);
            int problems = read_pack(path, true, NULL);
            if (problems && log)
                fprintf(log, "ccratch: %d problem%s in %s (ccratch --check-blocks says which)\n",
                        problems, problems == 1 ? "" : "s", path);
        }
    }
    g_sorted = malloc((size_t)(g_block_count ? g_block_count : 1) * sizeof *g_sorted);
    if (g_sorted) {
        for (int i = 0; i < g_block_count; ++i)
            g_sorted[i] = i;
        qsort(g_sorted, (size_t)g_block_count, sizeof *g_sorted, compare_sorted);
    }
}

const ccr_lib_block *ccr_lib_find(const char *name, int nargs)
{
    if (!g_sorted || !name || !*name)
        return NULL;
    int lo = 0, hi = g_block_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const ccr_lib_block *b = &g_blocks[g_sorted[mid]];
        int c = strcmp(name, b->call);
        if (!c)
            c = nargs - b->nparams;
        if (!c)
            return b;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

bool ccr_lib_pack_matches(const ccr_lib_pack *p, const char *include)
{
    if (!p->header[0])
        return true;
    const size_t n = strlen(include), h = strlen(p->header);
    /* "raylib.h", "../raylib.h", "external/raylib.h" */
    return n >= h && strcmp(include + n - h, p->header) == 0 &&
           (n == h || include[n - h - 1] == '/');
}

int ccr_lib_check(const char *path, FILE *out)
{
    int problems = read_pack(path, false, out);
    if (!problems && out)
        fprintf(out, "%s: no problems\n", path);
    return problems;
}

/* ------------------------------------------------------------ drafts */

/* DrawTextEx -> "draw text ex"; SDL_CreateWindow -> "SDL create window" */
static void humanize(const char *name, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *c = name; *c && n + 2 < cap; ++c) {
        if (*c == '_') {
            if (n && out[n - 1] != ' ')
                out[n++] = ' ';
            continue;
        }
        const bool upper = isupper((unsigned char)*c);
        const bool after_lower = c > name && (islower((unsigned char)c[-1]) || isdigit((unsigned char)c[-1]));
        const bool before_lower = upper && c > name && isupper((unsigned char)c[-1]) && islower((unsigned char)c[1]);
        if ((upper && after_lower) || before_lower)
            if (n && out[n - 1] != ' ')
                out[n++] = ' ';
        out[n++] = *c;
    }
    out[n] = '\0';
    /* words in lower case, except ones that are all capitals (SDL, GL) */
    for (char *w = out; *w;) {
        char *e = w;
        bool caps = true;
        int len = 0;
        while (*e && *e != ' ') {
            caps = caps && !islower((unsigned char)*e);
            ++e;
            ++len;
        }
        if (!caps || len == 1)
            for (char *c = w; c < e; ++c)
                *c = (char)tolower((unsigned char)*c);
        w = *e ? e + 1 : e;
    }
}

bool ccr_lib_make(const char *header, const char *const *clang_args, int nargs, FILE *out)
{
    ccr_ast ast;
    if (!ccr_ast_init(&ast))
        return false;
    if (!ccr_parse_file(header, clang_args, nargs, &ast)) {
        ccr_ast_free(&ast);
        return false;
    }
    const char *base = strrchr(header, '/');
    base = base ? base + 1 : header;
    char name[128];
    snprintf(name, sizeof name, "%.*s", (int)strcspn(base, "."), base);
    fprintf(out, "# Blocks for %s, a first draft by ccratch --make-blocks.\n"
                 "# Make the texts read well ({name} is a slot), add defaults, drop what\n"
                 "# nobody needs; text.ru: and friends translate a text.\n"
                 "name: %s\nheader: %s\ncolor: library\nblocks:\n", base, name, base);
    int made = 0;
    for (ccr_node_id c = ast.node_count ? ast.nodes[0].first_child : CCR_NONE; c != CCR_NONE;
         c = ast.nodes[c].next_sibling) {
        const ccr_node *f = &ast.nodes[c];
        if (f->kind != CCR_NODE_FUNCTION || (f->flags & CCR_FLAG_DEFINITION))
            continue;
        const char *fname = ccr_ast_str(&ast, f->name);
        if (!is_ident(fname))
            continue;
        char words[256], text[512] = "", params[512] = "";
        humanize(fname, words, sizeof words);
        size_t tl = (size_t)snprintf(text, sizeof text, "%s", words), pl = 0;
        int k = 0;
        for (ccr_node_id p = f->first_child; p != CCR_NONE; p = ast.nodes[p].next_sibling) {
            if (ast.nodes[p].kind != CCR_NODE_PARAM)
                continue;
            char pn[40];
            const char *given = ccr_ast_str(&ast, ast.nodes[p].name);
            if (is_ident(given) && strlen(given) < 32)
                snprintf(pn, sizeof pn, "%s", given);
            else
                snprintf(pn, sizeof pn, "arg%d", k + 1);
            if (tl < sizeof text)
                tl += (size_t)snprintf(text + tl, sizeof text - tl, " {%s}", pn);
            if (pl < sizeof params)
                pl += (size_t)snprintf(params + pl, sizeof params - pl, "%s%s", k ? ", " : "", pn);
            ++k;
        }
        if (strstr(ccr_ast_str(&ast, f->type), "...")) {   /* printf-like: the rest as text */
            if (tl < sizeof text)
                tl += (size_t)snprintf(text + tl, sizeof text - tl, " {more}");
            if (pl < sizeof params)
                pl += (size_t)snprintf(params + pl, sizeof params - pl, "%smore", k ? ", " : "");
        }
        const char *ret = ccr_ast_str(&ast, f->aux);
        if (strcmp(ret, "_Bool") == 0)
            ret = "bool";
        fprintf(out, "  - call: %s\n    text: \"%s\"\n", fname, text);
        if (pl)
            fprintf(out, "    params: [%s]\n", params);
        if (*ret && strcmp(ret, "void") != 0)
            fprintf(out, "    returns: \"%s\"\n", ret);
        ++made;
    }
    if (!made)
        fprintf(out, "  []   # no function declarations found\n");
    ccr_ast_free(&ast);
    return true;
}
