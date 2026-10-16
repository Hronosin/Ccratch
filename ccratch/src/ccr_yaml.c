#include "ccr_yaml.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct line {
    int   indent;
    char *text;      /* comment and trailing space cut; may be moved past a "- " */
    char *raw;       /* the whole line, for | and > blocks */
    int   no;
} line;

typedef struct parser {
    line *v;
    int   n, i;
    char *err;
    bool  failed;
} parser;

static void fail(parser *p, int no, const char *fmt, ...)
{
    if (p->failed)
        return;
    p->failed = true;
    int w = snprintf(p->err, 256, "line %d: ", no);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err + (w > 0 ? w : 0), 256 - (size_t)(w > 0 ? w : 0), fmt, ap);
    va_end(ap);
}

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static ccr_yaml_node *new_node(ccr_yaml_kind kind, int line_no)
{
    ccr_yaml_node *n = calloc(1, sizeof *n);
    if (n) {
        n->kind = kind;
        n->line = line_no;
    }
    return n;
}

static void free_node(ccr_yaml_node *n)
{
    while (n) {
        ccr_yaml_node *next = n->next;
        free_node(n->first);
        free(n->key);
        free(n->value);
        free(n);
        n = next;
    }
}

static void append(ccr_yaml_node *parent, ccr_yaml_node *child)
{
    if (!child)
        return;
    ccr_yaml_node **at = &parent->first;
    while (*at)
        at = &(*at)->next;
    *at = child;
}

/* ------------------------------------------------------------ scalars */

static const char *skip_space(const char *s)
{
    while (*s == ' ')
        ++s;
    return s;
}

/* A quoted scalar starting at *s (on the quote); moves *s past it. */
static char *quoted(parser *p, int no, const char **s)
{
    const char q = **s;
    const char *c = *s + 1;
    size_t cap = strlen(c) + 1, n = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    for (;;) {
        if (!*c) {
            fail(p, no, "a %s quote is never closed", q == '"' ? "double" : "single");
            free(out);
            return NULL;
        }
        if (*c == q) {
            if (q == '\'' && c[1] == '\'') {   /* '' is a quote in single quotes */
                out[n++] = '\'';
                c += 2;
                continue;
            }
            ++c;
            break;
        }
        if (q == '"' && *c == '\\' && c[1]) {
            ++c;
            switch (*c) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case '0': out[n++] = '\0'; break;
            default:  out[n++] = *c;   break;   /* \" \\ \/ and anything else as itself */
            }
            ++c;
            continue;
        }
        out[n++] = *c++;
    }
    out[n] = '\0';
    *s = c;
    return out;
}

static char *trimmed(const char *s, size_t n)
{
    while (n > 0 && (*s == ' '))
        ++s, --n;
    while (n > 0 && s[n - 1] == ' ')
        --n;
    return dup_n(s, n);
}

/* ------------------------------------------------------------ flow style */

static ccr_yaml_node *flow(parser *p, int no, const char **s, const char *stop);

/* [a, b] or {k: v} */
static ccr_yaml_node *flow_collection(parser *p, int no, const char **s)
{
    const bool seq = **s == '[';
    const char close = seq ? ']' : '}';
    ccr_yaml_node *n = new_node(seq ? CCR_YAML_SEQ : CCR_YAML_MAP, no);
    if (!n)
        return NULL;
    *s = skip_space(*s + 1);
    while (!p->failed && **s != close) {
        if (!**s) {
            fail(p, no, "'%c' is never closed (flow collections stay on one line)", seq ? '[' : '{');
            break;
        }
        ccr_yaml_node *item;
        if (seq) {
            item = flow(p, no, s, ",]");
        } else {
            /* key: value */
            char *key = NULL;
            if (**s == '"' || **s == '\'') {
                key = quoted(p, no, s);
            } else {
                const char *b = *s;
                while (**s && **s != ':' && **s != ',' && **s != '}')
                    ++*s;
                key = trimmed(b, (size_t)(*s - b));
            }
            *s = skip_space(*s);
            if (**s != ':') {
                fail(p, no, "expected ':' after '%s'", key ? key : "");
                free(key);
                break;
            }
            *s = skip_space(*s + 1);
            if (**s == ',' || **s == '}')
                item = new_node(CCR_YAML_NULL, no);
            else
                item = flow(p, no, s, ",}");
            if (item)
                item->key = key;
            else
                free(key);
        }
        append(n, item);
        *s = skip_space(*s);
        if (**s == ',')
            *s = skip_space(*s + 1);
        else if (**s != close && !p->failed)
            fail(p, no, "expected ',' or '%c'", close);
    }
    if (**s == close)
        ++*s;
    return n;
}

/* One value in flow style, up to (not past) a character of `stop`. */
static ccr_yaml_node *flow(parser *p, int no, const char **s, const char *stop)
{
    *s = skip_space(*s);
    if (**s == '[' || **s == '{')
        return flow_collection(p, no, s);
    ccr_yaml_node *n = new_node(CCR_YAML_SCALAR, no);
    if (!n)
        return NULL;
    if (**s == '"' || **s == '\'') {
        n->value = quoted(p, no, s);
    } else {
        const char *b = *s;
        while (**s && !strchr(stop, **s))
            ++*s;
        n->value = trimmed(b, (size_t)(*s - b));
    }
    return n;
}

/* The value after `key:` (or after "- "), on its own line. */
static ccr_yaml_node *inline_value(parser *p, int no, const char *s)
{
    const char *c = s;
    ccr_yaml_node *n;
    if (*c == '[' || *c == '{') {
        n = flow_collection(p, no, &c);
    } else if (*c == '"' || *c == '\'') {
        n = new_node(CCR_YAML_SCALAR, no);
        if (n)
            n->value = quoted(p, no, &c);
    } else {
        n = new_node(CCR_YAML_SCALAR, no);
        if (n)
            n->value = trimmed(c, strlen(c));
        return n;
    }
    c = skip_space(c);
    if (*c && !p->failed)
        fail(p, no, "something after the value: '%s'", c);
    return n;
}

/* ------------------------------------------------------------ blocks */

static bool is_dash(const char *t)
{
    return t[0] == '-' && (t[1] == ' ' || t[1] == '\0');
}

/* Where the ':' of `key: value` is, outside quotes; NULL if none. */
static const char *key_colon(const char *t)
{
    char q = 0;
    for (const char *c = t; *c; ++c) {
        if (q) {
            if (*c == q)
                q = 0;
        } else if ((*c == '"' || *c == '\'') && c == t) {
            q = *c;
        } else if (*c == ':' && (c[1] == ' ' || c[1] == '\0')) {
            return c;
        } else if (*c == '[' || *c == '{') {
            return NULL;   /* a flow value, not a key */
        }
    }
    return NULL;
}

static ccr_yaml_node *block(parser *p, int indent);

static ccr_yaml_node *block_seq(parser *p, int indent)
{
    ccr_yaml_node *seq = new_node(CCR_YAML_SEQ, p->v[p->i].no);
    while (!p->failed && p->i < p->n && p->v[p->i].indent == indent && is_dash(p->v[p->i].text)) {
        line *l = &p->v[p->i];
        const char *content = skip_space(l->text + 1);
        ccr_yaml_node *item;
        if (!*content) {
            ++p->i;
            if (p->i < p->n && p->v[p->i].indent > indent)
                item = block(p, p->v[p->i].indent);
            else
                item = new_node(CCR_YAML_NULL, l->no);
        } else {
            /* "- key: v" starts a mapping at the column of "key" */
            l->indent = indent + (int)(content - l->text);
            l->text = (char *)content;
            item = block(p, l->indent);
        }
        append(seq, item);
    }
    return seq;
}

static ccr_yaml_node *block_map(parser *p, int indent)
{
    ccr_yaml_node *map = new_node(CCR_YAML_MAP, p->v[p->i].no);
    while (!p->failed && p->i < p->n && p->v[p->i].indent == indent && !is_dash(p->v[p->i].text)) {
        line *l = &p->v[p->i];
        const char *colon = key_colon(l->text);
        if (!colon) {
            fail(p, l->no, "expected 'key: value', got '%s'", l->text);
            break;
        }
        char *key;
        if (l->text[0] == '"' || l->text[0] == '\'') {
            const char *c = l->text;
            key = quoted(p, l->no, &c);
        } else {
            key = trimmed(l->text, (size_t)(colon - l->text));
        }
        const char *rest = skip_space(colon + 1);
        const int no = l->no;
        ++p->i;
        ccr_yaml_node *value;
        if (!*rest) {
            if (p->i < p->n && p->v[p->i].indent > indent)
                value = block(p, p->v[p->i].indent);
            else if (p->i < p->n && p->v[p->i].indent == indent && is_dash(p->v[p->i].text))
                value = block_seq(p, indent);   /* key:\n- a\n- b */
            else
                value = new_node(CCR_YAML_NULL, no);
        } else if (rest[0] == '|' || rest[0] == '>') {
            /* a text block: the lines under it, as they are */
            const bool fold = rest[0] == '>';
            value = new_node(CCR_YAML_SCALAR, no);
            size_t cap = 1;
            for (int k = p->i; k < p->n && p->v[k].indent > indent; ++k)
                cap += strlen(p->v[k].raw) + 1;
            char *text = malloc(cap);
            size_t len = 0;
            int base = p->i < p->n ? p->v[p->i].indent : 0;
            while (text && p->i < p->n && p->v[p->i].indent > indent) {
                const line *b = &p->v[p->i++];
                const char *s = b->raw;
                for (int k = 0; k < base && *s == ' '; ++k)
                    ++s;
                if (len)
                    text[len++] = fold ? ' ' : '\n';
                memcpy(text + len, s, strlen(s));
                len += strlen(s);
            }
            if (text)
                text[len] = '\0';
            if (value)
                value->value = text;
            else
                free(text);
        } else {
            value = inline_value(p, no, rest);
            if (p->i < p->n && p->v[p->i].indent > indent && !p->failed)
                fail(p, p->v[p->i].no, "this line is indented under '%s', which already has a value",
                     key ? key : "");
        }
        if (value)
            value->key = key;
        else
            free(key);
        append(map, value);
    }
    return map;
}

static ccr_yaml_node *block(parser *p, int indent)
{
    const line *l = &p->v[p->i];
    if (is_dash(l->text))
        return block_seq(p, indent);
    if (key_colon(l->text))
        return block_map(p, indent);
    /* a lone value */
    ++p->i;
    return inline_value(p, l->no, l->text);
}

/* ------------------------------------------------------------ lines */

/* Cuts a # comment that is not inside quotes. */
static void cut_comment(char *t)
{
    char q = 0;
    for (char *c = t; *c; ++c) {
        if (q) {
            if (*c == '\\' && q == '"' && c[1])
                ++c;
            else if (*c == q)
                q = 0;
        } else if (*c == '"' || *c == '\'') {
            if (c == t || c[-1] == ' ' || c[-1] == '[' || c[-1] == '{' || c[-1] == ',' ||
                c[-1] == ':' || c[-1] == '-')
                q = *c;
        } else if (*c == '#' && (c == t || c[-1] == ' ')) {
            *c = '\0';
            break;
        }
    }
    size_t n = strlen(t);
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\r'))
        t[--n] = '\0';
}

bool ccr_yaml_parse(const char *text, ccr_yaml *out)
{
    memset(out, 0, sizeof *out);
    parser p = { .err = out->error };

    /* split into lines; each keeps two copies: cut and raw */
    int cap = 64;
    p.v = malloc((size_t)cap * sizeof *p.v);
    char *all = dup_n(text, strlen(text));
    if (!p.v || !all) {
        free(p.v);
        free(all);
        snprintf(out->error, sizeof out->error, "out of memory");
        return false;
    }
    int no = 0;
    for (char *s = all; s && *s;) {
        char *nl = strchr(s, '\n');
        if (nl)
            *nl = '\0';
        ++no;
        char *raw = s;
        size_t rn = strlen(raw);
        if (rn && raw[rn - 1] == '\r')
            raw[--rn] = '\0';
        int indent = 0;
        while (raw[indent] == ' ')
            ++indent;
        if (raw[indent] == '\t') {
            fail(&p, no, "a tab in the indentation (YAML wants spaces)");
            break;
        }
        char *cut = dup_n(raw + indent, strlen(raw + indent));
        if (cut)
            cut_comment(cut);
        const bool blank = !cut || !*cut;
        /* blank and comment lines matter only inside | blocks: kept raw */
        if (p.n == cap) {
            cap *= 2;
            line *v = realloc(p.v, (size_t)cap * sizeof *v);
            if (!v)
                break;
            p.v = v;
        }
        if (blank) {
            free(cut);
        } else {
            p.v[p.n++] = (line){ indent, cut, raw, no };
        }
        s = nl ? nl + 1 : NULL;
    }

    /* the cut text moves along a line as "- " are taken off: remember the
     * allocations to free them */
    char **owned = malloc((size_t)(p.n ? p.n : 1) * sizeof *owned);
    for (int k = 0; owned && k < p.n; ++k)
        owned[k] = p.v[k].text;

    if (!p.failed && p.n > 0) {
        out->root = block(&p, p.v[0].indent);
        if (!p.failed && p.i < p.n)
            fail(&p, p.v[p.i].no, "this line does not fit the indentation above it");
    } else if (!p.failed) {
        out->root = new_node(CCR_YAML_NULL, 1);
    }

    for (int k = 0; owned && k < p.n; ++k)
        free(owned[k]);
    free(owned);
    free(p.v);
    free(all);
    if (p.failed) {
        free_node(out->root);
        out->root = NULL;
        return false;
    }
    return true;
}

bool ccr_yaml_load(const char *path, ccr_yaml *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(out->error, sizeof out->error, "cannot open %s", path);
        return false;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (size_t got; buf && (got = fread(buf + len, 1, cap - len - 1, f)) > 0;) {
        len += got;
        if (cap - len < 2) {
            char *b = realloc(buf, cap * 2);
            if (!b) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = b;
            cap *= 2;
        }
    }
    fclose(f);
    if (!buf) {
        snprintf(out->error, sizeof out->error, "out of memory");
        return false;
    }
    buf[len] = '\0';
    bool ok = ccr_yaml_parse(buf, out);
    free(buf);
    return ok;
}

void ccr_yaml_free(ccr_yaml *y)
{
    free_node(y->root);
    y->root = NULL;
}

const ccr_yaml_node *ccr_yaml_get(const ccr_yaml_node *map, const char *key)
{
    if (!map || map->kind != CCR_YAML_MAP)
        return NULL;
    for (const ccr_yaml_node *c = map->first; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

const char *ccr_yaml_str(const ccr_yaml_node *map, const char *key, const char *fallback)
{
    const ccr_yaml_node *n = ccr_yaml_get(map, key);
    return n && n->kind == CCR_YAML_SCALAR && n->value ? n->value : fallback;
}
