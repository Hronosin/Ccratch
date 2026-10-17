/*
 * The Build button without the window: reading what compilers, linkers
 * and CMake say, and building, running and stopping a real project.
 */
#define _DEFAULT_SOURCE
#include "ccr_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

static void test_parse(void)
{
    ccr_build_diag d;
    CHECK(ccr_build_parse_line("/p/src/main.c:12:5: error: 'x' undeclared (first use in this function)", &d) &&
              d.kind == CCR_BD_ERROR && strcmp(d.file, "/p/src/main.c") == 0 && d.line == 12 && d.col == 5 &&
              strcmp(d.message, "'x' undeclared (first use in this function)") == 0,
          "gcc error: %s %u %u [%s]", d.file, d.line, d.col, d.message);
    CHECK(ccr_build_parse_line("src/util.c:3:10: warning: unused variable 'y' [-Wunused-variable]", &d) &&
              d.kind == CCR_BD_WARNING && d.line == 3 && strcmp(d.file, "src/util.c") == 0, "warning");
    CHECK(ccr_build_parse_line("main.c:7: error: no column", &d) && d.line == 7 && d.col == 0 &&
              strcmp(d.file, "main.c") == 0, "no column");
    CHECK(ccr_build_parse_line("/p/main.c:1:10: fatal error: nope.h: No such file or directory", &d) &&
              d.kind == CCR_BD_ERROR && d.line == 1 && strcmp(d.message, "nope.h: No such file or directory") == 0,
          "fatal error: [%s]", d.message);
    CHECK(ccr_build_parse_line("/p/main.c:4:6: note: declared here", &d) && d.kind == CCR_BD_NOTE, "note");
    CHECK(ccr_build_parse_line("collect2: error: ld returned 1 exit status", &d) && d.kind == CCR_BD_ERROR &&
              d.file[0] == '\0', "collect2 is no place: %s", d.file);
    CHECK(ccr_build_parse_line("/usr/bin/ld: main.c:(.text+0x9): undefined reference to `zlibVersion'", &d) &&
              strcmp(d.symbol, "zlibVersion") == 0 && strcmp(d.file, "main.c") == 0 && d.line == 0,
          "ld: [%s] [%s]", d.symbol, d.file);
    CHECK(ccr_build_parse_line("/tmp/x/src/main.c:6:(.text+0x9): undefined reference to `foo'", &d) &&
              strcmp(d.symbol, "foo") == 0 && d.line == 6 && strcmp(d.file, "/tmp/x/src/main.c") == 0,
          "ld with a line: [%s] %u", d.file, d.line);
    CHECK(ccr_build_parse_line("ld.lld: error: undefined symbol: bar", &d) && strcmp(d.symbol, "bar") == 0, "lld");
    CHECK(ccr_build_parse_line("CMake Error at CMakeLists.txt:12 (add_executable):", &d) && d.from_cmake &&
              d.kind == CCR_BD_ERROR && strcmp(d.file, "CMakeLists.txt") == 0 && d.line == 12,
          "cmake error: %s %u", d.file, d.line);
    CHECK(ccr_build_parse_line("CMake Warning (dev) at cmake/x.cmake:3 (message):", &d) &&
              d.kind == CCR_BD_WARNING && d.line == 3 && strcmp(d.file, "cmake/x.cmake") == 0, "cmake warning");
    CHECK(ccr_build_parse_line("CMake Error: The source directory \"/nope\" does not exist.", &d) &&
              d.file[0] == '\0' && strstr(d.message, "does not exist"), "cmake, no place: [%s]", d.message);
    CHECK(!ccr_build_parse_line("[3/4] Building C object CMakeFiles/x.dir/src/main.c.o", &d), "progress");
    CHECK(!ccr_build_parse_line("    5 |     return y;", &d), "the code under a message");
    CHECK(!ccr_build_parse_line("src/main.c: In function 'main':", &d), "In function");
}

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fputs(text, f);
    return fclose(f) == 0;
}

/* Polls until it is idle (or a minute has gone). */
static void wait_idle(ccr_job *j)
{
    const double t0 = ccr_job_now();
    while (ccr_job_busy(j) && ccr_job_now() - t0 < 60.0) {
        ccr_job_poll(j);
        usleep(10000);
    }
}

static bool log_has(const ccr_job *j, const char *what)
{
    for (int i = 0; i < j->line_count; ++i)
        if (strstr(j->lines[i].text, what))
            return true;
    return false;
}

static void dump(const ccr_job *j)
{
    for (int i = 0; i < j->line_count; ++i)
        fprintf(stderr, "  | %s\n", j->lines[i].text);
}

static void test_real_build(void)
{
    char root[256], p[512];
    snprintf(root, sizeof root, "/tmp/ccratch_test_job_%d", (int)getpid());
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/CMakeLists.txt", root);
    write_text(p, "cmake_minimum_required(VERSION 3.16)\nproject(hello LANGUAGES C)\n"
                  "add_executable(hello main.c)\n");
    snprintf(p, sizeof p, "%s/main.c", root);
    write_text(p, "#include <stdio.h>\n\nint main(void)\n{\n    int unused;\n"
                  "    printf(\"%d\\n\", nope);\n    return 0;\n}\n");

    ccr_job j;
    ccr_job_init(&j);
    CHECK(ccr_job_build(&j, root, true), "the build did not start");
    wait_idle(&j);
    CHECK(j.result == CCR_JOB_FAILED, "a broken file built? result %d", j.result);
    CHECK(j.errors == 1, "%d errors, want 1", j.errors);
    bool found = false;
    for (int i = 0; i < j.diag_count; ++i) {
        const ccr_build_diag *d = &j.diags[i];
        if (d->kind == CCR_BD_ERROR && d->line == 6 && strstr(d->file, "/main.c") && d->file[0] == '/' &&
            strstr(d->message, "nope"))
            found = true;
    }
    CHECK(found, "no error at main.c:6 about nope");
    CHECK(!j.ran, "ran a program that did not build");
    if (g_failures)
        dump(&j);

    /* fixed: it builds (no configure this time), runs, says what it printed */
    write_text(p, "#include <stdio.h>\n\nint main(void)\n{\n    printf(\"hello from the test\\n\");\n"
                  "    return 3;\n}\n");
    CHECK(ccr_job_build(&j, root, true), "the second build did not start");
    CHECK(j.step == CCR_JOB_BUILD, "configured again: step %d", j.step);
    wait_idle(&j);
    CHECK(j.result == CCR_JOB_BUILT && j.errors == 0, "result %d, %d errors", j.result, j.errors);
    CHECK(j.ran && j.run_exit == 3, "ran %d, exit %d", j.ran, j.run_exit);
    CHECK(strstr(j.exe, "/hello") != NULL, "the program: %s", j.exe);
    CHECK(log_has(&j, "hello from the test"), "its output is not in the log");
    if (g_failures)
        dump(&j);

    /* a linker error points at the call */
    write_text(p, "int missing_one(void);\nint main(void)\n{\n    return missing_one();\n}\n");
    ccr_job_build(&j, root, false);
    wait_idle(&j);
    found = false;
    for (int i = 0; i < j.diag_count; ++i)
        found = found || strcmp(j.diags[i].symbol, "missing_one") == 0;
    CHECK(j.result == CCR_JOB_FAILED && found && j.errors == 1, "undefined reference: result %d, %d errors",
          j.result, j.errors);

    /* stopped: a program that would run for a minute */
    write_text(p, "#include <unistd.h>\nint main(void)\n{\n    sleep(60);\n    return 0;\n}\n");
    ccr_job_build(&j, root, true);
    const double t0 = ccr_job_now();
    while ((!j.ran || !ccr_job_busy(&j)) && ccr_job_now() - t0 < 60.0) {
        ccr_job_poll(&j);
        usleep(10000);
    }
    CHECK(j.step == CCR_JOB_RUN, "not running: step %d", j.step);
    usleep(100000);
    ccr_job_stop(&j);
    const double t1 = ccr_job_now();
    wait_idle(&j);
    CHECK(j.result == CCR_JOB_STOPPED && ccr_job_now() - t1 < 5.0, "stop: result %d after %.1f s", j.result,
          ccr_job_now() - t1);

    /* CMake's own errors point at CMakeLists.txt */
    snprintf(p, sizeof p, "%s/CMakeLists.txt", root);
    write_text(p, "cmake_minimum_required(VERSION 3.16)\nproject(hello LANGUAGES C)\n"
                  "add_executable(hello main.c)\nadd_executable(hello main.c)\n");
    ccr_job_build(&j, root, false);
    wait_idle(&j);
    found = false;
    for (int i = 0; i < j.diag_count; ++i)
        found = found || (j.diags[i].from_cmake && j.diags[i].line == 4 && strstr(j.diags[i].file, "/CMakeLists.txt") &&
                          strstr(j.diags[i].message, "add_executable cannot create target") &&
                          strstr(j.diags[i].message, "already exists"));
    CHECK(j.result == CCR_JOB_FAILED && found, "CMake error at line 4: result %d", j.result);
    if (!found)
        dump(&j);
    ccr_job_free(&j);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "could not clean %s\n", root);
}

int main(void)
{
    test_parse();
    test_real_build();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("job: all checks passed\n");
    return g_failures ? 1 : 0;
}
