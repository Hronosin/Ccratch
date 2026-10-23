#define _GNU_SOURCE   /* strverscmp */
#include "ccr_target.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static const char *const g_keys[CCR_TARGET_COUNT] = { "native", "linux-arm64", "linux-arm32", "android-arm64",
                                                      "android-arm32" };
static const char *const g_short[CCR_TARGET_COUNT] = { "", "arm64", "arm", "android", "android32" };
static const char *const g_suffix[CCR_TARGET_COUNT] = { "", "-arm64", "-arm32", "-android-arm64",
                                                        "-android-arm32" };

const char *ccr_target_key(ccr_target_kind k) { return (unsigned)k < CCR_TARGET_COUNT ? g_keys[k] : "native"; }
const char *ccr_target_short(ccr_target_kind k) { return (unsigned)k < CCR_TARGET_COUNT ? g_short[k] : ""; }
const char *ccr_target_suffix(ccr_target_kind k) { return (unsigned)k < CCR_TARGET_COUNT ? g_suffix[k] : ""; }

int ccr_target_from_key(const char *key)
{
    for (int k = 0; key && k < CCR_TARGET_COUNT; ++k)
        if (strcmp(key, g_keys[k]) == 0)
            return k;
    return -1;
}

const char *ccr_target_host_machine(void)
{
    static struct utsname u;
    static bool got;
    if (!got)
        got = uname(&u) == 0;
    return got ? u.machine : "?";
}

/* ------------------------------------------------------------ looking */

/* A path into out[cap]; false (and nothing copied) if it does not fit. */
static bool put_path(char *out, size_t cap, const char *s)
{
    const size_t n = strlen(s);
    if (n >= cap)
        return false;
    memcpy(out, s, n + 1);
    return true;
}

static bool is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* `name` in $PATH: its full path in `out`. */
static bool find_exe(const char *name, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path; *p;) {
        const char *colon = strchr(p, ':');
        const size_t n = colon ? (size_t)(colon - p) : strlen(p);
        char full[1024];
        snprintf(full, sizeof full, "%.*s/%s", (int)n, p, name);
        if (n && access(full, X_OK) == 0 && is_file(full)) {
            return put_path(out, cap, full);
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
}

/* A C library for `triple` to link with: Debian's /usr/<triple>, Fedora's
 * sys-root under it. */
static bool find_sysroot(const char *triple, char *out, size_t cap)
{
    static const char *const roots[] = { "/usr/%s", "/usr/%s/sys-root", "/usr/%s/libc" };
    static const char *const crt[] = { "lib/crt1.o", "usr/lib/crt1.o", "usr/lib64/crt1.o", "lib64/crt1.o" };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        char root[512], f[700];
        snprintf(root, sizeof root, roots[i], triple);
        for (size_t k = 0; k < sizeof crt / sizeof crt[0]; ++k) {
            snprintf(f, sizeof f, "%s/%s", root, crt[k]);
            if (is_file(f)) {
                return put_path(out, cap, root);
            }
        }
    }
    return false;
}

/* The newest NDK there is. */
static bool find_ndk(char *out, size_t cap)
{
    static const char *const envs[] = { "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK", "NDK_HOME" };
    char f[1200];
    for (size_t i = 0; i < sizeof envs / sizeof envs[0]; ++i) {
        const char *v = getenv(envs[i]);
        if (v && *v) {
            snprintf(f, sizeof f, "%s/build/cmake/android.toolchain.cmake", v);
            if (is_file(f)) {
                return put_path(out, cap, v);
            }
        }
    }
    /* <sdk>/ndk/<version>: the highest version */
    const char *sdk = getenv("ANDROID_HOME");
    if (!sdk || !*sdk)
        sdk = getenv("ANDROID_SDK_ROOT");
    const char *home = getenv("HOME");
    const char *const bases[3][2] = {
        { sdk && *sdk ? sdk : NULL, "" },
        { home, "/Android/Sdk" },
        { home, "/Android/sdk" },
    };
    for (int s = 0; s < 3; ++s) {
        if (!bases[s][0])
            continue;
        char dir[800];
        int w = snprintf(dir, sizeof dir, "%s%s/ndk", bases[s][0], bases[s][1]);
        if (w < 0 || (size_t)w >= sizeof dir)
            continue;
        DIR *d = opendir(dir);
        if (!d)
            continue;
        char best[256] = "";
        for (struct dirent *e; (e = readdir(d)) != NULL;) {
            if (e->d_name[0] == '.')
                continue;
            w = snprintf(f, sizeof f, "%s/%s/build/cmake/android.toolchain.cmake", dir, e->d_name);
            if (w > 0 && (size_t)w < sizeof f && is_file(f) && strverscmp(e->d_name, best) > 0)
                snprintf(best, sizeof best, "%s", e->d_name);
        }
        closedir(d);
        if (best[0]) {
            w = snprintf(out, cap, "%s/%s", dir, best);
            return w > 0 && (size_t)w < cap;
        }
    }
    return false;
}

static bool find_adb(char *out, size_t cap)
{
    if (find_exe("adb", out, cap))
        return true;
    const char *sdk = getenv("ANDROID_HOME");
    char f[800];
    if (sdk && *sdk) {
        snprintf(f, sizeof f, "%s/platform-tools/adb", sdk);
        if (access(f, X_OK) == 0) {
            return put_path(out, cap, f);
        }
    }
    const char *home = getenv("HOME");
    if (home) {
        snprintf(f, sizeof f, "%s/Android/Sdk/platform-tools/adb", home);
        if (access(f, X_OK) == 0) {
            return put_path(out, cap, f);
        }
    }
    return false;
}

/* What each Linux target is called by each tool. */
typedef struct triples {
    const char *gnu, *clang, *zig, *processor, *qemu;
} triples;

static const triples *linux_triples(ccr_target_kind k)
{
    static const triples a64 = { "aarch64-linux-gnu", "aarch64-linux-gnu", "aarch64-linux-musl", "aarch64",
                                 "qemu-aarch64" };
    static const triples a32 = { "arm-linux-gnueabihf", "armv7a-linux-gnueabihf", "arm-linux-musleabihf", "arm",
                                 "qemu-arm" };
    return k == CCR_TARGET_LINUX_ARM64 ? &a64 : k == CCR_TARGET_LINUX_ARM32 ? &a32 : NULL;
}

void ccr_target_probe(ccr_target_kind k, ccr_target_info *out)
{
    memset(out, 0, sizeof *out);
    out->kind = k;
    if (k == CCR_TARGET_NATIVE) {
        out->how = CCR_HOW_NATIVE;
        out->can_run = true;
        snprintf(out->compiler, sizeof out->compiler, "cc");
        return;
    }
    if (k == CCR_TARGET_ANDROID_ARM64 || k == CCR_TARGET_ANDROID_ARM32) {
        if (find_ndk(out->ndk, sizeof out->ndk)) {
            out->how = CCR_HOW_NDK;
            const char *slash = strrchr(out->ndk, '/');
            snprintf(out->compiler, sizeof out->compiler, "Android NDK %.400s", slash ? slash + 1 : out->ndk);
        }
        out->can_run = find_adb(out->runner, sizeof out->runner);
        out->can_run &= out->how != CCR_HOW_NONE;
        return;
    }
    const triples *t = linux_triples(k);
    char gcc[300], path[600];
    snprintf(gcc, sizeof gcc, "%s-gcc", t->gnu);
    /* CCRATCH_CROSS=gcc|zig|clang: only that one (the tests try each) */
    const char *only = getenv("CCRATCH_CROSS");
    const bool any = !only || !*only;
    if ((any || strcmp(only, "gcc") == 0) && find_exe(gcc, path, sizeof path) &&
        find_sysroot(t->gnu, out->sysroot, sizeof out->sysroot)) {
        out->how = CCR_HOW_GCC;
        snprintf(out->compiler, sizeof out->compiler, "%s", gcc);
    } else if ((any || strcmp(only, "zig") == 0) && find_exe("zig", path, sizeof path)) {
        out->how = CCR_HOW_ZIG;
        out->sysroot[0] = '\0';   /* static: needs none to run */
        snprintf(out->compiler, sizeof out->compiler, "zig cc -target %s", t->zig);
    } else if ((any || strcmp(only, "clang") == 0) && find_exe("clang", path, sizeof path) &&
               find_exe("ld.lld", path, sizeof path) && find_sysroot(t->gnu, out->sysroot, sizeof out->sysroot)) {
        out->how = CCR_HOW_CLANG;
        snprintf(out->compiler, sizeof out->compiler, "clang --target=%s", t->clang);
    }
    if (out->how != CCR_HOW_NONE) {
        char q[64];
        snprintf(q, sizeof q, "%s-static", t->qemu);
        out->can_run = find_exe(t->qemu, out->runner, sizeof out->runner) ||
                       find_exe(q, out->runner, sizeof out->runner);
        /* on such a machine itself, it just runs */
        if (!out->can_run && strcmp(ccr_target_host_machine(), t->processor) == 0)
            out->can_run = true;
    }
}

/* ------------------------------------------------------------ CMake */

static const char *keep(char *store, size_t cap, size_t *used, const char *fmt, const char *a, const char *b)
{
    if (*used >= cap)
        return "";
    char *at = store + *used;
    const int n = snprintf(at, cap - *used, fmt, a ? a : "", b ? b : "");
    *used += n >= 0 ? (size_t)n + 1 : 0;
    if (*used > cap)
        *used = cap;
    return at;
}

static bool write_text(const char *path, const char *text, bool exec)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    const bool ok = fputs(text, f) >= 0;
    if (fclose(f) != 0 || !ok)
        return false;
    return !exec || chmod(path, 0755) == 0;
}

static void make_dirs(const char *dir)
{
    char p[1200];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; ++s)
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

bool ccr_target_cmake_args(const ccr_target_info *t, const char *build_dir, const char **argv, int *n, int cap,
                           char *store, size_t store_cap)
{
    size_t used = 0;
    if (t->how == CCR_HOW_NONE)
        return false;
    if (t->how == CCR_HOW_NATIVE)
        return true;
    if (t->how == CCR_HOW_NDK) {
        if (*n + 3 > cap)
            return false;
        argv[(*n)++] = keep(store, store_cap, &used, "-DCMAKE_TOOLCHAIN_FILE=%s/build/cmake/android.toolchain.cmake%s",
                            t->ndk, "");
        argv[(*n)++] = t->kind == CCR_TARGET_ANDROID_ARM64 ? "-DANDROID_ABI=arm64-v8a" : "-DANDROID_ABI=armeabi-v7a";
        argv[(*n)++] = "-DANDROID_PLATFORM=android-24";
        return true;
    }
    /* Linux on ARM: a toolchain file of Ccratch's own, in the build folder */
    const triples *tr = linux_triples(t->kind);
    make_dirs(build_dir);
    char file[1300], text[4096];
    size_t o = (size_t)snprintf(text, sizeof text,
                                "# Made by Ccratch: building for %s with %s.\n"
                                "set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR %s)\n",
                                tr->gnu, t->compiler, tr->processor);
    if (t->how == CCR_HOW_GCC) {
        o += (size_t)snprintf(text + o, sizeof text - o,
                              "set(CMAKE_C_COMPILER %s-gcc)\nset(CMAKE_CXX_COMPILER %s-g++)\n"
                              "set(CMAKE_ASM_COMPILER %s-gcc)\nset(CMAKE_FIND_ROOT_PATH %s)\n",
                              tr->gnu, tr->gnu, tr->gnu, t->sysroot);
    } else if (t->how == CCR_HOW_CLANG) {
        /* Debian's /usr/<triple> is no sysroot (its libc.so names absolute
         * paths): clang finds it, and gcc-cross, by the target alone */
        char debian[600];
        snprintf(debian, sizeof debian, "/usr/%s", tr->gnu);
        if (strcmp(t->sysroot, debian) != 0)
            o += (size_t)snprintf(text + o, sizeof text - o, "set(CMAKE_SYSROOT %s)\n", t->sysroot);
        o += (size_t)snprintf(text + o, sizeof text - o,
                              "set(CMAKE_C_COMPILER clang)\nset(CMAKE_C_COMPILER_TARGET %s)\n"
                              "set(CMAKE_CXX_COMPILER clang++)\nset(CMAKE_CXX_COMPILER_TARGET %s)\n"
                              "set(CMAKE_ASM_COMPILER clang)\nset(CMAKE_ASM_COMPILER_TARGET %s)\n"
                              "set(CMAKE_EXE_LINKER_FLAGS_INIT -fuse-ld=lld)\n",
                              tr->clang, tr->clang, tr->clang);
    } else {   /* zig: little scripts CMake can call a compiler */
        static const char *const tools[3][2] = { { "zig-cc", "cc" }, { "zig-c++", "c++" }, { "zig-ar", "ar" } };
        for (int i = 0; i < 3; ++i) {
            char script[600], path[1300];
            snprintf(path, sizeof path, "%s/%s", build_dir, tools[i][0]);
            if (i < 2)
                snprintf(script, sizeof script, "#!/bin/sh\nexec zig %s -target %s \"$@\"\n", tools[i][1], tr->zig);
            else
                snprintf(script, sizeof script, "#!/bin/sh\nexec zig ar \"$@\"\n");
            if (!write_text(path, script, true))
                return false;
        }
        o += (size_t)snprintf(text + o, sizeof text - o,
                              "set(CMAKE_C_COMPILER %s/zig-cc)\nset(CMAKE_CXX_COMPILER %s/zig-c++)\n"
                              "set(CMAKE_ASM_COMPILER %s/zig-cc)\nset(CMAKE_AR %s/zig-ar)\n"
                              "set(CMAKE_RANLIB true)\n"
                              "# static: runs on any such Linux, and in qemu, needing nothing\n"
                              "set(CMAKE_EXE_LINKER_FLAGS_INIT -static)\n",
                              build_dir, build_dir, build_dir, build_dir);
    }
    if (o < sizeof text)
        snprintf(text + o, sizeof text - o,
                 "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
                 "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n");
    snprintf(file, sizeof file, "%s/ccratch-toolchain.cmake", build_dir);
    if (!write_text(file, text, false) || *n + 1 > cap)
        return false;
    argv[(*n)++] = keep(store, store_cap, &used, "-DCMAKE_TOOLCHAIN_FILE=%s%s", file, "");
    return true;
}

bool ccr_target_run_argv(const ccr_target_info *t, const char *exe, const char **argv, char *store,
                         size_t store_cap)
{
    size_t used = 0;
    int n = 0;
    if (t->how == CCR_HOW_NATIVE) {
        argv[n++] = exe;
    } else if (t->how == CCR_HOW_NDK) {
        if (!t->can_run)
            return false;
        /* onto the phone, and run it there; its output comes back through adb */
        argv[n++] = "sh";
        argv[n++] = "-c";
        argv[n++] = "adb=\"$1\"; exe=\"$2\"; to=/data/local/tmp/$(basename \"$exe\"); "
                    "\"$adb\" push \"$exe\" \"$to\" >/dev/null && \"$adb\" shell chmod 755 \"$to\" && "
                    "\"$adb\" shell \"$to\"";
        argv[n++] = "sh";
        argv[n++] = keep(store, store_cap, &used, "%s%s", t->runner, "");
        argv[n++] = exe;
    } else if (t->runner[0]) {
        argv[n++] = keep(store, store_cap, &used, "%s%s", t->runner, "");
        if (t->sysroot[0]) {
            argv[n++] = "-L";
            argv[n++] = keep(store, store_cap, &used, "%s%s", t->sysroot, "");
        }
        argv[n++] = exe;
    } else if (t->can_run) {
        argv[n++] = exe;   /* on such a machine */
    } else {
        return false;
    }
    argv[n] = NULL;
    return true;
}
