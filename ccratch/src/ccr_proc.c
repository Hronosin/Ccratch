#define _GNU_SOURCE   /* pipe2, execvpe */
#include "ccr_proc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

bool ccr_proc_in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || strchr(name, '/'))
        return name && access(name, X_OK) == 0;
    char buf[4096];
    for (const char *p = path; *p;) {
        const char *colon = strchr(p, ':');
        const size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n && snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, name) < (int)sizeof buf &&
            access(buf, X_OK) == 0)
            return true;
        p += n + (colon != NULL);
    }
    return false;
}

/* The environment, with LC_ALL=C first if asked (built before fork: the
 * child may only do what is safe between fork and exec). */
static char **make_env(bool c_locale)
{
    size_t n = 0;
    while (environ && environ[n])
        ++n;
    char **env = malloc((n + 3) * sizeof *env);
    if (!env)
        return NULL;
    size_t k = 0;
    if (c_locale) {
        env[k++] = "LC_ALL=C";
        env[k++] = "LANGUAGE=C";
    }
    for (size_t i = 0; i < n; ++i) {
        if (c_locale && (strncmp(environ[i], "LC_ALL=", 7) == 0 || strncmp(environ[i], "LANGUAGE=", 9) == 0))
            continue;
        env[k++] = environ[i];
    }
    env[k] = NULL;
    return env;
}

bool ccr_proc_start(ccr_proc *p, const char *const *argv, const char *cwd, bool c_locale,
                    char *err, size_t err_cap)
{
    memset(p, 0, sizeof *p);
    p->fd = -1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        snprintf(err, err_cap, "pipe: %s", strerror(errno));
        return false;
    }
    char **env = make_env(c_locale);
    if (!env) {
        close(fds[0]);
        close(fds[1]);
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    fflush(NULL);
    const pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_cap, "fork: %s", strerror(errno));
        free(env);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        /* the child: its own group, output into the pipe, no stdin */
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
            dup2(devnull, 0);
        if (cwd && chdir(cwd) != 0) {
            static const char msg[] = "ccratch: cannot enter the folder to run in\n";
            if (write(2, msg, sizeof msg - 1) < 0) { /* nothing to do */ }
            _exit(126);
        }
        execvpe(argv[0], (char *const *)argv, env);
        static const char msg[] = ": not found (is it installed?)\n";
        if (write(2, argv[0], strlen(argv[0])) < 0 || write(2, msg, sizeof msg - 1) < 0) { /* */ }
        _exit(127);
    }
    setpgid(pid, pid);   /* both sides: whichever runs first */
    free(env);
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    p->pid = pid;
    p->fd = fds[0];
    return true;
}

static void emit_lines(ccr_proc *p, const char *buf, size_t n, ccr_proc_line_fn fn, void *ctx)
{
    for (size_t i = 0; i < n; ++i) {
        const char c = buf[i];
        if (c == '\n' || p->partial_len + 1 >= sizeof p->partial) {
            if (c != '\n')
                p->partial[p->partial_len++] = c;   /* a very long line: cut */
            while (p->partial_len && p->partial[p->partial_len - 1] == '\r')
                --p->partial_len;
            p->partial[p->partial_len] = '\0';
            if (fn)
                fn(ctx, p->partial);
            p->partial_len = 0;
        } else if (c != '\0') {
            p->partial[p->partial_len++] = c;
        }
    }
}

/* Everything readable now; false at the end of the pipe. */
static bool drain(ccr_proc *p, ccr_proc_line_fn fn, void *ctx)
{
    char buf[8192];
    for (int rounds = 0; rounds < 64 && p->fd >= 0; ++rounds) {
        const ssize_t got = read(p->fd, buf, sizeof buf);
        if (got > 0) {
            emit_lines(p, buf, (size_t)got, fn, ctx);
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return true;
        return false;   /* 0: closed; or an error */
    }
    return true;
}

static void finish(ccr_proc *p, ccr_proc_line_fn fn, void *ctx)
{
    if (p->partial_len) {
        p->partial[p->partial_len] = '\0';
        if (fn)
            fn(ctx, p->partial);
        p->partial_len = 0;
    }
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
    p->pid = 0;
    p->finished = true;
}

bool ccr_proc_poll(ccr_proc *p, ccr_proc_line_fn fn, void *ctx)
{
    if (p->finished || p->pid == 0)
        return false;
    const bool open_pipe = drain(p, fn, ctx);
    if (p->stop_at > 0.0 && now() - p->stop_at > 1.0) {
        kill(-p->pid, SIGKILL);
        p->stop_at = -1.0;   /* once */
    }
    int status = 0;
    const pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid) {
        p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                     : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
        /* whatever it wrote last; a child it left behind may keep the pipe
         * open, so this does not wait for the end of it */
        drain(p, fn, ctx);
        finish(p, fn, ctx);
        return false;
    }
    if (r < 0 && errno == ECHILD) {
        p->exit_code = 1;
        finish(p, fn, ctx);
        return false;
    }
    (void)open_pipe;
    return true;
}

void ccr_proc_stop(ccr_proc *p)
{
    if (p->pid > 0 && !p->finished && p->stop_at == 0.0) {
        kill(-p->pid, SIGTERM);
        p->stop_at = now();
    }
}

void ccr_proc_kill_wait(ccr_proc *p)
{
    if (p->pid > 0 && !p->finished) {
        kill(-p->pid, SIGKILL);
        int status;
        waitpid(p->pid, &status, 0);
        p->exit_code = 128 + SIGKILL;
        finish(p, NULL, NULL);
    } else if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}
