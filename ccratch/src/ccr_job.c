#define _DEFAULT_SOURCE   /* realpath, DT_* */
#include "ccr_job.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINES 6000    /* then the oldest go */

double ccr_job_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void ccr_job_init(ccr_job *j)
{
    memset(j, 0, sizeof *j);
    j->proc.fd = -1;
    j->cmake_open = -1;
}

static void clear_log(ccr_job *j)
{
    for (int i = 0; i < j->line_count; ++i)
        free(j->lines[i].text);
    j->line_count = 0;
    j->dropped = 0;
    j->diag_count = 0;
    j->errors = j->warnings = 0;
    j->cmake_open = -1;
    ++j->generation;
}

void ccr_job_free(ccr_job *j)
{
    ccr_proc_kill_wait(&j->proc);
    clear_log(j);
    free(j->lines);
    free(j->diags);
    ccr_job_init(j);
}

bool ccr_job_busy(const ccr_job *j)
{
    return j->step != CCR_JOB_IDLE;
}

/* ----------------------------------------------------------- the log */

/* The project's own folder said once, at the top: in the log its files
 * read src/main.c, not /home/.../game/src/main.c. */
static void shorten(const ccr_job *j, const char *in, char *out, size_t cap)
{
    const size_t pl = strlen(j->project);
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap;) {
        if (pl && strncmp(p, j->project, pl) == 0 && (p[pl] == '/' || p[pl] == '\0' || p[pl] == ' ')) {
            p += pl;
            if (*p == '/')
                ++p;
            else if (o + 1 < cap)
                out[o++] = '.';
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static int add_line(ccr_job *j, const char *text_in, ccr_log_kind kind)
{
    char text[4200];
    shorten(j, text_in, text, sizeof text);
    if (j->line_count == MAX_LINES) {   /* a long log: the oldest thousand go */
        const int cut = 1000;
        for (int i = 0; i < cut; ++i)
            free(j->lines[i].text);
        memmove(j->lines, j->lines + cut, (size_t)(j->line_count - cut) * sizeof *j->lines);
        j->line_count -= cut;
        j->dropped += cut;
        for (int i = 0; i < j->diag_count; ++i)
            j->diags[i].log_line -= cut;   /* may go negative: off the top */
    }
    if (j->line_count == j->line_cap) {
        const int cap = j->line_cap ? j->line_cap * 2 : 256;
        ccr_log_line *v = realloc(j->lines, (size_t)cap * sizeof *v);
        if (!v)
            return -1;
        j->lines = v;
        j->line_cap = cap;
    }
    char *copy = strdup(text);
    if (!copy)
        return -1;
    j->lines[j->line_count] = (ccr_log_line){ copy, kind, -1 };
    return j->line_count++;
}

static void say(ccr_job *j, ccr_log_kind kind, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    add_line(j, buf, kind);
}

/* ------------------------------------------------- reading the output */

static bool ends_with_number(const char *s, size_t n, size_t *start, uint32_t *value)
{
    size_t i = n;
    while (i > 0 && isdigit((unsigned char)s[i - 1]))
        --i;
    if (i == n || n - i > 9)
        return false;
    *start = i;
    *value = (uint32_t)strtoul(s + i, NULL, 10);
    return true;
}

/* "path:12:5" or "path:12" (or just "path") in front of a message. */
static void split_place(const char *s, size_t n, ccr_build_diag *d)
{
    size_t at;
    uint32_t a, b;
    if (ends_with_number(s, n, &at, &b) && at > 1 && s[at - 1] == ':') {
        size_t at2;
        if (ends_with_number(s, at - 1, &at2, &a) && at2 > 1 && s[at2 - 1] == ':') {
            d->line = a;
            d->col = b;
            n = at2 - 1;
        } else {
            d->line = b;
            n = at - 1;
        }
    }
    snprintf(d->file, sizeof d->file, "%.*s", (int)n, s);
}

static void copy_trimmed(char *out, size_t cap, const char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        --n;
    snprintf(out, cap, "%.*s", (int)n, s);
}

bool ccr_build_parse_line(const char *line, ccr_build_diag *out)
{
    memset(out, 0, sizeof *out);
    out->log_line = -1;

    /* CMake Error at CMakeLists.txt:12 (add_executable):
     * CMake Warning (dev) at foo.cmake:3 (message): */
    if (strncmp(line, "CMake ", 6) == 0) {
        const char *at = strstr(line, " at ");
        const bool err = strncmp(line + 6, "Error", 5) == 0;
        const bool warn = strncmp(line + 6, "Warning", 7) == 0 || strncmp(line + 6, "Deprecation Warning", 19) == 0;
        if (!err && !warn)
            return false;
        out->kind = err ? CCR_BD_ERROR : CCR_BD_WARNING;
        out->from_cmake = true;
        if (at) {
            const char *p = at + 4;
            const char *paren = strstr(p, " (");
            size_t n = paren ? (size_t)(paren - p) : strcspn(p, ":");
            if (!paren) {   /* "at file:12:" */
                n = strlen(p);
                while (n && (p[n - 1] == ':' || isspace((unsigned char)p[n - 1])))
                    --n;
            }
            split_place(p, n, out);
        } else {
            /* CMake Error: the message itself */
            const char *colon = strchr(line, ':');
            if (colon)
                copy_trimmed(out->message, sizeof out->message, colon + 1);
        }
        return true;
    }

    /* the GNU linker: "...main.c:(.text+0x9): undefined reference to `foo'"
     * (with debug info: "main.c:6:(.text+0x9): ...") */
    const char *undef = strstr(line, "undefined reference to `");
    if (undef) {
        out->kind = CCR_BD_ERROR;
        const char *sym = undef + strlen("undefined reference to `");
        const size_t sn = strcspn(sym, "'");
        snprintf(out->symbol, sizeof out->symbol, "%.*s", (int)sn, sym);
        snprintf(out->message, sizeof out->message, "undefined reference to `%s'", out->symbol);
        const char *paren = strstr(line, ":(");
        if (paren && paren < undef) {
            const char *b = line;
            /* skip "/usr/bin/ld: " and the like: the place is the last
             * "word: " before it */
            for (const char *q = line; q < paren; ++q)
                if (q[0] == ':' && q[1] == ' ')
                    b = q + 2;
            split_place(b, (size_t)(paren - b), out);
        }
        return true;
    }
    /* lld: "ld.lld: error: undefined symbol: foo" */
    const char *lld = strstr(line, "undefined symbol: ");
    if (lld) {
        out->kind = CCR_BD_ERROR;
        copy_trimmed(out->symbol, sizeof out->symbol, lld + strlen("undefined symbol: "));
        snprintf(out->message, sizeof out->message, "undefined symbol: %s", out->symbol);
        return true;
    }

    /* GCC and Clang: "src/main.c:12:5: error: ..." */
    static const struct { const char *tag; ccr_diag_kind kind; } tags[] = {
        { ": fatal error: ", CCR_BD_ERROR }, { ": error: ", CCR_BD_ERROR },
        { ": warning: ", CCR_BD_WARNING },   { ": note: ", CCR_BD_NOTE },
    };
    for (size_t t = 0; t < sizeof tags / sizeof tags[0]; ++t) {
        const char *hit = strstr(line, tags[t].tag);
        if (!hit)
            continue;
        out->kind = tags[t].kind;
        copy_trimmed(out->message, sizeof out->message, hit + strlen(tags[t].tag));
        /* "collect2: error: ld returned 1 exit status", "ninja: error: ...":
         * a tool's name, no place */
        split_place(line, (size_t)(hit - line), out);
        if (!out->line && (strncmp(out->file, "collect2", 8) == 0 || strncmp(out->file, "ninja", 5) == 0 ||
                           strncmp(out->file, "make", 4) == 0 || strncmp(out->file, "gmake", 5) == 0 ||
                           strstr(out->file, "/ld") || strcmp(out->file, "ld") == 0 ||
                           strncmp(out->file, "cc1", 3) == 0 || strncmp(out->file, "gcc", 3) == 0 ||
                           strncmp(out->file, "clang", 5) == 0))
            out->file[0] = '\0';
        return true;
    }
    return false;
}

bool ccr_san_parse_line(ccr_san_state *st, const char *line, const char *project, ccr_build_diag *out)
{
    memset(out, 0, sizeof *out);
    out->log_line = -1;
    out->kind = CCR_BD_ERROR;
    /* UBSan: the place first, like a compiler */
    const char *ub = strstr(line, ": runtime error: ");
    if (ub) {
        copy_trimmed(out->message, sizeof out->message, ub + 2);
        split_place(line, (size_t)(ub - line), out);
        return out->line > 0;
    }
    const char *asan = strstr(line, "ERROR: AddressSanitizer: ");
    if (asan) {   /* "heap-buffer-overflow on address 0x... at pc ...": the first words */
        const char *w = asan + strlen("ERROR: AddressSanitizer: ");
        const char *on = strstr(w, " on ");
        const size_t n = on ? (size_t)(on - w) : strcspn(w, "(");
        snprintf(st->what, sizeof st->what, "AddressSanitizer: %.*s", (int)(n < 150 ? n : 150), w);
        st->want_place = true;
        return false;
    }
    if (strstr(line, "ERROR: LeakSanitizer: ")) {   /* each leak says itself below */
        st->want_place = false;
        return false;
    }
    if (strncmp(line, "Direct leak of ", 15) == 0) {
        const char *from = strstr(line, " allocated from");
        const size_t n = from ? (size_t)(from - line) : strlen(line);
        snprintf(st->what, sizeof st->what, "LeakSanitizer: %.*s", (int)(n < 150 ? n : 150), line);
        st->want_place = true;
        return false;
    }
    if (strncmp(line, "Indirect leak of ", 17) == 0 || strncmp(line, "SUMMARY: ", 9) == 0) {
        st->want_place = false;
        return false;
    }
    /* a frame: "    #1 0x55d0c4 in main /home/me/game/main.c:8:14" */
    const char *p = line + strspn(line, " \t");
    if (!st->want_place || p[0] != '#' || !isdigit((unsigned char)p[1]))
        return false;
    const char *in = strstr(p, " in ");
    if (!in)
        return false;
    const char *where = strchr(in + 4, ' ');   /* past the function's name */
    if (!where)
        return false;
    ++where;
    const size_t pn = project ? strlen(project) : 0;
    if (pn == 0 || strncmp(where, project, pn) != 0 || where[pn] != '/')
        return false;   /* libc, libasan: not the user's code */
    size_t n = strlen(where);
    while (n && isspace((unsigned char)where[n - 1]))
        --n;
    split_place(where, n, out);
    if (!out->line)
        return false;
    snprintf(out->message, sizeof out->message, "%s", st->what);
    st->want_place = false;
    return true;
}

static ccr_log_kind kind_of(ccr_diag_kind k)
{
    return k == CCR_BD_ERROR ? CCR_LOG_ERROR : k == CCR_BD_WARNING ? CCR_LOG_WARNING : CCR_LOG_NOTE;
}

/* A path from the output, made absolute: compilers write them from the
 * build folder, CMake from the project's. */
static void absolute(const ccr_job *j, ccr_build_diag *d)
{
    if (!d->file[0] || d->file[0] == '/')
        return;
    char p[2400], real[4096];
    snprintf(p, sizeof p, "%s/%s", d->from_cmake ? j->project : j->build_dir, d->file);
    if (realpath(p, real) == NULL) {
        snprintf(p, sizeof p, "%s/%s", j->project, d->file);   /* the other one */
        if (realpath(p, real) == NULL)
            return;   /* left as written: matched by name later */
    }
    const size_t n = strlen(real);
    if (n < sizeof d->file)
        memcpy(d->file, real, n + 1);
}

static void record_diag(ccr_job *j, ccr_build_diag *d, int at);

static void on_line(void *ctx, const char *line)
{
    ccr_job *j = ctx;
    if (j->step == CCR_JOB_RUN) {   /* the program's own output: just text, but for a sanitizer */
        ccr_build_diag sd;
        const bool placed = ccr_san_parse_line(&j->san, line, j->project, &sd);
        const int at = add_line(j, line, placed ? CCR_LOG_ERROR : CCR_LOG_TEXT);
        if (placed && at >= 0)
            record_diag(j, &sd, at);
        return;
    }
    /* the lines under "CMake Error at ...:" say what is wrong */
    if (j->cmake_open >= 0) {
        ccr_build_diag *d = &j->diags[j->cmake_open];
        if (line[0] == ' ' && line[1] == ' ') {
            /* its first paragraph, as one line */
            if (line[strspn(line, " ")] && !j->cmake_said) {
                char part[400];
                copy_trimmed(part, sizeof part, line);
                const size_t n = strlen(d->message);
                snprintf(d->message + n, sizeof d->message - n, "%s%s", n ? " " : "", part);
                ++j->generation;
            }
            const int at = add_line(j, line, kind_of(d->kind));
            if (at >= 0)
                j->lines[at].diag = j->cmake_open;
            return;
        }
        if (!line[0] && d->message[0]) {   /* the paragraph ended */
            j->cmake_said = true;
            add_line(j, line, CCR_LOG_TEXT);
            return;
        }
        if (line[0] && strncmp(line, "Call Stack", 10) != 0)
            j->cmake_open = -1;
    }

    ccr_build_diag d;
    if (!ccr_build_parse_line(line, &d)) {
        /* "[3/7] Building C object ...", "    5 |   foo();", "      |   ^~~" */
        const bool quiet = line[0] == '[' || strstr(line, " | ") != NULL ||
                           (line[0] == ' ' && strchr(line, '|')) || strncmp(line, "-- ", 3) == 0;
        add_line(j, line, quiet ? CCR_LOG_QUIET : CCR_LOG_TEXT);
        return;
    }
    absolute(j, &d);
    const int at = add_line(j, line, kind_of(d.kind));
    if (at >= 0)
        record_diag(j, &d, at);
}

/* `d`, said on log line `at`: kept, counted, and the line points at it. */
static void record_diag(ccr_job *j, ccr_build_diag *d_in, int at)
{
    ccr_build_diag d = *d_in;
    if (j->diag_count == j->diag_cap) {
        const int cap = j->diag_cap ? j->diag_cap * 2 : 32;
        ccr_build_diag *v = realloc(j->diags, (size_t)cap * sizeof *v);
        if (!v)
            return;
        j->diags = v;
        j->diag_cap = cap;
    }
    d.log_line = at;
    j->diags[j->diag_count] = d;
    j->lines[at].diag = j->diag_count;
    if (d.from_cmake) {
        j->cmake_open = j->diag_count;
        j->cmake_said = d.message[0] != '\0';   /* "CMake Error: ..." said it already */
    }
    ++j->diag_count;
    /* a tool's own "error" (collect2: ld returned 1) does not count twice */
    const bool counts = d.file[0] || d.symbol[0] || d.from_cmake;
    if (d.kind == CCR_BD_ERROR && counts)
        ++j->errors;
    else if (d.kind == CCR_BD_WARNING && counts)
        ++j->warnings;
    ++j->generation;
}

/* ------------------------------------------------------------- steps */

static bool start(ccr_job *j, const char *const *argv, const char *cwd, bool c_locale)
{
    char line[2048] = "$";
    for (int i = 0; argv[i]; ++i) {
        const size_t n = strlen(line);
        snprintf(line + n, sizeof line - n, " %s", argv[i]);
    }
    add_line(j, line, CCR_LOG_COMMAND);
    char err[256];
    if (!ccr_proc_start(&j->proc, argv, cwd, c_locale, err, sizeof err)) {
        say(j, CCR_LOG_BAD, "%s", err);
        return false;
    }
    return true;
}

static bool exists(const char *dir, const char *name)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static void make_dirs(const char *dir)
{
    char p[1400];
    snprintf(p, sizeof p, "%s", dir);
    for (char *c = p + 1; *c; ++c)
        if (*c == '/') {
            *c = '\0';
            mkdir(p, 0755);
            *c = '/';
        }
    mkdir(p, 0755);
}

static bool start_configure(ccr_job *j)
{
    make_dirs(j->build_dir);
    const char *argv[16];
    int n = 0;
    argv[n++] = "cmake";
    argv[n++] = "-S";
    argv[n++] = j->project;
    argv[n++] = "-B";
    argv[n++] = j->build_dir;
    if (ccr_proc_in_path("ninja")) {
        argv[n++] = "-G";
        argv[n++] = "Ninja";
    }
    if (j->mode == CCR_BUILD_RELEASE) {
        argv[n++] = "-DCMAKE_BUILD_TYPE=Release";
    } else {
        argv[n++] = "-DCMAKE_BUILD_TYPE=Debug";
        if (j->mode == CCR_BUILD_SANITIZE) {   /* MSVC would want /fsanitize=address; this is gcc and clang */
            argv[n++] = "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer";
            argv[n++] = "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer";
            argv[n++] = "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined";
        }
    }
    argv[n++] = "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
    argv[n] = NULL;
    j->step = CCR_JOB_CONFIGURE;
    return start(j, argv, j->project, true);
}

static bool start_build(ccr_job *j)
{
    char jobs[24];
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    snprintf(jobs, sizeof jobs, "%ld", cpus > 0 ? cpus : 2);
    const char *argv[] = { "cmake", "--build", j->build_dir, "-j", jobs, NULL };
    j->step = CCR_JOB_BUILD;
    return start(j, argv, j->project, true);
}

static void done(ccr_job *j, ccr_job_result r)
{
    j->step = CCR_JOB_IDLE;
    j->result = r;
    j->ended = ccr_job_now();
}

bool ccr_job_build(ccr_job *j, const char *project, bool run_after)
{
    if (ccr_job_busy(j))
        return false;
    clear_log(j);
    j->project[0] = '\0';   /* said in full once; after that, paths in it are short */
    say(j, CCR_LOG_QUIET, "project: %s", project);
    snprintf(j->project, sizeof j->project, "%s", project);
    static const char *const dirs[CCR_BUILD_MODE_COUNT] = { "build", "build-release", "build-sanitize" };
    snprintf(j->build_dir, sizeof j->build_dir, "%s/.ccratch/%s", project,
             dirs[(unsigned)j->mode < CCR_BUILD_MODE_COUNT ? j->mode : 0]);
    j->exe[0] = '\0';
    j->run_after = run_after;
    j->ran = false;
    j->run_exit = 0;
    j->started = ccr_job_now();
    j->result = CCR_JOB_RUNNING;
    if (!ccr_proc_in_path("cmake")) {
        say(j, CCR_LOG_BAD, "cmake: not found. Install CMake (dnf install cmake) and try again.");
        done(j, CCR_JOB_NO_CMAKE);
        return false;
    }
    /* configured already (and the generator wrote its files): straight on;
     * cmake --build itself configures again if CMakeLists.txt changed */
    const bool ready = exists(j->build_dir, "CMakeCache.txt") &&
                       (exists(j->build_dir, "build.ninja") || exists(j->build_dir, "Makefile"));
    const bool ok = ready ? start_build(j) : start_configure(j);
    if (!ok)
        done(j, CCR_JOB_FAILED);
    return ok;
}

bool ccr_job_run(ccr_job *j)
{
    if (ccr_job_busy(j))
        return false;
    if (!j->exe[0] && !ccr_build_find_exe(j->build_dir, j->exe, (int)sizeof j->exe)) {
        say(j, CCR_LOG_BAD, "no program in %s: is there an add_executable()?", j->build_dir);
        return false;
    }
    j->result = CCR_JOB_RUNNING;
    j->step = CCR_JOB_RUN;
    j->ran = true;
    j->san = (ccr_san_state){ 0 };
    const char *argv[] = { j->exe, NULL };
    if (!start(j, argv, j->project, false)) {   /* its own language: it is the user's program */
        done(j, CCR_JOB_FAILED);
        return false;
    }
    return true;
}

void ccr_job_stop(ccr_job *j)
{
    if (ccr_job_busy(j))
        ccr_proc_stop(&j->proc);
}

void ccr_job_poll(ccr_job *j)
{
    if (!ccr_job_busy(j))
        return;
    if (ccr_proc_poll(&j->proc, on_line, j))
        return;
    const int code = j->proc.exit_code;
    const bool stopped = j->proc.stop_at != 0.0;
    j->cmake_open = -1;
    if (stopped) {
        say(j, CCR_LOG_BAD, "stopped");
        done(j, CCR_JOB_STOPPED);
        return;
    }
    switch (j->step) {
    case CCR_JOB_CONFIGURE:
        if (code == 127) {
            done(j, CCR_JOB_NO_CMAKE);
        } else if (code != 0) {
            say(j, CCR_LOG_BAD, "CMake could not configure the project (code %d)", code);
            done(j, CCR_JOB_FAILED);
        } else if (!start_build(j)) {
            done(j, CCR_JOB_FAILED);
        }
        break;
    case CCR_JOB_BUILD:
        if (code != 0) {
            say(j, CCR_LOG_BAD, "the build failed (code %d)", code);
            done(j, CCR_JOB_FAILED);
            break;
        }
        ccr_build_find_exe(j->build_dir, j->exe, (int)sizeof j->exe);
        say(j, CCR_LOG_GOOD, "built in %.1f s%s%s", ccr_job_now() - j->started, j->exe[0] ? ": " : "",
            j->exe);
        done(j, CCR_JOB_BUILT);
        if (j->run_after && j->exe[0])
            ccr_job_run(j);
        break;
    case CCR_JOB_RUN:
        j->run_exit = code;
        if (code == 0)
            say(j, CCR_LOG_GOOD, "the program finished (code 0)");
        else
            say(j, CCR_LOG_BAD, "the program finished with code %d%s", code,
                code > 128 && code < 160 ? " (killed by a signal)" : "");
        done(j, CCR_JOB_BUILT);
        break;
    case CCR_JOB_IDLE:
        break;
    }
}

/* ------------------------------------------------ the program it made */

static bool is_elf_exe(const char *path, double *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char h[18];
    const size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    /* ELF, and e_type EXEC (2) or DYN (3: a PIE); a .so is DYN too, so
     * those are told apart by name */
    if (n < 18 || memcmp(h, "\x7f" "ELF", 4) != 0 || (h[16] != 2 && h[16] != 3))
        return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strstr(base, ".so"))
        return false;
    *mtime = (double)st.st_mtime;
    return true;
}

static void find_in(const char *dir, int depth, char *best, int cap, double *best_t)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "CMakeFiles") == 0 || strcmp(e->d_name, "_deps") == 0)
            continue;
        char p[2048];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p)
            continue;
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        double t;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3)
                find_in(p, depth + 1, best, cap, best_t);
        } else if (is_elf_exe(p, &t) && t >= *best_t) {
            *best_t = t;
            snprintf(best, (size_t)cap, "%s", p);
        }
    }
    closedir(d);
}

bool ccr_build_find_exe(const char *build_dir, char *out, int cap)
{
    double t = -1.0;
    out[0] = '\0';
    find_in(build_dir, 0, out, cap, &t);
    return out[0] != '\0';
}
