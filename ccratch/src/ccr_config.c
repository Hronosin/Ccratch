#define _GNU_SOURCE   /* readlink */
#include "ccr_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONFIG_SIZE (64u * 1024u)

bool ccr_config_home(const char *xdg_var, const char *fallback, char *out, size_t cap)
{
    const char *x = getenv(xdg_var);
    if (x && *x == '/')
        return snprintf(out, cap, "%s", x) < (int)cap;
    const char *home = getenv("HOME");
    if (!home || !*home)
        return false;
    return snprintf(out, cap, "%s/%s", home, fallback) < (int)cap;
}

int ccr_config_data_dirs(const char *sub, const char *env_var, char dirs[][1024], int cap)
{
    int n = 0;
    const char *env = getenv(env_var);
    if (env && *env && n < cap)
        snprintf(dirs[n++], 1024, "%s", env);
    char base[900];
    if (n < cap && ccr_config_home("XDG_DATA_HOME", ".local/share", base, sizeof base))
        snprintf(dirs[n++], 1024, "%s/ccratch/%s", base, sub);
    char exe[900];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0 && n < cap) {
        exe[len] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(dirs[n++], 1024, "%s/../share/ccratch/%s", exe, sub);
        }
    }
    return n;
}

static bool config_path(char *out, size_t cap)
{
    char base[900];
    if (!ccr_config_home("XDG_CONFIG_HOME", ".config", base, sizeof base))
        return false;
    return snprintf(out, cap, "%s/ccratch/config", base) < (int)cap;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* Is this line "key = ..." for `key`? */
static bool line_is(const char *line, const char *key)
{
    char copy[256];
    snprintf(copy, sizeof copy, "%s", line);
    char *k = trim(copy), *eq = strchr(k, '=');
    if (!eq || *k == '#')
        return false;
    *eq = '\0';
    return strcmp(trim(k), key) == 0;
}

bool ccr_config_get(const char *key, char *out, size_t cap)
{
    char path[1024];
    if (!config_path(path, sizeof path))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        if (!line_is(line, key))
            continue;
        char *v = trim(strchr(line, '=') + 1);
        found = *v && snprintf(out, cap, "%s", v) < (int)cap;
    }
    fclose(f);
    return found;
}

static bool mkdir_p(char *path)
{
    for (char *p = path + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        int r = mkdir(path, 0755);
        *p = '/';
        if (r != 0 && errno != EEXIST)
            return false;
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool ccr_config_layout_path(const char *file, char *out, size_t cap, bool make)
{
    char home[1024], real[4096];
    if (!ccr_config_home("XDG_DATA_HOME", ".local/share", home, sizeof home))
        return false;
    if (!realpath(file, real))
        snprintf(real, sizeof real, "%s", file);
    uint64_t h = 1469598103934665603ull;   /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)real; *p; ++p)
        h = (h ^ *p) * 1099511628211ull;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/ccratch/layout", home);
    if (make && !mkdir_p(dir))
        return false;
    return snprintf(out, cap, "%s/%016llx.layout", dir, (unsigned long long)h) < (int)cap;
}

bool ccr_config_file(char *out, size_t cap)
{
    return config_path(out, cap);
}

bool ccr_config_set(const char *key, const char *value)
{
    if (strpbrk(key, "=\n\r#") || (value && strpbrk(value, "\n\r")))
        return false;
    char path[1024], tmp[1100], dir[1024];
    if (!config_path(path, sizeof path))
        return false;
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = '\0';
    if (!mkdir_p(dir))
        return false;

    /* what is there now, to keep */
    char *old = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        old = malloc(MAX_CONFIG_SIZE + 1);
        size_t n = old ? fread(old, 1, MAX_CONFIG_SIZE, f) : 0;
        if (old)
            old[n] = '\0';
        fclose(f);
    }

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *w = fopen(tmp, "wb");
    if (!w) {
        free(old);
        return false;
    }
    if (!old)
        fputs("# Ccratch settings\n", w);
    for (char *line = old, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (!line_is(line, key))
            fprintf(w, "%s\n", line);
    }
    free(old);
    if (value)
        fprintf(w, "%s = %s\n", key, value);
    bool ok = fclose(w) == 0;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        remove(tmp);
    return ok;
}
