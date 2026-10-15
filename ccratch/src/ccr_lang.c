#define _GNU_SOURCE   /* readlink */
#include "ccr_lang.h"
#include "ccr_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define MAX_FORMS      4
#define MAX_PACK_SIZE  (1u << 20)

typedef struct entry {
    const char *key;
    bool        plural;
    const char *en;
} entry;

static const entry g_entries[CCR_T_COUNT] = {
#define X(id, key, plural, en) [CCR_T_##id] = { key, plural, en },
    CCR_STRINGS(X)
#undef X
};

static const struct { const char *name; ccr_plural_rule rule; int forms; } g_rules[] = {
    { "germanic", CCR_PLURAL_ONE_OTHER, 2 },
    { "one-other", CCR_PLURAL_ONE_OTHER, 2 },
    { "slavic",   CCR_PLURAL_SLAVIC,    3 },
    { "polish",   CCR_PLURAL_POLISH,    3 },
    { "czech",    CCR_PLURAL_CZECH,     3 },
    { "french",   CCR_PLURAL_FRENCH,    2 },
    { "none",     CCR_PLURAL_NONE,      1 },
};

static int forms_of(ccr_plural_rule r)
{
    for (size_t i = 0; i < sizeof g_rules / sizeof g_rules[0]; ++i)
        if (g_rules[i].rule == r)
            return g_rules[i].forms;
    return 2;
}

static int form_for(ccr_plural_rule r, unsigned long n)
{
    const unsigned long m10 = n % 10, m100 = n % 100;
    switch (r) {
    case CCR_PLURAL_ONE_OTHER: return n == 1 ? 0 : 1;
    case CCR_PLURAL_FRENCH:    return n <= 1 ? 0 : 1;
    case CCR_PLURAL_NONE:      return 0;
    case CCR_PLURAL_CZECH:     return n == 1 ? 0 : (n >= 2 && n <= 4) ? 1 : 2;
    case CCR_PLURAL_SLAVIC:
        if (m10 == 1 && m100 != 11)
            return 0;
        return (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? 1 : 2;
    case CCR_PLURAL_POLISH:
        if (n == 1)
            return 0;
        return (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? 1 : 2;
    case CCR_PLURAL_RULE_COUNT:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------ tables */

/* One language: every string split into its plural forms. */
typedef struct table {
    char           *arena;                       /* owns the text */
    const char     *form[CCR_T_COUNT][MAX_FORMS];
    int             nforms[CCR_T_COUNT];         /* 0: not given, use English */
    ccr_plural_rule rule;
    char            code[16];
    char            name[64];
} table;

static table g_en;      /* built-in English, made on first use */
static table g_pack;    /* the pack in use, if any */
static bool  g_en_ready, g_pack_used;

/* Splits "a|b|c" (in place) into forms. */
static int split_forms(char *s, const char **out)
{
    int n = 0;
    out[n++] = s;
    for (char *p = s; *p; ++p)
        if (*p == '|' && n < MAX_FORMS) {
            *p = '\0';
            out[n++] = p + 1;
        }
    return n;
}

static void english_ready(void)
{
    if (g_en_ready)
        return;
    size_t total = 0;
    for (int i = 0; i < CCR_T_COUNT; ++i)
        total += strlen(g_entries[i].en) + 1;
    g_en.arena = malloc(total);
    char *p = g_en.arena;
    for (int i = 0; i < CCR_T_COUNT; ++i) {
        const char *en = g_entries[i].en;
        if (!p) {                       /* out of memory: unsplit, still readable */
            g_en.form[i][0] = en;
            g_en.nforms[i] = 1;
            continue;
        }
        size_t n = strlen(en) + 1;
        memcpy(p, en, n);
        g_en.nforms[i] = g_entries[i].plural ? split_forms(p, g_en.form[i]) : 1;
        if (!g_entries[i].plural)
            g_en.form[i][0] = p;
        p += n;
    }
    g_en.rule = CCR_PLURAL_ONE_OTHER;
    snprintf(g_en.code, sizeof g_en.code, "en");
    snprintf(g_en.name, sizeof g_en.name, "English");
    g_en_ready = true;
}

static const char *lookup(ccr_text id, unsigned long n)
{
    english_ready();
    if ((unsigned)id >= CCR_T_COUNT)
        return "";
    const table *t = (g_pack_used && g_pack.nforms[id] > 0) ? &g_pack : &g_en;
    int f = g_entries[id].plural ? form_for(t->rule, n) : 0;
    if (f >= t->nforms[id])
        f = t->nforms[id] - 1;
    return t->form[id][f];
}

const char *ccr_tr(ccr_text id)                    { return lookup(id, 1); }
const char *ccr_trn(ccr_text id, unsigned long n)  { return lookup(id, n); }

int ccr_trf(char *out, size_t cap, ccr_text id, ...)
{
    va_list ap;
    va_start(ap, id);
    int r = vsnprintf(out, cap, ccr_tr(id), ap);   /* formats checked on load */
    va_end(ap);
    return r;
}

int ccr_trnf(char *out, size_t cap, ccr_text id, unsigned long n, ...)
{
    va_list ap;
    va_start(ap, n);
    int r = vsnprintf(out, cap, ccr_trn(id, n), ap);
    va_end(ap);
    return r;
}

const char *ccr_lang_code(void) { english_ready(); return g_pack_used ? g_pack.code : g_en.code; }
const char *ccr_lang_name(void) { english_ready(); return g_pack_used ? g_pack.name : g_en.name; }

void ccr_lang_reset(void)
{
    free(g_pack.arena);
    memset(&g_pack, 0, sizeof g_pack);
    g_pack_used = false;
}

/* ---------------------------------------------------- format checking */

/*
 * The printf conversions in s, as "length+conversion" codes ("s", "u",
 * "lu", "d"), so "%5s" and "%-s" count the same as "%s". False for
 * anything we refuse to pass to printf: %n, '*' widths, a lone '%'.
 */
static bool conversions(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = s; *p; ++p) {
        if (*p != '%')
            continue;
        ++p;
        if (*p == '%')
            continue;
        while (*p && strchr("-+ #0", *p))
            ++p;
        while (isdigit((unsigned char)*p))
            ++p;
        if (*p == '.') {
            ++p;
            while (isdigit((unsigned char)*p))
                ++p;
        }
        if (*p == '*')
            return false;
        while (*p && strchr("hlzjt", *p)) {
            if (o + 2 >= cap)
                return false;
            out[o++] = *p++;
        }
        if (!*p || !strchr("sdiuxXcfgeEp", *p))
            return false;
        if (o + 3 >= cap)
            return false;
        out[o++] = *p;
        out[o++] = ',';
    }
    out[o] = '\0';
    return true;
}

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p >> 5) == 6 ? 1 : (*p >> 4) == 14 ? 2 : (*p >> 3) == 30 ? 3 : -1;
        if (n < 0)
            return false;
        ++p;
        for (int i = 0; i < n; ++i, ++p)
            if ((*p & 0xC0) != 0x80)
                return false;
    }
    return true;
}

/* ------------------------------------------------------- pack files */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* \n, \t, \\ and \| (a '|' that does not split plural forms; kept as
 * \x01 until the split, then turned back). */
static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
            *o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == '|' ? '\x01' : *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static int find_key(const char *key)
{
    for (int i = 0; i < CCR_T_COUNT; ++i)
        if (strcmp(g_entries[i].key, key) == 0)
            return i;
    return -1;
}

static char *read_small_file(const char *path, FILE *log)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (log)
            fprintf(log, "%s: cannot open: %s\n", path, strerror(errno));
        return NULL;
    }
    char *buf = malloc(MAX_PACK_SIZE + 1);
    size_t n = buf ? fread(buf, 1, MAX_PACK_SIZE + 1, f) : 0;
    fclose(f);
    if (!buf || n > MAX_PACK_SIZE) {
        if (log)
            fprintf(log, "%s: %s\n", path, buf ? "too big for a language pack" : "out of memory");
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static bool code_from_path(const char *path, char *code, size_t cap);

/*
 * Reads a pack into t. Every problem is one line in `log` and one in the
 * count; a broken string is dropped (English is used), the rest is kept.
 * With `strict`, keys the pack leaves out are problems too.
 */
static int load_pack(table *t, const char *path, FILE *log, bool strict, bool *ok)
{
    *ok = false;
    memset(t, 0, sizeof *t);
    t->rule = CCR_PLURAL_ONE_OTHER;
    if (!code_from_path(path, t->code, sizeof t->code))
        snprintf(t->code, sizeof t->code, "?");
    t->arena = read_small_file(path, log);
    if (!t->arena)
        return 1;
    english_ready();

    int problems = 0;
    bool rule_given = false;
    unsigned line_no = 0;
    char *next = t->arena;
    if ((unsigned char)next[0] == 0xEF && (unsigned char)next[1] == 0xBB &&
        (unsigned char)next[2] == 0xBF)
        next += 3;   /* a BOM, from some Windows editor */

#define PROBLEM(...)                                                 \
    do {                                                             \
        ++problems;                                                  \
        if (log) {                                                   \
            fprintf(log, "%s:%u: ", path, line_no);                  \
            fprintf(log, __VA_ARGS__);                               \
            fputc('\n', log);                                        \
        }                                                            \
    } while (0)

    while (next && *next) {
        char *line = next;
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        ++line_no;
        line = trim(line);
        if (!*line || *line == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq) {
            PROBLEM("expected 'key = text'");
            continue;
        }
        *eq = '\0';
        char *key = trim(line), *val = trim(eq + 1);
        if (!valid_utf8(val)) {
            PROBLEM("'%s': not valid UTF-8 (save the file as UTF-8)", key);
            continue;
        }
        unescape(val);

        if (*key == '@') {
            if (strcmp(key, "@name") == 0) {
                snprintf(t->name, sizeof t->name, "%s", val);
            } else if (strcmp(key, "@plural") == 0) {
                size_t i = 0;
                for (; i < sizeof g_rules / sizeof g_rules[0]; ++i)
                    if (strcmp(g_rules[i].name, val) == 0)
                        break;
                if (i == sizeof g_rules / sizeof g_rules[0]) {
                    PROBLEM("unknown @plural '%s' (known: germanic, slavic, polish, czech, french, none)",
                            val);
                } else {
                    t->rule = g_rules[i].rule;
                    rule_given = true;
                }
            } else if (strcmp(key, "@author") != 0 && strcmp(key, "@note") != 0) {
                PROBLEM("unknown setting '%s'", key);
            }
            continue;
        }

        int id = find_key(key);
        if (id < 0) {
            PROBLEM("unknown key '%s' (from a newer or older Ccratch?)", key);
            continue;
        }
        if (t->nforms[id])
            PROBLEM("'%s' given twice, the last one wins", key);

        const char *forms[MAX_FORMS];
        int n = g_entries[id].plural ? split_forms(val, forms) : 1;
        if (!g_entries[id].plural)
            forms[0] = val;
        for (int f = 0; f < n; ++f)
            for (char *c = (char *)forms[f]; *c; ++c)
                if (*c == '\x01')
                    *c = '|';
        if (g_entries[id].plural && n != forms_of(t->rule)) {
            PROBLEM("'%s' has %d form%s, @plural %s wants %d (split them with |)", key, n,
                    n == 1 ? "" : "s", rule_given ? "here" : "(germanic, by default)",
                    forms_of(t->rule));
            t->nforms[id] = 0;
            continue;
        }
        /* a translation takes exactly the arguments the English does */
        bool good = true;
        if (strchr(g_entries[id].en, '%')) {
            char want[64], got[64];
            conversions(g_en.form[id][0], want, sizeof want);
            for (int f = 0; f < n && good; ++f)
                if (!conversions(forms[f], got, sizeof got) || strcmp(want, got) != 0) {
                    PROBLEM("'%s': the %%-codes must be the English ones, in order (%s)", key,
                            g_entries[id].en);
                    good = false;
                }
        }
        if (!good) {
            t->nforms[id] = 0;
            continue;
        }
        for (int f = 0; f < n; ++f)
            t->form[id][f] = forms[f];
        t->nforms[id] = n;
    }
#undef PROBLEM

    if (strict) {
        line_no = 0;
        for (int i = 0; i < CCR_T_COUNT; ++i)
            if (!t->nforms[i]) {
                ++problems;
                if (log)
                    fprintf(log, "%s: missing '%s' (English: %s)\n", path, g_entries[i].key,
                            g_entries[i].en);
            }
    }
    if (!t->name[0])
        snprintf(t->name, sizeof t->name, "%s", t->code);
    *ok = true;
    return problems;
}

/* "dir/pt-BR.lang" -> "pt-BR"; false if it is not a .lang file name. */
static bool code_from_path(const char *path, char *code, size_t cap)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n <= 5 || strcmp(base + n - 5, ".lang") != 0 || n - 5 >= cap)
        return false;
    memcpy(code, base, n - 5);
    code[n - 5] = '\0';
    for (char *c = code; *c; ++c)
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
            return false;
    return true;
}

int ccr_lang_check(const char *path, FILE *out)
{
    table t;
    bool ok;
    int problems = load_pack(&t, path, out, true, &ok);
    if (ok && problems == 0 && out)
        fprintf(out, "%s: %s, all %d strings, no problems\n", path, t.name, (int)CCR_T_COUNT);
    free(t.arena);
    return problems;
}

void ccr_lang_write_template(FILE *out)
{
    fprintf(out,
            "# Ccratch language pack. Save as CODE.lang (ru.lang, pt-BR.lang...) in\n"
            "# ~/.local/share/ccratch/lang/ and pick it with the language button.\n"
            "#\n"
            "# Plural strings have one form per case, split by |; how many depends\n"
            "# on @plural: germanic (1 | other), slavic (1 | 2-4 | 5+), polish,\n"
            "# czech, french (0-1 | other), none (one form).\n"
            "# Keep the %%s/%%u/%%d codes of the English, in the same order.\n"
            "# \\n is a new line, \\| a | that does not split forms.\n"
            "\n@name   = English\n@plural = germanic\n\n");
    for (int i = 0; i < CCR_T_COUNT; ++i) {
        fprintf(out, "%s = ", g_entries[i].key);
        for (const char *c = g_entries[i].en; *c; ++c) {
            if (*c == '\n')
                fputs("\\n", out);
            else
                fputc(*c, out);
        }
        fputc('\n', out);
    }
}

/* --------------------------------------------------- finding packs */

static int compare_info(const void *a, const void *b)
{
    return strcmp(((const ccr_lang_info *)a)->code, ((const ccr_lang_info *)b)->code);
}

int ccr_lang_scan(ccr_lang_info *out, int cap)
{
    if (cap <= 0)
        return 0;
    english_ready();
    int n = 0;
    snprintf(out[n].code, sizeof out[n].code, "en");
    snprintf(out[n].name, sizeof out[n].name, "English");
    out[n++].path[0] = '\0';

    char dirs[4][1024];
    int nd = ccr_config_data_dirs("lang", "CCRATCH_LANG_DIR", dirs, 4);
    for (int d = 0; d < nd; ++d) {
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        for (struct dirent *e = readdir(dir); e && n < cap; e = readdir(dir)) {
            char code[16];
            if (!code_from_path(e->d_name, code, sizeof code))
                continue;
            bool seen = false;
            for (int i = 0; i < n && !seen; ++i)
                seen = strcmp(out[i].code, code) == 0;
            if (seen)
                continue;
            ccr_lang_info *li = &out[n];
            snprintf(li->code, sizeof li->code, "%s", code);
            if (snprintf(li->path, sizeof li->path, "%s/%s", dirs[d], e->d_name) >= (int)sizeof li->path)
                continue;
            /* the name, without loading the whole pack */
            snprintf(li->name, sizeof li->name, "%s", code);
            FILE *f = fopen(li->path, "rb");
            if (f) {
                char line[256];
                while (fgets(line, sizeof line, f)) {
                    char *k = trim(line), *eq = strchr(k, '=');
                    if (eq && strncmp(k, "@name", 5) == 0) {
                        char *v = trim(eq + 1);
                        v[strcspn(v, "\r\n")] = '\0';
                        if (*v && valid_utf8(v))
                            snprintf(li->name, sizeof li->name, "%s", v);
                        break;
                    }
                }
                fclose(f);
            }
            ++n;
        }
        closedir(dir);
    }
    if (n > 2)
        qsort(out + 1, (size_t)(n - 1), sizeof *out, compare_info);   /* English stays first */
    return n;
}

bool ccr_lang_use(const char *code, FILE *log)
{
    if (!code || !*code || strcmp(code, "en") == 0) {
        ccr_lang_reset();
        return true;
    }
    static ccr_lang_info found[64];
    int n = ccr_lang_scan(found, 64);
    for (int i = 1; i < n; ++i) {
        if (strcmp(found[i].code, code) != 0)
            continue;
        table t;
        bool ok;
        int problems = load_pack(&t, found[i].path, log, false, &ok);
        if (!ok) {
            free(t.arena);
            return false;
        }
        if (problems && log)
            fprintf(log, "ccratch: %d problem%s in %s, English used for those\n", problems,
                    problems == 1 ? "" : "s", found[i].path);
        snprintf(t.code, sizeof t.code, "%s", code);
        ccr_lang_reset();
        g_pack = t;
        g_pack_used = true;
        return true;
    }
    return false;
}

bool ccr_lang_save_choice(const char *code)
{
    return ccr_config_set("lang", code);
}

/* "ru_UA.UTF-8" -> tries "ru-UA", then "ru". */
static bool use_locale(const char *loc, FILE *log)
{
    if (!loc || !*loc || strcmp(loc, "C") == 0 || strcmp(loc, "POSIX") == 0 ||
        strncmp(loc, "C.", 2) == 0)
        return false;
    char code[16];
    size_t n = strcspn(loc, ".@");
    if (n >= sizeof code)
        return false;
    memcpy(code, loc, n);
    code[n] = '\0';
    char *us = strchr(code, '_');
    if (us)
        *us = '-';
    if (ccr_lang_use(code, log))
        return true;
    if (us) {
        *us = '\0';
        return ccr_lang_use(code, log);
    }
    return false;
}

void ccr_lang_pick(const char *cli, FILE *log)
{
    if (cli) {
        if (!ccr_lang_use(cli, log) && log)
            fprintf(log, "ccratch: no language pack '%s', staying in English\n", cli);
        return;
    }
    const char *env = getenv("CCRATCH_LANG");
    if (env && *env && ccr_lang_use(env, log))
        return;
    char saved[16];
    if (ccr_config_get("lang", saved, sizeof saved) && ccr_lang_use(saved, log))
        return;
    const char *vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (size_t i = 0; i < 3; ++i) {
        const char *v = getenv(vars[i]);
        if (v && *v) {   /* the first one set decides, like gettext */
            use_locale(v, log);
            return;
        }
    }
}
