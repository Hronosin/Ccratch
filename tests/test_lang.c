/*
 * Language packs: plural rules, fallbacks, broken packs, remembering the
 * choice. No window.
 *
 *   test_lang SCRATCH_DIR
 */
#define _POSIX_C_SOURCE 200809L   /* setenv */
#include "ccr_lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static char g_dir[900];

static void write_file(const char *name, const char *text)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static void test_plurals(void)
{
    write_file("lang/xs.lang",
               "@name = Slavic test\n@plural = slavic\n"
               "bar.loose = %u one|%u few|%u many\n");
    CHECK(ccr_lang_use("xs", stderr), "xs did not load");
    static const struct { unsigned long n; const char *want; } cases[] = {
        { 0, "0 many" },   { 1, "1 one" },     { 2, "2 few" },     { 4, "4 few" },
        { 5, "5 many" },   { 11, "11 many" },  { 12, "12 many" },  { 14, "14 many" },
        { 21, "21 one" },  { 22, "22 few" },   { 25, "25 many" },  { 101, "101 one" },
        { 111, "111 many" }, { 112, "112 many" }, { 122, "122 few" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char out[64];
        ccr_trnf(out, sizeof out, CCR_T_BAR_LOOSE, cases[i].n, (unsigned)cases[i].n);
        CHECK(strcmp(out, cases[i].want) == 0, "slavic %lu: '%s', want '%s'", cases[i].n, out,
              cases[i].want);
    }
    /* not in the pack: English, with English plurals */
    char out[128];
    ccr_trnf(out, sizeof out, CCR_T_LEFT_OUT, 3, 3u);
    CHECK(strcmp(out, "3 loose stacks left out.") == 0, "fallback: '%s'", out);
    CHECK(strcmp(ccr_lang_name(), "Slavic test") == 0, "name: %s", ccr_lang_name());

    ccr_lang_use("en", NULL);
    ccr_trnf(out, sizeof out, CCR_T_BAR_LOOSE, 1, 1u);
    CHECK(strcmp(out, "1 loose stack (not compiled)") == 0, "english one: '%s'", out);
    ccr_trnf(out, sizeof out, CCR_T_BAR_LOOSE, 0, 0u);
    CHECK(strcmp(out, "0 loose stacks (not compiled)") == 0, "english zero: '%s'", out);
}

/* A pack cannot make printf read arguments that are not there. */
static void test_broken_pack(void)
{
    write_file("lang/xb.lang",
               "@name = Broken\n"
               "status.saved = %d %s\n"                  /* wrong codes */
               "status.not_saved = %n\n"                 /* never */
               "status.cannot_write = no args at all\n"  /* missing %s */
               "status.no_backup = %s %s\n"              /* one too many */
               "status.language = %*s\n"                 /* '*' width reads an int */
               "bar.zoom = %d%\n"                        /* lone % */
               "bar.loose = only one form %u\n"          /* germanic wants 2 */
               "status.cannot_read = Fine: %-20s\n"      /* width is harmless */
               "nonsense.key = ?\n"
               "no equals sign here\n"
               "palette.title = Bl\\|ocks\\nnew line\n"   /* escapes */
               "@plural = martian\n");
    CHECK(ccr_lang_use("xb", NULL), "xb did not load at all");
    CHECK(strcmp(ccr_tr(CCR_T_SAVED), "Saved %s (original in %s~).") == 0, "bad %%d kept");
    CHECK(strcmp(ccr_tr(CCR_T_NOT_SAVED), "Not saved: %s") == 0, "%%n kept");
    CHECK(strcmp(ccr_tr(CCR_T_CANNOT_WRITE), "Not saved: could not write %s") == 0, "dropped %%s kept");
    CHECK(strstr(ccr_tr(CCR_T_NO_BACKUP), "backup") != NULL, "extra %%s kept");
    CHECK(strcmp(ccr_tr(CCR_T_LANGUAGE), "Language: %s") == 0, "%%*s kept");
    CHECK(strcmp(ccr_tr(CCR_T_BAR_ZOOM), "zoom %d%%") == 0, "lone %% kept");
    CHECK(strstr(ccr_trn(CCR_T_BAR_LOOSE, 2), "loose stacks") != NULL, "bad plural kept");
    CHECK(strcmp(ccr_tr(CCR_T_CANNOT_READ), "Fine: %-20s") == 0, "a harmless width was refused");
    CHECK(strcmp(ccr_tr(CCR_T_PALETTE_TITLE), "Bl|ocks\nnew line") == 0, "escapes: '%s'",
          ccr_tr(CCR_T_PALETTE_TITLE));

    char path[1024];
    snprintf(path, sizeof path, "%s/lang/xb.lang", g_dir);
    FILE *devnull = fopen("/dev/null", "w");
    int problems = ccr_lang_check(path, devnull);
    fclose(devnull);
    /* 7 bad strings + unknown key + no '=' + bad @plural, and the rest missing */
    CHECK(problems > 10, "only %d problems found in a broken pack", problems);
    ccr_lang_use("en", NULL);
}

static void test_missing_pack(void)
{
    CHECK(!ccr_lang_use("zz", NULL), "a pack that does not exist loaded");
    CHECK(strcmp(ccr_lang_code(), "en") == 0, "not English after a failed switch");
}

static void test_scan(void)
{
    ccr_lang_info langs[16];
    int n = ccr_lang_scan(langs, 16);
    CHECK(n >= 3 && strcmp(langs[0].code, "en") == 0, "scan: %d packs, first %s", n, langs[0].code);
    bool xs = false;
    for (int i = 0; i < n; ++i)
        xs |= strcmp(langs[i].code, "xs") == 0 && strcmp(langs[i].name, "Slavic test") == 0;
    CHECK(xs, "scan did not find xs with its name");
}

/* The language picked in the window is the one next time. */
static void test_remembered(void)
{
    write_file("config/ccratch/config", "# mine\nzoom = 150\nlang = old\n");
    CHECK(ccr_lang_save_choice("xs"), "could not save the choice");
    char path[1024];
    snprintf(path, sizeof path, "%s/config/ccratch/config", g_dir);
    FILE *f = fopen(path, "rb");
    char text[256] = "";
    if (f) {
        text[fread(text, 1, sizeof text - 1, f)] = '\0';
        fclose(f);
    }
    CHECK(strstr(text, "zoom = 150") && strstr(text, "# mine"), "other settings lost: %s", text);
    CHECK(strstr(text, "lang = xs") && !strstr(text, "lang = old"), "lang not replaced: %s", text);

    unsetenv("CCRATCH_LANG");
    setenv("LANG", "de_DE.UTF-8", 1);
    ccr_lang_pick(NULL, NULL);
    CHECK(strcmp(ccr_lang_code(), "xs") == 0, "saved choice ignored: %s", ccr_lang_code());

    setenv("CCRATCH_LANG", "xb", 1);
    ccr_lang_pick(NULL, NULL);
    CHECK(strcmp(ccr_lang_code(), "xb") == 0, "CCRATCH_LANG ignored");

    ccr_lang_pick("en", NULL);
    CHECK(strcmp(ccr_lang_code(), "en") == 0, "--lang ignored");
    unsetenv("CCRATCH_LANG");
}

/* Nothing saved: the system language, "xs_XX.UTF-8" -> xs. */
static void test_locale(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/config/ccratch/config", g_dir);
    remove(path);
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    setenv("LANG", "xs_XX.UTF-8", 1);
    ccr_lang_pick(NULL, NULL);
    CHECK(strcmp(ccr_lang_code(), "xs") == 0, "LANG ignored: %s", ccr_lang_code());
    setenv("LANG", "C.UTF-8", 1);
    ccr_lang_use("en", NULL);
    ccr_lang_pick(NULL, NULL);
    CHECK(strcmp(ccr_lang_code(), "en") == 0, "C locale is not English");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s SCRATCH_DIR\n", argv[0]);
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    char sub[1024];
    mkdir(g_dir, 0755);
    snprintf(sub, sizeof sub, "%s/lang", g_dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/config", g_dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/config/ccratch", g_dir);
    mkdir(sub, 0755);

    /* only our packs and our config, whatever the machine has */
    snprintf(sub, sizeof sub, "%s/lang", g_dir);
    setenv("CCRATCH_LANG_DIR", sub, 1);
    snprintf(sub, sizeof sub, "%s/nowhere", g_dir);
    setenv("XDG_DATA_HOME", sub, 1);
    snprintf(sub, sizeof sub, "%s/config", g_dir);
    setenv("XDG_CONFIG_HOME", sub, 1);

    test_plurals();
    test_broken_pack();
    test_missing_pack();
    test_scan();
    test_remembered();
    test_locale();
    ccr_lang_reset();

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("lang: all checks passed\n");
    return g_failures ? 1 : 0;
}
