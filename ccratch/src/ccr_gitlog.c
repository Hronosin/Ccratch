#define _DEFAULT_SOURCE   /* realpath */
#include "ccr_gitlog.h"
#include "ccr_git.h"
#include "ccr_lang.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* git's words when it fails: its last line that says something. */
static bool run(const char *root, const char *const *args, char *err, int cap)
{
    int code = 0;
    char *out = ccr_git_run(root, args, &code);
    if (cap)
        err[0] = '\0';
    if (!out) {
        snprintf(err, (size_t)cap, "git did not start");
        return false;
    }
    if (code != 0 && cap) {
        char *best = NULL;
        for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n"))
            if (*line && strncmp(line, "hint:", 5) != 0)
                if (!best || strncmp(line, "error:", 6) == 0 || strncmp(line, "fatal:", 6) == 0 ||
                    strncmp(line, "CONFLICT", 8) == 0)
                    best = line;
        snprintf(err, (size_t)cap, "%s", best ? best : "git said no");
    }
    free(out);
    return code == 0;
}

/* ------------------------------------------------------------ graph */

/* Lanes, as gitk and VS Code draw them: each lane waits for a commit; a
 * commit takes the first lane waiting for it (a new one if none), hands
 * it to its first parent, and opens or joins lanes for the others. */
void ccr_git_graph_lanes(ccr_graph_row *rows, const char (*parents_of)[400], int n)
{
    char lanes[64][41];
    int nl = 0;
    for (int r = 0; r < n; ++r) {
        ccr_graph_row *row = &rows[r];
        row->edge_count = 0;
        int col = -1;
        for (int i = 0; i < nl; ++i)
            if (strcmp(lanes[i], row->hash) == 0) {
                if (col < 0)
                    col = i;
                else if (row->edge_count < CCR_GRAPH_EDGES)   /* another lane ends here */
                    row->edges[row->edge_count++] = (ccr_graph_edge){ (int8_t)i, (int8_t)col, true };
            }
        if (col < 0) {
            col = nl < 64 ? nl++ : 63;
            snprintf(lanes[col], sizeof lanes[0], "%s", row->hash);
        }
        row->lane = col;
        /* the parents */
        char ps[8][41];
        int np = 0;
        const char *p = parents_of[r];
        while (*p && np < 8) {
            while (*p == ' ')
                ++p;
            size_t len = strcspn(p, " ");
            if (!len)
                break;
            snprintf(ps[np++], sizeof ps[0], "%.*s", (int)(len < 40 ? len : 40), p);
            p += len;
        }
        row->parents = np;
        /* the lanes after this row: ended lanes go, the commit's goes to
         * its first parent, others join or open */
        char next[64][41];
        int nn = 0, map[64];
        for (int i = 0; i < nl; ++i) {
            map[i] = -1;
            const bool ends = strcmp(lanes[i], row->hash) == 0 && i != col;
            if (ends)
                continue;
            if (i == col) {
                if (np == 0)
                    continue;   /* the first commit: its lane ends */
                map[i] = nn;
                snprintf(next[nn++], sizeof next[0], "%s", ps[0]);
            } else {
                map[i] = nn;
                snprintf(next[nn++], sizeof next[0], "%s", lanes[i]);
            }
        }
        for (int i = 0; i < nl; ++i)
            if (map[i] >= 0 && row->edge_count < CCR_GRAPH_EDGES)
                row->edges[row->edge_count++] = (ccr_graph_edge){ (int8_t)i, (int8_t)map[i], false };
        for (int k = 1; k < np; ++k) {
            int at = -1;
            for (int j = 0; j < nn; ++j)
                if (strcmp(next[j], ps[k]) == 0)
                    at = j;
            if (at < 0 && nn < 64) {
                at = nn;
                snprintf(next[nn++], sizeof next[0], "%s", ps[k]);
            }
            if (at >= 0 && row->edge_count < CCR_GRAPH_EDGES)
                row->edges[row->edge_count++] = (ccr_graph_edge){ (int8_t)col, (int8_t)at, false };
        }
        memcpy(lanes, next, sizeof next[0] * (size_t)nn);
        nl = nn;
    }
}

int ccr_git_graph(const char *root, ccr_graph_row *out, int cap)
{
    char n[16];
    snprintf(n, sizeof n, "-%d", cap);
    /* every branch and tag, not the stash's own commits */
    const char *args[] = { "log", "--branches", "--tags", "--remotes", "HEAD", "--topo-order", n,
                           "--format=%H%x09%P%x09%h%x09%an%x09%at%x09%D%x09"
                           "%(trailers:key=Co-authored-by,separator=%x1f)%x09%s", NULL };
    int code = 0;
    char *o = ccr_git_run(root, args, &code);
    if (!o || code != 0) {
        free(o);
        return 0;
    }
    char (*parents)[400] = calloc((size_t)(cap > 0 ? cap : 1), sizeof *parents);
    int k = 0;
    for (char *line = o, *next; parents && line && *line && k < cap; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        char *f[8] = { line };
        int nf = 1;
        for (char *c = line; *c && nf < 8; ++c)   /* the subject last: a tab in it stays */
            if (*c == '\t') {
                *c = '\0';
                f[nf++] = c + 1;
            }
        if (nf < 8)
            continue;
        ccr_graph_row *r = &out[k];
        memset(r, 0, sizeof *r);
        snprintf(r->hash, sizeof r->hash, "%s", f[0]);
        snprintf(parents[k], sizeof parents[0], "%s", f[1]);
        snprintf(r->short_hash, sizeof r->short_hash, "%s", f[2]);
        snprintf(r->author, sizeof r->author, "%s", f[3]);
        ccr_git_ago((int64_t)strtoll(f[4], NULL, 10), r->when, (int)sizeof r->when);
        snprintf(r->refs, sizeof r->refs, "%s", f[5]);
        snprintf(r->subject, sizeof r->subject, "%s", f[7]);
        r->coauthor_count = ccr_git_coauthors(f[6], r->author, r->coauthors, CCR_GIT_COAUTHORS);
        r->head = strncmp(f[5], "HEAD", 4) == 0;
        ++k;
    }
    if (parents)
        ccr_git_graph_lanes(out, (const char (*)[400])parents, k);
    free(parents);
    free(o);
    return k;
}

/* --------------------------------------------------------- commits */

char *ccr_git_show(const char *root, const char *hash)
{
    const char *args[] = { "show", "--no-ext-diff", "--stat", "--patch", "--format=%H%n%an <%ae>%n%ad%n%n%B", hash, NULL };
    int code = 0;
    return ccr_git_run(root, args, &code);
}

bool ccr_git_revert(const char *root, const char *hash, char *err, int cap)
{
    const char *args[] = { "revert", "--no-edit", hash, NULL };
    return run(root, args, err, cap);
}

bool ccr_git_cherry_pick(const char *root, const char *hash, char *err, int cap)
{
    const char *args[] = { "cherry-pick", hash, NULL };
    return run(root, args, err, cap);
}

bool ccr_git_branch_from(const char *root, const char *name, const char *hash, char *err, int cap)
{
    const char *args[] = { "switch", "-c", name, hash, NULL };
    return run(root, args, err, cap);
}

/* ---------------------------------------------------- co-authors */

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        const int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* "  Name <mail>  " -> its name and mail (either may come out empty). */
static void person_split(const char *s, size_t len, char *name, size_t ncap, char *mail, size_t mcap)
{
    while (len && (*s == ' ' || *s == '\t'))
        ++s, --len;
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
        --len;
    const char *lt = memchr(s, '<', len);
    size_t nl = lt ? (size_t)(lt - s) : len;
    while (nl && s[nl - 1] == ' ')
        --nl;
    snprintf(name, ncap, "%.*s", (int)nl, s);
    mail[0] = '\0';
    if (lt) {
        const char *gt = memchr(lt, '>', len - (size_t)(lt - s));
        const size_t ml = gt ? (size_t)(gt - lt - 1) : len - (size_t)(lt - s) - 1;
        snprintf(mail, mcap, "%.*s", (int)ml, lt + 1);
    }
    if (!name[0] && mail[0])   /* "<mail>" alone: the mail is the name */
        snprintf(name, ncap, "%s", mail);
}

int ccr_git_coauthors(const char *message, const char *author, char out[][64], int cap)
{
    static const char key[] = "co-authored-by:";
    int n = 0;
    for (const char *p = message; p && *p && n < cap;) {
        const char *e = p + strcspn(p, "\n\x1f");
        while (p < e && (*p == ' ' || *p == '\t'))
            ++p;
        const size_t kl = sizeof key - 1;
        bool is = (size_t)(e - p) > kl;
        for (size_t i = 0; is && i < kl; ++i)
            is = ((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]) == key[i];
        if (is) {
            char name[64], mail[160];
            person_split(p + kl, (size_t)(e - p) - kl, name, sizeof name, mail, sizeof mail);
            bool dup = !name[0] || (author && same_name(name, author));
            for (int k = 0; k < n && !dup; ++k)
                dup = same_name(out[k], name);
            if (!dup)
                snprintf(out[n++], 64, "%s", name);
        }
        p = *e ? e + 1 : e;
    }
    return n;
}

char *ccr_git_with_coauthors(const char *message, const char (*people)[160], int n)
{
    size_t len = strlen(message), cap = len + 4;
    for (int i = 0; i < n; ++i)
        cap += strlen(people[i]) + 20;
    char *o = malloc(cap);
    if (!o)
        return NULL;
    memcpy(o, message, len + 1);
    while (len && (o[len - 1] == '\n' || o[len - 1] == ' '))
        o[--len] = '\0';
    char named[CCR_GIT_COAUTHORS * 4][64];
    const int had = ccr_git_coauthors(message, NULL, named, (int)(sizeof named / sizeof named[0]));
    /* already a trailer block at its end: the new lines join it */
    const char *last = strrchr(o, '\n');
    bool block = had > 0 && last && strncmp(last + 1, "Co-authored-by:", 15) == 0;
    for (int i = 0; i < n; ++i) {
        char name[64], mail[160];
        person_split(people[i], strlen(people[i]), name, sizeof name, mail, sizeof mail);
        bool dup = !name[0];
        for (int k = 0; k < had && !dup; ++k)
            dup = same_name(named[k], name);
        for (int k = 0; k < i && !dup; ++k)
            dup = strcmp(people[k], people[i]) == 0;
        if (dup)
            continue;
        len += (size_t)snprintf(o + len, cap - len, "%sCo-authored-by: %s", block ? "\n" : "\n\n", people[i]);
        block = true;
    }
    return o;
}

typedef struct person_count {
    char who[160];   /* "Name <mail>" as first seen */
    char key[160];   /* the mail (or the name), small letters */
    int  count;
} person_count;

static void count_person(person_count **v, int *n, int *cap, const char *s, size_t len, const char *me)
{
    char name[64], mail[160], key[160];
    person_split(s, len, name, sizeof name, mail, sizeof mail);
    if (!name[0])
        return;
    snprintf(key, sizeof key, "%s", mail[0] ? mail : name);
    for (char *c = key; *c; ++c)
        if (*c >= 'A' && *c <= 'Z')
            *c += 32;
    if (me[0] && strcmp(key, me) == 0)
        return;
    for (int i = 0; i < *n; ++i)
        if (strcmp((*v)[i].key, key) == 0) {
            ++(*v)[i].count;
            return;
        }
    if (*n == *cap) {
        const int nc = *cap ? *cap * 2 : 32;
        person_count *nv = realloc(*v, (size_t)nc * sizeof *nv);
        if (!nv)
            return;
        *v = nv;
        *cap = nc;
    }
    person_count *p = &(*v)[(*n)++];
    if (mail[0])
        snprintf(p->who, sizeof p->who, "%.63s <%.90s>", name, mail);
    else
        snprintf(p->who, sizeof p->who, "%s", name);
    snprintf(p->key, sizeof p->key, "%s", key);
    p->count = 1;
}

int ccr_git_people(const char *root, char out[][160], int cap)
{
    char me[160] = "";
    {
        const char *args[] = { "config", "user.email", NULL };
        int code = 0;
        char *o = ccr_git_run(root, args, &code);
        if (o && code == 0) {
            snprintf(me, sizeof me, "%.*s", (int)strcspn(o, "\r\n"), o);
            for (char *c = me; *c; ++c)
                if (*c >= 'A' && *c <= 'Z')
                    *c += 32;
        }
        free(o);
    }
    const char *args[] = { "log", "-n", "500", "--format=%an <%ae>%x1f%(trailers:key=Co-authored-by,valueonly,separator=%x1f)", NULL };
    int code = 0;
    char *o = ccr_git_run(root, args, &code);
    if (!o || code != 0) {
        free(o);
        return 0;
    }
    person_count *v = NULL;
    int n = 0, vcap = 0;
    for (const char *p = o; *p;) {
        const size_t l = strcspn(p, "\n\x1f");
        if (l)
            count_person(&v, &n, &vcap, p, l, me);
        p += l;
        if (*p)
            ++p;
    }
    free(o);
    /* the most frequent first; the same count: as first seen (newest) */
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && v[j].count > v[j - 1].count; --j) {
            const person_count t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    const int k = n < cap ? n : cap;
    for (int i = 0; i < k; ++i)
        snprintf(out[i], 160, "%s", v[i].who);
    free(v);
    return k;
}

/* ------------------------------------------------------------ blame */

static int commit_index(ccr_blame *b, const char *hash)
{
    for (int i = 0; i < b->commit_count; ++i)
        if (strcmp(b->commits[i].hash, hash) == 0)
            return i;
    ccr_blame_commit *v = realloc(b->commits, (size_t)(b->commit_count + 1) * sizeof *v);
    if (!v)
        return -1;
    b->commits = v;
    ccr_blame_commit *c = &v[b->commit_count];
    memset(c, 0, sizeof *c);
    snprintf(c->hash, sizeof c->hash, "%s", hash);
    c->uncommitted = strspn(hash, "0") == strlen(hash);
    return b->commit_count++;
}

/* Blame does not read messages: the co-authors of its commits, asked for
 * together (a few dozen at a time). */
static void blame_coauthors(const char *dir, ccr_blame *b)
{
    enum { CHUNK = 64 };
    for (int from = 0; from < b->commit_count; from += CHUNK) {
        const char *args[CHUNK + 5];
        int n = 0;
        args[n++] = "show";
        args[n++] = "-s";
        args[n++] = "--format=%H%x09%(trailers:key=Co-authored-by,separator=%x1f)";
        for (int i = from; i < b->commit_count && i < from + CHUNK; ++i)
            if (!b->commits[i].uncommitted)
                args[n++] = b->commits[i].hash;
        if (n == 3)
            continue;
        args[n] = NULL;
        int code = 0;
        char *o = ccr_git_run(dir, args, &code);
        for (char *line = o, *next; o && code == 0 && line && *line; line = next) {
            next = strchr(line, '\n');
            if (next)
                *next++ = '\0';
            char *tab = strchr(line, '\t');
            if (!tab || !tab[1])
                continue;
            *tab = '\0';
            for (int i = from; i < b->commit_count && i < from + CHUNK; ++i)
                if (strcmp(b->commits[i].hash, line) == 0) {
                    ccr_blame_commit *c = &b->commits[i];
                    c->coauthor_count = ccr_git_coauthors(tab + 1, c->author, c->coauthors, CCR_GIT_COAUTHORS);
                    break;
                }
        }
        free(o);
    }
}

bool ccr_git_blame(const char *path, ccr_blame *out)
{
    memset(out, 0, sizeof *out);
    char real[PATH_MAX], dir[PATH_MAX];
    if (!realpath(path, real))
        return false;
    snprintf(dir, sizeof dir, "%s", real);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    *slash = '\0';
    const char *args[] = { "blame", "--porcelain", "--", real, NULL };
    int code = 0;
    char *o = ccr_git_run(dir, args, &code);
    if (!o || code != 0) {
        free(o);
        return false;
    }
    int cur = -1, cap = 0;
    for (char *line = o, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (line[0] == '\t') {   /* the line itself: it belongs to the commit above */
            if (out->line_count == cap) {
                cap = cap ? cap * 2 : 256;
                int *v = realloc(out->line_commit, (size_t)cap * sizeof *v);
                if (!v)
                    break;
                out->line_commit = v;
            }
            out->line_commit[out->line_count++] = cur;
            continue;
        }
        /* "<40 hex> <orig> <final> [<count>]" starts a line's header */
        size_t hl = strspn(line, "0123456789abcdef");
        if (hl == 40 && line[40] == ' ') {
            line[40] = '\0';
            cur = commit_index(out, line);
            continue;
        }
        if (cur < 0)
            continue;
        ccr_blame_commit *c = &out->commits[cur];
        if (strncmp(line, "author ", 7) == 0)
            snprintf(c->author, sizeof c->author, "%s", line + 7);
        else if (strncmp(line, "author-time ", 12) == 0)
            c->time = strtoll(line + 12, NULL, 10);
        else if (strncmp(line, "summary ", 8) == 0)
            snprintf(c->summary, sizeof c->summary, "%s", line + 8);
    }
    free(o);
    blame_coauthors(dir, out);
    return out->line_count > 0;
}

void ccr_git_blame_free(ccr_blame *b)
{
    free(b->commits);
    free(b->line_commit);
    memset(b, 0, sizeof *b);
}

void ccr_git_date(int64_t when, char *out, int cap)
{
    const time_t t = (time_t)when;
    struct tm tmv;
    if (cap <= 0)
        return;
    out[0] = '\0';
    if (localtime_r(&t, &tmv))
        strftime(out, (size_t)cap, "%d.%m.%Y %H:%M", &tmv);
}

void ccr_git_ago(int64_t when, char *out, int cap)
{
    const int64_t now = (int64_t)time(NULL);
    int64_t d = now - when;
    if (d < 0)
        d = 0;
    if (d < 60) {
        ccr_trf(out, (size_t)cap, CCR_T_AGO_NOW);
        return;
    }
    static const struct { int64_t secs; ccr_text t; } units[] = {
        { 365LL * 86400, CCR_T_AGO_YEARS }, { 30LL * 86400, CCR_T_AGO_MONTHS }, { 7LL * 86400, CCR_T_AGO_WEEKS },
        { 86400, CCR_T_AGO_DAYS }, { 3600, CCR_T_AGO_HOURS }, { 60, CCR_T_AGO_MINUTES },
    };
    for (size_t i = 0; i < sizeof units / sizeof units[0]; ++i)
        if (d >= units[i].secs) {
            const unsigned long n = (unsigned long)(d / units[i].secs);
            ccr_trnf(out, (size_t)cap, units[i].t, n, (unsigned)n);
            return;
        }
}

/* ------------------------------------------------------------ stash */

bool ccr_git_stash_push(const char *root, const char *message, char *err, int cap)
{
    char when[64] = "Ccratch";
    if (!message || !*message) {   /* named after when it was put aside */
        const time_t now = time(NULL);
        struct tm tmv;
        if (localtime_r(&now, &tmv))
            strftime(when, sizeof when, "%d.%m %H:%M", &tmv);
    }
    const char *args[] = { "stash", "push", "--include-untracked", "-m", message && *message ? message : when,
                           NULL };
    return run(root, args, err, cap);
}

int ccr_git_stash_list(const char *root, ccr_stash_entry *out, int cap)
{
    const char *args[] = { "stash", "list", "--format=%gs", NULL };
    int code = 0;
    char *o = ccr_git_run(root, args, &code);
    if (!o)
        return 0;
    int n = 0;
    for (char *line = o, *next; line && *line && n < cap; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        snprintf(out[n++].what, sizeof out[0].what, "%s", line);
    }
    free(o);
    return code == 0 ? n : 0;
}

bool ccr_git_stash_apply(const char *root, int index, bool pop, char *err, int cap)
{
    char ref[32];
    snprintf(ref, sizeof ref, "stash@{%d}", index);
    const char *args[] = { "stash", pop ? "pop" : "apply", ref, NULL };
    return run(root, args, err, cap);
}

bool ccr_git_stash_drop(const char *root, int index, char *err, int cap)
{
    char ref[32];
    snprintf(ref, sizeof ref, "stash@{%d}", index);
    const char *args[] = { "stash", "drop", ref, NULL };
    return run(root, args, err, cap);
}

/* ------------------------------------------------------------ merge */

static bool has_ref(const char *root, const char *ref)
{
    const char *args[] = { "rev-parse", "-q", "--verify", ref, NULL };
    int code = 1;
    free(ccr_git_run(root, args, &code));
    return code == 0;
}

ccr_git_op ccr_git_pending(const char *root)
{
    if (has_ref(root, "MERGE_HEAD"))
        return CCR_GIT_OP_MERGE;
    if (has_ref(root, "REVERT_HEAD"))
        return CCR_GIT_OP_REVERT;
    if (has_ref(root, "CHERRY_PICK_HEAD"))
        return CCR_GIT_OP_PICK;
    return CCR_GIT_OP_NONE;
}

bool ccr_git_merging(const char *root)
{
    return ccr_git_pending(root) != CCR_GIT_OP_NONE;
}

bool ccr_git_merge(const char *root, const char *branch, bool *conflicts, char *err, int cap)
{
    *conflicts = false;
    const char *args[] = { "merge", "--no-edit", branch, NULL };
    if (run(root, args, err, cap))
        return true;
    *conflicts = ccr_git_merging(root);
    return false;
}

bool ccr_git_merge_abort(const char *root, char *err, int cap)
{
    const ccr_git_op op = ccr_git_pending(root);
    const char *what = op == CCR_GIT_OP_REVERT ? "revert" : op == CCR_GIT_OP_PICK ? "cherry-pick" : "merge";
    const char *args[] = { what, "--abort", NULL };
    return run(root, args, err, cap);
}

char *ccr_git_resolve_text(const char *text, ccr_resolve how)
{
    const size_t n = strlen(text);
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    size_t k = 0;
    /* 0: outside; 1: ours (<<<<<<< to =======); 2: theirs (to >>>>>>>);
     * ours stays aside until theirs ends, for BOTH */
    int state = 0;
    size_t ours_b = 0, ours_e = 0;
    for (const char *line = text; *line;) {
        const char *e = strchr(line, '\n');
        const size_t len = e ? (size_t)(e - line) + 1 : strlen(line);
        if (state == 0 && strncmp(line, "<<<<<<<", 7) == 0) {
            state = 1;
            ours_b = (size_t)(line + len - text);
        } else if (state == 1 && strncmp(line, "|||||||", 7) == 0) {
            ours_e = (size_t)(line - text);
            state = 3;   /* the common ancestor (diff3): skipped */
        } else if ((state == 1 || state == 3) && strncmp(line, "=======", 7) == 0) {
            if (state == 1)
                ours_e = (size_t)(line - text);
            state = 2;
            if (how != CCR_RESOLVE_THEIRS) {   /* ours first */
                memcpy(out + k, text + ours_b, ours_e - ours_b);
                k += ours_e - ours_b;
            }
        } else if (state == 2 && strncmp(line, ">>>>>>>", 7) == 0) {
            state = 0;
        } else if (state == 2) {
            if (how != CCR_RESOLVE_OURS) {
                memcpy(out + k, line, len);
                k += len;
            }
        } else if (state == 0) {
            memcpy(out + k, line, len);
            k += len;
        }
        line += len;
    }
    out[k] = '\0';
    return out;
}

bool ccr_git_resolve(const char *root, const char *path, ccr_resolve how, char *err, int cap)
{
    char full[PATH_MAX + 1100];
    snprintf(full, sizeof full, "%s/%s", root, path);
    FILE *f = fopen(full, "rb");
    if (!f) {
        snprintf(err, (size_t)cap, "cannot read %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
    const bool read_ok = text && fread(text, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!read_ok) {
        free(text);
        snprintf(err, (size_t)cap, "cannot read %s", path);
        return false;
    }
    text[n] = '\0';
    char *done = ccr_git_resolve_text(text, how);
    free(text);
    f = done ? fopen(full, "wb") : NULL;
    const bool wrote = f && fwrite(done, 1, strlen(done), f) == strlen(done);
    if (f)
        fclose(f);
    free(done);
    if (!wrote) {
        snprintf(err, (size_t)cap, "cannot write %s", path);
        return false;
    }
    const char *args[] = { "add", "--", path, NULL };
    return run(root, args, err, cap);
}

bool ccr_git_merge_commit(const char *root, char *err, int cap)
{
    const char *args[] = { "commit", "--no-edit", "-q", NULL };
    return run(root, args, err, cap);
}
