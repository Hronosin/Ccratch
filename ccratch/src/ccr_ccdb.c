#define _DEFAULT_SOURCE   /* realpath */
#include "ccr_ccdb.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------- finding */

static void consider(const char *path, char *best, int cap, double *best_t)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if ((double)st.st_mtime > *best_t) {
        *best_t = (double)st.st_mtime;
        snprintf(best, (size_t)cap, "%s", path);
    }
}

/* DIR/<every folder>/compile_commands.json */
static void consider_under(const char *dir, const char *prefix, char *best, int cap, double *best_t)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.' || (prefix && strncmp(e->d_name, prefix, strlen(prefix)) != 0))
            continue;
        char p[1400];
        if (snprintf(p, sizeof p, "%s/%s/compile_commands.json", dir, e->d_name) < (int)sizeof p)
            consider(p, best, cap, best_t);
    }
    closedir(d);
}

bool ccr_ccdb_find(const char *project, char *out, int cap)
{
    double t = -1.0;
    out[0] = '\0';
    char p[1400];
    static const char *const direct[] = { "", "/build", "/.ccratch/build", "/.ccratch/build-release",
                                                   "/.ccratch/build-sanitize" };
    for (size_t i = 0; i < sizeof direct / sizeof direct[0]; ++i) {
        snprintf(p, sizeof p, "%s%s/compile_commands.json", project, direct[i]);
        consider(p, out, cap, &t);
    }
    snprintf(p, sizeof p, "%s/build", project);
    consider_under(p, NULL, out, cap, &t);            /* build/debug (presets) */
    consider_under(project, "cmake-build-", out, cap, &t);   /* CLion */
    snprintf(p, sizeof p, "%s/out/build", project);
    consider_under(p, NULL, out, cap, &t);            /* Visual Studio */
    return out[0] != '\0';
}

/* ------------------------------------------- just enough JSON for it */

typedef struct json {
    const char *s, *end;
    bool ok;
} json;

static void ws(json *j)
{
    while (j->s < j->end && isspace((unsigned char)*j->s))
        ++j->s;
}

/* A string, unescaped, malloc'd (NULL on a malformed one). */
static char *jstring(json *j)
{
    ws(j);
    if (j->s >= j->end || *j->s != '"') {
        j->ok = false;
        return NULL;
    }
    ++j->s;
    size_t cap = 64, n = 0;
    char *out = malloc(cap);
    while (out && j->s < j->end && *j->s != '"') {
        char c = *j->s++;
        if (c == '\\' && j->s < j->end) {
            c = *j->s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {   /* \uXXXX: ASCII as itself, the rest as '?' */
                unsigned v = 0;
                for (int k = 0; k < 4 && j->s < j->end; ++k, ++j->s)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*j->s) ? *j->s - '0'
                                                                          : (tolower((unsigned char)*j->s) - 'a' + 10));
                c = v < 128 ? (char)v : '?';
                break;
            }
            default: break;   /* \" \\ \/ */
            }
        }
        if (n + 2 > cap) {
            cap *= 2;
            char *b = realloc(out, cap);
            if (!b) {
                free(out);
                out = NULL;
                break;
            }
            out = b;
        }
        out[n++] = c;
    }
    if (!out || j->s >= j->end) {
        free(out);
        j->ok = false;
        return NULL;
    }
    ++j->s;   /* the closing quote */
    out[n] = '\0';
    return out;
}

static void jskip(json *j, int depth)
{
    ws(j);
    if (j->s >= j->end || depth > 64) {
        j->ok = false;
        return;
    }
    const char c = *j->s;
    if (c == '"') {
        free(jstring(j));
    } else if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        ++j->s;
        ws(j);
        if (j->s < j->end && *j->s == close) {
            ++j->s;
            return;
        }
        while (j->ok) {
            if (c == '{') {
                free(jstring(j));
                ws(j);
                if (j->s >= j->end || *j->s != ':') { j->ok = false; return; }
                ++j->s;
            }
            jskip(j, depth + 1);
            ws(j);
            if (j->s < j->end && *j->s == ',') { ++j->s; continue; }
            if (j->s < j->end && *j->s == close) { ++j->s; return; }
            j->ok = false;
        }
    } else {   /* number, true, false, null */
        while (j->s < j->end && !strchr(",]} \t\r\n", *j->s))
            ++j->s;
    }
}

/* One entry of the database. */
typedef struct entry {
    char  *directory, *file, *command;
    char **arguments;
    int    argc;
} entry;

static void entry_free(entry *e)
{
    free(e->directory);
    free(e->file);
    free(e->command);
    for (int i = 0; i < e->argc; ++i)
        free(e->arguments[i]);
    free(e->arguments);
    memset(e, 0, sizeof *e);
}

static bool jentry(json *j, entry *e)
{
    memset(e, 0, sizeof *e);
    ws(j);
    if (j->s >= j->end || *j->s != '{') {
        j->ok = false;
        return false;
    }
    ++j->s;
    ws(j);
    if (j->s < j->end && *j->s == '}') {
        ++j->s;
        return true;
    }
    while (j->ok) {
        char *key = jstring(j);
        ws(j);
        if (!key || j->s >= j->end || *j->s != ':') {
            free(key);
            j->ok = false;
            break;
        }
        ++j->s;
        ws(j);
        if (strcmp(key, "directory") == 0 && !e->directory) {
            e->directory = jstring(j);
        } else if (strcmp(key, "file") == 0 && !e->file) {
            e->file = jstring(j);
        } else if (strcmp(key, "command") == 0 && !e->command) {
            e->command = jstring(j);
        } else if (strcmp(key, "arguments") == 0 && !e->arguments && j->s < j->end && *j->s == '[') {
            ++j->s;
            int cap = 0;
            ws(j);
            if (j->s < j->end && *j->s == ']') {
                ++j->s;
            } else {
                while (j->ok) {
                    char *a = jstring(j);
                    if (!a)
                        break;
                    if (e->argc == cap) {
                        cap = cap ? cap * 2 : 32;
                        char **v = realloc(e->arguments, (size_t)cap * sizeof *v);
                        if (!v) {
                            free(a);
                            j->ok = false;
                            break;
                        }
                        e->arguments = v;
                    }
                    e->arguments[e->argc++] = a;
                    ws(j);
                    if (j->s < j->end && *j->s == ',') { ++j->s; continue; }
                    if (j->s < j->end && *j->s == ']') { ++j->s; break; }
                    j->ok = false;
                }
            }
        } else {
            jskip(j, 1);
        }
        free(key);
        ws(j);
        if (j->s < j->end && *j->s == ',') { ++j->s; continue; }
        if (j->s < j->end && *j->s == '}') { ++j->s; break; }
        j->ok = false;
    }
    return j->ok;
}

/* ------------------------------------------------------ the arguments */

/* A command line split the way a shell would: quotes and backslashes. */
static int split_command(const char *cmd, char ***out)
{
    int n = 0, cap = 0;
    char **v = NULL;
    const char *p = cmd;
    char *buf = malloc(strlen(cmd) + 1);
    if (!buf)
        return 0;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            ++p;
        if (!*p)
            break;
        size_t k = 0;
        char quote = 0;
        while (*p && (quote || !isspace((unsigned char)*p))) {
            if (quote) {
                if (*p == quote) { quote = 0; ++p; continue; }
                if (*p == '\\' && quote == '"' && p[1]) { buf[k++] = p[1]; p += 2; continue; }
                buf[k++] = *p++;
            } else if (*p == '"' || *p == '\'') {
                quote = *p++;
            } else if (*p == '\\' && p[1]) {
                buf[k++] = p[1];
                p += 2;
            } else {
                buf[k++] = *p++;
            }
        }
        buf[k] = '\0';
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **nv = realloc(v, (size_t)cap * sizeof *nv);
            if (!nv)
                break;
            v = nv;
        }
        v[n] = strdup(buf);
        if (!v[n])
            break;
        ++n;
    }
    free(buf);
    *out = v;
    return n;
}

/* a/b/../c -> a/c, ./ dropped: for paths through folders that are gone */
static void tidy(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap;) {
        if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            p += 2;
        } else if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) {
            while (n > 0 && out[n - 1] != '/')
                --n;
            if (n > 0)
                --n;   /* and its slash */
            p += 3;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
}

static bool same_path(const char *a, const char *b)
{
    char ra[4096], rb[4096];
    if (realpath(a, ra) && realpath(b, rb))
        return strcmp(ra, rb) == 0;
    tidy(a, ra, sizeof ra);
    if (!realpath(b, rb))
        tidy(b, rb, sizeof rb);
    return strcmp(ra, rb) == 0;
}

static void tidy(const char *in, char *out, size_t cap);

/* `file` of an entry, as a whole path. */
static void entry_path(const entry *e, char *out, size_t cap)
{
    char raw[8400];
    if (!e->file)
        raw[0] = '\0';
    else if (e->file[0] == '/' || !e->directory)
        snprintf(raw, sizeof raw, "%s", e->file);
    else
        snprintf(raw, sizeof raw, "%s/%s", e->directory, e->file);
    tidy(raw, out, cap);
}

static size_t common_prefix(const char *a, const char *b)
{
    size_t n = 0, last_slash = 0;
    while (a[n] && a[n] == b[n]) {
        if (a[n] == '/')
            last_slash = n;
        ++n;
    }
    return last_slash;
}

/* Keeps what changes how the code reads; relative paths made whole. */
static void keep_args(ccr_ccdb_args *out, const entry *e)
{
    char **argv = e->arguments;
    int argc = e->argc;
    char **split = NULL;
    if (!argc && e->command)
        argc = split_command(e->command, &split), argv = split;

    size_t need = 1;
    for (int i = 0; i < argc; ++i)
        need += strlen(argv[i]) + 1 + (e->directory ? strlen(e->directory) + 1 : 0) + 4;
    out->store = malloc(need);
    size_t used = 0;
    static const char *const with_path[] = { "-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros" };
    for (int i = 1; out->store && i < argc && out->n + 2 < CCR_CCDB_MAX_ARGS; ++i) {   /* argv[0]: the compiler */
        const char *a = argv[i];
        const char *flag = NULL, *val = NULL;
        bool joined = false;
        for (size_t k = 0; k < sizeof with_path / sizeof with_path[0]; ++k) {
            const size_t fl = strlen(with_path[k]);
            if (strncmp(a, with_path[k], fl) == 0) {
                if (a[fl] == '\0' && i + 1 < argc) {
                    flag = with_path[k];
                    val = argv[++i];
                } else if (a[fl] && strcmp(with_path[k], "-I") == 0) {
                    flag = "-I";
                    val = a + fl;
                    joined = true;
                }
                break;
            }
        }
        if (flag) {
            char *f = out->store + used;
            used += (size_t)sprintf(f, "%s", flag) + 1;
            char *v = out->store + used;
            if (val[0] != '/' && e->directory)
                used += (size_t)sprintf(v, "%s/%s", e->directory, val) + 1;
            else
                used += (size_t)sprintf(v, "%s", val) + 1;
            (void)joined;
            out->v[out->n++] = f;
            out->v[out->n++] = v;
        } else if (strncmp(a, "-D", 2) == 0 || strncmp(a, "-U", 2) == 0 || strncmp(a, "-std=", 5) == 0) {
            const char *val2 = NULL;
            if ((strcmp(a, "-D") == 0 || strcmp(a, "-U") == 0) && i + 1 < argc)
                val2 = argv[++i];
            char *f = out->store + used;
            used += (size_t)sprintf(f, "%s%s", a, val2 ? val2 : "") + 1;
            out->v[out->n++] = f;
        }
    }
    for (int i = 0; split && i < argc; ++i)
        free(split[i]);
    free(split);
}

bool ccr_ccdb_args_for(const char *db, const char *path, ccr_ccdb_args *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(db, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 256L * 1024 * 1024) {
        fclose(f);
        return false;
    }
    char *text = malloc((size_t)len);
    const bool read_ok = text && fread(text, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!read_ok) {
        free(text);
        return false;
    }

    char real[4096];
    if (!realpath(path, real))
        snprintf(real, sizeof real, "%s", path);
    json j = { text, text + len, true };
    ws(&j);
    entry best = { 0 };
    size_t best_score = 0;
    bool exact = false;
    if (j.s < j.end && *j.s == '[') {
        ++j.s;
        ws(&j);
        while (j.ok && j.s < j.end && *j.s != ']' && !exact) {
            entry e;
            if (!jentry(&j, &e)) {
                entry_free(&e);
                break;
            }
            char p[4200];
            entry_path(&e, p, sizeof p);
            if (p[0] && same_path(p, real)) {
                entry_free(&best);
                best = e;
                exact = true;
            } else {
                /* a header, or a file it does not build: the entry whose
                 * file is nearest in the folder tree */
                const size_t score = p[0] ? common_prefix(p, real) + 1 : 0;
                if (score > best_score) {
                    entry_free(&best);
                    best = e;
                    best_score = score;
                } else {
                    entry_free(&e);
                }
            }
            ws(&j);
            if (j.s < j.end && *j.s == ',') {
                ++j.s;
                ws(&j);
            }
        }
    }
    free(text);
    if (!best.file) {
        entry_free(&best);
        return false;
    }
    snprintf(out->db, sizeof out->db, "%s", db);
    entry_path(&best, real, sizeof real);
    snprintf(out->from, sizeof out->from, "%.1000s", real);
    out->exact = exact;
    keep_args(out, &best);
    entry_free(&best);
    return out->store != NULL;
}

void ccr_ccdb_args_free(ccr_ccdb_args *a)
{
    free(a->store);
    memset(a, 0, sizeof *a);
}
