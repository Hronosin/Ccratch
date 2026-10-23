#include "ccr_search.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ case */

/* One letter (at p, `n` bytes left) to small letters, in place in `out`
 * (as long as it was). Latin, Cyrillic (Ukrainian, Belarusian too). The
 * bytes it took. */
static size_t fold_char(const unsigned char *p, size_t n, unsigned char *out)
{
    if (p[0] < 0x80) {
        out[0] = (unsigned char)((p[0] >= 'A' && p[0] <= 'Z') ? p[0] + 32 : p[0]);
        return 1;
    }
    if (n >= 2 && p[0] == 0xD0 && p[1] >= 0x80 && p[1] <= 0x8F) {   /* Ѐ-Џ: Ё Є І Ї Ў... */
        out[0] = 0xD1;
        out[1] = (unsigned char)(p[1] + 0x10);
        return 2;
    }
    if (n >= 2 && p[0] == 0xD0 && p[1] >= 0x90 && p[1] <= 0x9F) {   /* А-П */
        out[0] = 0xD0;
        out[1] = (unsigned char)(p[1] + 0x20);
        return 2;
    }
    if (n >= 2 && p[0] == 0xD0 && p[1] >= 0xA0 && p[1] <= 0xAF) {   /* Р-Я */
        out[0] = 0xD1;
        out[1] = (unsigned char)(p[1] - 0x20);
        return 2;
    }
    if (n >= 2 && p[0] == 0xD2 && p[1] == 0x90) {                    /* Ґ */
        out[0] = 0xD2;
        out[1] = 0x91;
        return 2;
    }
    size_t k = (p[0] >> 5) == 6 ? 2 : (p[0] >> 4) == 14 ? 3 : (p[0] >> 3) == 30 ? 4 : 1;
    if (k > n)
        k = n;
    memcpy(out, p, k);
    return k;
}

static void fold(const char *in, size_t len, char *out)
{
    const unsigned char *p = (const unsigned char *)in;
    for (size_t i = 0; i < len;)
        i += fold_char(p + i, len - i, (unsigned char *)out + i);
}

/* The same letter in capitals (the other way round from fold_char). */
static size_t upper_char(const unsigned char *p, size_t n, unsigned char *out)
{
    if (p[0] < 0x80) {
        out[0] = (unsigned char)((p[0] >= 'a' && p[0] <= 'z') ? p[0] - 32 : p[0]);
        return 1;
    }
    if (n >= 2 && p[0] == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) {   /* а-п */
        out[0] = 0xD0;
        out[1] = (unsigned char)(p[1] - 0x20);
        return 2;
    }
    if (n >= 2 && p[0] == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) {   /* р-я */
        out[0] = 0xD0;
        out[1] = (unsigned char)(p[1] + 0x20);
        return 2;
    }
    if (n >= 2 && p[0] == 0xD1 && p[1] >= 0x90 && p[1] <= 0x9F) {   /* ѐ-џ */
        out[0] = 0xD0;
        out[1] = (unsigned char)(p[1] - 0x10);
        return 2;
    }
    if (n >= 2 && p[0] == 0xD2 && p[1] == 0x91) {
        out[0] = 0xD2;
        out[1] = 0x90;
        return 2;
    }
    size_t k = (p[0] >> 5) == 6 ? 2 : (p[0] >> 4) == 14 ? 3 : (p[0] >> 3) == 30 ? 4 : 1;
    if (k > n)
        k = n;
    memcpy(out, p, k);
    return k;
}

/* Is the letter at p a capital (1), a small one (-1), or no letter (0)? */
static int letter_case(const unsigned char *p, size_t n, size_t *took)
{
    unsigned char lo[4], up[4];
    const size_t k = fold_char(p, n, lo);
    upper_char(p, n, up);
    *took = k;
    const bool differs = memcmp(lo, up, k) != 0;
    if (!differs)
        return 0;
    return memcmp(p, up, k) == 0 ? 1 : -1;
}

/* ---------------------------------------------------------- regions */

void ccr_search_regions(const char *text, size_t len, unsigned char *kinds)
{
    size_t i = 0;
    while (i < len) {
        const char c = text[i];
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {   /* to the end of the line (a \ at it goes on) */
            size_t j = i;
            while (j < len && text[j] != '\n') {
                if (text[j] == '\\' && j + 1 < len && text[j + 1] == '\n')
                    ++j;
                ++j;
            }
            memset(kinds + i, CCR_IN_COMMENT, j - i);
            i = j;
        } else if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < len && !(text[j] == '*' && text[j + 1] == '/'))
                ++j;
            j = j + 2 <= len ? j + 2 : len;
            memset(kinds + i, CCR_IN_COMMENT, j - i);
            i = j;
        } else if (c == 'R' && i + 1 < len && text[i + 1] == '"' &&
                   (i == 0 || !((text[i - 1] >= 'a' && text[i - 1] <= 'z') || (text[i - 1] >= 'A' && text[i - 1] <= 'Z') ||
                                (text[i - 1] >= '0' && text[i - 1] <= '9') || text[i - 1] == '_'))) {
            /* R"delim( ... )delim" */
            size_t j = i + 2, d0 = j;
            while (j < len && text[j] != '(' && j - d0 < 16)
                ++j;
            const size_t dl = j - d0;
            size_t k = j + 1;
            for (; k < len; ++k)
                if (text[k] == ')' && k + 1 + dl < len && memcmp(text + k + 1, text + d0, dl) == 0 &&
                    text[k + 1 + dl] == '"')
                    break;
            k = k + 2 + dl <= len ? k + 2 + dl : len;
            memset(kinds + i, CCR_IN_STRING, k - i);
            i = k;
        } else if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < len && text[j] != c && text[j] != '\n') {
                if (text[j] == '\\' && j + 1 < len)
                    ++j;
                ++j;
            }
            j = j < len && text[j] == c ? j + 1 : j;
            memset(kinds + i, CCR_IN_STRING, j - i);
            i = j;
        } else if ((c >= '0' && c <= '9') ) {   /* 1'000'000: a C++ digit separator is no quote */
            size_t j = i;
            while (j < len && ((text[j] >= '0' && text[j] <= '9') || (text[j] >= 'a' && text[j] <= 'z') ||
                               (text[j] >= 'A' && text[j] <= 'Z') || text[j] == '.' || text[j] == '_' ||
                               (text[j] == '\'' && j + 1 < len && text[j + 1] >= '0' && text[j + 1] <= 'z')))
                ++j;
            memset(kinds + i, CCR_IN_CODE, j - i);
            i = j;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {   /* u8"..", L'x': the prefix is the literal's */
            size_t j = i;
            while (j < len && ((text[j] >= 'a' && text[j] <= 'z') || (text[j] >= 'A' && text[j] <= 'Z') ||
                               (text[j] >= '0' && text[j] <= '9') || text[j] == '_'))
                ++j;
            const bool prefix = j < len && (text[j] == '"' || text[j] == '\'') && j - i <= 2 &&
                                (text[i] == 'u' || text[i] == 'U' || text[i] == 'L');
            memset(kinds + i, prefix ? CCR_IN_STRING : CCR_IN_CODE, j - i);
            i = j;
        } else {
            kinds[i++] = CCR_IN_CODE;
        }
    }
}

/* ----------------------------------------------------------- search */

static bool word_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

int ccr_search_text(const char *text, size_t len, const char *query, const ccr_search_opts *o,
                    ccr_search_hit *out, int cap)
{
    const size_t ql = query ? strlen(query) : 0;
    if (!ql || ql > len)
        return 0;
    char *hay = NULL, *needle = NULL;
    const char *h = text, *q = query;
    if (!o->match_case) {
        hay = malloc(len);
        needle = malloc(ql);
        if (!hay || !needle) {
            free(hay);
            free(needle);
            return 0;
        }
        fold(text, len, hay);
        fold(query, ql, needle);
        h = hay;
        q = needle;
    }
    unsigned char *kinds = malloc(len);
    if (kinds)
        ccr_search_regions(text, len, kinds);
    const unsigned where = o->where ? o->where : CCR_IN_ALL;
    int n = 0, line = 1;
    size_t counted = 0;   /* lines are counted up to here */
    for (size_t i = 0; i + ql <= len;) {
        const char *at = memchr(h + i, q[0], len - ql + 1 - i);
        if (!at)
            break;
        i = (size_t)(at - h);
        if (memcmp(h + i, q, ql) != 0) {
            ++i;
            continue;
        }
        const unsigned kind = kinds ? kinds[i] : CCR_IN_CODE;
        const bool whole = !o->whole_word ||
                           ((i == 0 || !word_char((unsigned char)text[i - 1])) &&
                            (i + ql == len || !word_char((unsigned char)text[i + ql])));
        if (!(kind & where) || !whole) {
            ++i;
            continue;
        }
        for (; counted < i; ++counted)
            if (text[counted] == '\n')
                ++line;
        if (n < cap)
            out[n] = (ccr_search_hit){ (uint32_t)i, (uint32_t)ql, line, kind };
        ++n;
        i += ql;
    }
    free(kinds);
    free(hay);
    free(needle);
    return n;
}

/* --------------------------------------------------------- replace */

void ccr_search_replacement(const char *text, const ccr_search_hit *h, const char *with, bool preserve_case,
                            char *out, size_t cap)
{
    if (!cap)
        return;
    const size_t wl = strlen(with);
    char *cased = malloc(wl + 1);
    if (!cased) {
        out[0] = '\0';
        return;
    }
    memcpy(cased, with, wl + 1);
    if (preserve_case) {
        int ups = 0, lows = 0, first = 0;
        const unsigned char *p = (const unsigned char *)text + h->offset;
        for (size_t i = 0, took = 1; i < h->len; i += took) {
            const int c = letter_case(p + i, h->len - i, &took);
            if (c && !first)
                first = c;
            ups += c > 0;
            lows += c < 0;
        }
        const unsigned char *w = (const unsigned char *)with;
        if (ups > 1 && lows == 0) {   /* HELLO: HI */
            for (size_t i = 0; i < wl;)
                i += upper_char(w + i, wl - i, (unsigned char *)cased + i);
        } else if (first > 0) {       /* Hello: Hi */
            size_t i = 0, took = 1;
            while (i < wl && letter_case(w + i, wl - i, &took) == 0)
                i += took;
            if (i < wl)
                upper_char(w + i, wl - i, (unsigned char *)cased + i);
        } else if (first < 0) {       /* hello: hi */
            for (size_t i = 0; i < wl;)
                i += fold_char(w + i, wl - i, (unsigned char *)cased + i);
        }
    }
    size_t n = 0;
    const bool literal = h->where == CCR_IN_STRING;
    for (size_t i = 0; i < wl && n + 3 < cap; ++i) {
        const char c = cased[i];
        if (literal && (c == '"' || c == '\'' || c == '\\')) {
            out[n++] = '\\';
            out[n++] = c;
        } else if (literal && c == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    free(cased);
}

char *ccr_search_replace(const char *text, size_t len, const ccr_search_hit *hits, int n, const char *with,
                         bool preserve_case, size_t *out_len)
{
    const size_t wl = strlen(with);
    const size_t one = wl * 2 + 8;   /* escaped: at most twice as long */
    size_t cap = len + (size_t)n * one + 1;
    char *o = malloc(cap), *rep = malloc(one);
    if (!o || !rep) {
        free(o);
        free(rep);
        return NULL;
    }
    rep[0] = '\0';
    size_t w = 0, from = 0;
    for (int i = 0; i < n; ++i) {
        if (hits[i].offset < from || hits[i].offset + hits[i].len > len)
            continue;   /* overlapping or out of the text: left */
        memcpy(o + w, text + from, hits[i].offset - from);
        w += hits[i].offset - from;
        ccr_search_replacement(text, &hits[i], with, preserve_case, rep, one);
        const size_t rl = strlen(rep);
        memcpy(o + w, rep, rl);
        w += rl;
        from = hits[i].offset + hits[i].len;
    }
    memcpy(o + w, text + from, len - from);
    w += len - from;
    o[w] = '\0';
    free(rep);
    if (out_len)
        *out_len = w;
    return o;
}

void ccr_search_line(const char *text, size_t len, uint32_t offset, char *out, size_t cap, int *at)
{
    if (!cap)
        return;
    size_t b = offset > len ? len : offset;
    while (b > 0 && text[b - 1] != '\n')
        --b;
    while (b < offset && (text[b] == ' ' || text[b] == '\t'))
        ++b;
    size_t e = offset;
    while (e < len && text[e] != '\n' && text[e] != '\r')
        ++e;
    /* a long line: start a little before the match, so it shows */
    if (offset - b > 40 && e - b >= cap - 1) {
        b = offset - 30;
        while (b < offset && ((unsigned char)text[b] & 0xC0u) == 0x80u)
            ++b;
    }
    size_t n = e - b < cap - 1 ? e - b : cap - 1;
    while (n > 0 && n < e - b && ((unsigned char)text[b + n] & 0xC0u) == 0x80u)
        --n;   /* not in the middle of a letter */
    memcpy(out, text + b, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; ++i)
        if (out[i] == '\t')
            out[i] = ' ';
    if (at)
        *at = offset - b <= n ? (int)(offset - b) : -1;
}
