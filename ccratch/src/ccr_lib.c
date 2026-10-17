#define _DEFAULT_SOURCE   /* access, mkdir */
#include "ccr_lib.h"
#include "ccr_api.h"
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
#include <sys/stat.h>
#include <unistd.h>

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
    static const char *const known[] = { "call", "text", "params", "defaults", "returns", "types" };
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
    const char *inc = ccr_yaml_str(r, "include", NULL);
    if (inc)
        snprintf(pack.include, sizeof pack.include, "%s", inc);
    else if (pack.header[0])
        snprintf(pack.include, sizeof pack.include, "\"%s\"", pack.header);
    snprintf(pack.link, sizeof pack.link, "%s", ccr_yaml_str(r, "link", ""));
    snprintf(pack.find, sizeof pack.find, "%s", ccr_yaml_str(r, "find_package", ""));
    static const char *const known[] = { "name", "header", "include", "link", "find_package", "color",
                                         "blocks", "enums" };
    for (const ccr_yaml_node *k = r->first; k; k = k->next) {
        bool ok = strncmp(k->key, "name.", 5) == 0;
        for (size_t i = 0; i < sizeof known / sizeof known[0] && !ok; ++i)
            ok = strcmp(k->key, known[i]) == 0;
        if (!ok)
            PROBLEM("%d: unknown key '%s'", k->line, k->key);
    }
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

void ccr_lib_project_dir(const char *project, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s/.ccratch/blocks", project);
}

bool ccr_lib_user_dir(char *out, int cap)
{
    char base[900];
    if (!ccr_config_home("XDG_DATA_HOME", ".local/share", base, sizeof base))
        return false;
    return snprintf(out, (size_t)cap, "%s/ccratch/blocks", base) < cap;
}

void ccr_lib_load(FILE *log)
{
    ccr_lib_load_project(NULL, log);
}

void ccr_lib_load_project(const char *project, FILE *log)
{
    ccr_lib_unload();
    char dirs[5][1024];
    int nd = 0;
    if (project && *project)   /* the project's own packs win */
        ccr_lib_project_dir(project, dirs[nd++], 1024);
    nd += ccr_config_data_dirs("blocks", "CCRATCH_BLOCKS_DIR", dirs + nd, 4);
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

/* "raylib.h" -> "raylib"; "SDL.h" -> "SDL" */
static void stem_of(const char *header, char *out, size_t cap)
{
    const char *base = strrchr(header, '/');
    base = base ? base + 1 : header;
    snprintf(out, cap, "%.*s", (int)strcspn(base, "."), base);
}

/* The library behind it: its binary, checked if found. */
static void check_abi(ccr_api *api, const char *stem, const char *project, char *link, size_t cap)
{
    char bin[1024];
    link[0] = '\0';
    /* raylib.h: libraylib; zlib.h: libz; libpng.h: libpng */
    char names[3][128];
    int n = 0;
    snprintf(names[n++], sizeof names[0], "%s", stem);
    const size_t len = strlen(stem);
    if (len > 3 && strcmp(stem + len - 3, "lib") == 0)
        snprintf(names[n++], sizeof names[0], "%.*s", (int)(len - 3), stem);
    if (len > 3 && strncmp(stem, "lib", 3) == 0)
        snprintf(names[n++], sizeof names[0], "%s", stem + 3);
    for (int i = 0; i < n; ++i)
        if (ccr_api_find_binary(names[i], project, bin, (int)sizeof bin) &&
            ccr_api_check_binary(api, bin) > 0) {
            snprintf(link, cap, "%s", names[i]);
            return;
        }
}

/* `rest` if `path` is inside folder `dir`, else NULL */
static const char *under(const char *path, const char *dir)
{
    const size_t n = strlen(dir);
    if (!n || strncmp(path, dir, n) != 0)
        return NULL;
    if (dir[n - 1] == '/')
        return path + n;
    return path[n] == '/' ? path + n + 1 : NULL;
}

void ccr_lib_include_for(const char *header, const char *project, char *out, int cap)
{
    char real[4096], proj[4096];
    if (!realpath(header, real))
        snprintf(real, sizeof real, "%s", header);
    static const char *const sys[] = { "/usr/local/include", "/usr/include" };
    for (size_t i = 0; i < sizeof sys / sizeof sys[0]; ++i) {
        const char *rest = under(real, sys[i]);
        if (rest && *rest) {
            snprintf(out, (size_t)cap, "<%s>", rest);
            return;
        }
    }
    if (project && *project && realpath(project, proj)) {
        char inc[4200];
        snprintf(inc, sizeof inc, "%s/include", proj);
        const char *rest = under(real, inc);   /* -I PROJECT/include */
        if (!rest)
            rest = under(real, proj);          /* -I PROJECT */
        if (rest && *rest) {
            snprintf(out, (size_t)cap, "\"%s\"", rest);
            return;
        }
    }
    const char *base = strrchr(real, '/');
    snprintf(out, (size_t)cap, "\"%s\"", base ? base + 1 : real);
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* `dir`/`name` (or `name` alone if `dir` is NULL), tidied up, if it is a file */
static bool try_in(const char *dir, const char *name, char *out, int cap)
{
    char p[4200], real[4096];
    if (snprintf(p, sizeof p, "%s%s%s", dir ? dir : "", dir ? "/" : "", name) >= (int)sizeof p ||
        !is_file(p) || !realpath(p, real))
        return false;
    const size_t n = strlen(real);
    if (n >= (size_t)cap)
        return false;
    memcpy(out, real, n + 1);
    return true;
}

bool ccr_lib_find_header(const char *spelled, const char *from, const char *project,
                         const char *const *clang_args, int nargs, char *out, int cap)
{
    char name[1024];
    while (*spelled == ' ' || *spelled == '\t')
        ++spelled;
    snprintf(name, sizeof name, "%s", spelled + (*spelled == '<' || *spelled == '"'));
    size_t n = strlen(name);
    while (n && (name[n - 1] == ' ' || name[n - 1] == '>' || name[n - 1] == '"'))
        name[--n] = '\0';
    if (!n)
        return false;
    const char *base = strrchr(name, '/');
    if (!strchr(base ? base : name, '.') && n + 2 < sizeof name)
        memcpy(name + n, ".h", 3);   /* "zlib": zlib.h */

    if (name[0] == '/')
        return try_in(NULL, name, out, cap);
    if (name[0] == '~' && name[1] == '/')
        return getenv("HOME") && try_in(getenv("HOME"), name + 2, out, cap);

    char dir[4096];
    if (from && *from) {   /* next to the file */
        snprintf(dir, sizeof dir, "%s", from);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else snprintf(dir, sizeof dir, ".");
        if (try_in(dir, name, out, cap))
            return true;
    }
    if (project && *project) {
        if (try_in(project, name, out, cap))
            return true;
        snprintf(dir, sizeof dir, "%s/include", project);
        if (try_in(dir, name, out, cap))
            return true;
    }
    for (int i = 0; i < nargs; ++i) {   /* -Idir, -I dir, -isystem dir */
        const char *a = clang_args[i];
        const char *d = strncmp(a, "-I", 2) == 0 && a[2] ? a + 2
                      : (strcmp(a, "-I") == 0 || strcmp(a, "-isystem") == 0) && i + 1 < nargs
                        ? clang_args[++i] : NULL;
        if (d && try_in(d, name, out, cap))
            return true;
    }
    static const char *const sys[] = {
        "/usr/local/include", "/usr/include", "/usr/include/x86_64-linux-gnu",
        "/usr/include/aarch64-linux-gnu", "/opt/homebrew/include",
    };
    for (size_t i = 0; i < sizeof sys / sizeof sys[0]; ++i)
        if (try_in(sys[i], name, out, cap))
            return true;
    return try_in(NULL, name, out, cap);   /* from where Ccratch was started */
}

bool ccr_lib_make(const char *header, const char *const *clang_args, int nargs,
                  const char *project, FILE *out)
{
    ccr_api api;
    char err[256];
    if (!ccr_api_scan(header, clang_args, nargs, &api, err, (int)sizeof err))
        return false;
    char stem[128], link[128], include[160];
    stem_of(header, stem, sizeof stem);
    check_abi(&api, stem, project, link, sizeof link);
    ccr_lib_include_for(header, project, include, (int)sizeof include);
    ccr_api_write_pack(&api, stem, include, link, out);
    ccr_api_free(&api);
    return true;
}

static bool make_dirs(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", dir);
    for (char *c = p + 1; *c; ++c)
        if (*c == '/') {
            *c = '\0';
            mkdir(p, 0755);
            *c = '/';
        }
    return mkdir(p, 0755) == 0 || access(p, F_OK) == 0;
}

bool ccr_lib_create_pack(const char *header, const char *include, const char *dir,
                         const char *const *clang_args, int nargs, const char *project,
                         char *path, int path_cap, int *count, char *err, int err_cap)
{
    char stem[128];
    stem_of(header, stem, sizeof stem);
    *count = 0;
    if (!make_dirs(dir)) {
        snprintf(err, (size_t)err_cap, "cannot make %s", dir);
        return false;
    }
    if (snprintf(path, (size_t)path_cap, "%s/%s.yaml", dir, stem) >= path_cap) {
        snprintf(err, (size_t)err_cap, "path too long");
        return false;
    }
    if (access(path, F_OK) == 0)
        return true;   /* already made, maybe edited since: kept */

    ccr_api api;
    if (!ccr_api_scan(header, clang_args, nargs, &api, err, err_cap))
        return false;
    char link[128];
    check_abi(&api, stem, project, link, sizeof link);
    FILE *f = fopen(path, "wx");
    if (!f) {
        snprintf(err, (size_t)err_cap, "cannot write %s", path);
        ccr_api_free(&api);
        return false;
    }
    ccr_api_write_pack(&api, stem, include, link, f);
    *count = api.func_count;
    ccr_api_free(&api);
    return fclose(f) == 0;
}
