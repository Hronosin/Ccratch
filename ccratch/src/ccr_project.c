#define _DEFAULT_SOURCE   /* realpath, DT_DIR */
#include "ccr_project.h"
#include "ccr_api.h"
#include "ccr_parse.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_FILES 400

/* ------------------------------------------------------- the cache */

/* What a file declared the last time it was read, while it is unchanged. */
typedef struct cached_file {
    char    path[1024];
    long    mtime, size;
    ccr_api api;
    bool    used;
} cached_file;

static cached_file    *g_cache;
static int             g_cache_n, g_cache_cap;
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static const ccr_api *cached(const char *path, const char *const *args, int nargs)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return NULL;
    for (int i = 0; i < g_cache_n; ++i) {
        cached_file *c = &g_cache[i];
        if (strcmp(c->path, path) != 0)
            continue;
        if (c->mtime == (long)st.st_mtime && c->size == (long)st.st_size) {
            c->used = true;
            return &c->api;
        }
        ccr_api_free(&c->api);
        memset(&c->api, 0, sizeof c->api);
        if (!ccr_parse_file_api(path, args, nargs, &c->api))
            memset(&c->api, 0, sizeof c->api);
        c->mtime = (long)st.st_mtime;
        c->size = (long)st.st_size;
        c->used = true;
        return &c->api;
    }
    if (g_cache_n == g_cache_cap) {
        const int cap = g_cache_cap ? g_cache_cap * 2 : 64;
        cached_file *v = realloc(g_cache, (size_t)cap * sizeof *v);
        if (!v)
            return NULL;
        g_cache = v;
        g_cache_cap = cap;
    }
    cached_file *c = &g_cache[g_cache_n++];
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s", path);
    if (!ccr_parse_file_api(path, args, nargs, &c->api))
        memset(&c->api, 0, sizeof c->api);
    c->mtime = (long)st.st_mtime;
    c->size = (long)st.st_size;
    c->used = true;
    return &c->api;
}

/* ------------------------------------------------------- the files */

static bool skip_dir(const char *name)
{
    return name[0] == '.' || strcmp(name, "build") == 0 || strncmp(name, "build-", 6) == 0 ||
           strncmp(name, "cmake-build", 11) == 0 || strcmp(name, "_deps") == 0 || strcmp(name, "out") == 0 ||
           strcmp(name, "node_modules") == 0 || strcmp(name, "CMakeFiles") == 0;
}

static bool ends_with(const char *s, const char *tail)
{
    const size_t n = strlen(s), t = strlen(tail);
    return n > t && strcmp(s + n - t, tail) == 0;
}

static void walk(const char *dir, int depth, char (*out)[1024], int *n, bool headers)
{
    if (depth > 8 || *n >= MAX_FILES)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    for (struct dirent *e = readdir(d); e && *n < MAX_FILES; e = readdir(d)) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p)
            continue;
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!skip_dir(e->d_name))
                walk(p, depth + 1, out, n, headers);
        } else if (S_ISREG(st.st_mode) && ends_with(e->d_name, headers ? ".h" : ".c")) {
            snprintf(out[(*n)++], 1024, "%s", p);
        }
    }
    closedir(d);
}

static int by_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* ------------------------------------------------------- the blocks */

static bool is_ident(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_'))
        return false;
    for (; *s; ++s)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return false;
    return true;
}

/* A function as a block that reads as its call does: name ( a , b ). */
static void make_block(const ccr_api_func *f, const char *include, ccr_lib_block *b)
{
    memset(b, 0, sizeof *b);
    snprintf(b->call, sizeof b->call, "%s", f->name);
    snprintf(b->returns, sizeof b->returns, "%.63s", f->ret[0] ? f->ret : "void");
    snprintf(b->include, sizeof b->include, "%s", include);
    size_t len = (size_t)snprintf(b->text, sizeof b->text, "%s (", f->name);
    for (int k = 0; k < f->nparams && k < CCR_LIB_MAX_PARAMS; ++k) {
        char name[32];
        const char *pn = f->params[k].name;
        while (*pn == '_')
            ++pn;
        bool twice = false;
        for (int j = 0; j < b->nparams; ++j)
            twice = twice || strcmp(b->params[j], pn) == 0;
        if (is_ident(pn) && !twice && strlen(pn) < sizeof name)
            snprintf(name, sizeof name, "%.31s", pn);
        else
            snprintf(name, sizeof name, "arg%d", k + 1);
        snprintf(b->params[b->nparams], sizeof b->params[0], "%s", name);
        snprintf(b->types[b->nparams], sizeof b->types[0], "%.63s", f->params[k].type);
        ++b->nparams;
        if (len < sizeof b->text)
            len += (size_t)snprintf(b->text + len, sizeof b->text - len, "%s {%s}", k ? " ," : "", name);
    }
    if (len < sizeof b->text)
        snprintf(b->text + len, sizeof b->text - len, " )");
}

static int find_block(const ccr_project_result *r, const char *name)
{
    for (int i = 0; i < r->count; ++i)
        if (strcmp(r->blocks[i].call, name) == 0)
            return i;
    return -1;
}

/* How a file of the project includes `header`: its path from the project,
 * or from its include/ folder. */
static void include_of(const char *root, const char *header, char *out, size_t cap)
{
    const size_t n = strlen(root);
    const char *rel = strncmp(header, root, n) == 0 && header[n] == '/' ? header + n + 1 : header;
    if (strncmp(rel, "include/", 8) == 0)
        rel += 8;
    /* most projects put -Iinclude or include headers beside the .c: the name
     * alone reads in both, and a deeper path keeps its folders */
    snprintf(out, cap, "\"%s\"", rel);
}

static void add_api(ccr_project_result *r, const ccr_api *api, const char *include, int *cap)
{
    for (int i = 0; i < api->func_count; ++i) {
        const ccr_api_func *f = &api->funcs[i];
        if (strcmp(f->name, "main") == 0 || f->variadic || find_block(r, f->name) >= 0)
            continue;   /* main is no block; a header's declaration came first */
        if (r->count == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            ccr_lib_block *v = realloc(r->blocks, (size_t)*cap * sizeof *v);
            if (!v)
                return;
            r->blocks = v;
        }
        make_block(f, include, &r->blocks[r->count++]);
    }
    for (int i = 0; i < api->enum_count; ++i) {
        const ccr_api_enum *e = &api->enums[i];
        if (!e->name[0] || !e->count)
            continue;
        ccr_project_enum *v = realloc(r->enums, (size_t)(r->enum_count + 1) * sizeof *v);
        if (!v)
            return;
        r->enums = v;
        ccr_project_enum *pe = &r->enums[r->enum_count];
        snprintf(pe->name, sizeof pe->name, "%s", e->name);
        pe->values = calloc((size_t)e->count, sizeof *pe->values);
        if (!pe->values)
            continue;
        pe->count = e->count;
        for (int k = 0; k < e->count; ++k)
            snprintf(pe->values[k], sizeof pe->values[k], "%s", e->values[k]);
        ++r->enum_count;
    }
}

bool ccr_project_read(const char *root, const char *const *clang_args, int nargs, ccr_project_result *out)
{
    memset(out, 0, sizeof *out);
    char real[1024];
    if (!root || !realpath(root, real))
        return false;
    static char files[MAX_FILES][1024];   /* only ever one scan at a time: the lock */
    pthread_mutex_lock(&g_cache_lock);
    int n = 0;
    walk(real, 0, files, &n, true);   /* headers first: their declarations win */
    const int headers = n;
    qsort(files, (size_t)headers, sizeof files[0], by_path);
    walk(real, 0, files, &n, false);
    qsort(files + headers, (size_t)(n - headers), sizeof files[0], by_path);
    for (int i = 0; i < g_cache_n; ++i)
        g_cache[i].used = false;
    /* -I the project and its include/: a header there reads */
    const char *args[96];
    int na = 0;
    char inc1[1100], inc2[1100];
    snprintf(inc1, sizeof inc1, "-I%s", real);
    snprintf(inc2, sizeof inc2, "-I%s/include", real);
    args[na++] = inc1;
    args[na++] = inc2;
    for (int i = 0; i < nargs && na < 94; ++i)
        args[na++] = clang_args[i];
    int cap = 0;
    for (int i = 0; i < n; ++i) {
        const ccr_api *api = cached(files[i], args, na);
        if (!api)
            continue;
        char include[200] = "";
        if (i < headers)
            include_of(real, files[i], include, sizeof include);
        add_api(out, api, include, &cap);
    }
    out->files = n;
    pthread_mutex_unlock(&g_cache_lock);
    return true;
}

void ccr_project_result_free(ccr_project_result *r)
{
    free(r->blocks);
    for (int i = 0; i < r->enum_count; ++i)
        free(r->enums[i].values);
    free(r->enums);
    memset(r, 0, sizeof *r);
}

void ccr_project_apply(const ccr_project_result *r, const char *name)
{
    for (int i = 0; i < r->enum_count; ++i) {
        const char *vals[256];
        int k = 0;
        for (; k < r->enums[i].count && k < 256; ++k)
            vals[k] = r->enums[i].values[k];
        ccr_lib_add_enum(r->enums[i].name, vals, k);
    }
    ccr_lib_set_project(name, r->blocks, r->count);
}

/* ------------------------------------------------------- the thread */

static struct {
    pthread_mutex_t lock;
    bool            running, again, done;
    char            root[1024];
    char           *args[64];
    int             nargs;
    ccr_project_result result;
    pthread_t       thread;
    bool            joinable;
} g_scan = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *scan_main(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_scan.lock);
        char root[1024];
        snprintf(root, sizeof root, "%s", g_scan.root);
        const char *args[64];
        const int nargs = g_scan.nargs;
        char *copies[64];
        for (int i = 0; i < nargs; ++i)
            copies[i] = strdup(g_scan.args[i] ? g_scan.args[i] : "");
        g_scan.again = false;
        pthread_mutex_unlock(&g_scan.lock);
        for (int i = 0; i < nargs; ++i)
            args[i] = copies[i] ? copies[i] : "";
        ccr_project_result r;
        const bool ok = ccr_project_read(root, args, nargs, &r);
        for (int i = 0; i < nargs; ++i)
            free(copies[i]);
        pthread_mutex_lock(&g_scan.lock);
        if (g_scan.again) {   /* asked again meanwhile: this one is old already */
            if (ok)
                ccr_project_result_free(&r);
            pthread_mutex_unlock(&g_scan.lock);
            continue;
        }
        ccr_project_result_free(&g_scan.result);
        if (ok)
            g_scan.result = r;
        g_scan.done = true;
        g_scan.running = false;
        pthread_mutex_unlock(&g_scan.lock);
        return NULL;
    }
}

void ccr_project_scan_start(const char *root, const char *const *clang_args, int nargs)
{
    pthread_mutex_lock(&g_scan.lock);
    snprintf(g_scan.root, sizeof g_scan.root, "%s", root);
    for (int i = 0; i < g_scan.nargs; ++i)
        free(g_scan.args[i]);
    g_scan.nargs = 0;
    for (int i = 0; i < nargs && i < 64; ++i)
        g_scan.args[g_scan.nargs++] = strdup(clang_args[i]);
    if (g_scan.running) {
        g_scan.again = true;
        pthread_mutex_unlock(&g_scan.lock);
        return;
    }
    if (g_scan.joinable)
        pthread_join(g_scan.thread, NULL);
    g_scan.joinable = false;
    g_scan.running = true;
    g_scan.done = false;
    if (pthread_create(&g_scan.thread, NULL, scan_main, NULL) == 0)
        g_scan.joinable = true;
    else
        g_scan.running = false;
    pthread_mutex_unlock(&g_scan.lock);
}

bool ccr_project_scan_take(ccr_project_result *out)
{
    pthread_mutex_lock(&g_scan.lock);
    const bool done = g_scan.done;
    if (done) {
        *out = g_scan.result;
        memset(&g_scan.result, 0, sizeof g_scan.result);
        g_scan.done = false;
    }
    pthread_mutex_unlock(&g_scan.lock);
    if (done && g_scan.joinable) {
        pthread_join(g_scan.thread, NULL);
        g_scan.joinable = false;
    }
    return done;
}

void ccr_project_shutdown(void)
{
    pthread_mutex_lock(&g_scan.lock);
    const bool join = g_scan.joinable;
    pthread_mutex_unlock(&g_scan.lock);
    if (join)
        pthread_join(g_scan.thread, NULL);   /* a scan finishes: it cannot be stopped halfway */
    g_scan.joinable = false;
    g_scan.running = false;
    ccr_project_result_free(&g_scan.result);
    for (int i = 0; i < g_scan.nargs; ++i)
        free(g_scan.args[i]);
    g_scan.nargs = 0;
    pthread_mutex_lock(&g_cache_lock);
    for (int i = 0; i < g_cache_n; ++i)
        ccr_api_free(&g_cache[i].api);
    free(g_cache);
    g_cache = NULL;
    g_cache_n = g_cache_cap = 0;
    pthread_mutex_unlock(&g_cache_lock);
}
