/*
 * ccr_mi - gdb's machine interface (GDB/MI), read.
 *
 * gdb started with --interpreter=mi2 answers in lines like
 *
 *     12^done,bkpt={number="1",file="main.c",line="9"}
 *     *stopped,reason="breakpoint-hit",frame={func="main",line="9"}
 *     ~"Reading symbols...\n"
 *
 * A record: an optional number (the token of the command it answers), a
 * kind (^ result, * async exec, = notify, ~ @ & streams), a class (done,
 * stopped...) and a list of name=value, where a value is a "string", a
 * {tuple} or a [list].
 */
#ifndef CCR_MI_H
#define CCR_MI_H

#include <stdbool.h>

typedef enum ccr_mi_kind {
    CCR_MI_STRING,
    CCR_MI_TUPLE,
    CCR_MI_LIST
} ccr_mi_kind;

typedef struct ccr_mi_value {
    ccr_mi_kind  kind;
    const char  *name;     /* "" for list items without one */
    const char  *str;      /* CCR_MI_STRING: the text, escapes undone */
    struct ccr_mi_value *first, *next;
} ccr_mi_value;

typedef struct ccr_mi_record {
    long         token;    /* -1: none */
    char         kind;     /* '^' '*' '=' '~' '@' '&', or 0: not MI (the program's own output) */
    const char  *klass;    /* "done", "stopped", "running", "error"...; streams: the text */
    ccr_mi_value *results; /* a tuple of what follows the class */
    char        *arena;    /* everything above points in here */
} ccr_mi_record;

/* Reads one line. False for "(gdb)" and empty lines; a line that is not
 * MI comes back with kind 0 and the line in klass. */
bool ccr_mi_parse(const char *line, ccr_mi_record *out);
void ccr_mi_free(ccr_mi_record *r);

const ccr_mi_value *ccr_mi_get(const ccr_mi_value *tuple, const char *name);
/* The string `name` in `tuple`, or `fallback`. */
const char *ccr_mi_str(const ccr_mi_value *tuple, const char *name, const char *fallback);

/* `text` as an MI c-string ("a \"b\"\n" with quotes and escapes), for
 * commands that take a file name with spaces. */
void ccr_mi_quote(const char *text, char *out, int cap);

#endif /* CCR_MI_H */
