#define _GNU_SOURCE   /* posix_openpt, ptsname */
#include "ccr_gdb.h"
#include "ccr_mi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { W_NONE, W_BREAK, W_FRAMES, W_VARS, W_DELETE };

static int g_pty_slave = -1;   /* kept open: no EIO between runs */
static int g_inferior = 0;     /* the program's pid, from gdb */

static void out_add(ccr_gdb *g, ccr_gdb_out_kind kind, const char *text)
{
    if (g->out_count == CCR_GDB_OUT_LINES) {   /* full: the oldest tenth goes */
        const int drop = CCR_GDB_OUT_LINES / 10;
        for (int i = 0; i < drop; ++i)
            free(g->out[i].text);
        memmove(g->out, g->out + drop, (size_t)(CCR_GDB_OUT_LINES - drop) * sizeof g->out[0]);
        g->out_count -= drop;
    }
    const size_t n = strlen(text);
    char *t = malloc(n + 1);
    if (!t)
        return;
    memcpy(t, text, n + 1);
    g->out[g->out_count++] = (ccr_gdb_out){ t, kind };
    ++g->generation;
}

void ccr_gdb_init(ccr_gdb *g)
{
    memset(g, 0, sizeof *g);
    g->pty = -1;
    g->proc.fd = g->proc.in_fd = -1;
    g->stopped_bp = -1;
}

bool ccr_gdb_available(void)
{
    return ccr_proc_in_path("gdb");
}

bool ccr_gdb_active(const ccr_gdb *g)
{
    return g->state == CCR_GDB_STARTING || g->state == CCR_GDB_RUNNING || g->state == CCR_GDB_STOPPED;
}

/* A command, numbered; `what` says what its answer is for. */
static void send(ccr_gdb *g, int what, int arg, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void send(ccr_gdb *g, int what, int arg, const char *fmt, ...)
{
    char cmd[2400];
    const long t = ++g->token;
    int n = snprintf(cmd, sizeof cmd, "%ld", t);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(cmd + n, sizeof cmd - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof cmd - 2)
        n = (int)sizeof cmd - 2;
    cmd[n++] = '\n';
    cmd[n] = '\0';
    if (what != W_NONE && g->pending_count < (int)(sizeof g->pending / sizeof g->pending[0]))
        g->pending[g->pending_count++] = (ccr_gdb_ask){ t, what, arg };
    ccr_proc_write(&g->proc, cmd);
}

static void set_one(ccr_gdb *g, int i)
{
    char loc[1100], q[1200];
    snprintf(loc, sizeof loc, "%s:%d", g->bps[i].file, g->bps[i].line);
    ccr_mi_quote(loc, q, (int)sizeof q);
    g->bps[i].sent = true;
    send(g, W_BREAK, i, "-break-insert -f %s", q);
}

static bool open_pty(ccr_gdb *g, char *name, size_t cap)
{
    const int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        if (m >= 0)
            close(m);
        return false;
    }
    const char *s = ptsname(m);
    if (!s) {
        close(m);
        return false;
    }
    snprintf(name, cap, "%s", s);
    if (g_pty_slave >= 0)
        close(g_pty_slave);
    g_pty_slave = open(name, O_RDWR | O_NOCTTY);
    if (g_pty_slave >= 0) {   /* plain lines: no echo, \n stays \n */
        struct termios t;
        if (tcgetattr(g_pty_slave, &t) == 0) {
            t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
            t.c_oflag &= ~(tcflag_t)ONLCR;
            tcsetattr(g_pty_slave, TCSANOW, &t);
        }
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    fcntl(m, F_SETFD, FD_CLOEXEC);
    g->pty = m;
    return true;
}

bool ccr_gdb_start(ccr_gdb *g, const char *exe, const char *cwd, char *err, int err_cap)
{
    if (ccr_gdb_active(g) || g->state == CCR_GDB_EXITED)
        ccr_gdb_stop(g);
    for (int i = 0; i < g->out_count; ++i)
        free(g->out[i].text);
    g->out_count = 0;
    g->frame_count = g->var_count = 0;
    g->reason[0] = g->signal[0] = g->error[0] = '\0';
    g->stopped_bp = -1;
    g->pending_count = 0;
    g->pty_partial_len = 0;
    g_inferior = 0;
    for (int i = 0; i < g->bp_count; ++i) {
        g->bps[i].number = 0;
        g->bps[i].hits = 0;
        g->bps[i].pending = false;
        g->bps[i].sent = false;
    }
    snprintf(g->exe, sizeof g->exe, "%s", exe);

    char tty[256];
    if (!open_pty(g, tty, sizeof tty)) {
        snprintf(err, (size_t)err_cap, "no terminal for the program: %s", strerror(errno));
        return false;
    }
    const char *argv[] = { "gdb", "--interpreter=mi2", "-q", "-nx", exe, NULL };
    if (!ccr_proc_start_in(&g->proc, argv, cwd, true, err, (size_t)err_cap)) {
        close(g->pty);
        g->pty = -1;
        return false;
    }
    g->state = CCR_GDB_STARTING;
    send(g, W_NONE, 0, "-gdb-set confirm off");
    send(g, W_NONE, 0, "-gdb-set pagination off");
    send(g, W_NONE, 0, "-gdb-set mi-async on");
    send(g, W_NONE, 0, "-gdb-set print elements 40");
    send(g, W_NONE, 0, "-gdb-set startup-with-shell off");
    send(g, W_NONE, 0, "-inferior-tty-set %s", tty);
    for (int i = 0; i < g->bp_count; ++i)
        set_one(g, i);
    send(g, W_NONE, 0, "-exec-run");
    ++g->generation;
    return true;
}

/* ------------------------------------------------------------ answers */

static int take_pending(ccr_gdb *g, long token, int *arg)
{
    for (int i = 0; i < g->pending_count; ++i) {
        if (g->pending[i].token != token)
            continue;
        const int what = g->pending[i].what;
        *arg = g->pending[i].arg;
        g->pending[i] = g->pending[--g->pending_count];
        return what;
    }
    return W_NONE;
}

static void read_frame(const ccr_mi_value *f, ccr_gdb_frame *out)
{
    snprintf(out->func, sizeof out->func, "%s", ccr_mi_str(f, "func", "??"));
    snprintf(out->file, sizeof out->file, "%s", ccr_mi_str(f, "fullname", ""));
    out->line = atoi(ccr_mi_str(f, "line", "0"));
    if (!out->line)
        out->file[0] = '\0';
}

static int bp_by_number(const ccr_gdb *g, int number)
{
    for (int i = 0; i < g->bp_count; ++i)
        if (g->bps[i].number == number && number > 0)
            return i;
    return -1;
}

static void ask_vars(ccr_gdb *g, int frame)
{
    send(g, W_VARS, frame, "-stack-list-variables --thread 1 --frame %d --all-values", frame);
}

static bool steppy(const char *reason)
{
    return strcmp(reason, "end-stepping-range") == 0 || strcmp(reason, "function-finished") == 0;
}

static void on_frames(ccr_gdb *g, const ccr_mi_value *res)
{
    const ccr_mi_value *stack = ccr_mi_get(res, "stack");
    g->frame_count = 0;
    for (const ccr_mi_value *f = stack ? stack->first : NULL; f && g->frame_count < CCR_GDB_MAX_FRAMES; f = f->next)
        read_frame(f, &g->frames[g->frame_count++]);
    g->frame = 0;
    /* stepped out of the program's own code (past the end of main, into
     * libc): back to code that has a source, or to the end */
    if (g->frame_count && !g->frames[0].file[0] && steppy(g->reason)) {
        bool source_above = false;
        for (int i = 1; i < g->frame_count; ++i)
            source_above = source_above || g->frames[i].file[0];
        g->state = CCR_GDB_RUNNING;
        g->finished_from[0] = '\0';   /* libc's own: what it returns is no news */
        send(g, W_NONE, 0, source_above ? "-exec-finish" : "-exec-continue");
        return;
    }
    ++g->generation;
}

static void on_vars(ccr_gdb *g, const ccr_mi_value *res, int frame)
{
    static ccr_gdb_var was[CCR_GDB_MAX_VARS];
    const int was_n = g->var_count;
    memcpy(was, g->vars, (size_t)was_n * sizeof was[0]);
    const bool same_place = frame == g->frame;
    g->var_count = 0;
    const ccr_mi_value *list = ccr_mi_get(res, "variables");
    for (const ccr_mi_value *v = list ? list->first : NULL; v && g->var_count < CCR_GDB_MAX_VARS; v = v->next) {
        ccr_gdb_var *o = &g->vars[g->var_count++];
        snprintf(o->name, sizeof o->name, "%s", ccr_mi_str(v, "name", "?"));
        const char *val = ccr_mi_str(v, "value", "...");
        /* one line: a struct printed over several becomes { a = 1, b = 2 } */
        size_t k = 0;
        bool space = false;
        for (const char *c = val; *c && k + 2 < sizeof o->value; ++c) {
            if (*c == '\n' || *c == '\r' || *c == '\t' || *c == ' ') {
                space = k > 0;
                continue;
            }
            if (space)
                o->value[k++] = ' ';
            space = false;
            o->value[k++] = *c;
        }
        o->value[k] = '\0';
        o->arg = ccr_mi_get(v, "arg") != NULL;
        o->changed = false;
        if (same_place && g->frame == 0)
            for (int i = 0; i < was_n; ++i)
                if (strcmp(was[i].name, o->name) == 0)
                    o->changed = strcmp(was[i].value, o->value) != 0;
    }
    g->frame = frame;
    ++g->generation;
}

static void on_stopped(ccr_gdb *g, const ccr_mi_value *res)
{
    snprintf(g->reason, sizeof g->reason, "%s", ccr_mi_str(res, "reason", ""));
    g->signal[0] = '\0';
    g->stopped_bp = -1;
    char note[600];
    if (strncmp(g->reason, "exited", 6) == 0) {
        g->state = CCR_GDB_EXITED;
        g->exit_code = (int)strtol(ccr_mi_str(res, "exit-code", "0"), NULL, 8);
        if (strcmp(g->reason, "exited-signalled") == 0) {
            snprintf(g->signal, sizeof g->signal, "%s", ccr_mi_str(res, "signal-name", "?"));
            g->exit_code = 128;
        }
        g->frame_count = 0;
        g->var_count = 0;
        ++g->generation;
        return;
    }
    g->state = CCR_GDB_STOPPED;
    if (strcmp(g->reason, "signal-received") == 0)
        snprintf(g->signal, sizeof g->signal, "%s", ccr_mi_str(res, "signal-name", "?"));
    const ccr_mi_value *f = ccr_mi_get(res, "frame");
    if (f) {
        g->frame_count = 1;
        read_frame(f, &g->frames[0]);
    }
    const int bp = bp_by_number(g, atoi(ccr_mi_str(res, "bkptno", "0")));
    if (bp >= 0) {
        g->stopped_bp = bp;
        ++g->bps[bp].hits;
    }
    if (g->signal[0]) {
        snprintf(note, sizeof note, "%s: %s", g->signal, ccr_mi_str(res, "signal-meaning", ""));
        out_add(g, CCR_GDB_OUT_ERROR, note);
    }
    /* out of a function: what it gave back */
    const char *ret = ccr_mi_str(res, "return-value", NULL);
    if (ret && strcmp(g->reason, "function-finished") == 0 && g->finished_from[0]) {
        snprintf(note, sizeof note, "%s -> %.400s", g->finished_from, ret);
        out_add(g, CCR_GDB_OUT_INFO, note);
    }
    g->finished_from[0] = '\0';
    g->frame = 0;
    for (int i = 0; i < g->bp_count; ++i)   /* set while it ran */
        if (!g->bps[i].sent)
            set_one(g, i);
    send(g, W_FRAMES, 0, "-stack-list-frames --thread 1");
    ask_vars(g, 0);
    ++g->generation;
}

static void on_record(ccr_gdb *g, const ccr_mi_record *r)
{
    switch (r->kind) {
    case 0:   /* gdb's own words outside MI (rare): shown as they are */
        out_add(g, CCR_GDB_OUT_INFO, r->klass);
        return;
    case '~': case '@': case '&':
        return;   /* gdb talking to a terminal: the panel says it better */
    case '=':
        if (strcmp(r->klass, "thread-group-started") == 0) {
            g_inferior = atoi(ccr_mi_str(r->results, "pid", "0"));
        } else if (strcmp(r->klass, "breakpoint-modified") == 0) {
            const ccr_mi_value *b = ccr_mi_get(r->results, "bkpt");
            const int i = bp_by_number(g, atoi(ccr_mi_str(b, "number", "0")));
            if (i >= 0)
                g->bps[i].pending = ccr_mi_get(b, "pending") != NULL;
        }
        return;
    case '*':
        if (strcmp(r->klass, "running") == 0) {
            g->state = CCR_GDB_RUNNING;
            ++g->generation;
        } else if (strcmp(r->klass, "stopped") == 0) {
            on_stopped(g, r->results);
        }
        return;
    case '^': {
        int arg = 0;
        const int what = r->token >= 0 ? take_pending(g, r->token, &arg) : W_NONE;
        const bool done = strcmp(r->klass, "done") == 0;
        if (strcmp(r->klass, "error") == 0) {
            snprintf(g->error, sizeof g->error, "%s", ccr_mi_str(r->results, "msg", "?"));
            if (what == W_BREAK && arg < g->bp_count) {
                g->bps[arg].pending = true;
            } else if (what != W_VARS && what != W_FRAMES) {
                out_add(g, CCR_GDB_OUT_ERROR, g->error);
                if (g->state == CCR_GDB_STARTING)   /* could not even start it */
                    g->state = CCR_GDB_EXITED;
            }
            ++g->generation;
            return;
        }
        if (strcmp(r->klass, "running") == 0) {
            g->state = CCR_GDB_RUNNING;
            ++g->generation;
            return;
        }
        if (strcmp(r->klass, "exit") == 0) {
            g->state = CCR_GDB_OFF;
            ++g->generation;
            return;
        }
        if (!done)
            return;
        if (what == W_BREAK && arg < g->bp_count) {
            const ccr_mi_value *b = ccr_mi_get(r->results, "bkpt");
            g->bps[arg].number = atoi(ccr_mi_str(b, "number", "0"));
            g->bps[arg].pending = ccr_mi_get(b, "pending") != NULL;
            ++g->generation;
        } else if (what == W_FRAMES) {
            on_frames(g, r->results);
        } else if (what == W_VARS) {
            on_vars(g, r->results, arg);
        }
        return;
    }
    default:
        return;
    }
}

void ccr_gdb_feed(ccr_gdb *g, const char *line)
{
    ccr_mi_record r;
    if (!ccr_mi_parse(line, &r))
        return;
    on_record(g, &r);
    ccr_mi_free(&r);
}

static void gdb_line(void *ctx, const char *line)
{
    ccr_gdb_feed(ctx, line);
}

/* What the program printed: whole lines; a prompt with no newline yet
 * waits until it stops or more comes. */
static void read_pty(ccr_gdb *g, bool flush)
{
    char buf[4096];
    for (int rounds = 0; rounds < 16 && g->pty >= 0; ++rounds) {
        const ssize_t got = read(g->pty, buf, sizeof buf);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; ++i) {
            const char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n' || g->pty_partial_len + 1 >= (int)sizeof g->pty_partial) {
                if (c != '\n')
                    g->pty_partial[g->pty_partial_len++] = c;
                g->pty_partial[g->pty_partial_len] = '\0';
                out_add(g, CCR_GDB_OUT_PROGRAM, g->pty_partial);
                g->pty_partial_len = 0;
            } else {
                g->pty_partial[g->pty_partial_len++] = c;
            }
        }
    }
    if (flush && g->pty_partial_len) {
        g->pty_partial[g->pty_partial_len] = '\0';
        out_add(g, CCR_GDB_OUT_PROGRAM, g->pty_partial);
        g->pty_partial_len = 0;
    }
}

void ccr_gdb_poll(ccr_gdb *g)
{
    if (g->state == CCR_GDB_OFF && (g->pty < 0 || !g->proc.pid))
        return;
    const ccr_gdb_state before = g->state;
    read_pty(g, false);
    if (g->proc.pid && !ccr_proc_poll(&g->proc, gdb_line, g)) {
        read_pty(g, true);
        if (g->state != CCR_GDB_EXITED)
            g->state = CCR_GDB_OFF;
        ++g->generation;
    }
    if (g->state != before && (g->state == CCR_GDB_STOPPED || g->state == CCR_GDB_EXITED))
        read_pty(g, true);
}

/* ------------------------------------------------------------ control */

static void go(ccr_gdb *g, const char *cmd)
{
    if (g->state != CCR_GDB_STOPPED)
        return;
    g->state = CCR_GDB_RUNNING;
    g->stopped_bp = -1;
    send(g, W_NONE, 0, "%s", cmd);
    ++g->generation;
}

void ccr_gdb_continue(ccr_gdb *g) { go(g, "-exec-continue"); }
void ccr_gdb_next(ccr_gdb *g)     { go(g, "-exec-next"); }
void ccr_gdb_step(ccr_gdb *g)     { go(g, "-exec-step"); }
void ccr_gdb_finish(ccr_gdb *g)
{
    if (g->state == CCR_GDB_STOPPED && g->frame_count > 0 && g->frames[0].file[0])
        snprintf(g->finished_from, sizeof g->finished_from, "%s()", g->frames[0].func);
    go(g, g->frame_count > 1 ? "-exec-finish" : "-exec-next");
}

void ccr_gdb_pause(ccr_gdb *g)
{
    if (g->state != CCR_GDB_RUNNING)
        return;
    send(g, W_NONE, 0, "-exec-interrupt");
    if (g_inferior > 0)
        kill(g_inferior, SIGINT);   /* and if gdb is not listening, this */
}

void ccr_gdb_stop(ccr_gdb *g)
{
    if (g->proc.pid && !g->proc.finished) {
        send(g, W_NONE, 0, "-gdb-exit");
        /* a moment for gdb to end the program itself */
        for (int i = 0; i < 50 && ccr_proc_poll(&g->proc, gdb_line, g); ++i) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        ccr_proc_kill_wait(&g->proc);
    }
    if (g_inferior > 0 && kill(g_inferior, 0) == 0)
        kill(g_inferior, SIGKILL);
    g_inferior = 0;
    read_pty(g, true);
    if (g->pty >= 0)
        close(g->pty);
    g->pty = -1;
    g->state = CCR_GDB_OFF;
    g->frame_count = g->var_count = 0;
    g->stopped_bp = -1;
    ++g->generation;
}

void ccr_gdb_free(ccr_gdb *g)
{
    ccr_gdb_stop(g);
    for (int i = 0; i < g->out_count; ++i)
        free(g->out[i].text);
    g->out_count = 0;
    if (g_pty_slave >= 0)
        close(g_pty_slave);
    g_pty_slave = -1;
}

void ccr_gdb_select_frame(ccr_gdb *g, int i)
{
    if (g->state != CCR_GDB_STOPPED || i < 0 || i >= g->frame_count)
        return;
    ask_vars(g, i);
}

/* ---------------------------------------------------------- breakpoints */

int ccr_gdb_find_break(const ccr_gdb *g, const char *file, int line)
{
    for (int i = 0; i < g->bp_count; ++i)
        if (g->bps[i].line == line && strcmp(g->bps[i].file, file) == 0)
            return i;
    return -1;
}

static void remove_at(ccr_gdb *g, int i)
{
    if (g->bps[i].number > 0 && ccr_gdb_active(g))
        send(g, W_NONE, 0, "-break-delete %d", g->bps[i].number);
    /* answers on their way for later ones point one lower */
    for (int k = 0; k < g->pending_count; ++k)
        if (g->pending[k].what == W_BREAK) {
            if (g->pending[k].arg == i)
                g->pending[k].what = W_NONE;
            else if (g->pending[k].arg > i)
                --g->pending[k].arg;
        }
    if (g->stopped_bp == i)
        g->stopped_bp = -1;
    else if (g->stopped_bp > i)
        --g->stopped_bp;
    memmove(&g->bps[i], &g->bps[i + 1], (size_t)(g->bp_count - i - 1) * sizeof g->bps[0]);
    --g->bp_count;
}

void ccr_gdb_set_break(ccr_gdb *g, const char *file, int line, bool on)
{
    const int at = ccr_gdb_find_break(g, file, line);
    if (!on) {
        if (at >= 0) {
            remove_at(g, at);
            ++g->generation;
        }
        return;
    }
    if (at >= 0 || g->bp_count == CCR_GDB_MAX_BPS || line <= 0)
        return;
    ccr_gdb_bp *b = &g->bps[g->bp_count++];
    memset(b, 0, sizeof *b);
    snprintf(b->file, sizeof b->file, "%s", file);
    b->line = line;
    /* while it runs gdb takes no new breakpoints (all-stop): at the next stop */
    if (g->state == CCR_GDB_STOPPED || g->state == CCR_GDB_STARTING)
        set_one(g, g->bp_count - 1);
    ++g->generation;
}

void ccr_gdb_clear_breaks(ccr_gdb *g, const char *file)
{
    for (int i = g->bp_count - 1; i >= 0; --i)
        if (strcmp(g->bps[i].file, file) == 0)
            remove_at(g, i);
    ++g->generation;
}
