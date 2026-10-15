#include "ccr_theme.h"
#include "ccr_config.h"
#include "ccr_lang.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

/* Every key a theme can set: the window's, then 3 per block category. */
enum { CAT_KEYS = CCR_CAT_COUNT * 3, ALL_KEYS = CCR_C_COUNT + CAT_KEYS };

static const char *const g_ui_keys[CCR_C_COUNT] = {
#define X(id, key, def) key,
    CCR_THEME_COLORS(X)
#undef X
};
static const ccr_color g_ui_default[CCR_C_COUNT] = {
#define X(id, key, def) def,
    CCR_THEME_COLORS(X)
#undef X
};
static const char *const g_cat_keys[CCR_CAT_COUNT] = {
#define X(id, key, f, e, t) key,
    CCR_THEME_CATEGORIES(X)
#undef X
};
static const ccr_color g_cat_default[CCR_CAT_COUNT][3] = {
#define X(id, key, f, e, t) { f, e, t },
    CCR_THEME_CATEGORIES(X)
#undef X
};

typedef struct theme {
    ccr_color ui[CCR_C_COUNT];
    ccr_color cat[CCR_CAT_COUNT][3];
    char      code[32];
    char      name[64];
} theme;

static theme g_theme;
static bool  g_ready;

static void builtin(theme *t)
{
    memcpy(t->ui, g_ui_default, sizeof t->ui);
    memcpy(t->cat, g_cat_default, sizeof t->cat);
    snprintf(t->code, sizeof t->code, "shrine");
    t->name[0] = '\0';   /* named by the language pack */
}

static void ready(void)
{
    if (!g_ready) {
        builtin(&g_theme);
        g_ready = true;
    }
}

ccr_color ccr_th(ccr_theme_color c)
{
    ready();
    return (unsigned)c < CCR_C_COUNT ? g_theme.ui[c] : 0xFF00FFFFu;
}

ccr_color ccr_th_cat(ccr_theme_cat cat, ccr_cat_role role)
{
    ready();
    return ((unsigned)cat < CCR_CAT_COUNT && (unsigned)role < 3) ? g_theme.cat[cat][role]
                                                                 : 0xFF00FFFFu;
}

const char *ccr_theme_code(void) { ready(); return g_theme.code; }

const char *ccr_theme_name(void)
{
    ready();
    return g_theme.name[0] ? g_theme.name : ccr_tr(CCR_T_THEME_SHRINE);
}

/* ------------------------------------------------------------- files */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* "#RRGGBB" or "#RRGGBBAA" */
static bool parse_color(const char *s, ccr_color *out)
{
    if (*s == '#')
        ++s;
    size_t n = strlen(s);
    if (n != 6 && n != 8)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    unsigned long v = strtoul(s, NULL, 16);
    *out = n == 6 ? (ccr_color)((v << 8) | 0xFFu) : (ccr_color)v;
    return true;
}

/* Which key: 0..CCR_C_COUNT-1 for the window, then category*3+role. */
static int key_index(const char *key)
{
    for (int i = 0; i < CCR_C_COUNT; ++i)
        if (strcmp(key, g_ui_keys[i]) == 0)
            return i;
    for (int c = 0; c < CCR_CAT_COUNT; ++c) {
        size_t n = strlen(g_cat_keys[c]);
        if (strncmp(key, g_cat_keys[c], n) != 0)
            continue;
        const char *rest = key + n;
        int role = !*rest ? CCR_FILL : strcmp(rest, "_edge") == 0 ? CCR_EDGE
                 : strcmp(rest, "_text") == 0 ? CCR_TEXT : -1;
        if (role >= 0)
            return CCR_C_COUNT + c * 3 + role;
    }
    return -1;
}

static bool code_from_path(const char *path, char *code, size_t cap)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n <= 6 || strcmp(base + n - 6, ".theme") != 0 || n - 6 >= cap)
        return false;
    memcpy(code, base, n - 6);
    code[n - 6] = '\0';
    for (char *c = code; *c; ++c)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
            return false;
    return true;
}

/*
 * Reads a theme over the built-in one. The name is @name.<language in
 * use> if there is one, else @name. Returns the problems found.
 */
static int load(theme *t, const char *path, FILE *log, bool complete, bool *ok)
{
    builtin(t);
    *ok = false;
    if (!code_from_path(path, t->code, sizeof t->code))
        snprintf(t->code, sizeof t->code, "?");
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (log)
            fprintf(log, "%s: cannot open\n", path);
        return 1;
    }
    char lang_key[48], plain_name[64] = "", local_name[64] = "";
    snprintf(lang_key, sizeof lang_key, "@name.%s", ccr_lang_code());
    bool seen[ALL_KEYS] = { false };
    int problems = 0;
    unsigned line_no = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        ++line_no;
        char *k = trim(line);
        if (!*k || *k == '#')
            continue;
        char *eq = strchr(k, '=');
        if (!eq) {
            ++problems;
            if (log)
                fprintf(log, "%s:%u: expected 'key = #RRGGBB'\n", path, line_no);
            continue;
        }
        *eq = '\0';
        char *v = trim(eq + 1);
        k = trim(k);
        if (strcmp(k, "@name") == 0) {
            snprintf(plain_name, sizeof plain_name, "%s", v);
            continue;
        }
        if (strcmp(k, lang_key) == 0) {
            snprintf(local_name, sizeof local_name, "%s", v);
            continue;
        }
        if (*k == '@')
            continue;   /* @name.xx for other languages, @author, @note */
        int idx = key_index(k);
        ccr_color c;
        if (idx < 0 || !parse_color(v, &c)) {
            ++problems;
            if (log)
                fprintf(log, "%s:%u: %s '%s'\n", path, line_no,
                        idx < 0 ? "unknown key" : "not a colour (#RRGGBB or #RRGGBBAA):",
                        idx < 0 ? k : v);
            continue;
        }
        seen[idx] = true;
        if (idx < CCR_C_COUNT)
            t->ui[idx] = c;
        else
            t->cat[(idx - CCR_C_COUNT) / 3][(idx - CCR_C_COUNT) % 3] = c;
    }
    fclose(f);
    snprintf(t->name, sizeof t->name, "%s", local_name[0] ? local_name
                                             : plain_name[0] ? plain_name : t->code);
    if (complete)
        for (int i = 0; i < ALL_KEYS; ++i)
            if (!seen[i]) {
                ++problems;
                if (log) {
                    if (i < CCR_C_COUNT)
                        fprintf(log, "%s: missing '%s'\n", path, g_ui_keys[i]);
                    else
                        fprintf(log, "%s: missing '%s%s'\n", path,
                                g_cat_keys[(i - CCR_C_COUNT) / 3],
                                (const char *const[]){ "", "_edge", "_text" }[(i - CCR_C_COUNT) % 3]);
                }
            }
    *ok = true;
    return problems;
}

int ccr_theme_check(const char *path, bool complete, FILE *out)
{
    theme t;
    bool ok;
    int problems = load(&t, path, out, complete, &ok);
    if (ok && !problems && out)
        fprintf(out, "%s: %s, no problems\n", path, t.name);
    return problems;
}

static int compare_info(const void *a, const void *b)
{
    return strcmp(((const ccr_theme_info *)a)->code, ((const ccr_theme_info *)b)->code);
}

int ccr_theme_scan(ccr_theme_info *out, int cap)
{
    if (cap <= 0)
        return 0;
    int n = 0;
    snprintf(out[n].code, sizeof out[n].code, "shrine");
    snprintf(out[n].name, sizeof out[n].name, "%s", ccr_tr(CCR_T_THEME_SHRINE));
    out[n++].path[0] = '\0';

    char dirs[4][1024];
    int nd = ccr_config_data_dirs("themes", "CCRATCH_THEME_DIR", dirs, 4);
    for (int d = 0; d < nd; ++d) {
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        for (struct dirent *e = readdir(dir); e && n < cap; e = readdir(dir)) {
            char code[32];
            if (!code_from_path(e->d_name, code, sizeof code))
                continue;
            bool seen = false;
            for (int i = 0; i < n && !seen; ++i)
                seen = strcmp(out[i].code, code) == 0;
            if (seen)
                continue;
            ccr_theme_info *ti = &out[n];
            if (snprintf(ti->path, sizeof ti->path, "%s/%s", dirs[d], e->d_name) >= (int)sizeof ti->path)
                continue;
            snprintf(ti->code, sizeof ti->code, "%s", code);
            theme t;
            bool ok;
            load(&t, ti->path, NULL, false, &ok);
            snprintf(ti->name, sizeof ti->name, "%s", t.name);
            ++n;
        }
        closedir(dir);
    }
    if (n > 2)
        qsort(out + 1, (size_t)(n - 1), sizeof *out, compare_info);   /* Shrine stays first */
    return n;
}

bool ccr_theme_use(const char *code, FILE *log)
{
    if (!code || !*code || strcmp(code, "shrine") == 0) {
        builtin(&g_theme);
        g_ready = true;
        return true;
    }
    static ccr_theme_info found[64];
    int n = ccr_theme_scan(found, 64);
    for (int i = 1; i < n; ++i) {
        if (strcmp(found[i].code, code) != 0)
            continue;
        theme t;
        bool ok;
        int problems = load(&t, found[i].path, log, false, &ok);
        if (!ok)
            return false;
        if (problems && log)
            fprintf(log, "ccratch: %d problem%s in %s, the rest of it is used\n", problems,
                    problems == 1 ? "" : "s", found[i].path);
        g_theme = t;
        g_ready = true;
        return true;
    }
    return false;
}

void ccr_theme_pick(const char *cli, FILE *log)
{
    if (cli) {
        if (!ccr_theme_use(cli, log) && log)
            fprintf(log, "ccratch: no theme '%s', staying with the shrine\n", cli);
        return;
    }
    char saved[32];
    if (ccr_config_get("theme", saved, sizeof saved))
        ccr_theme_use(saved, log);
}

bool ccr_theme_save_choice(const char *code)
{
    return ccr_config_set("theme", code);
}
