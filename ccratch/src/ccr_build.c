#define _DEFAULT_SOURCE
#include "ccr_build.h"
#include "ccr_asm.h"
#include "ccr_files.h"
#include "ccr_lib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What a source file says about itself: a main(), and what it includes. */
typedef struct scanned {
    bool has_main;
    char includes[64][128];
    int  include_count;
} scanned;

static void scan_source(const char *path, scanned *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '#') {
            ++p;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (strncmp(p, "include", 7) == 0) {
                p += 7;
                while (*p == ' ' || *p == '\t')
                    ++p;
                if ((*p == '<' || *p == '"') && out->include_count < 64) {
                    const char close = *p == '<' ? '>' : '"';
                    const char *e = strchr(p + 1, close);
                    if (e && e - p - 1 < 127)
                        snprintf(out->includes[out->include_count++], 128, "%.*s", (int)(e - p - 1), p + 1);
                }
            }
            continue;
        }
        /* int main( / void main( at the start of a line */
        if (strncmp(p, "int ", 4) == 0 || strncmp(p, "void ", 5) == 0) {
            p += *p == 'i' ? 4 : 5;
            while (*p == ' ' || *p == '\t' || *p == '*')
                ++p;
            if (strncmp(p, "main", 4) == 0) {
                p += 4;
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (*p == '(')
                    out->has_main = true;
            }
        }
    }
    fclose(f);
}

static bool skip_for_program(const char *rel)
{
    static const char *const dirs[] = { "test", "tests", "example", "examples", "bench", "benchmarks",
                                        "demo", "demos", "docs", "doc" };
    char first[128];
    const char *slash = strchr(rel, '/');
    if (!slash)
        return false;
    snprintf(first, sizeof first, "%.*s", (int)(slash - rel), rel);
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i)
        if (strcmp(first, dirs[i]) == 0)
            return true;
    return false;
}

/* Words of `list` ("raylib m") not in `have` yet, appended to it. */
static void add_words(char *have, size_t cap, const char *list)
{
    char word[128];
    for (const char *p = list; *p;) {
        while (*p == ' ')
            ++p;
        size_t n = strcspn(p, " ");
        if (!n)
            break;
        snprintf(word, sizeof word, "%.*s", (int)n, p);
        p += n;
        bool seen = false;
        for (const char *h = have; (h = strstr(h, word)) != NULL; h += strlen(word))
            if ((h == have || h[-1] == ' ') && (h[strlen(word)] == ' ' || h[strlen(word)] == '\0'))
                seen = true;
        if (!seen) {
            const size_t l = strlen(have);
            snprintf(have + l, cap - l, "%s%s", l ? " " : "", word);
        }
    }
}

static void project_name(const char *project, char *out, size_t cap)
{
    const char *base = strrchr(project, '/');
    base = (base && base[1]) ? base + 1 : project;
    size_t n = 0;
    if (isdigit((unsigned char)*base))
        n = (size_t)snprintf(out, cap, "app_");
    for (const char *c = base; *c && n + 1 < cap; ++c)
        out[n++] = (isalnum((unsigned char)*c) || *c == '_' || *c == '-') ? *c : '_';
    out[n] = '\0';
    if (!n)
        snprintf(out, cap, "app");
}

/* What a file includes, as libraries to link. */
typedef struct libs {
    char links[512];
    char finds[8][96];
    int  nf;
    bool libm, threads;
} libs;

static void collect(const scanned *sc, libs *l)
{
    for (int k = 0; k < sc->include_count; ++k) {
        const char *inc = sc->includes[k];
        l->libm = l->libm || strcmp(inc, "math.h") == 0;
        l->threads = l->threads || strcmp(inc, "pthread.h") == 0;
        for (int p = 0; p < ccr_lib_pack_count(); ++p) {
            const ccr_lib_pack *pk = ccr_lib_pack_at(p);
            if (!pk->header[0] || !ccr_lib_pack_matches(pk, inc))
                continue;
            if (pk->link[0])
                add_words(l->links, sizeof l->links, pk->link);
            bool seen = false;
            for (int j = 0; j < l->nf; ++j)
                seen = seen || strcmp(l->finds[j], pk->find) == 0;
            if (pk->find[0] && !seen && l->nf < 8)
                snprintf(l->finds[l->nf++], sizeof l->finds[0], "%s", pk->find);
        }
    }
}

/* What a whole assembly file is written for. */
static ccr_asm_syntax asm_syntax_of_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return ccr_asm_syntax_of(path, NULL, 0);
    static char buf[1 << 16];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return ccr_asm_syntax_of(path, buf, n);
}

/* 0 x86, 1 64-bit ARM, 2 32-bit ARM. */
static int asm_family(ccr_asm_syntax s)
{
    return s == CCR_ASM_ARM64 ? 1 : s == CCR_ASM_ARM32 ? 2 : 0;
}

static int host_family(void)
{
#if defined(__aarch64__)
    return 1;
#elif defined(__arm__)
    return 2;
#else
    return 0;
#endif
}

/* An assembly file with a main: of its own (the program starts there). */
static bool asm_has_main(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        found = strncmp(p, "main:", 5) == 0;
    }
    fclose(f);
    return found;
}

static int compare_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char *ccr_build_cmake_text(const char *project, const char *main_hint, int *files_out)
{
    if (files_out)
        *files_out = 0;
    ccr_files tree = { 0 };
    if (!ccr_files_scan(&tree, project, NULL))
        return NULL;
    const size_t root_len = strlen(tree.root);

    char **sources = calloc((size_t)tree.count + 1, sizeof *sources);
    char **mains = calloc((size_t)tree.count + 1, sizeof *mains);
    int ns = 0, nm = 0;
    bool any_asm = false;
    char nasm_note[1024] = "";
    libs L = { 0 };
    char *text = NULL;
    size_t text_len = 0;
    FILE *out = NULL;
    if (!sources || !mains)
        goto done;

    for (int i = 0; i < tree.count; ++i) {
        const ccr_file_entry *e = &tree.v[i];
        const size_t n = strlen(e->path);
        const bool c_file = n > 2 && strcmp(e->path + n - 2, ".c") == 0;
        const bool h_file = n > 2 && strcmp(e->path + n - 2, ".h") == 0;
        const bool s_file = !e->dir && ccr_asm_is_asm(e->path);
        if (e->dir || (!c_file && !h_file && !s_file))
            continue;
        const char *rel = e->path + root_len + (e->path[root_len] == '/');
        if (s_file) {
            /* GNU as: built with the C files (gcc assembles it); NASM would want a language of its own */
            const ccr_asm_syntax syn = asm_syntax_of_file(e->path);
            if (syn == CCR_ASM_NASM) {
                const size_t k = strlen(nasm_note);
                snprintf(nasm_note + k, sizeof nasm_note - k, "# %s is NASM: build it with nasm (it is left out)\n", rel);
                continue;
            }
            if (asm_family(syn) != host_family()) {   /* ARM on a PC, x86 on a Pi */
                const size_t k = strlen(nasm_note);
                snprintf(nasm_note + k, sizeof nasm_note - k,
                         "# %s is %s assembly: it builds on such a machine, or with a cross compiler (left out)\n",
                         rel, syn == CCR_ASM_ARM64 ? "64-bit ARM" : syn == CCR_ASM_ARM32 ? "32-bit ARM" : "x86");
                continue;
            }
            if (skip_for_program(rel))
                continue;
            any_asm = true;
            const bool hinted_s = main_hint && ccr_files_same(e->path, main_hint);
            if (!asm_has_main(e->path)) {
                sources[ns++] = strdup(rel);
            } else if (hinted_s && nm > 0) {   /* the open file's main wins */
                mains[nm++] = mains[0];
                mains[0] = strdup(rel);
            } else {
                mains[nm++] = strdup(rel);
            }
            continue;
        }
        const bool hinted = main_hint && ccr_files_same(e->path, main_hint);
        if (skip_for_program(rel) && !hinted)
            continue;
        scanned sc;
        scan_source(e->path, &sc);
        if (!c_file || !sc.has_main)
            collect(&sc, &L);   /* a main() left out does not link anything */
        if (!c_file)
            continue;
        if (sc.has_main) {
            /* the open file's main() wins; the others wait in a comment */
            if (hinted && nm > 0) {
                mains[nm++] = mains[0];
                mains[0] = strdup(rel);
            } else {
                mains[nm++] = strdup(rel);
            }
        } else {
            sources[ns++] = strdup(rel);
        }
    }
    if (ns + nm == 0)
        goto done;
    if (nm && !ccr_asm_is_asm(mains[0])) {   /* the program's own main() */
        char full[2400];
        scanned sc;
        snprintf(full, sizeof full, "%s/%s", tree.root, mains[0]);
        scan_source(full, &sc);
        collect(&sc, &L);
    }
    qsort(sources, (size_t)ns, sizeof *sources, compare_str);

    out = open_memstream(&text, &text_len);
    if (!out)
        goto done;
    char name[128];
    project_name(tree.root, name, sizeof name);
    fprintf(out, "# Made by Ccratch for %s. Open it as blocks, or as text: it is plain CMake.\n", name);
    fprintf(out, "cmake_minimum_required(VERSION 3.16)\n");
    fprintf(out, "project(%s LANGUAGES C%s)\n\n", name, any_asm ? " ASM" : "");
    fprintf(out, "set(CMAKE_C_STANDARD 11)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n\n");
    fprintf(out, "add_executable(${PROJECT_NAME}\n");
    if (nm)
        fprintf(out, "    %s\n", mains[0]);
    for (int i = 0; i < ns; ++i)
        fprintf(out, "    %s\n", sources[i]);
    fprintf(out, ")\n");
    for (int i = 1; i < nm; ++i)
        fprintf(out, "# %s has a main() of its own: a program of its own, left out\n", mains[i]);
    fputs(nasm_note, out);

    char inc_dir[1200];
    struct stat st;
    snprintf(inc_dir, sizeof inc_dir, "%s/include", tree.root);
    if (stat(inc_dir, &st) == 0 && S_ISDIR(st.st_mode))
        fprintf(out, "target_include_directories(${PROJECT_NAME} PRIVATE include)\n");
    fprintf(out, "\nif(MSVC)\n    target_compile_options(${PROJECT_NAME} PRIVATE /W4)\n"
                 "else()\n    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)\nendif()\n");

    if (L.nf || L.links[0] || L.threads || L.libm)
        fputc('\n', out);
    for (int i = 0; i < L.nf; ++i)
        fprintf(out, "find_package(%s)\n", L.finds[i]);
    if (L.threads)
        fprintf(out, "find_package(Threads REQUIRED)\n");
    if (L.links[0] || L.threads)
        fprintf(out, "target_link_libraries(${PROJECT_NAME} PRIVATE%s%s%s)\n", L.links[0] ? " " : "",
                L.links, L.threads ? " Threads::Threads" : "");
    if (L.libm)
        fprintf(out, "if(UNIX)\n    target_link_libraries(${PROJECT_NAME} PRIVATE m)\nendif()\n");
    if (files_out)
        *files_out = ns + (nm ? 1 : 0);

done:
    if (out && fclose(out) != 0) {
        free(text);
        text = NULL;
    }
    if (sources)
        for (int i = 0; i < ns; ++i)
            free(sources[i]);
    if (mains)
        for (int i = 0; i < nm; ++i)
            free(mains[i]);
    free(sources);
    free(mains);
    ccr_files_free(&tree);
    return text;
}

ccr_cmake_made ccr_build_make_cmake(const char *project, const char *main_hint, char *path,
                                    int cap, int *files_out)
{
    snprintf(path, (size_t)cap, "%s/CMakeLists.txt", project);
    if (access(path, F_OK) == 0)
        return CCR_CMAKE_THERE;
    char *text = ccr_build_cmake_text(project, main_hint, files_out);
    if (!text)
        return CCR_CMAKE_NO_SOURCES;
    FILE *f = fopen(path, "wx");
    bool ok = f && fputs(text, f) >= 0;
    if (f && fclose(f) != 0)
        ok = false;
    free(text);
    return ok ? CCR_CMAKE_MADE : CCR_CMAKE_CANNOT;
}
