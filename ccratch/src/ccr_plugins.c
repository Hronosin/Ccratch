#define _GNU_SOURCE   /* RTLD_LOCAL */
#include "ccr_plugins.h"
#include "ccr_asm.h"
#include "ccratch/ccr_plugin.h"

#include "ccr_cmake.h"
#include "ccr_config.h"
#include "ccr_parse.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLUGINS 32

_Static_assert((int)CCR_FIND_ERROR == (int)CCR_FINDING_ERROR && (int)CCR_FIND_PRAISE == (int)CCR_FINDING_PRAISE,
               "ccr_find_level mirrors ccr_finding_level");

#define MAX_SETTINGS 16

typedef struct loaded {
    void             *handle;
    const ccr_plugin *p;
    size_t            size;       /* its struct_size: what it has */
    char              file[256];  /* "code_hater.so" */
    char              path[1100];
    bool              enabled;
    int               nsettings;
    char              values[MAX_SETTINGS][48];   /* each setting's value now */
} loaded;

static loaded g_plugins[MAX_PLUGINS];
static int    g_count;

/* Has the plugin's struct this field? (An older one stops before it.) */
#define HAS(l, field) ((l)->size >= offsetof(ccr_plugin, field) + sizeof((l)->p->field))

static void host_log(ccr_log_level level, const char *plugin, const char *message)
{
    static const char *const names[] = { "debug", "info", "warning", "error" };
    if (level == CCR_LOG_DEBUG && !getenv("CCRATCH_PLUGIN_DEBUG"))
        return;
    fprintf(stderr, "ccratch: plugin %s: %s%s%s\n", plugin ? plugin : "?",
            level != CCR_LOG_INFO ? names[(unsigned)level < 4 ? level : 3] : "", level != CCR_LOG_INFO ? ": " : "",
            message ? message : "");
}

static const ccr_host_api g_host = {
    .abi_version = CCR_PLUGIN_ABI_VERSION,
    .struct_size = sizeof(ccr_host_api),
    .log = host_log,
};

void ccr_plugins_unload(void)
{
    for (int i = 0; i < g_count; ++i) {
        if (g_plugins[i].p && g_plugins[i].p->shutdown)
            g_plugins[i].p->shutdown();
        if (g_plugins[i].handle)
            dlclose(g_plugins[i].handle);
    }
    memset(g_plugins, 0, sizeof g_plugins);
    g_count = 0;
}

static bool ends_with(const char *s, const char *tail)
{
    const size_t n = strlen(s), t = strlen(tail);
    return n > t && strcmp(s + n - t, tail) == 0;
}

static void load_one(const char *dir, const char *file, FILE *log)
{
    if (g_count == MAX_PLUGINS)
        return;
    for (int i = 0; i < g_count; ++i)
        if (strcmp(g_plugins[i].file, file) == 0)
            return;   /* found earlier: that one wins */
    loaded *l = &g_plugins[g_count];
    memset(l, 0, sizeof *l);
    snprintf(l->file, sizeof l->file, "%.255s", file);
    snprintf(l->path, sizeof l->path, "%.800s/%.255s", dir, file);
    l->handle = dlopen(l->path, RTLD_NOW | RTLD_LOCAL);
    if (!l->handle) {
        if (log)
            fprintf(log, "ccratch: plugin %s: %s\n", l->path, dlerror());
        return;
    }
    ccr_plugin_entry_fn entry;
    *(void **)&entry = dlsym(l->handle, CCR_PLUGIN_ENTRY_SYMBOL);
    l->p = entry ? entry() : NULL;
    const char *why = !entry  ? "no " CCR_PLUGIN_ENTRY_SYMBOL "()"
                    : !l->p   ? CCR_PLUGIN_ENTRY_SYMBOL "() gave nothing"
                    : l->p->abi_version != CCR_PLUGIN_ABI_VERSION ? "made for another plugin ABI"
                    : l->p->struct_size < offsetof(ccr_plugin, title) || !l->p->name ? "its ccr_plugin is broken"
                                                                                     : NULL;
    if (!why && l->p->init && !l->p->init(&g_host))
        why = "init() said no";
    if (why) {
        if (log)
            fprintf(log, "ccratch: plugin %s: %s, skipped\n", l->path, why);
        dlclose(l->handle);
        memset(l, 0, sizeof *l);
        return;
    }
    l->size = l->p->struct_size;
    char key[160], v[48];
    snprintf(key, sizeof key, "plugin.%s", l->p->name);
    l->enabled = !(ccr_config_get(key, v, sizeof v) && strcmp(v, "off") == 0);
    /* its settings: what the config says, if it is one of the values; else its default */
    if (HAS(l, setting_count) && l->p->settings)
        for (uint32_t k = 0; k < l->p->setting_count && l->nsettings < MAX_SETTINGS; ++k) {
            const ccr_plugin_setting *st = &l->p->settings[k];
            if (!st->key)
                continue;
            char *val = l->values[l->nsettings++];
            snprintf(val, sizeof l->values[0], "%s", st->def ? st->def : "");
            snprintf(key, sizeof key, "plugin.%s.%s", l->p->name, st->key);
            if (!ccr_config_get(key, v, sizeof v))
                continue;
            bool fits = false;
            if (st->kind == CCR_SETTING_TOGGLE)
                fits = strcmp(v, "on") == 0 || strcmp(v, "off") == 0;
            else
                for (const char *const *c = st->choices; c && *c && !fits; ++c)
                    fits = strcmp(*c, v) == 0;
            if (fits)
                snprintf(val, sizeof l->values[0], "%s", v);
        }
    ++g_count;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

int ccr_plugins_load(FILE *log)
{
    ccr_plugins_unload();
    char dirs[4][1024];
    const int nd = ccr_config_data_dirs("plugins", "CCRATCH_PLUGIN_DIR", dirs, 4);
    for (int d = 0; d < nd; ++d) {
        DIR *dp = opendir(dirs[d]);
        if (!dp)
            continue;
        /* in name order, so the list is the same every time */
        char names[MAX_PLUGINS][256];
        int n = 0;
        for (struct dirent *e; (e = readdir(dp)) != NULL && n < MAX_PLUGINS;)
            if (ends_with(e->d_name, ".so"))
                snprintf(names[n++], sizeof names[0], "%s", e->d_name);
        closedir(dp);
        qsort(names, (size_t)n, sizeof names[0], by_name);
        for (int i = 0; i < n; ++i)
            load_one(dirs[d], names[i], log);
    }
    return g_count;
}

int ccr_plugins_count(void)
{
    return g_count;
}

static const loaded *at(int i)
{
    return i >= 0 && i < g_count ? &g_plugins[i] : NULL;
}

const char *ccr_plugins_name(int i)
{
    return at(i) ? at(i)->p->name : "";
}

const char *ccr_plugins_title(int i)
{
    const loaded *l = at(i);
    if (!l)
        return "";
    return HAS(l, title) && l->p->title ? l->p->title : l->p->name;
}

const char *ccr_plugins_version(int i)
{
    return at(i) && at(i)->p->version ? at(i)->p->version : "";
}

const char *ccr_plugins_author(int i)
{
    const loaded *l = at(i);
    return l && HAS(l, author) && l->p->author ? l->p->author : "";
}

const char *ccr_plugins_path(int i)
{
    return at(i) ? at(i)->path : "";
}

const char *ccr_plugins_describe(int i, const char *ui_language)
{
    const loaded *l = at(i);
    if (!l)
        return "";
    if (HAS(l, describe) && l->p->describe) {
        const char *d = l->p->describe(ui_language ? ui_language : "en");
        if (d && *d)
            return d;
    }
    return l->p->description ? l->p->description : "";
}

uint32_t ccr_plugins_color(int i)
{
    const loaded *l = at(i);
    return l && HAS(l, color) ? l->p->color : 0u;
}

bool ccr_plugins_is_checker(int i)
{
    const loaded *l = at(i);
    return l && HAS(l, check) && l->p->check != NULL;
}

bool ccr_plugins_enabled(int i)
{
    return at(i) && at(i)->enabled;
}

void ccr_plugins_set_enabled(int i, bool on, bool save)
{
    if (!at(i))
        return;
    g_plugins[i].enabled = on;
    if (save) {
        char key[96];
        snprintf(key, sizeof key, "plugin.%s", g_plugins[i].p->name);
        if (!ccr_config_set(key, on ? "on" : "off"))
            fprintf(stderr, "ccratch: could not remember %s in ~/.config/ccratch\n", key);
    }
}

/* ------------------------------------------------------------ settings */

static const ccr_plugin_setting *setting_at(int i, int k)
{
    const loaded *l = at(i);
    if (!l || k < 0 || k >= l->nsettings)
        return NULL;
    /* the k-th with a key (the host skipped those without) */
    int seen = 0;
    for (uint32_t j = 0; j < l->p->setting_count; ++j)
        if (l->p->settings[j].key && seen++ == k)
            return &l->p->settings[j];
    return NULL;
}

int ccr_plugins_setting_count(int i)
{
    return at(i) ? at(i)->nsettings : 0;
}

const char *ccr_plugins_setting_key(int i, int k)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    return st ? st->key : "";
}

bool ccr_plugins_setting_is_toggle(int i, int k)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    return st && st->kind == CCR_SETTING_TOGGLE;
}

/* The plugin's own words for `what`, else `english`. */
static const char *words(int i, const char *what, const char *english, const char *ui_language)
{
    const loaded *l = at(i);
    if (l && HAS(l, setting_text) && l->p->setting_text && ui_language) {
        const char *t = l->p->setting_text(ui_language, what);
        if (t && *t)
            return t;
    }
    return english ? english : "";
}

const char *ccr_plugins_setting_title(int i, int k, const char *ui_language)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    return st ? words(i, st->key, st->title ? st->title : st->key, ui_language) : "";
}

const char *ccr_plugins_setting_hint(int i, int k, const char *ui_language)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    if (!st)
        return "";
    char what[96];
    snprintf(what, sizeof what, "%s.hint", st->key);
    return words(i, what, st->hint, ui_language);
}

int ccr_plugins_setting_choice_count(int i, int k)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    int n = 0;
    if (st && st->kind == CCR_SETTING_CHOICE)
        while (st->choices && st->choices[n])
            ++n;
    return n;
}

const char *ccr_plugins_setting_choice(int i, int k, int c)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    return st && c >= 0 && c < ccr_plugins_setting_choice_count(i, k) ? st->choices[c] : "";
}

const char *ccr_plugins_setting_choice_title(int i, int k, int c, const char *ui_language)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    if (!st || c < 0 || c >= ccr_plugins_setting_choice_count(i, k))
        return "";
    char what[160];
    snprintf(what, sizeof what, "%s=%s", st->key, st->choices[c]);
    return words(i, what, st->choice_titles && st->choice_titles[c] ? st->choice_titles[c] : st->choices[c],
                 ui_language);
}

const char *ccr_plugins_setting_value(int i, int k)
{
    const loaded *l = at(i);
    return l && k >= 0 && k < l->nsettings ? l->values[k] : "";
}

void ccr_plugins_set_setting(int i, int k, const char *value, bool save)
{
    const ccr_plugin_setting *st = setting_at(i, k);
    if (!st || !value)
        return;
    snprintf(g_plugins[i].values[k], sizeof g_plugins[i].values[k], "%s", value);
    if (save) {
        char key[160];
        snprintf(key, sizeof key, "plugin.%s.%s", g_plugins[i].p->name, st->key);
        if (!ccr_config_set(key, value))
            fprintf(stderr, "ccratch: could not remember %s in ~/.config/ccratch\n", key);
    }
}

/* ------------------------------------------------------------ checking */

typedef struct collect {
    ccr_finding *list;
    int          n, cap;
    int          plugin;
    uint32_t     node_count;
} collect;

static const char *on_setting(const ccr_plugin_file *file, const char *key)
{
    const collect *c = file->host_data;
    for (int k = 0; key && k < ccr_plugins_setting_count(c->plugin); ++k)
        if (strcmp(ccr_plugins_setting_key(c->plugin, k), key) == 0)
            return ccr_plugins_setting_value(c->plugin, k);
    return "";
}

static void on_report(const ccr_plugin_file *file, uint32_t node, ccr_finding_level level, const char *title,
                      const char *message);

static void on_poke(const ccr_plugin_file *file, uint32_t node, const char *say)
{
    collect *c = file->host_data;
    if (node >= c->node_count)
        return;
    /* its last finding about that node gets the poke; or one of its own */
    for (int k = c->n - 1; k >= 0; --k)
        if (c->list[k].plugin == c->plugin && c->list[k].node == node) {
            c->list[k].poke = true;
            snprintf(c->list[k].say, sizeof c->list[k].say, "%s", say ? say : "");
            return;
        }
    on_report(file, node, CCR_FINDING_ERROR, say, say);
    if (c->n && c->list[c->n - 1].node == node) {
        c->list[c->n - 1].poke = true;
        snprintf(c->list[c->n - 1].say, sizeof c->list[c->n - 1].say, "%s", say ? say : "");
    }
}

static void on_report(const ccr_plugin_file *file, uint32_t node, ccr_finding_level level, const char *title,
                      const char *message)
{
    collect *c = file->host_data;
    if (node >= c->node_count || (unsigned)level > CCR_FINDING_ERROR || c->n >= 4096)
        return;
    if (c->n == c->cap) {
        const int cap = c->cap ? c->cap * 2 : 16;
        ccr_finding *v = realloc(c->list, (size_t)cap * sizeof *v);
        if (!v)
            return;
        c->list = v;
        c->cap = cap;
    }
    ccr_finding *f = &c->list[c->n++];
    f->plugin = c->plugin;
    f->node = node;
    f->level = (ccr_find_level)level;
    f->poke = false;
    f->say[0] = '\0';
    snprintf(f->title, sizeof f->title, "%s", title ? title : "");
    snprintf(f->message, sizeof f->message, "%s", message ? message : "");
}

static const ccr_ast *g_sort_ast;

static int by_line(const void *a, const void *b)
{
    const ccr_finding *x = a, *y = b;
    const uint32_t lx = g_sort_ast->nodes[x->node].line, ly = g_sort_ast->nodes[y->node].line;
    if (lx != ly)
        return lx < ly ? -1 : 1;
    return (int)y->level - (int)x->level;   /* the worst first */
}

int ccr_plugins_check(const ccr_ast *ast, const char *path, const char *ui_language, bool plain, ccr_finding **out)
{
    *out = NULL;
    bool any = false;
    for (int i = 0; i < g_count; ++i)
        any |= g_plugins[i].enabled && ccr_plugins_is_checker(i);
    if (!any || !ast || ast->node_count == 0)
        return 0;

    /* the tree, as plugins see it */
    ccr_plugin_node *nodes = calloc(ast->node_count, sizeof *nodes);
    if (!nodes)
        return 0;
    uint32_t root = CCR_PLUGIN_NONE;
    for (uint32_t k = 0; k < ast->node_count; ++k) {
        const ccr_node *n = &ast->nodes[k];
        ccr_plugin_node *p = &nodes[k];
        p->kind = ccr_node_kind_name(n->kind);
        p->name = ccr_ast_str(ast, n->name);
        p->type = ccr_ast_str(ast, n->type);
        p->op = ccr_ast_str(ast, n->op);
        p->parent = n->parent;
        p->first_child = n->first_child;
        p->next_sibling = n->next_sibling;
        p->line = n->line;
        p->column = n->column;
        p->flags = ((n->flags & CCR_FLAG_DEFINITION) ? CCR_PNODE_DEFINITION : 0u) |
                   ((n->flags & CCR_FLAG_POSTFIX) ? CCR_PNODE_POSTFIX : 0u) |
                   ((n->flags & CCR_FLAG_ARROW) ? CCR_PNODE_ARROW : 0u) |
                   ((n->flags & CCR_FLAG_INIT) ? CCR_PNODE_INIT : 0u) |
                   ((n->flags & CCR_FLAG_ANGLED) ? CCR_PNODE_ANGLED : 0u) |
                   ((n->flags & CCR_FLAG_ASM) ? CCR_PNODE_ASM : 0u);
        const bool has_text = !(n->flags & CCR_FLAG_NEW) && n->src_e > n->src_b && n->src_e <= ast->source_len;
        p->src_begin = has_text ? n->src_b : 0;
        p->src_end = has_text ? n->src_e : 0;
        if (n->parent == CCR_NONE && n->kind == CCR_NODE_TRANSLATION_UNIT && !(n->flags & (CCR_FLAG_LOOSE | CCR_FLAG_DEAD)) &&
            root == CCR_PLUGIN_NONE)
            root = k;
    }
    /* outside the program: under a loose or dead root, or switched off */
    for (uint32_t k = 0; k < ast->node_count; ++k) {
        bool outside = false;
        for (ccr_node_id a = k; a != CCR_NONE && !outside; a = ast->nodes[a].parent) {
            const ccr_node *n = &ast->nodes[a];
            outside = (n->flags & (CCR_FLAG_COMMENTED | CCR_FLAG_DEAD)) ||
                      (n->parent == CCR_NONE && a != root);
        }
        if (outside)
            nodes[k].flags |= CCR_PNODE_OUTSIDE;
    }

    collect c = { .node_count = ast->node_count };
    ccr_plugin_file file = {
        .struct_size = sizeof file,
        .path = path ? path : "",
        .language = ast->lang == CCR_LANG_CMAKE ? "cmake" : ast->lang == CCR_LANG_ASM ? "asm"
                  : ast->cpp ? "c++" : "c",
        .ui_language = ui_language ? ui_language : "en",
        .plain = plain,
        .nodes = nodes,
        .node_count = ast->node_count,
        .root = root,
        .source = ast->source ? ast->source : "",
        .source_len = ast->source ? ast->source_len : 0,
        .report = on_report,
        .host_data = &c,
        .setting = on_setting,
        .poke = on_poke,
    };
    for (int i = 0; i < g_count; ++i) {
        if (!g_plugins[i].enabled || !ccr_plugins_is_checker(i))
            continue;
        c.plugin = i;
        g_plugins[i].p->check(&file);
    }
    free(nodes);
    if (c.n == 0) {
        free(c.list);
        return 0;
    }
    g_sort_ast = ast;
    qsort(c.list, (size_t)c.n, sizeof *c.list, by_line);
    *out = c.list;
    return c.n;
}

int ccr_plugins_check_file(const char *path, const char *ui_language, FILE *out)
{
    ccr_ast ast;
    if (!ccr_ast_init(&ast))
        return -1;
    bool ok;
    if (ccr_cmake_is_cmake(path)) {
        ccr_cmake_parse_file(path, &ast);
        ok = ast.source != NULL;
    } else if (ccr_asm_is_asm(path)) {
        ccr_asm_parse_file(path, &ast);
        ok = ast.source != NULL;
    } else {
        ok = ccr_parse_file(path, NULL, 0, &ast);
    }
    if (!ok) {
        ccr_ast_free(&ast);
        return -1;
    }
    ccr_finding *f = NULL;
    const int n = ccr_plugins_check(&ast, path, ui_language, false, &f);
    static const char *const level[] = { "praise", "nit", "warning", "error" };
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        const ccr_node *node = &ast.nodes[f[i].node];
        fprintf(out, "%s:%u:%u: %s: [%s] %s\n", path, node->line, node->column, level[f[i].level],
                ccr_plugins_name(f[i].plugin), f[i].title);
        if (f[i].message[0])
            fprintf(out, "    %s\n", f[i].message);
        if (f[i].poke)
            fprintf(out, "    *nose rubbed in it*: %s\n", f[i].say);
        bad += f[i].level >= CCR_FIND_WARNING;
    }
    if (n == 0)
        fprintf(out, "%s: nothing to say\n", path);
    free(f);
    ccr_ast_free(&ast);
    return bad;
}
