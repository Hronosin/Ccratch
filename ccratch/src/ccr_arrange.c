#include "ccr_arrange.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const g_kind_keys[CCR_AK_COUNT] = {
    "includes", "macros", "types", "globals", "protos", "main", "funcs", "classes", "loose", "rest",
};

static const char *const g_order_keys[CCR_AO_COUNT] = { "file", "name", "size", "calls" };

const char *ccr_akind_key(unsigned kind_bit)
{
    for (int i = 0; i < CCR_AK_COUNT; ++i)
        if (kind_bit == 1u << i)
            return g_kind_keys[i];
    return "";
}

const char *ccr_aorder_key(ccr_aorder o)
{
    return (int)o >= 0 && o < CCR_AO_COUNT ? g_order_keys[o] : "file";
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* "types globals | order=name wrap=1200" */
static bool parse_column(char *v, ccr_arrange_col *c, char *why, size_t cap)
{
    memset(c, 0, sizeof *c);
    char *opts = strchr(v, '|');
    if (opts)
        *opts++ = '\0';
    for (char *w = strtok(v, " \t,"); w; w = strtok(NULL, " \t,")) {
        int k = 0;
        while (k < CCR_AK_COUNT && strcmp(w, g_kind_keys[k]) != 0)
            ++k;
        if (k == CCR_AK_COUNT) {
            snprintf(why, cap, "no kind of thing called '%s'", w);
            return false;
        }
        c->kinds |= 1u << k;
    }
    for (char *w = opts ? strtok(opts, " \t,") : NULL; w; w = strtok(NULL, " \t,")) {
        if (strncmp(w, "order=", 6) == 0) {
            int k = 0;
            while (k < CCR_AO_COUNT && strcmp(w + 6, g_order_keys[k]) != 0)
                ++k;
            if (k == CCR_AO_COUNT) {
                snprintf(why, cap, "no order called '%s'", w + 6);
                return false;
            }
            c->order = (ccr_aorder)k;
        } else if (strncmp(w, "wrap=", 5) == 0) {
            char *end;
            const double h = strtod(w + 5, &end);
            if (*end || h < 0.0) {
                snprintf(why, cap, "'%s' is no height", w + 5);
                return false;
            }
            c->wrap = (float)h;
        } else {
            snprintf(why, cap, "no option '%s' (order=, wrap=)", w);
            return false;
        }
    }
    if (!c->kinds) {
        snprintf(why, cap, "a column of nothing");
        return false;
    }
    return true;
}

bool ccr_arrangement_parse(const char *text, ccr_arrangement *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof *out);
    if (err_cap)
        err[0] = '\0';
    char line[600];
    int no = 0;
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p = e ? e + 1 : NULL;
        ++no;
        char *l = trim(line);
        if (!*l || *l == '#')
            continue;
        char *eq = strchr(l, '=');
        if (!eq) {
            snprintf(err, err_cap, "line %d: no '='", no);
            return false;
        }
        *eq = '\0';
        char *key = trim(l), *val = trim(eq + 1), why[160];
        if (strcmp(key, "@name") == 0) {
            snprintf(out->name, sizeof out->name, "%s", val);
        } else if (strncmp(key, "@name.", 6) == 0) {
            for (int k = 0; k < 4; ++k)
                if (!out->names[k][0][0]) {
                    snprintf(out->names[k][0], sizeof out->names[k][0], "%s", key + 6);
                    snprintf(out->names[k][1], sizeof out->names[k][1], "%s", val);
                    break;
                }
        } else if (key[0] == '@') {
            continue;
        } else if (strcmp(key, "column") == 0) {
            if (out->ncols == CCR_ARRANGE_MAX_COLS) {
                snprintf(err, err_cap, "line %d: %d columns at most", no, CCR_ARRANGE_MAX_COLS);
                return false;
            }
            if (!parse_column(val, &out->cols[out->ncols], why, sizeof why)) {
                snprintf(err, err_cap, "line %d: %s", no, why);
                return false;
            }
            ++out->ncols;
        } else {
            snprintf(err, err_cap, "line %d: no key '%s' (@name, column)", no, key);
            return false;
        }
    }
    if (!out->ncols) {
        snprintf(err, err_cap, "no columns");
        return false;
    }
    if (!out->name[0])
        snprintf(out->name, sizeof out->name, "?");
    return true;
}

const char *ccr_arrangement_title(const ccr_arrangement *a, const char *ui_language)
{
    for (int k = 0; k < 4 && ui_language; ++k)
        if (a->names[k][0][0] && strncmp(ui_language, a->names[k][0], strlen(a->names[k][0])) == 0)
            return a->names[k][1];
    return a->name;
}

void ccr_arrangement_write(const ccr_arrangement *a, char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "# a Ccratch arrangement: columns, left to right\n@name = %s\n", a->name);
    for (int k = 0; k < 4 && n < cap; ++k)
        if (a->names[k][0][0])
            n += (size_t)snprintf(out + n, cap - n, "@name.%s = %s\n", a->names[k][0], a->names[k][1]);
    for (int c = 0; c < a->ncols && n < cap; ++c) {
        const ccr_arrange_col *col = &a->cols[c];
        n += (size_t)snprintf(out + n, cap - n, "column =");
        for (int k = 0; k < CCR_AK_COUNT && n < cap; ++k)
            if (col->kinds & (1u << k))
                n += (size_t)snprintf(out + n, cap - n, " %s", g_kind_keys[k]);
        if (n < cap && (col->order != CCR_AO_FILE || col->wrap > 0.0f)) {
            n += (size_t)snprintf(out + n, cap - n, " |");
            if (col->order != CCR_AO_FILE && n < cap)
                n += (size_t)snprintf(out + n, cap - n, " order=%s", g_order_keys[col->order]);
            if (col->wrap > 0.0f && n < cap)
                n += (size_t)snprintf(out + n, cap - n, " wrap=%.0f", (double)col->wrap);
        }
        if (n < cap)
            n += (size_t)snprintf(out + n, cap - n, "\n");
    }
}

void ccr_arrangement_code(const char *name, char *out, size_t cap)
{
    size_t o = 0;
    bool other = false;
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 1 < cap; ++p) {
        h = (h ^ *p) * 16777619u;
        if (isalnum(*p) && *p < 0x80)
            out[o++] = (char)tolower(*p);
        else if (o && out[o - 1] != '_')
            out[o++] = '_';
        other = other || *p >= 0x80;
    }
    while (o && out[o - 1] == '_')
        --o;
    out[o] = '\0';
    if (!o || other)   /* letters a file name cannot carry well: a number of its own */
        snprintf(out + o, cap - o, "%sarrangement_%04x", o ? "_" : "", h & 0xFFFFu);
}
