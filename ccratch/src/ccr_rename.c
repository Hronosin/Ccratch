#define _DEFAULT_SOURCE   /* realpath */
#include "ccr_rename.h"
#include "ccr_parse.h"
#include "ccr_project.h"

#include <clang-c/Index.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 400

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    fclose(f);
    if (s) {
        s[n] = '\0';
        if (len)
            *len = (size_t)n;
    }
    return s;
}

static bool write_all(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    const bool ok = fwrite(text, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static bool ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

bool ccr_rename_valid(const char *name)
{
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
        "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "_Bool", "_Complex", "_Imaginary", "_Alignas", "_Alignof", "_Atomic",
        "_Generic", "_Noreturn", "_Static_assert", "_Thread_local", "bool", "true", "false", "NULL",
    };
    if (!name || !*name || !(isalpha((unsigned char)*name) || *name == '_') || strlen(name) >= 128)
        return false;
    for (const char *c = name; *c; ++c)
        if (!ident_char(*c))
            return false;
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; ++i)
        if (strcmp(name, keywords[i]) == 0)
            return false;
    return true;
}

/* ------------------------------------------------------------- clang */

static CXTranslationUnit parse(CXIndex index, const char *path, const char *root, const char *const *args,
                               int nargs)
{
    const char *all[128];
    int n = 0;
    char inc1[1100], inc2[1100];
    snprintf(inc1, sizeof inc1, "-I%s", root);
    snprintf(inc2, sizeof inc2, "-I%s/include", root);
    all[n++] = "-x";
    all[n++] = "c";
    all[n++] = inc1;
    all[n++] = inc2;
    bool std_given = false;
    for (int i = 0; i < nargs && n < 120; ++i) {
        std_given = std_given || strncmp(args[i], "-std=", 5) == 0;
        all[n++] = args[i];
    }
    if (!std_given)
        all[n++] = "-std=c11";
    const char *resource = ccr_parse_resource_dir(NULL);
    if (resource) {
        all[n++] = "-resource-dir";
        all[n++] = resource;
    }
    CXTranslationUnit tu = NULL;
    if (clang_parseTranslationUnit2(index, path, all, n, NULL, 0, CXTranslationUnit_KeepGoing, &tu) !=
        CXError_Success)
        return NULL;
    return tu;
}

static void str_copy(char *dst, size_t cap, CXString s)
{
    const char *c = clang_getCString(s);
    snprintf(dst, cap, "%s", c ? c : "");
    clang_disposeString(s);
}

/* The thing a cursor names: itself if it declares, else what it refers to. */
static CXCursor named(CXCursor c)
{
    if (clang_isDeclaration(clang_getCursorKind(c)))
        return c;
    return clang_getCursorReferenced(c);
}

typedef struct collect_ctx {
    const char *usr;
    const char *name;
    size_t      name_len;
    CXFile      main_file;
    const char *text;
    size_t      text_len;
    uint32_t   *offsets;
    int         count, cap;
} collect_ctx;

static void add_offset(collect_ctx *c, uint32_t off)
{
    for (int i = 0; i < c->count; ++i)
        if (c->offsets[i] == off)
            return;
    if (c->count == c->cap) {
        const int cap = c->cap ? c->cap * 2 : 32;
        uint32_t *v = realloc(c->offsets, (size_t)cap * sizeof *v);
        if (!v)
            return;
        c->offsets = v;
        c->cap = cap;
    }
    c->offsets[c->count++] = off;
}

static enum CXChildVisitResult collect(CXCursor cur, CXCursor parent, CXClientData data)
{
    (void)parent;
    collect_ctx *c = data;
    CXFile file;
    unsigned off = 0;
    clang_getSpellingLocation(clang_getCursorLocation(cur), &file, NULL, NULL, &off);
    if (!file || !clang_File_isEqual(file, c->main_file))
        return CXChildVisit_Recurse;   /* an include's: that file is read on its own */
    const CXCursor target = named(cur);
    if (clang_Cursor_isNull(target))
        return CXChildVisit_Recurse;
    char usr[512];
    str_copy(usr, sizeof usr, clang_getCursorUSR(target));
    if (!usr[0] || strcmp(usr, c->usr) != 0)
        return CXChildVisit_Recurse;
    /* the name is where the cursor is, or a little after (`struct foo`) */
    for (size_t at = off; at < c->text_len && at < (size_t)off + 64; ++at) {
        if (at + c->name_len <= c->text_len && memcmp(c->text + at, c->name, c->name_len) == 0 &&
            (at == 0 || !ident_char(c->text[at - 1])) &&
            (at + c->name_len == c->text_len || !ident_char(c->text[at + c->name_len]))) {
            add_offset(c, (uint32_t)at);
            break;
        }
        if (at > off && (c->text[at] == ';' || c->text[at] == '{' || c->text[at] == '('))
            break;
    }
    return CXChildVisit_Recurse;
}

/* Every place `usr` is named in file `path`, as offsets into `text`. */
static int places_in(CXIndex index, const char *path, const char *root, const char *const *args, int nargs,
                     const char *usr, const char *name, const char *text, size_t len, uint32_t **out)
{
    *out = NULL;
    /* the name is not even there: no need to ask clang */
    if (!strstr(text, name))
        return 0;
    CXTranslationUnit tu = parse(index, path, root, args, nargs);
    if (!tu)
        return 0;
    collect_ctx c = { .usr = usr, .name = name, .name_len = strlen(name), .text = text, .text_len = len };
    c.main_file = clang_getFile(tu, path);
    clang_visitChildren(clang_getTranslationUnitCursor(tu), collect, &c);
    clang_disposeTranslationUnit(tu);
    *out = c.offsets;
    return c.count;
}

bool ccr_rename_find(const char *root, const char *path, uint32_t offset, const char *const *clang_args,
                     int nargs, ccr_rename *out, char *err, int err_cap)
{
    memset(out, 0, sizeof *out);
    if (err_cap)
        err[0] = '\0';
    char real_root[PATH_MAX], real_path[PATH_MAX];
    if (!realpath(root, real_root) || !realpath(path, real_path)) {
        snprintf(err, (size_t)err_cap, "no such file");
        return false;
    }
    size_t len = 0;
    char *text = read_all(real_path, &len);
    if (!text || offset >= len) {
        free(text);
        snprintf(err, (size_t)err_cap, "nothing there");
        return false;
    }
    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = index ? parse(index, real_path, real_root, clang_args, nargs) : NULL;
    if (!tu) {
        free(text);
        if (index)
            clang_disposeIndex(index);
        snprintf(err, (size_t)err_cap, "clang could not read %s", path);
        return false;
    }
    /* what is at that byte */
    CXFile file = clang_getFile(tu, real_path);
    CXCursor at = clang_getCursor(tu, clang_getLocationForOffset(tu, file, offset));
    CXCursor target = named(at);
    if (clang_Cursor_isNull(target) || (clang_getCursorKind(target) >= CXCursor_FirstInvalid &&
                                            clang_getCursorKind(target) <= CXCursor_LastInvalid)) {
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        free(text);
        snprintf(err, (size_t)err_cap, "no name there");
        return false;
    }
    str_copy(out->usr, sizeof out->usr, clang_getCursorUSR(target));
    str_copy(out->old_name, sizeof out->old_name, clang_getCursorSpelling(target));
    /* only what the project itself declares: not printf, not a macro */
    CXFile decl_file;
    clang_getSpellingLocation(clang_getCursorLocation(target), &decl_file, NULL, NULL, NULL);
    char decl_path[PATH_MAX] = "", real_decl[PATH_MAX] = "";
    if (decl_file)
        str_copy(decl_path, sizeof decl_path, clang_getFileName(decl_file));
    const size_t rl = strlen(real_root);
    const bool own = decl_path[0] && realpath(decl_path, real_decl) && strncmp(real_decl, real_root, rl) == 0 &&
                     real_decl[rl] == '/';
    clang_disposeTranslationUnit(tu);
    free(text);
    if (!out->usr[0] || !out->old_name[0] || !own) {
        clang_disposeIndex(index);
        snprintf(err, (size_t)err_cap, "%s is not the project's own",
                 out->old_name[0] ? out->old_name : "that");
        return false;
    }

    /* every file of the project */
    char (*files)[1024] = malloc((size_t)MAX_FILES * sizeof *files);
    const int nfiles = files ? ccr_project_files(real_root, files, MAX_FILES) : 0;
    out->files = malloc((size_t)(nfiles ? nfiles : 1) * sizeof *out->files);
    for (int i = 0; out->files && i < nfiles; ++i) {
        size_t flen = 0;
        char *ftext = read_all(files[i], &flen);
        if (!ftext)
            continue;
        uint32_t *offs = NULL;
        const int n = places_in(index, files[i], real_root, clang_args, nargs, out->usr, out->old_name, ftext,
                                flen, &offs);
        if (n > 0) {
            const int fi = out->file_count++;
            snprintf(out->files[fi], sizeof out->files[fi], "%s", files[i]);
            ccr_rename_place *v = realloc(out->places, (size_t)(out->place_count + n) * sizeof *v);
            if (v) {
                out->places = v;
                for (int k = 0; k < n; ++k)
                    out->places[out->place_count++] = (ccr_rename_place){ fi, offs[k] };
            }
        }
        free(offs);
        free(ftext);
    }
    free(files);
    clang_disposeIndex(index);
    if (out->place_count == 0) {
        snprintf(err, (size_t)err_cap, "%s is named nowhere", out->old_name);
        return false;
    }
    return true;
}

void ccr_rename_free(ccr_rename *r)
{
    free(r->files);
    free(r->places);
    memset(r, 0, sizeof *r);
}

static int by_offset_desc(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) - (x > y);
}

bool ccr_rename_apply(const ccr_rename *r, const char *new_name, ccr_rename_undo *undo, char *err, int err_cap)
{
    if (undo)
        memset(undo, 0, sizeof *undo);
    if (!ccr_rename_valid(new_name)) {
        snprintf(err, (size_t)err_cap, "%s is no C name", new_name);
        return false;
    }
    const size_t old_len = strlen(r->old_name), new_len = strlen(new_name);
    char **before = calloc((size_t)(r->file_count ? r->file_count : 1), sizeof *before);
    char **after = calloc((size_t)(r->file_count ? r->file_count : 1), sizeof *after);
    size_t *after_len = calloc((size_t)(r->file_count ? r->file_count : 1), sizeof *after_len);
    bool ok = before && after && after_len;
    /* everything worked out first; then written */
    for (int f = 0; ok && f < r->file_count; ++f) {
        size_t len = 0;
        before[f] = read_all(r->files[f], &len);
        if (!before[f]) {
            snprintf(err, (size_t)err_cap, "cannot read %s", r->files[f]);
            ok = false;
            break;
        }
        uint32_t offs[4096];
        int n = 0;
        for (int i = 0; i < r->place_count && n < 4096; ++i)
            if (r->places[i].file == f)
                offs[n++] = r->places[i].offset;
        qsort(offs, (size_t)n, sizeof offs[0], by_offset_desc);
        char *text = malloc(len + (size_t)n * (new_len + 1) + 1);
        if (!text) {
            ok = false;
            break;
        }
        memcpy(text, before[f], len + 1);
        size_t tl = len;
        for (int i = 0; i < n; ++i) {   /* from the end: earlier offsets stay right */
            const uint32_t o = offs[i];
            if (o + old_len > tl || memcmp(text + o, r->old_name, old_len) != 0)
                continue;   /* not what was found: the file changed since */
            memmove(text + o + new_len, text + o + old_len, tl - o - old_len + 1);
            memcpy(text + o, new_name, new_len);
            tl = tl - old_len + new_len;
        }
        after[f] = text;
        after_len[f] = tl;
    }
    for (int f = 0; ok && f < r->file_count; ++f) {
        if (!write_all(r->files[f], after[f], after_len[f])) {
            snprintf(err, (size_t)err_cap, "cannot write %s", r->files[f]);
            for (int k = 0; k < f; ++k)   /* the ones written go back */
                write_all(r->files[k], before[k], strlen(before[k]));
            ok = false;
        }
    }
    if (ok && undo) {
        undo->files = malloc((size_t)(r->file_count ? r->file_count : 1) * sizeof *undo->files);
        if (undo->files) {
            memcpy(undo->files, r->files, (size_t)r->file_count * sizeof *undo->files);
            undo->before = before;
            undo->after = after;
            undo->count = r->file_count;
            before = after = NULL;
        }
    }
    for (int f = 0; f < r->file_count; ++f) {
        if (before)
            free(before[f]);
        if (after)
            free(after[f]);
    }
    free(before);
    free(after);
    free(after_len);
    return ok;
}

int ccr_rename_undo_apply(const ccr_rename_undo *u)
{
    for (int i = 0; i < u->count; ++i) {   /* all still as the rename left them? */
        char *now = read_all(u->files[i], NULL);
        const bool same = now && strcmp(now, u->after[i]) == 0;
        free(now);
        if (!same)
            return -1;
    }
    int n = 0;
    for (int i = 0; i < u->count; ++i)
        n += write_all(u->files[i], u->before[i], strlen(u->before[i]));
    return n;
}

void ccr_rename_undo_free(ccr_rename_undo *u)
{
    for (int i = 0; i < u->count; ++i) {
        free(u->before[i]);
        free(u->after[i]);
    }
    free(u->before);
    free(u->after);
    free(u->files);
    memset(u, 0, sizeof *u);
}
