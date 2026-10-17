#include "ccr_mi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One allocation per record: the values first, the strings after them. */
typedef struct reader {
    const char   *p;
    ccr_mi_value *vals;
    int           nval, cap_val;
    char         *strs;
    size_t        nstr, cap_str;
    bool          ok;
} reader;

static ccr_mi_value *new_value(reader *r, ccr_mi_kind k, const char *name)
{
    if (r->nval == r->cap_val) {
        r->ok = false;
        return NULL;
    }
    ccr_mi_value *v = &r->vals[r->nval++];
    memset(v, 0, sizeof *v);
    v->kind = k;
    v->name = name;
    v->str = "";
    return v;
}

static char *put_str(reader *r, const char *s, size_t n)
{
    if (r->nstr + n + 1 > r->cap_str) {
        r->ok = false;
        return (char *)"";
    }
    char *at = r->strs + r->nstr;
    memcpy(at, s, n);
    at[n] = '\0';
    r->nstr += n + 1;
    return at;
}

/* "..." with C escapes, at r->p. */
static const char *c_string(reader *r)
{
    if (*r->p != '"') {
        r->ok = false;
        return "";
    }
    ++r->p;
    char *at = r->strs + r->nstr;
    size_t n = 0;
    while (*r->p && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\' && *r->p) {
            const char e = *r->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'e': c = 27; break;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                int v = e - '0';
                for (int k = 0; k < 2 && *r->p >= '0' && *r->p <= '7'; ++k)
                    v = v * 8 + (*r->p++ - '0');
                c = (char)v;
                break;
            }
            default: c = e; break;   /* \" \\ */
            }
        }
        if (r->nstr + n + 2 > r->cap_str) {
            r->ok = false;
            return "";
        }
        at[n++] = c;
    }
    if (*r->p != '"') {
        r->ok = false;
        return "";
    }
    ++r->p;
    at[n] = '\0';
    r->nstr += n + 1;
    return at;
}

static const char *ident(reader *r)
{
    const char *b = r->p;
    while (*r->p && (isalnum((unsigned char)*r->p) || *r->p == '_' || *r->p == '-'))
        ++r->p;
    return put_str(r, b, (size_t)(r->p - b));
}

static ccr_mi_value *value(reader *r, const char *name, int depth);

/* name=value, ... until `close` */
static void results(reader *r, ccr_mi_value *into, char close, bool names, int depth)
{
    ccr_mi_value *last = NULL;
    while (r->ok && *r->p && *r->p != close) {
        const char *name = "";
        if (names || (isalpha((unsigned char)*r->p) || *r->p == '_')) {
            name = ident(r);
            if (*r->p != '=') {
                r->ok = false;
                return;
            }
            ++r->p;
        }
        ccr_mi_value *v = value(r, name, depth + 1);
        if (!v)
            return;
        if (last)
            last->next = v;
        else
            into->first = v;
        last = v;
        if (*r->p == ',')
            ++r->p;
        else if (*r->p != close)
            r->ok = close == '\0' && *r->p == '\0';
    }
}

static ccr_mi_value *value(reader *r, const char *name, int depth)
{
    if (depth > 64) {
        r->ok = false;
        return NULL;
    }
    if (*r->p == '"') {
        ccr_mi_value *v = new_value(r, CCR_MI_STRING, name);
        if (v)
            v->str = c_string(r);
        return r->ok ? v : NULL;
    }
    if (*r->p == '{' || *r->p == '[') {
        const char close = *r->p == '{' ? '}' : ']';
        ccr_mi_value *v = new_value(r, close == '}' ? CCR_MI_TUPLE : CCR_MI_LIST, name);
        if (!v)
            return NULL;
        ++r->p;
        results(r, v, close, close == '}', depth);
        if (!r->ok || *r->p != close) {
            r->ok = false;
            return NULL;
        }
        ++r->p;
        return v;
    }
    r->ok = false;
    return NULL;
}

bool ccr_mi_parse(const char *line, ccr_mi_record *out)
{
    memset(out, 0, sizeof *out);
    out->token = -1;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        --len;
    if (len == 0)
        return false;
    if (len >= 5 && strncmp(line, "(gdb)", 5) == 0) {
        bool rest = false;
        for (size_t i = 5; i < len; ++i)
            rest = rest || !isspace((unsigned char)line[i]);
        if (!rest)
            return false;
    }

    const size_t nval = len / 2 + 8;
    const size_t vbytes = nval * sizeof(ccr_mi_value);
    char *mem = malloc(vbytes + len + 64);
    if (!mem)
        return false;
    reader r = { .p = line, .vals = (ccr_mi_value *)(void *)mem, .cap_val = (int)nval,
                 .strs = mem + vbytes, .cap_str = len + 64, .ok = true };
    out->arena = mem;

    const char *p = line;
    long token = -1;
    if (isdigit((unsigned char)*p)) {
        token = 0;
        while (isdigit((unsigned char)*p))
            token = token * 10 + (*p++ - '0');
    }
    const char kind = *p;
    if (strchr("^*=+", kind) && kind) {
        r.p = p + 1;
        out->token = token;
        out->kind = kind == '+' ? '=' : kind;
        out->klass = ident(&r);
        out->results = new_value(&r, CCR_MI_TUPLE, "");
        if (*r.p == ',') {
            ++r.p;
            results(&r, out->results, '\0', true, 0);
        }
        if (r.ok && out->results)
            return true;
    } else if (strchr("~@&", kind) && kind && token < 0 && p[1] == '"') {
        r.p = p + 1;
        out->kind = kind;
        out->klass = c_string(&r);
        out->results = new_value(&r, CCR_MI_TUPLE, "");
        if (r.ok)
            return true;
    }
    /* not MI: the program's own output, as it is */
    memset(out, 0, sizeof *out);
    out->token = -1;
    out->arena = mem;
    r.nstr = 0;
    r.nval = 0;
    r.ok = true;
    out->klass = put_str(&r, line, len);
    out->results = new_value(&r, CCR_MI_TUPLE, "");
    return true;
}

void ccr_mi_free(ccr_mi_record *r)
{
    free(r->arena);
    memset(r, 0, sizeof *r);
    r->token = -1;
}

const ccr_mi_value *ccr_mi_get(const ccr_mi_value *tuple, const char *name)
{
    for (const ccr_mi_value *v = tuple ? tuple->first : NULL; v; v = v->next)
        if (strcmp(v->name, name) == 0)
            return v;
    return NULL;
}

const char *ccr_mi_str(const ccr_mi_value *tuple, const char *name, const char *fallback)
{
    const ccr_mi_value *v = ccr_mi_get(tuple, name);
    return v && v->kind == CCR_MI_STRING ? v->str : fallback;
}

void ccr_mi_quote(const char *text, char *out, int cap)
{
    int k = 0;
    if (cap < 3) {
        if (cap > 0)
            out[0] = '\0';
        return;
    }
    out[k++] = '"';
    for (const char *c = text; *c && k < cap - 3; ++c) {
        if (*c == '"' || *c == '\\')
            out[k++] = '\\';
        out[k++] = *c;
    }
    out[k++] = '"';
    out[k] = '\0';
}
