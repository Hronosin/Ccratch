/*
 * The debugger: gdb's MI read right, and a real program stopped, stepped
 * and read (when gdb is installed and may trace).
 */
#define _POSIX_C_SOURCE 200809L   /* nanosleep */
#include "ccr_gdb.h"
#include "ccr_mi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void test_mi(void)
{
    ccr_mi_record r;
    CHECK(!ccr_mi_parse("(gdb) ", &r), "the prompt is no record");
    CHECK(ccr_mi_parse("12^done,bkpt={number=\"3\",file=\"a b.c\",line=\"9\",thread-groups=[\"i1\"]}", &r) &&
          r.token == 12 && r.kind == '^' && strcmp(r.klass, "done") == 0, "a result");
    const ccr_mi_value *b = ccr_mi_get(r.results, "bkpt");
    CHECK(b && b->kind == CCR_MI_TUPLE && strcmp(ccr_mi_str(b, "file", ""), "a b.c") == 0 &&
          strcmp(ccr_mi_str(b, "line", ""), "9") == 0, "a tuple's strings");
    const ccr_mi_value *tg = ccr_mi_get(b, "thread-groups");
    CHECK(tg && tg->kind == CCR_MI_LIST && tg->first && strcmp(tg->first->str, "i1") == 0, "a list of strings");
    ccr_mi_free(&r);

    CHECK(ccr_mi_parse("^done,variables=[{name=\"s\",value=\"0x40 \\\"hi\\\\n\\\"\"},{name=\"n\",arg=\"1\",value=\"5\"}]", &r) &&
          r.token == -1, "variables");
    const ccr_mi_value *vs = ccr_mi_get(r.results, "variables");
    CHECK(vs && vs->first && strcmp(ccr_mi_str(vs->first, "value", ""), "0x40 \"hi\\n\"") == 0,
          "escapes undone: [%s]", vs && vs->first ? ccr_mi_str(vs->first, "value", "") : "");
    CHECK(vs && vs->first && vs->first->next && ccr_mi_get(vs->first->next, "arg"), "the second, an argument");
    ccr_mi_free(&r);

    CHECK(ccr_mi_parse("^done,stack=[frame={level=\"0\",func=\"sq\"},frame={level=\"1\",func=\"main\"}]", &r), "stack");
    const ccr_mi_value *st = ccr_mi_get(r.results, "stack");
    CHECK(st && st->first && st->first->next && strcmp(ccr_mi_str(st->first->next, "func", ""), "main") == 0,
          "a list of named tuples");
    ccr_mi_free(&r);

    CHECK(ccr_mi_parse("~\"\\320\\237\\321\\200\\320\\270\\n\"", &r) && r.kind == '~' && strcmp(r.klass, "При\n") == 0,
          "octal escapes: UTF-8 back");
    ccr_mi_free(&r);
    CHECK(ccr_mi_parse("i=0 total=0", &r) && r.kind == 0 && strcmp(r.klass, "i=0 total=0") == 0, "not MI");
    ccr_mi_free(&r);
    CHECK(ccr_mi_parse("^done,broken={a=\"1\"", &r) && r.kind == 0, "cut short: not MI, kept as text");
    ccr_mi_free(&r);
    CHECK(ccr_mi_parse("*stopped,reason=\"exited\",exit-code=\"01\"", &r) && r.kind == '*' &&
          strcmp(ccr_mi_str(r.results, "exit-code", ""), "01") == 0, "async");
    ccr_mi_free(&r);

    char q[64];
    ccr_mi_quote("my \"dir\"/a.c:3", q, (int)sizeof q);
    CHECK(strcmp(q, "\"my \\\"dir\\\"/a.c:3\"") == 0, "quoted: %s", q);
}

/* Polls until the program stops (or ends); false after 15 s. */
static bool wait_stop(ccr_gdb *g)
{
    for (int i = 0; i < 1500; ++i) {
        ccr_gdb_poll(g);
        if ((g->state == CCR_GDB_STOPPED && g->frame_count > 0 && g->pending_count == 0) ||
            g->state == CCR_GDB_EXITED || g->state == CCR_GDB_OFF)
            return g->state == CCR_GDB_STOPPED;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

static const char *var(const ccr_gdb *g, const char *name)
{
    for (int i = 0; i < g->var_count; ++i)
        if (strcmp(g->vars[i].name, name) == 0)
            return g->vars[i].value;
    return "(none)";
}

static bool printed(const ccr_gdb *g, const char *text)
{
    for (int i = 0; i < g->out_count; ++i)
        if (g->out[i].kind == CCR_GDB_OUT_PROGRAM && strcmp(g->out[i].text, text) == 0)
            return true;
    return false;
}

static ccr_gdb g;   /* big */

static void test_run(const char *exe, const char *src)
{
    ccr_gdb_init(&g);
    char err[256];
    ccr_gdb_set_break(&g, src, 17, true);
    CHECK(g.bp_count == 1 && ccr_gdb_find_break(&g, src, 17) == 0, "a breakpoint before the start");
    CHECK(ccr_gdb_start(&g, exe, NULL, err, (int)sizeof err), "start: %s", err);
    CHECK(wait_stop(&g), "did not stop (state %d, %s)", g.state, g.error);
    if (g.state != CCR_GDB_STOPPED) {
        for (int i = 0; i < g.out_count; ++i)
            fprintf(stderr, "  | %s\n", g.out[i].text);
        ccr_gdb_free(&g);
        return;
    }
    CHECK(strcmp(g.reason, "breakpoint-hit") == 0 && g.stopped_bp == 0 && g.bps[0].hits == 1, "at the breakpoint");
    CHECK(g.frames[0].line == 17 && strcmp(g.frames[0].func, "main") == 0 && strstr(g.frames[0].file, "debugme.c"),
          "where: %s:%d in %s", g.frames[0].file, g.frames[0].line, g.frames[0].func);
    CHECK(strcmp(var(&g, "i"), "0") == 0 && strcmp(var(&g, "total"), "0") == 0, "i=%s total=%s", var(&g, "i"),
          var(&g, "total"));
    CHECK(strcmp(var(&g, "p"), "{a = 1, b = 2}") == 0, "a struct on one line: %s", var(&g, "p"));

    ccr_gdb_next(&g);
    CHECK(wait_stop(&g) && g.frames[0].line == 18, "next: line %d", g.frames[0].line);
    ccr_gdb_next(&g);
    CHECK(wait_stop(&g) && printed(&g, "i=0 total=0"), "the program's line, as it printed it");

    ccr_gdb_continue(&g);
    CHECK(wait_stop(&g) && strcmp(var(&g, "i"), "1") == 0 && g.bps[0].hits == 2, "the second time round: i=%s",
          var(&g, "i"));
    ccr_gdb_step(&g);
    CHECK(wait_stop(&g) && strcmp(g.frames[0].func, "sq") == 0 && g.frame_count == 2 && g.frames[0].line == 8,
          "step into sq: %s:%d", g.frames[0].func, g.frames[0].line);
    CHECK(strcmp(var(&g, "v"), "1") == 0, "its argument: v=%s", var(&g, "v"));
    bool is_arg = false;
    for (int i = 0; i < g.var_count; ++i)
        is_arg = is_arg || (strcmp(g.vars[i].name, "v") == 0 && g.vars[i].arg);
    CHECK(is_arg, "v is an argument");
    ccr_gdb_select_frame(&g, 1);
    CHECK(wait_stop(&g) && g.frame == 1 && strcmp(var(&g, "total"), "0") == 0, "main's variables from inside sq");
    ccr_gdb_finish(&g);
    CHECK(wait_stop(&g) && strcmp(g.frames[0].func, "main") == 0, "out of sq: %s", g.frames[0].func);
    bool told = false;
    for (int i = 0; i < g.out_count; ++i)
        told = told || (g.out[i].kind == CCR_GDB_OUT_INFO && strcmp(g.out[i].text, "sq() -> 1") == 0);
    CHECK(told, "what sq gave back, told");

    /* a breakpoint set while stopped, one taken away */
    ccr_gdb_set_break(&g, src, 17, false);
    ccr_gdb_set_break(&g, src, 20, true);
    ccr_gdb_continue(&g);
    CHECK(wait_stop(&g) && g.frames[0].line == 20 && strcmp(var(&g, "total"), "5") == 0,
          "past the loop: line %d total=%s", g.frames[0].line, var(&g, "total"));
    ccr_gdb_next(&g);
    CHECK(wait_stop(&g), "p.a = total");
    bool changed = false;
    for (int i = 0; i < g.var_count; ++i)
        changed = changed || (strcmp(g.vars[i].name, "p") == 0 && g.vars[i].changed);
    CHECK(changed, "p changed: marked");

    ccr_gdb_continue(&g);
    wait_stop(&g);
    CHECK(g.state == CCR_GDB_EXITED && g.exit_code == 0, "ended: state %d, code %d", g.state, g.exit_code);
    CHECK(printed(&g, "i=2 total=5"), "all it printed");

    /* again, then stopped halfway */
    CHECK(ccr_gdb_start(&g, exe, NULL, err, (int)sizeof err) && wait_stop(&g), "a second run");
    CHECK(g.bps[0].hits == 1, "hits counted afresh");
    ccr_gdb_stop(&g);
    CHECK(g.state == CCR_GDB_OFF && !ccr_gdb_active(&g), "stopped");
    ccr_gdb_free(&g);
}

static bool printed_kind(const ccr_gdb *gd, ccr_gdb_out_kind k, const char *text)
{
    for (int i = 0; i < gd->out_count; ++i)
        if (gd->out[i].kind == k && strcmp(gd->out[i].text, text) == 0)
            return true;
    return false;
}

/* A program that asks: its prompt is seen before the newline, the
 * answer typed goes to its stdin, Ctrl+D ends its input. */
static void test_input(const char *exe)
{
    ccr_gdb_init(&g);
    char err[256];
    CHECK(ccr_gdb_start(&g, exe, NULL, err, (int)sizeof err), "start: %s", err);
    bool asked = false;
    for (int i = 0; i < 1500 && !asked; ++i) {
        ccr_gdb_poll(&g);
        asked = strcmp(ccr_gdb_partial(&g), "name? ") == 0;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK(asked, "the prompt, with no newline yet: [%s]", ccr_gdb_partial(&g));
    ccr_gdb_input(&g, "Ann");
    ccr_gdb_input(&g, "7");
    ccr_gdb_input_end(&g);
    wait_stop(&g);
    CHECK(g.state == CCR_GDB_EXITED && g.exit_code == 0, "ended: %d %d", g.state, g.exit_code);
    CHECK(printed_kind(&g, CCR_GDB_OUT_PROGRAM, "name? ") && printed_kind(&g, CCR_GDB_OUT_INPUT, "Ann") &&
          printed(&g, "hi Ann") && printed(&g, "got 7") && printed(&g, "the end"), "the conversation");
    for (int i = 0; g_failures && i < g.out_count; ++i)
        fprintf(stderr, "  %d| %s\n", g.out[i].kind, g.out[i].text);
    ccr_gdb_free(&g);
}

int main(int argc, char **argv)
{
    test_mi();
    if (argc >= 3 && ccr_gdb_available())
        test_run(argv[1], argv[2]);
    if (argc >= 4 && ccr_gdb_available())
        test_input(argv[3]);
    else
        printf("gdb: no gdb, the live part skipped\n");
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("gdb: all checks passed\n");
    return g_failures ? 1 : 0;
}
