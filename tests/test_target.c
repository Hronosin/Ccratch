/*
 * Building for other machines: a project of C and assembly for each
 * (examples/hello.s, hello_arm64.s, hello_arm.s), its CMakeLists.txt made
 * by Ccratch, built for every target this computer has the tools for, and
 * run where something here can run it (qemu for ARM Linux). A target with
 * no tools must say what it wants, not fail strangely.
 */
#define _DEFAULT_SOURCE
#include "ccr_build.h"
#include "ccr_job.h"
#include "ccr_target.h"

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

static bool copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    bool ok = a && b;
    char buf[8192];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, a)) > 0;)
        ok = fwrite(buf, 1, n, b) == n;
    if (a)
        fclose(a);
    if (b)
        ok = fclose(b) == 0 && ok;
    return ok;
}

static void wait_for(ccr_job *j)
{
    const double t0 = ccr_job_now();
    while (ccr_job_busy(j) && ccr_job_now() - t0 < 240.0) {
        ccr_job_poll(j);
        usleep(20000);
    }
}

/* The log as one text. */
static void log_text(const ccr_job *j, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < j->line_count && o + 1 < cap; ++i)
        o += (size_t)snprintf(out + o, cap - o, "%s\n", j->lines[i].text);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_target EXAMPLES_DIR\n");
        return 2;
    }
    CHECK(ccr_target_from_key("linux-arm64") == CCR_TARGET_LINUX_ARM64 && ccr_target_from_key("x") == -1 &&
          strcmp(ccr_target_key(CCR_TARGET_ANDROID_ARM32), "android-arm32") == 0, "keys");

    char root[256], p[600], q[600];
    snprintf(root, sizeof root, "/tmp/ccratch_test_target_%d", (int)getpid());
    mkdir(root, 0755);
    static const char *const files[] = { "hello.s", "hello_arm64.s", "hello_arm.s" };
    for (int i = 0; i < 3; ++i) {
        snprintf(p, sizeof p, "%s/%s", argv[1], files[i]);
        snprintf(q, sizeof q, "%s/%s", root, files[i]);
        CHECK(copy(p, q), "copy %s", files[i]);
    }
    char made[700];
    int n = 0;
    CHECK(ccr_build_make_cmake(root, NULL, made, (int)sizeof made, &n) == CCR_CMAKE_MADE, "a CMakeLists.txt");

    static char log[1 << 16];
    int built = 0;
    for (int k = 0; k < CCR_TARGET_COUNT; ++k) {
        ccr_target_info t;
        ccr_target_probe((ccr_target_kind)k, &t);
        ccr_job j;
        ccr_job_init(&j);
        j.target = (ccr_target_kind)k;
        j.mode = CCR_BUILD_DEBUG;
        const bool started = ccr_job_build(&j, root, true);
        wait_for(&j);
        log_text(&j, log, sizeof log);
        if (t.how == CCR_HOW_NONE) {
            CHECK(!started && j.result == CCR_JOB_FAILED && strstr(log, "nothing here builds for"),
                  "%s without tools says what it wants:\n%s", ccr_target_key(t.kind), log);
            printf("%-14s no tools: said so\n", ccr_target_key(t.kind));
        } else {
            CHECK(j.result == CCR_JOB_BUILT, "%s (%s) did not build:\n%s", ccr_target_key(t.kind), t.compiler, log);
            ++built;
            if (j.ran) {
                CHECK(j.run_exit == 0 && strstr(log, "1\n2\n3\n4\n5\n"), "%s: ran, but:\n%s", ccr_target_key(t.kind),
                      log);
                printf("%-14s built with %s, ran in %s: 1..5\n", ccr_target_key(t.kind), t.compiler,
                       t.runner[0] ? t.runner : "this computer");
            } else {
                CHECK(strstr(log, "built for"), "%s: built, not run, and says so:\n%s", ccr_target_key(t.kind), log);
                printf("%-14s built with %s (nothing here runs it)\n", ccr_target_key(t.kind), t.compiler);
            }
        }
        ccr_job_free(&j);
    }
    CHECK(built >= 1, "not even this computer");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "(could not remove %s)\n", root);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("targets: ok");
    return 0;
}
