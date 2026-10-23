/*
 * ccr_target - what the Build button builds for: this computer, or another
 * machine. Linux on 64-bit or 32-bit ARM (a Raspberry Pi, an ARM board or
 * server), or Android (a phone) on either.
 *
 * Each target is probed for the tools it needs, in the order they work
 * best:
 *
 *   Linux ARM   a cross gcc with its C library (aarch64-linux-gnu-gcc and
 *               /usr/aarch64-linux-gnu, Debian/Ubuntu's gcc-aarch64-linux-gnu);
 *               else zig (zig cc carries a C library for every machine: a
 *               static program, on any distribution); else clang, with a
 *               sysroot and lld. Run with qemu-aarch64 / qemu-arm, if there.
 *   Android     the NDK ($ANDROID_NDK_HOME, $ANDROID_NDK_ROOT,
 *               ~/Android/Sdk/ndk/<newest>), its CMake toolchain file; run
 *               on a phone plugged in, through adb (pushed to
 *               /data/local/tmp, run there).
 *
 * The CMake side: a toolchain file written into the build folder (or the
 * NDK's), and its own build folder per target, so switching keeps both.
 * A CMakeLists.txt Ccratch makes picks the assembly for the machine by
 * CMAKE_SYSTEM_PROCESSOR, so one project builds for all of them.
 *
 * Knows nothing about the window.
 */
#ifndef CCR_TARGET_H
#define CCR_TARGET_H

#include <stdbool.h>
#include <stddef.h>

typedef enum ccr_target_kind {
    CCR_TARGET_NATIVE,
    CCR_TARGET_LINUX_ARM64,
    CCR_TARGET_LINUX_ARM32,
    CCR_TARGET_ANDROID_ARM64,
    CCR_TARGET_ANDROID_ARM32,
    CCR_TARGET_COUNT
} ccr_target_kind;

typedef enum ccr_target_how {
    CCR_HOW_NONE,      /* nothing found to build with */
    CCR_HOW_NATIVE,    /* the system's compiler */
    CCR_HOW_GCC,       /* a cross gcc and its C library */
    CCR_HOW_ZIG,       /* zig cc, static */
    CCR_HOW_CLANG,     /* clang --target, a sysroot, lld */
    CCR_HOW_NDK        /* the Android NDK */
} ccr_target_how;

typedef struct ccr_target_info {
    ccr_target_kind kind;
    ccr_target_how  how;
    bool            can_run;        /* here: natively, in qemu, or on a phone over adb */
    char            compiler[512];  /* what compiles: "aarch64-linux-gnu-gcc", "zig cc"... */
    char            runner[512];    /* qemu-aarch64, adb: the program, found */
    char            sysroot[512];   /* the target's C library (gcc, clang) */
    char            ndk[1024];      /* the NDK (Android) */
} ccr_target_info;

/* "native", "linux-arm64"...: the config's words; and back (-1: none). */
const char     *ccr_target_key(ccr_target_kind k);
int             ccr_target_from_key(const char *key);
/* "arm64", "android"...: short, for the Build button ("" for this computer). */
const char     *ccr_target_short(ccr_target_kind k);
/* The build folder's suffix: "-arm64" ("" for this computer). */
const char     *ccr_target_suffix(ccr_target_kind k);
/* This computer's machine: "x86_64", "aarch64"... */
const char     *ccr_target_host_machine(void);

/* Looks for the tools `k` needs. */
void ccr_target_probe(ccr_target_kind k, ccr_target_info *out);

/* What CMake needs for it: -D arguments into argv (from *n on, at most
 * `cap` in all; the strings live in `store`), a toolchain file written into
 * `build_dir` when it is Ccratch's own. False if it cannot be built. */
bool ccr_target_cmake_args(const ccr_target_info *t, const char *build_dir, const char **argv, int *n, int cap,
                           char *store, size_t store_cap);

/* How to run `exe` on it: argv (at most 8; strings in `store`). False if
 * nothing here can run it. */
bool ccr_target_run_argv(const ccr_target_info *t, const char *exe, const char **argv, char *store,
                         size_t store_cap);

#endif /* CCR_TARGET_H */
