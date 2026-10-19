/*
 * Every character of every shipped language pack can be drawn: Nunito has
 * it (and draws it: ccr_fonts.h), or the font the pack names with @font
 * has it. A pack that changes without tools/make_font.py being run again
 * fails here, naming the characters.
 *
 *   test_fonts LANG_DIR FONTS_DIR
 */
#include "ccr_fonts.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    unsigned char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len > 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)len + 1)) != NULL) {
            *n = fread(buf, 1, (size_t)len, f);
            buf[*n] = '\0';
        }
    }
    fclose(f);
    return buf;
}

/* ------------------------------------------------- a TrueType cmap */

typedef struct font {
    unsigned char *data;
    size_t         size;
    uint32_t       cmap;   /* the offset of the Unicode subtable, 0: none */
    int            format;
} font;

static uint32_t be16(const font *f, size_t at) { return at + 2 <= f->size ? (uint32_t)(f->data[at] << 8 | f->data[at + 1]) : 0; }
static uint32_t be32(const font *f, size_t at) { return (be16(f, at) << 16) | be16(f, at + 2); }

static bool font_open(font *f, const char *path)
{
    memset(f, 0, sizeof *f);
    f->data = slurp(path, &f->size);
    if (!f->data)
        return false;
    const uint32_t tables = be16(f, 4);
    uint32_t cmap = 0;
    for (uint32_t i = 0; i < tables; ++i)
        if (memcmp(f->data + 12 + 16 * i, "cmap", 4) == 0)
            cmap = be32(f, 12 + 16 * i + 8);
    if (!cmap)
        return false;
    /* the best Unicode subtable: format 12 (all of Unicode), else 4 */
    const uint32_t subs = be16(f, cmap + 2);
    for (uint32_t i = 0; i < subs; ++i) {
        const uint32_t platform = be16(f, cmap + 4 + 8 * i), encoding = be16(f, cmap + 6 + 8 * i);
        const uint32_t at = cmap + be32(f, cmap + 8 + 8 * i);
        const int format = (int)be16(f, at);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (unicode && (format == 12 || (format == 4 && f->format != 12))) {
            f->cmap = at;
            f->format = format;
        }
    }
    return f->cmap != 0;
}

static bool font_has(const font *f, uint32_t c)
{
    if (f->format == 12) {
        const uint32_t groups = be32(f, f->cmap + 12);
        for (uint32_t i = 0; i < groups; ++i) {
            const size_t g = f->cmap + 16 + 12 * (size_t)i;
            if (c >= be32(f, g) && c <= be32(f, g + 4))
                return true;
        }
        return false;
    }
    if (f->format != 4 || c > 0xFFFF)
        return false;
    const uint32_t segs = be16(f, f->cmap + 6) / 2;
    const size_t ends = f->cmap + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs,
                 ranges = deltas + 2 * segs;
    for (uint32_t i = 0; i < segs; ++i) {
        if (c > be16(f, ends + 2 * i))
            continue;
        const uint32_t start = be16(f, starts + 2 * i);
        if (c < start)
            return false;
        const uint32_t ro = be16(f, ranges + 2 * i);
        if (ro == 0)
            return ((c + be16(f, deltas + 2 * i)) & 0xFFFF) != 0;
        const size_t at = ranges + 2 * i + ro + 2 * (c - start);
        return be16(f, at) != 0;
    }
    return false;
}

/* ------------------------------------------------------ the packs */

typedef struct pack {
    char  code[32];
    char  font[64];
    char  base[32];
    char *text;
} pack;

static pack g_packs[96];
static int  g_npacks;

static void read_pack(const char *dir, const char *file)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    size_t n;
    char *text = (char *)slurp(path, &n);
    if (!text || g_npacks == 96)
        return;
    pack *p = &g_packs[g_npacks++];
    memset(p, 0, sizeof *p);
    snprintf(p->code, sizeof p->code, "%.*s", (int)(strlen(file) - 5), file);
    p->text = text;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (strncmp(line, "@font", 5) == 0 || strncmp(line, "@base", 5) == 0) {
            char *v = strchr(line, '=');
            if (v) {
                ++v;
                while (*v == ' ')
                    ++v;
                const size_t len = next ? (size_t)(next - v) : strlen(v);
                if (line[1] == 'f')
                    snprintf(p->font, sizeof p->font, "%.*s", (int)len, v);
                else
                    snprintf(p->base, sizeof p->base, "%.*s", (int)len, v);
            }
        }
        line = next ? next + 1 : NULL;
    }
}

static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p;
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : 3;
    c &= n == 0 ? 0x7Fu : n == 1 ? 0x1Fu : n == 2 ? 0x0Fu : 0x07u;
    ++p;
    for (int i = 0; i < n && *p; ++i, ++p)
        c = (c << 6) | (*p & 0x3Fu);
    *s = (const char *)p;
    return c;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test_fonts LANG_DIR FONTS_DIR\n");
        return 2;
    }
    DIR *d = opendir(argv[1]);
    if (!d) {
        fprintf(stderr, "no %s\n", argv[1]);
        return 1;
    }
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        const size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".lang") == 0)
            read_pack(argv[1], e->d_name);
    }
    closedir(d);

    font nunito;
    char path[1200];
    snprintf(path, sizeof path, "%s/nunito-ccratch.ttf", argv[2]);
    if (!font_open(&nunito, path)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    int checked = 0;
    for (int i = 0; i < g_npacks; ++i) {
        pack *p = &g_packs[i];
        if (!p->font[0])   /* a plain-words pack: its base's font */
            for (int k = 0; k < g_npacks; ++k)
                if (strcmp(g_packs[k].code, p->base) == 0)
                    snprintf(p->font, sizeof p->font, "%s", g_packs[k].font);
        font extra = { 0 };
        if (p->font[0]) {
            snprintf(path, sizeof path, "%s/%s-ccratch.ttf", argv[2], p->font);
            if (!font_open(&extra, path)) {
                fprintf(stderr, "FAIL %s: @font %s, but there is no %s\n", p->code, p->font, path);
                ++g_failures;
            }
        }
        char missing[400] = "";
        size_t ml = 0;
        int nmissing = 0;
        bool comment = false, line_start = true;
        for (const char *s = p->text; *s;) {
            const unsigned c = next_cp(&s);
            if (line_start)
                comment = c == '#';   /* a comment: never shown */
            line_start = c == '\n';
            if (c < 0x20 || c == 0x7F || comment)
                continue;
            ++checked;
            const bool ok = ccr_font_nunito((int)c) ? font_has(&nunito, c) : extra.data && font_has(&extra, c);
            if (ok)
                continue;
            if (nmissing < 40) {   /* the first few, each once */
                char one[8];
                const char *q = s;
                int back = 1;
                while (q - back > p->text && ((unsigned char)q[-back] & 0xC0) == 0x80)
                    ++back;
                snprintf(one, sizeof one, "%.*s", back, q - back);
                if (!strstr(missing, one) && ml + strlen(one) + 2 < sizeof missing)
                    ml += (size_t)snprintf(missing + ml, sizeof missing - ml, "%s ", one);
            }
            ++nmissing;
        }
        if (nmissing) {
            fprintf(stderr, "FAIL %s: %d character(s) no font draws: %s(run tools/make_font.py)\n", p->code,
                    nmissing, missing);
            ++g_failures;
        }
        free(extra.data);
    }
    for (int i = 0; i < g_npacks; ++i)
        free(g_packs[i].text);
    free(nunito.data);
    if (g_failures)
        fprintf(stderr, "%d pack(s) cannot be drawn\n", g_failures);
    else
        printf("fonts: %d packs, %d characters, all drawn\n", g_npacks, checked);
    return g_failures ? 1 : 0;
}
