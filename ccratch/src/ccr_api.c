#define _DEFAULT_SOURCE   /* realpath */
#include "ccr_api.h"
#include "ccr_parse.h"

#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------ building */

ccr_api_func *ccr_api_add_func(ccr_api *api, const char *name)
{
    for (int i = 0; i < api->func_count; ++i)   /* declared twice: one function */
        if (strcmp(api->funcs[i].name, name) == 0)
            return &api->funcs[i];
    if (strlen(name) >= sizeof api->funcs[0].name)
        return NULL;
    if (api->func_count == api->func_cap) {
        int cap = api->func_cap ? api->func_cap * 2 : 128;
        ccr_api_func *v = realloc(api->funcs, (size_t)cap * sizeof *v);
        if (!v)
            return NULL;
        api->funcs = v;
        api->func_cap = cap;
    }
    ccr_api_func *f = &api->funcs[api->func_count++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    return f;
}

ccr_api_enum *ccr_api_add_enum(ccr_api *api, const char *name)
{
    if (api->enum_count == api->enum_cap) {
        int cap = api->enum_cap ? api->enum_cap * 2 : 32;
        ccr_api_enum *v = realloc(api->enums, (size_t)cap * sizeof *v);
        if (!v)
            return NULL;
        api->enums = v;
        api->enum_cap = cap;
    }
    ccr_api_enum *e = &api->enums[api->enum_count++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    return e;
}

void ccr_api_enum_value(ccr_api_enum *e, const char *value)
{
    char **v = realloc(e->values, (size_t)(e->count + 1) * sizeof *v);
    if (!v)
        return;
    e->values = v;
    e->values[e->count] = malloc(strlen(value) + 1);
    if (e->values[e->count])
        strcpy(e->values[e->count++], value);
}

void ccr_api_free(ccr_api *api)
{
    for (int i = 0; i < api->enum_count; ++i) {
        for (int k = 0; k < api->enums[i].count; ++k)
            free(api->enums[i].values[k]);
        free(api->enums[i].values);
    }
    free(api->enums);
    free(api->funcs);
    memset(api, 0, sizeof *api);
}

bool ccr_api_scan(const char *header, const char *const *clang_args, int nargs, ccr_api *out,
                  char *err, int err_cap)
{
    memset(out, 0, sizeof *out);
    struct stat st;
    if (stat(header, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, (size_t)err_cap, "%s: no such file", header);
        return false;
    }
    if (!ccr_parse_api(header, clang_args, nargs, out)) {
        snprintf(err, (size_t)err_cap, "clang could not read %s", header);
        ccr_api_free(out);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ binaries */

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* libNAME.so* or libNAME.a in `dir` */
static bool find_in(const char *dir, const char *name, char *out, int cap)
{
    char want_so[160], want_a[160];
    snprintf(want_so, sizeof want_so, "lib%s.so", name);
    snprintf(want_a, sizeof want_a, "lib%s.a", name);
    snprintf(out, (size_t)cap, "%s/%s", dir, want_so);
    if (exists(out))
        return true;
    snprintf(out, (size_t)cap, "%s/%s", dir, want_a);
    if (exists(out))
        return true;
    DIR *d = opendir(dir);   /* libNAME.so.5.5 */
    if (!d)
        return false;
    bool found = false;
    const size_t n = strlen(want_so);
    for (struct dirent *e = readdir(d); e && !found; e = readdir(d))
        if (strncmp(e->d_name, want_so, n) == 0 && e->d_name[n] == '.') {
            snprintf(out, (size_t)cap, "%s/%s", dir, e->d_name);
            found = exists(out);
        }
    closedir(d);
    return found;
}

/* A few folders down from `dir` (build trees put libraries deep). */
static bool find_below(const char *dir, const char *name, int depth, char *out, int cap)
{
    if (find_in(dir, name, out, cap))
        return true;
    if (depth == 0)
        return false;
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e = readdir(d); e && !found; e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        char sub[2048];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            found = find_below(sub, name, depth - 1, out, cap);
    }
    closedir(d);
    return found;
}

bool ccr_api_find_binary(const char *name, const char *project, char *out, int cap)
{
    if (project && *project && find_below(project, name, 4, out, cap))
        return true;
    static const char *const sys[] = { "/usr/local/lib", "/usr/local/lib64", "/usr/lib64", "/usr/lib",
                                       "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu" };
    for (size_t i = 0; i < sizeof sys / sizeof sys[0]; ++i)
        if (find_in(sys[i], name, out, cap))
            return true;
    return false;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* Marks the functions an ELF object defines globally; how many it has. */
static int elf_exports(ccr_api *api, const unsigned char *b, size_t len, bool dynamic)
{
    if (len < sizeof(Elf64_Ehdr) || memcmp(b, ELFMAG, SELFMAG) != 0 || b[EI_CLASS] != ELFCLASS64 ||
        b[EI_DATA] != ELFDATA2LSB)
        return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > len)
        return -1;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(b + eh->e_shoff);
    int total = 0;
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != (dynamic ? SHT_DYNSYM : SHT_SYMTAB) || sh[i].sh_link >= eh->e_shnum)
            continue;
        const Elf64_Shdr *strs = &sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > len || strs->sh_offset + strs->sh_size > len)
            continue;
        const Elf64_Sym *sym = (const Elf64_Sym *)(b + sh[i].sh_offset);
        const size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < n; ++k) {
            const unsigned bind = ELF64_ST_BIND(sym[k].st_info), type = ELF64_ST_TYPE(sym[k].st_info);
            if (type != STT_FUNC || (bind != STB_GLOBAL && bind != STB_WEAK) ||
                sym[k].st_shndx == SHN_UNDEF || sym[k].st_name >= strs->sh_size)
                continue;
            const char *name = (const char *)(b + strs->sh_offset + sym[k].st_name);
            ++total;
            for (int f = 0; f < api->func_count; ++f)
                if (strcmp(api->funcs[f].name, name) == 0)
                    api->funcs[f].exported = true;
        }
    }
    return total;
}

int ccr_api_check_binary(ccr_api *api, const char *binary)
{
    size_t len;
    unsigned char *b = slurp(binary, &len);
    if (!b)
        return -1;
    int total = -1;
    if (len > 8 && memcmp(b, "!<arch>\n", 8) == 0) {
        /* a static library: every object in it */
        total = 0;
        for (size_t at = 8; at + 60 <= len;) {
            char size_s[11];
            memcpy(size_s, b + at + 48, 10);
            size_s[10] = '\0';
            const size_t size = (size_t)strtoull(size_s, NULL, 10);
            const size_t body = at + 60;
            if (body + size > len)
                break;
            const int k = elf_exports(api, b + body, size, false);
            if (k > 0)
                total += k;
            at = body + size + (size & 1);
        }
    } else {
        total = elf_exports(api, b, len, true);
    }
    free(b);
    if (total <= 0)
        return total;
    /* keep what the binary has, and what the header defines itself */
    int keep = 0;
    for (int i = 0; i < api->func_count; ++i)
        if (api->funcs[i].exported || api->funcs[i].inline_def)
            api->funcs[keep++] = api->funcs[i];
    api->func_count = keep;
    snprintf(api->binary, sizeof api->binary, "%s", binary);
    return total;
}

/* ------------------------------------------------------------ the pack */

/* DrawTextEx -> "draw text ex"; SDL_CreateWindow -> "SDL create window" */
static void humanize(const char *name, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *c = name; *c && n + 2 < cap; ++c) {
        if (*c == '_') {
            if (n && out[n - 1] != ' ')
                out[n++] = ' ';
            continue;
        }
        const bool upper = isupper((unsigned char)*c);
        const bool after_lower = c > name && (islower((unsigned char)c[-1]) || isdigit((unsigned char)c[-1]));
        const bool before_lower = upper && c > name && isupper((unsigned char)c[-1]) && islower((unsigned char)c[1]);
        if ((upper && after_lower) || before_lower)
            if (n && out[n - 1] != ' ')
                out[n++] = ' ';
        out[n++] = *c;
    }
    out[n] = '\0';
    for (char *w = out; *w;) {   /* lower case, but SDL and GL stay */
        char *e = w;
        bool caps = true;
        int len = 0;
        while (*e && *e != ' ') {
            caps = caps && !islower((unsigned char)*e);
            ++e;
            ++len;
        }
        if (!caps || len == 1)
            for (char *c = w; c < e; ++c)
                *c = (char)tolower((unsigned char)*c);
        w = *e ? e + 1 : e;
    }
}

/* `@block TEXT` (or `@block.ru TEXT`) in a doc comment; false if none. */
static bool annotation(const char *doc, const char *tag, char *out, size_t cap)
{
    for (const char *c = doc; (c = strstr(c, tag)) != NULL; c += strlen(tag)) {
        const char *after = c + strlen(tag);
        if (*after != ' ' && *after != '\t')
            continue;   /* @block.ru when looking for @block */
        while (*after == ' ' || *after == '\t')
            ++after;
        size_t n = strcspn(after, "\r\n");
        while (n > 0 && (after[n - 1] == ' ' || after[n - 1] == '/' || after[n - 1] == '*'))
            --n;
        snprintf(out, cap, "%.*s", (int)n, after);
        return true;
    }
    return false;
}

static bool is_ident(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_'))
        return false;
    for (; *s; ++s)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return false;
    return true;
}

/* YAML double quoted: \ and " escaped */
static void yaml_quoted(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            fputc('\\', out);
        fputc(*s, out);
    }
    fputc('"', out);
}

void ccr_api_write_pack(const ccr_api *api, const char *name, const char *include,
                        const char *link, FILE *out)
{
    bool picked = false;   /* the library chose its blocks with @block */
    char tmp[512];
    for (int i = 0; i < api->func_count && !picked; ++i)
        picked = annotation(api->funcs[i].doc, "@block", tmp, sizeof tmp);

    int kept = 0;
    for (int i = 0; i < api->func_count; ++i)
        kept += !picked || annotation(api->funcs[i].doc, "@block", tmp, sizeof tmp);
    fprintf(out, "# Blocks for %s: %d function%s", include, kept, kept == 1 ? "" : "s");
    if (api->binary[0])
        fprintf(out, ", the ones %s exports", api->binary);
    fprintf(out, ".\n# Made by Ccratch. Make the texts read well ({name} is a slot), add\n"
                 "# defaults, drop what nobody needs; text.ru: and friends translate.\n");
    fprintf(out, "name: %s\nheader: ", name);
    /* the header as files name it, without <> or quotes */
    char bare[256];
    snprintf(bare, sizeof bare, "%s", include + (include[0] == '<' || include[0] == '"'));
    size_t bl = strlen(bare);
    if (bl && (bare[bl - 1] == '>' || bare[bl - 1] == '"'))
        bare[bl - 1] = '\0';
    yaml_quoted(out, bare);
    fprintf(out, "\ninclude: ");
    yaml_quoted(out, include);
    if (link && *link)
        fprintf(out, "\nlink: %s", link);
    fprintf(out, "\ncolor: library\nblocks:\n");

    if (!kept)
        fprintf(out, "  []   # no public functions found\n");
    for (int i = 0; i < api->func_count; ++i) {
        const ccr_api_func *f = &api->funcs[i];
        char text[512], ru[512];
        const bool has = annotation(f->doc, "@block", text, sizeof text);
        if (picked && !has)
            continue;
        const bool has_ru = annotation(f->doc, "@block.ru", ru, sizeof ru);
        char pnames[CCR_API_MAX_PARAMS + 1][48];
        int np = 0;
        for (int k = 0; k < f->nparams; ++k) {
            const char *pn = f->params[k].name;
            while (*pn == '_')   /* glibc's __format: format */
                ++pn;
            bool twice = false;
            for (int j = 0; j < np; ++j)
                twice = twice || strcmp(pnames[j], pn) == 0;
            if (is_ident(pn) && !twice)
                snprintf(pnames[np], sizeof pnames[np], "%s", pn);
            else
                snprintf(pnames[np], sizeof pnames[np], "arg%d", k + 1);
            ++np;
        }
        if (f->variadic)   /* printf-like: the rest as one slot of text */
            snprintf(pnames[np++], sizeof pnames[0], "more");
        if (!has) {
            humanize(f->name, text, sizeof text);
            size_t tl = strlen(text);
            /* the slots say what they are: "deflate {strm} flush {flush}" (the
             * first reads on from the name; argN says nothing) */
            for (int k = 0; k < np && tl < sizeof text; ++k) {
                char label[64] = "";
                if (k > 0 && f->variadic && k == np - 1)
                    snprintf(label, sizeof label, "with");   /* printf {format} with {more} */
                else if (k > 0 && strncmp(pnames[k], "arg", 3) != 0)
                    humanize(pnames[k], label, sizeof label);
                tl += (size_t)snprintf(text + tl, sizeof text - tl, "%s%s {%s}", label[0] ? " " : "",
                                       label, pnames[k]);
            }
        }
        fprintf(out, "  - call: %s\n    text: ", f->name);
        yaml_quoted(out, text);
        if (has_ru) {
            fprintf(out, "\n    text.ru: ");
            yaml_quoted(out, ru);
        }
        if (np) {
            fprintf(out, "\n    params: [");
            for (int k = 0; k < np; ++k)
                fprintf(out, "%s%s", k ? ", " : "", pnames[k]);
            fprintf(out, "]\n    types: [");
            for (int k = 0; k < f->nparams; ++k) {
                fputs(k ? ", " : "", out);
                yaml_quoted(out, f->params[k].type);
            }
            if (f->variadic) {
                fputs(f->nparams ? ", " : "", out);
                yaml_quoted(out, "...");
            }
            fputc(']', out);
        }
        if (f->ret[0] && strcmp(f->ret, "void") != 0) {
            fprintf(out, "\n    returns: ");
            yaml_quoted(out, f->ret);
        }
        fputc('\n', out);
    }

    /* the enums: the values a slot of that type takes (choices, later) */
    int named = 0;
    for (int i = 0; i < api->enum_count; ++i)
        named += api->enums[i].name[0] && api->enums[i].count > 0;
    if (named) {
        fprintf(out, "enums:\n");
        for (int i = 0; i < api->enum_count; ++i) {
            const ccr_api_enum *e = &api->enums[i];
            if (!e->name[0] || !e->count || !is_ident(e->name))
                continue;
            fprintf(out, "  %s: [", e->name);
            for (int k = 0; k < e->count; ++k)
                fprintf(out, "%s%s", k ? ", " : "", e->values[k]);
            fprintf(out, "]\n");
        }
    }
}
