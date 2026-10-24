#include "ccr_asm.h"
#include "ccr_template.h"
#include "ccr_theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------ files */

static const char *ext_of(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    return dot && dot != base ? dot : "";
}

bool ccr_asm_is_asm(const char *path)
{
    const char *e = ext_of(path);
    return strcmp(e, ".s") == 0 || strcmp(e, ".S") == 0 || strcasecmp(e, ".asm") == 0 ||
           strcasecmp(e, ".nasm") == 0;
}

/* `word` as a directive of its own somewhere in the text (not in a comment
 * we care about: a guess is fine here). */
static bool mentions(const char *text, size_t len, const char *word)
{
    const size_t n = strlen(word);
    for (size_t i = 0; i + n <= len; ++i)
        if (strncasecmp(text + i, word, n) == 0 && (i == 0 || isspace((unsigned char)text[i - 1])))
            return true;
    return false;
}

static bool word_in(const char *w, const char *const *list)
{
    for (; *list; ++list)
        if (strcmp(w, *list) == 0)
            return true;
    return false;
}

/* x86 or ARM? The same .s says neither, so the words in it vote: its
 * registers (%rax, x0, r4, lr) and mnemonics only one of them has. */
static ccr_asm_syntax guess_arch(const char *text, size_t len)
{
    static const char *const x86[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "eax", "ebx", "ecx",
                                       "edx", "esi", "edi", "ebp", "esp", "movq", "movl", "movb", "pushq", "popq",
                                       "leaq", "leal", "addq", "addl", "subq", "subl", "cmpq", "cmpl", "syscall",
                                       "cltq", "cqto", "retq", "jmp", "jne", "je", "jle", "jg", "movzbl", NULL };
    static const char *const a64[] = { "xzr", "wzr", "adrp", "ldp", "stp", "cbz", "cbnz", "tbz", "tbnz", "movk",
                                       "movz", "csel", "csinc", "cset", "madd", "msub", "sxtw", "uxtw", "ldur",
                                       "stur", "fp", NULL };
    static const char *const a32[] = { "bx", "blx", "ldmfd", "stmfd", "ldmia", "stmia", "stmdb", ".syntax", ".thumb",
                                       ".thumb_func", ".arm", ".fpu", ".eabi_attribute", ".code", "lr", "pc", "ip",
                                       "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "bne", "beq", "ble", "blt",
                                       "bgt", "bge", "bhi", "bls", NULL };
    int vx = 0, v64 = 0, v32 = 0;
    for (size_t i = 0; i < len;) {
        const char c = text[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) {
            ++i;
            continue;
        }
        size_t k = i;
        char w[24];
        size_t n = 0;
        while (k < len && (isalnum((unsigned char)text[k]) || text[k] == '_' || text[k] == '.')) {
            if (n + 1 < sizeof w)
                w[n++] = (char)tolower((unsigned char)text[k]);
            ++k;
        }
        w[n] = '\0';
        const bool percent = i > 0 && text[i - 1] == '%';
        if (percent && isalpha((unsigned char)w[0]) && strcmp(w, "function") != 0 && strcmp(w, "progbits") != 0 &&
            strcmp(w, "object") != 0 && strcmp(w, "nobits") != 0)
            vx += 2;   /* %rax: AT&T */
        else if (word_in(w, x86))
            ++vx;
        else if (word_in(w, a64) || (n >= 2 && n <= 3 && (w[0] == 'x' || w[0] == 'w') &&
                                     isdigit((unsigned char)w[1]) && atoi(w + 1) <= 30 &&
                                     (n == 2 || isdigit((unsigned char)w[2]))) ||
                 strncmp(w, "b.", 2) == 0)
            ++v64;
        else if (word_in(w, a32))
            ++v32;
        i = k;
    }
    if (v64 == 0 && v32 == 0)
        return CCR_ASM_ATT;
    if (vx >= v64 + v32)
        return CCR_ASM_ATT;
    return v64 >= v32 ? CCR_ASM_ARM64 : CCR_ASM_ARM32;
}

ccr_asm_syntax ccr_asm_syntax_of(const char *path, const char *text, size_t len)
{
    const char *e = path ? ext_of(path) : "";
    if (strcasecmp(e, ".asm") == 0 || strcasecmp(e, ".nasm") == 0)
        return CCR_ASM_NASM;
    if (text && mentions(text, len, ".intel_syntax") && !mentions(text, len, ".att_syntax"))
        return CCR_ASM_INTEL;
    return text ? guess_arch(text, len) : CCR_ASM_ATT;
}

const char *ccr_asm_syntax_name(ccr_asm_syntax s)
{
    static const char *const names[] = { "att", "intel", "nasm", "arm64", "arm32" };
    return (unsigned)s < 5 ? names[s] : "att";
}

const char *ccr_asm_comment(ccr_asm_syntax s)
{
    return s == CCR_ASM_NASM ? "; " : s == CCR_ASM_ARM64 ? "// " : s == CCR_ASM_ARM32 ? "@ " : "# ";
}

/* ------------------------------------------------------------ words */

static void lower(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; s[n] && n + 1 < cap; ++n)
        out[n] = (char)tolower((unsigned char)s[n]);
    out[n] = '\0';
}

static bool in_list(const char *w, const char *const *list)
{
    for (; *list; ++list)
        if (strcmp(w, *list) == 0)
            return true;
    return false;
}

/* `w` is `base`, maybe with AT&T's size letter (movq, addl). */
static bool is_op(const char *w, const char *const *bases)
{
    if (in_list(w, bases))
        return true;
    const size_t n = strlen(w);
    if (n < 2 || !strchr("bwlq", w[n - 1]))
        return false;
    char b[32];
    if (n - 1 >= sizeof b)
        return false;
    memcpy(b, w, n - 1);
    b[n - 1] = '\0';
    return in_list(b, bases);
}

/* The last word: "rep movsb" -> "movsb". */
static const char *last_word(const char *m)
{
    const char *sp = strrchr(m, ' ');
    return sp ? sp + 1 : m;
}

/* ARM's condition codes: beq, b.ne, bhi... */
static bool arm_cond(const char *c)
{
    static const char *const conds[] = { "eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls",
                                         "ge", "lt", "gt", "le", "al", NULL };
    return in_list(c, conds);
}

/* Thumb's width: beq.n, b.w */
static void no_width(char *w)
{
    const size_t n = strlen(w);
    if (n > 2 && w[n - 2] == '.' && (w[n - 1] == 'n' || w[n - 1] == 'w'))
        w[n - 2] = '\0';
}

bool ccr_asm_is_jump(const char *mnemonic)
{
    char w[32];
    lower(last_word(mnemonic), w, sizeof w);
    static const char *const loops[] = { "loop", "loope", "loopne", "loopz", "loopnz", "jecxz", "jrcxz", "jcxz",
                                         "cbz", "cbnz", "tbz", "tbnz", NULL };
    if ((w[0] == 'j' && w[1] && isalpha((unsigned char)w[1])) || in_list(w, loops))
        return true;
    no_width(w);
    return strcmp(w, "b") == 0 || (w[0] == 'b' && w[1] == '.' && arm_cond(w + 2)) ||
           (w[0] == 'b' && strlen(w) == 3 && arm_cond(w + 1));
}

bool ccr_asm_ends_flow(const char *mnemonic)
{
    char w[32];
    lower(last_word(mnemonic), w, sizeof w);
    static const char *const ends[] = { "jmp", "jmpq", "jmpl", "ret", "retq", "retl", "retn", "retf", "iret",
                                        "iretq", "iretd", "ud2", "sysret", "sysretq", "sysexit",
                                        "b", "br", "bx", "eret", "udf", NULL };
    no_width(w);
    return in_list(w, ends);
}

static const char *const g_data[] = {
    ".byte", ".word", ".short", ".long", ".int", ".quad", ".octa", ".string", ".asciz", ".ascii", ".zero",
    ".space", ".skip", ".fill", ".float", ".single", ".double", ".2byte", ".4byte", ".8byte", ".string8",
    ".comm", ".lcomm", ".set", ".equ", ".equiv", "=", "db", "dw", "dd", "dq", "dt", "do", "dy", "dz", "resb",
    "resw", "resd", "resq", "rest", "reso", "resy", "resz", "equ", "times", "incbin", NULL,
};

int ccr_asm_category(const char *mnemonic)
{
    char w[48];
    lower(last_word(mnemonic), w, sizeof w);
    static const char *const nasm_dir[] = { "section", "segment", "global", "extern", "bits", "default", "align",
                                            "alignb", "org", "cpu", "common", "static", "use16", "use32",
                                            "use64", NULL };
    static const char *const control[] = { "call", "ret", "int", "int3", "into", "syscall", "sysenter",
                                           "sysexit", "sysret", "iret", "leave", "enter", "hlt", "ud2", NULL };
    static const char *const moves[] = { "mov", "movabs", "movzx", "movsx", "movsxd", "movzb", "movzw", "movsb",
                                         "movsw", "movsl", "movslq", "movzbl", "movzwl", "movzbq", "movsbl",
                                         "movsbq", "movswl", "movswq", "movzbw", "movsbw", "lea", "push", "pop",
                                         "pushf", "popf", "xchg", "cbw", "cwde", "cdqe", "cwd", "cdq", "cqo",
                                         "cltq", "cqto", "cltd", "bswap", "stos", "lods", "movd", "movq",
                                         "movaps", "movups", "movdqa", "movdqu", "movss", "movsd", "xadd",
                                         "cmpxchg", NULL };
    static const char *const maths[] = { "add", "sub", "adc", "sbb", "inc", "dec", "neg", "imul", "mul", "idiv",
                                         "div", "and", "or", "xor", "not", "shl", "shr", "sal", "sar", "rol",
                                         "ror", "rcl", "rcr", "cmp", "test", "bt", "bts", "btr", "btc", "popcnt",
                                         "lzcnt", "tzcnt", "bsf", "bsr", "shld", "shrd", "andn", NULL };
    if (w[0] == '#' || w[0] == '%')
        return CCR_CAT_PREPROC;
    /* ARM's: loads and stores move data; b, bl, cbz, svc are control */
    {
        char a[48];
        snprintf(a, sizeof a, "%s", w);
        no_width(a);
        static const char *const arm_ctl[] = { "b", "bl", "blx", "blr", "br", "bx", "cbz", "cbnz", "tbz", "tbnz",
                                               "svc", "swi", "eret", "udf", "wfi", "wfe", NULL };
        static const char *const arm_move[] = { "adr", "adrp", "movz", "movk", "movn", "mvn", "push", "pop", "sxtw",
                                                "uxtw", "sxtb", "uxtb", "sxth", "uxth", NULL };
        static const char *const arm_math[] = { "add", "adds", "sub", "subs", "mul", "madd", "msub", "sdiv", "udiv",
                                                "and", "ands", "orr", "eor", "bic", "lsl", "lsr", "asr", "ror", "cmp",
                                                "cmn", "tst", "teq", "neg", "negs", "rsb", "rsbs", "csel", "csinc",
                                                "cset", "cinc", "umull", "smull", "mla", "mls", NULL };
        if (in_list(a, arm_ctl) || ccr_asm_is_jump(a) || (a[0] == 'b' && a[1] == 'l' && arm_cond(a + 2)))
            return CCR_CAT_CONTROL;
        if (in_list(a, arm_move) || strncmp(a, "ldr", 3) == 0 || strncmp(a, "str", 3) == 0 ||
            strncmp(a, "ldp", 3) == 0 || strncmp(a, "stp", 3) == 0 || strncmp(a, "ldm", 3) == 0 ||
            strncmp(a, "stm", 3) == 0 || strncmp(a, "ldur", 4) == 0 || strncmp(a, "stur", 4) == 0)
            return CCR_CAT_VARIABLE;
        const size_t n = strlen(a);
        if (in_list(a, arm_math) || (n > 2 && arm_cond(a + n - 2) && (a[n - 2] = '\0', in_list(a, arm_math))))
            return CCR_CAT_OPERATOR;   /* addeq: add, if equal */
    }
    if (in_list(w, g_data) || (strcmp(w, "=") == 0))
        return CCR_CAT_TYPE;
    if (w[0] == '.' || w[0] == '[' || in_list(w, nasm_dir))
        return CCR_CAT_PREPROC;
    if (ccr_asm_is_jump(w) || is_op(w, control) || strncmp(w, "ret", 3) == 0 || strncmp(w, "iret", 4) == 0)
        return CCR_CAT_CONTROL;
    if (strncmp(w, "cmov", 4) == 0 || is_op(w, moves))
        return CCR_CAT_VARIABLE;
    if ((strncmp(w, "set", 3) == 0 && w[3]) || is_op(w, maths))
        return CCR_CAT_OPERATOR;
    return CCR_CAT_CALL;
}

/* --------------------------------------------------------- scanning */

typedef struct span { uint32_t b, e; } span;

enum { ST_LABEL, ST_INSN, ST_PRE };

typedef struct astmt {
    int      kind;
    span     all;     /* the whole statement */
    span     name;    /* label, mnemonic (prefixes and all), #include */
    span     label;   /* NASM's `msg db ...`, GNU's `x = 1`: msg, x */
    uint32_t arg0, nargs, line;
    bool     first;   /* first thing on its line */
} astmt;

typedef struct scan {
    const char    *s;
    uint32_t       n, i, line;
    ccr_asm_syntax syn;
    bool           cpp;
    astmt         *st;
    uint32_t       nst, st_cap;
    span          *args;
    uint32_t       narg, arg_cap;
    bool           oom;
} scan;

static bool grow(void **p, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need)
        c *= 2;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q)
        return false;
    *p = q;
    *cap = c;
    return true;
}

static bool hspace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
static bool nasm(const scan *S) { return S->syn == CCR_ASM_NASM; }

static bool word_char(const scan *S, char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == '@' || c == '?' ||
           (nasm(S) && (c == '#' || c == '~')) || (unsigned char)c >= 0x80;
}

static bool comment_at(const scan *S, uint32_t i)
{
    const char c = S->s[i];
    if (nasm(S))
        return c == ';';
    if (c == '/' && i + 1 < S->n && (S->s[i + 1] == '*' || S->s[i + 1] == '/'))
        return true;
    if (!CCR_ASM_IS_ARM(S->syn))
        return c == '#';
    if (c == '@')
        return S->syn == CCR_ASM_ARM32;   /* AArch64 has @function, not @ comments */
    if (c != '#')
        return false;
    /* ARM: # is a number (mov r0, #1), and a comment only first on its line */
    for (uint32_t k = i; k > 0 && S->s[k - 1] != '\n'; --k)
        if (!(S->s[k - 1] == ' ' || S->s[k - 1] == '\t' || S->s[k - 1] == '\r'))
            return false;
    return true;
}

/* Past a comment at S->i; a line comment stops before its '\n'. */
static void skip_comment(scan *S)
{
    if (!nasm(S) && S->s[S->i] == '/' && S->i + 1 < S->n && S->s[S->i + 1] == '*') {
        S->i += 2;
        while (S->i < S->n && !(S->s[S->i] == '*' && S->i + 1 < S->n && S->s[S->i + 1] == '/')) {
            S->line += S->s[S->i] == '\n';
            ++S->i;
        }
        S->i = S->i + 2 <= S->n ? S->i + 2 : S->n;
        return;
    }
    while (S->i < S->n && S->s[S->i] != '\n')
        ++S->i;
}

/* Past a "string" or 'c' at S->i (never past the line). */
static void skip_quote(scan *S)
{
    const char q = S->s[S->i++];
    if (q == '\'' && !nasm(S)) {   /* GNU: 'a is a character, the closing quote optional */
        if (S->i < S->n && S->s[S->i] == '\\')
            ++S->i;
        if (S->i < S->n && S->s[S->i] != '\n')
            ++S->i;
        if (S->i < S->n && S->s[S->i] == '\'')
            ++S->i;
        return;
    }
    while (S->i < S->n && S->s[S->i] != q && S->s[S->i] != '\n') {
        if (S->s[S->i] == '\\' && q != '\'' && S->i + 1 < S->n && S->s[S->i + 1] != '\n')
            ++S->i;
        ++S->i;
    }
    if (S->i < S->n && S->s[S->i] == q)
        ++S->i;
}

static bool line_end(const scan *S, uint32_t i)
{
    return i >= S->n || S->s[i] == '\n' || comment_at(S, i) || (!nasm(S) && S->s[i] == ';');
}

static void trim(const scan *S, uint32_t *b, uint32_t *e)
{
    while (*b < *e && hspace(S->s[*b]))
        ++*b;
    while (*e > *b && hspace(S->s[*e - 1]))
        --*e;
}

static void push_arg_keep(scan *S, uint32_t b, uint32_t e, bool keep_empty)
{
    trim(S, &b, &e);
    if (e <= b && !keep_empty)
        return;
    if (!grow((void **)&S->args, &S->arg_cap, S->narg + 1, sizeof *S->args)) {
        S->oom = true;
        return;
    }
    S->args[S->narg++] = (span){ b, e };
}

static void push_arg(scan *S, uint32_t b, uint32_t e) { push_arg_keep(S, b, e, false); }

/* Operands up to the statement's end, split at the commas outside ( ) [ ]. */
static void operands(scan *S)
{
    while (S->i < S->n && hspace(S->s[S->i]))
        ++S->i;
    uint32_t ab = S->i;
    int depth = 0;
    while (S->i < S->n && !line_end(S, S->i)) {
        const char c = S->s[S->i];
        if (c == '"' || c == '\'' || (c == '`' && nasm(S))) {
            skip_quote(S);
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if ((c == ')' || c == ']' || c == '}') && depth > 0)
            --depth;
        else if (c == ',' && depth == 0) {
            push_arg_keep(S, ab, S->i, true);   /* .p2align 4,,10: the empty one counts */
            ab = S->i + 1;
        }
        ++S->i;
    }
    push_arg(S, ab, S->i);
}

/* The rest of the line (and lines it continues with a \) as one operand. */
static void rest_of_line(scan *S)
{
    uint32_t b = S->i;
    while (S->i < S->n && S->s[S->i] != '\n') {
        if (S->s[S->i] == '\\' && S->i + 1 < S->n && S->s[S->i + 1] == '\n') {
            S->i += 2;
            ++S->line;
            continue;
        }
        ++S->i;
    }
    push_arg(S, b, S->i);
}

static uint32_t read_word(const scan *S, uint32_t i)
{
    while (i < S->n && word_char(S, S->s[i]))
        ++i;
    return i;
}

static bool span_is(const scan *S, span sp, const char *const *list)
{
    char w[48];
    const uint32_t n = sp.e - sp.b;
    if (n == 0 || n >= sizeof w)
        return false;
    for (uint32_t k = 0; k < n; ++k)
        w[k] = (char)tolower((unsigned char)S->s[sp.b + k]);
    w[n] = '\0';
    return in_list(w, list);
}

static bool push_stmt(scan *S, const astmt *t)
{
    if (!grow((void **)&S->st, &S->st_cap, S->nst + 1, sizeof *S->st)) {
        S->oom = true;
        return false;
    }
    S->st[S->nst++] = *t;
    return true;
}

static const char *const g_cpp_words[] = { "include", "define", "undef", "if", "ifdef", "ifndef", "elif", "else",
                                           "endif", "error", "warning", "pragma", "line", NULL };
static const char *const g_prefixes[] = { "rep", "repe", "repz", "repne", "repnz", "lock", "notrack", "bnd",
                                          "data16", "data32", "addr32", "rex64", "xacquire", "xrelease", NULL };
static const char *const g_nasm_data[] = { "db", "dw", "dd", "dq", "dt", "do", "dy", "dz", "resb", "resw", "resd",
                                           "resq", "rest", "reso", "resy", "resz", "equ", "times", "incbin",
                                           NULL };

/* One statement at S->i (not a space, comment or line end). */
static void statement(scan *S, bool first)
{
    astmt t = { .kind = ST_INSN, .line = S->line, .arg0 = S->narg, .first = first };
    const uint32_t b = S->i;
    t.all.b = b;
    const char c = S->s[b];

    if ((c == '#' && S->cpp && first) || (c == '%' && nasm(S))) {
        /* the preprocessor's: #include <x.h>, %define N 3 */
        uint32_t k = b + 1;
        while (k < S->n && hspace(S->s[k]))
            ++k;
        const uint32_t we = read_word(S, k);
        t.kind = ST_PRE;
        t.name = (span){ b, we };
        S->i = we;
        rest_of_line(S);
    } else if (c == '[' && nasm(S)) {   /* [bits 64], [section .text] */
        while (S->i < S->n && S->s[S->i] != ']' && S->s[S->i] != '\n')
            ++S->i;
        if (S->i < S->n && S->s[S->i] == ']')
            ++S->i;
        t.name = (span){ b, S->i };
    } else {
        uint32_t we = read_word(S, b);
        if (we == b) {   /* nothing we know: the statement as it is */
            while (S->i < S->n && !line_end(S, S->i))
                ++S->i;
            t.name = (span){ b, S->i };
            trim(S, &t.name.b, &t.name.e);
        } else {
            uint32_t k = we;
            while (k < S->n && hspace(S->s[k]))
                ++k;
            if (k < S->n && S->s[k] == ':' && !(k + 1 < S->n && S->s[k + 1] == ':')) {
                t.kind = ST_LABEL;
                t.name = (span){ b, we };
                t.all.e = k + 1;
                S->i = k + 1;
                push_stmt(S, &t);
                return;
            }
            if (!nasm(S) && k < S->n && S->s[k] == '=' && !(k + 1 < S->n && S->s[k + 1] == '=')) {
                t.label = (span){ b, we };   /* x = 1 */
                t.name = (span){ k, k + 1 };
                S->i = k + 1;
            } else {
                const uint32_t w2 = read_word(S, k);
                if (nasm(S) && w2 > k && span_is(S, (span){ k, w2 }, g_nasm_data) &&
                    !span_is(S, (span){ b, we }, g_nasm_data)) {
                    t.label = (span){ b, we };   /* msg db "hi" */
                    t.name = (span){ k, w2 };
                    S->i = w2;
                } else {
                    t.name = (span){ b, we };
                    S->i = we;
                    /* rep movsb, lock xadd: one mnemonic */
                    span last = t.name;
                    while (span_is(S, last, g_prefixes)) {
                        uint32_t q = S->i;
                        while (q < S->n && hspace(S->s[q]))
                            ++q;
                        const uint32_t qe = read_word(S, q);
                        if (qe == q)
                            break;
                        last = (span){ q, qe };
                        t.name.e = qe;
                        S->i = qe;
                    }
                }
            }
            operands(S);
        }
    }
    t.nargs = S->narg - t.arg0;
    t.all.e = S->i;
    trim(S, &t.all.b, &t.all.e);
    push_stmt(S, &t);
}

static void scan_all(scan *S)
{
    S->line = S->line ? S->line : 1;
    uint32_t line_start = S->i;
    while (S->i < S->n && !S->oom) {
        const char c = S->s[S->i];
        if (c == '\n') {
            ++S->line;
            line_start = ++S->i;
            continue;
        }
        if (hspace(c)) {
            ++S->i;
            continue;
        }
        bool first = true;
        for (uint32_t k = line_start; k < S->i; ++k)
            first &= hspace(S->s[k]);
        if (c == ';' && !nasm(S)) {   /* GNU: a statement separator */
            ++S->i;
            continue;
        }
        if (comment_at(S, S->i)) {
            bool directive = false;
            if (c == '#' && S->cpp && first && !nasm(S)) {
                uint32_t k = S->i + 1;
                while (k < S->n && hspace(S->s[k]))
                    ++k;
                directive = span_is(S, (span){ k, read_word(S, k) }, g_cpp_words);
            }
            if (!directive) {
                const uint32_t l0 = S->line;
                skip_comment(S);
                if (S->line != l0)
                    line_start = S->i;   /* a block comment over lines */
                continue;
            }
        }
        statement(S, first);
    }
}

/* -------------------------------------------------------- the tree */

typedef struct build {
    ccr_ast  *ast;
    scan     *S;
    /* labels of the file, and which of them start a hat */
    span     *labels;
    uint32_t  nlabels, label_cap;
    span     *globals;
    uint32_t  nglobals, glob_cap;
    bool      scopes;   /* .macro ... .endm as blocks with a mouth */
    bool      broken;   /* an end without its start: read again without */
} build;

static ccr_node_id node(build *B, ccr_node_kind kind, ccr_node_id parent, uint32_t b, uint32_t e, uint32_t line)
{
    ccr_node_id id = ccr_ast_add(B->ast, kind, parent);
    if (id == CCR_NONE)
        return CCR_NONE;
    ccr_node *n = &B->ast->nodes[id];
    n->src_b = n->place_b = n->ext_b = b;
    n->src_e = n->place_e = n->ext_e = e;
    n->line = line;
    uint32_t ls = b;
    while (ls > 0 && B->S->s[ls - 1] != '\n')
        --ls;
    n->column = b - ls + 1;
    return id;
}

static ccr_str sub(ccr_ast *a, const char *s, uint32_t b, uint32_t e)
{
    char buf[2048];
    const uint32_t n = e - b < sizeof buf - 1 ? e - b : (uint32_t)sizeof buf - 1;
    memcpy(buf, s + b, n);
    buf[n] = '\0';
    return ccr_ast_intern(a, buf);
}

static ccr_str bsub(build *B, span sp) { return sub(B->ast, B->S->s, sp.b, sp.e); }

static bool span_eq(const scan *S, span a, span b)
{
    return a.e - a.b == b.e - b.b && memcmp(S->s + a.b, S->s + b.b, a.e - a.b) == 0;
}

static bool span_eq_str(const scan *S, span a, const char *s)
{
    const size_t n = strlen(s);
    return a.e - a.b == n && memcmp(S->s + a.b, s, n) == 0;
}

static bool is_global(const build *B, span name)
{
    for (uint32_t i = 0; i < B->nglobals; ++i)
        if (span_eq(B->S, B->globals[i], name))
            return true;
    const uint32_t n = name.e - name.b;
    const char *s = B->S->s + name.b;
    return (n == 4 && memcmp(s, "main", 4) == 0) || (n == 6 && memcmp(s, "_start", 6) == 0);
}

static bool is_label(const build *B, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < B->nlabels; ++i)
        if (B->labels[i].e - B->labels[i].b == n && memcmp(B->S->s + B->labels[i].b, s, n) == 0)
            return true;
    return false;
}

/* The lower-case name of statement t (without spaces after # or %). */
static void name_of(const build *B, const astmt *t, char *out, size_t cap)
{
    size_t o = 0;
    for (uint32_t k = t->name.b; k < t->name.e && o + 1 < cap; ++k) {
        const char c = B->S->s[k];
        if (hspace(c) && o > 0 && (out[0] == '#' || out[0] == '%') && o == 1)
            continue;
        out[o++] = (char)tolower((unsigned char)c);
    }
    out[o] = '\0';
}

/* Global names: .globl x, global x:function, .type x, @function. */
static void find_globals(build *B)
{
    const scan *S = B->S;
    for (uint32_t i = 0; i < S->nst; ++i) {
        const astmt *t = &S->st[i];
        char w[32];
        name_of(B, t, w, sizeof w);
        if (t->kind == ST_LABEL) {
            if (grow((void **)&B->labels, &B->label_cap, B->nlabels + 1, sizeof *B->labels))
                B->labels[B->nlabels++] = t->name;
            continue;
        }
        const bool decl = strcmp(w, ".globl") == 0 || strcmp(w, ".global") == 0 || strcmp(w, "global") == 0 ||
                          strcmp(w, ".func") == 0 || strcmp(w, ".ent") == 0;
        const bool typed = strcmp(w, ".type") == 0 && t->nargs >= 2;
        if (!decl && !typed)
            continue;
        for (uint32_t a = 0; a < (typed ? 1 : t->nargs); ++a) {
            span sp = S->args[t->arg0 + a];
            if (typed) {
                const span kind = S->args[t->arg0 + 1];
                bool fn = false;
                for (uint32_t k = kind.b; k + 8 <= kind.e; ++k)
                    fn |= strncmp(S->s + k, "function", 8) == 0;
                if (!fn)
                    continue;
            }
            uint32_t e = sp.b;
            while (e < sp.e && word_char(S, S->s[e]))
                ++e;
            sp.e = e;   /* global main:function -> main */
            if (sp.e > sp.b &&
                grow((void **)&B->globals, &B->glob_cap, B->nglobals + 1, sizeof *B->globals))
                B->globals[B->nglobals++] = sp;
        }
    }
}

/* What kind of scope a statement opens or closes. */
typedef enum { SC_NONE, SC_OPEN, SC_OPEN_IF, SC_ELSE, SC_END } scope_role;

static scope_role role_of(const char *w)
{
    static const char *const opens[] = { ".macro", ".rept", ".irp", ".irpc", "%macro", "%imacro", "%rep", NULL };
    static const char *const ends[] = { ".endm", ".endmacro", ".endr", ".endif", "%endmacro", "%endrep", "%endr",
                                        "%endif", "#endif", NULL };
    if (in_list(w, opens))
        return SC_OPEN;
    if (in_list(w, ends))
        return SC_END;
    if (strcmp(w, ".else") == 0 || strcmp(w, "%else") == 0 || strcmp(w, "#else") == 0)
        return SC_ELSE;
    if (strncmp(w, ".if", 3) == 0 || strncmp(w, "%if", 3) == 0)
        return SC_OPEN_IF;
    if (strcmp(w, "#if") == 0 || strcmp(w, "#ifdef") == 0 || strcmp(w, "#ifndef") == 0)
        return SC_OPEN_IF;
    return SC_NONE;
}

const char *ccr_asm_end_word(const char *head)
{
    char w[32];
    lower(head, w, sizeof w);
    if (strcmp(w, ".macro") == 0) return ".endm";
    if (strcmp(w, ".rept") == 0 || strcmp(w, ".irp") == 0 || strcmp(w, ".irpc") == 0) return ".endr";
    if (strcmp(w, "%macro") == 0 || strcmp(w, "%imacro") == 0) return "%endmacro";
    if (strcmp(w, "%rep") == 0) return "%endrep";
    if (w[0] == '%') return "%endif";
    if (w[0] == '#') return "#endif";
    return ".endif";
}

const char *ccr_asm_else_word(const char *head)
{
    return head[0] == '%' ? "%else" : head[0] == '#' ? "#else" : ".else";
}

/* Does `end` close a scope opened by `head`? */
static bool closes(const char *head, const char *end)
{
    const char *want = ccr_asm_end_word(head);
    if (strcmp(want, end) == 0)
        return true;
    return (strcmp(head, ".macro") == 0 && strcmp(end, ".endmacro") == 0) ||
           (strcmp(head, "%rep") == 0 && strcmp(end, "%endr") == 0);
}

static bool is_section(const char *w)
{
    static const char *const s[] = { ".text", ".data", ".bss", ".section", "section", "segment", ".rodata", NULL };
    return in_list(w, s);
}

typedef struct frame {
    ccr_node_id node, body;
    char        head[32];
    bool        is_if;
} frame;

typedef struct hat {
    ccr_node_id fn, block;
    int         kind;    /* 0 file, 1 section, 2 label */
    bool        code;    /* holds more than directives */
    bool        inside;  /* a label hat whose .globl/.type/.p2align came first: they
                            and the label are in its stack, in the file's order */
    uint32_t    start;   /* where text for its first statement starts */
} hat;

static ccr_node_id one_text(build *B, ccr_node_id parent, const astmt *t)
{
    const scan *S = B->S;
    if (!t->nargs)
        return node(B, CCR_NODE_TEXT, parent, t->name.e, t->name.e, t->line);
    const span a = S->args[t->arg0], z = S->args[t->arg0 + t->nargs - 1];
    ccr_node_id x = node(B, CCR_NODE_TEXT, parent, a.b, z.e, t->line);
    if (x != CCR_NONE)
        B->ast->nodes[x].name = sub(B->ast, S->s, a.b, z.e);
    return x;
}

/* A statement as a node of `parent`. */
static ccr_node_id stmt_node(build *B, ccr_node_id parent, const astmt *t)
{
    ccr_ast *a = B->ast;
    const scan *S = B->S;
    ccr_node_id id;
    if (t->kind == ST_LABEL) {
        id = node(B, CCR_NODE_LABEL, parent, t->all.b, t->all.e, t->line);
        if (id != CCR_NONE)
            a->nodes[id].name = bsub(B, t->name);
    } else {
        char w[32];
        name_of(B, t, w, sizeof w);
        /* jne .L2: a jump to a label here gets its arrow */
        /* (cbz x0, .L2, tbz w1, #3, .L2: the label last, what is tested before it) */
        if (t->kind == ST_INSN && t->nargs >= 1 && t->nargs <= 3 && ccr_asm_is_jump(w)) {
            span g = S->args[t->arg0 + t->nargs - 1];
            static const char *const dist[] = { "short", "near", "far", NULL };
            uint32_t k = g.b;
            while (k < g.e && isalpha((unsigned char)S->s[k]))
                ++k;
            if (k < g.e && hspace(S->s[k]) && span_is(S, (span){ g.b, k }, dist)) {
                g.b = k;
                while (g.b < g.e && hspace(S->s[g.b]))
                    ++g.b;
            }
            /* (a jump to a global label is a tail call: another hat, no arrow) */
            if (read_word(S, g.b) == g.e && g.e > g.b && is_label(B, S->s + g.b, g.e - g.b) && !is_global(B, g)) {
                id = node(B, CCR_NODE_GOTO, parent, t->all.b, t->all.e, t->line);
                if (id != CCR_NONE) {
                    a->nodes[id].name = bsub(B, g);
                    a->nodes[id].op = bsub(B, t->name);
                    a->nodes[id].flags |= CCR_FLAG_ASM;
                    for (uint32_t q = 0; q + 1 < t->nargs; ++q) {
                        const span sp = S->args[t->arg0 + q];
                        ccr_node_id x = node(B, CCR_NODE_TEXT, id, sp.b, sp.e, t->line);
                        if (x != CCR_NONE)
                            a->nodes[x].name = bsub(B, sp);
                    }
                }
                return id;
            }
        }
        id = node(B, CCR_NODE_CALL, parent, t->all.b, t->all.e, t->line);
        if (id == CCR_NONE)
            return CCR_NONE;
        a->nodes[id].name = bsub(B, t->name);
        if (t->label.e > t->label.b)
            a->nodes[id].type = bsub(B, t->label);
        for (uint32_t k = 0; k < t->nargs; ++k) {
            const span sp = S->args[t->arg0 + k];
            ccr_node_id x = node(B, CCR_NODE_TEXT, id, sp.b, sp.e, t->line);
            if (x != CCR_NONE)
                a->nodes[x].name = bsub(B, sp);
        }
    }
    if (id != CCR_NONE)
        a->nodes[id].flags |= CCR_FLAG_ASM;
    return id;
}

/* ------------------------------------------------ ranges and indents */

static uint32_t line_start(const char *s, uint32_t at)
{
    while (at > 0 && s[at - 1] != '\n')
        --at;
    return at;
}

static bool alone_from_line_start(const char *s, uint32_t at)
{
    for (uint32_t k = line_start(s, at); k < at; ++k)
        if (!hspace(s[k]))
            return false;
    return true;
}

static uint32_t trailing_comment(scan *S, uint32_t at, uint32_t limit)
{
    uint32_t i = at;
    while (i < limit && hspace(S->s[i]))
        ++i;
    if (i >= limit || !comment_at(S, i))
        return at;
    const uint32_t save_i = S->i, save_line = S->line;
    S->i = i;
    skip_comment(S);
    const uint32_t e = S->i < limit ? S->i : limit;
    S->i = save_i;
    S->line = save_line;
    return e;
}

/* Gives a Block's statements their comments, blank lines and indents, and
 * the Block its range; `prev` is where text before its first statement
 * starts, `empty_at` where an empty one sits. */
static void finish_block(build *B, ccr_node_id blk, uint32_t prev, uint32_t empty_at, unsigned depth)
{
    ccr_ast *a = B->ast;
    const char *s = B->S->s;
    if (depth > 200)
        return;
    bool first = true;
    ccr_str indent = 0;
    for (ccr_node_id c = a->nodes[blk].first_child; c != CCR_NONE; c = a->nodes[c].next_sibling) {
        uint32_t j = prev, nl = 0;
        while (j < a->nodes[c].src_b && isspace((unsigned char)s[j])) {
            nl += s[j] == '\n';
            ++j;
        }
        a->nodes[c].ext_b = j;
        if (!first && nl >= 2)
            a->nodes[c].flags |= CCR_FLAG_BLANK_BEFORE;
        a->nodes[c].ext_e = trailing_comment(B->S, a->nodes[c].src_e, B->S->n);
        prev = a->nodes[c].ext_e;
        first = false;
        if (alone_from_line_start(s, a->nodes[c].src_b)) {
            const uint32_t ls = line_start(s, a->nodes[c].src_b);
            char ind[130];
            const uint32_t n = a->nodes[c].src_b - ls < 120 ? a->nodes[c].src_b - ls : 120;
            ind[0] = '|';   /* "|": known to be none (0 is "not known") */
            memcpy(ind + 1, s + ls, n);
            ind[n + 1] = '\0';
            a->nodes[c].extra_indent = ccr_ast_intern(a, ind);
            if (!indent && a->nodes[c].kind != CCR_NODE_LABEL && n > 0)
                indent = ccr_ast_intern(a, ind + 1);
        }
        /* the bodies of .macro, .if... */
        for (ccr_node_id k = a->nodes[c].first_child; k != CCR_NONE; k = a->nodes[k].next_sibling)
            if (a->nodes[k].kind == CCR_NODE_BLOCK)
                finish_block(B, k, a->nodes[k].src_b, a->nodes[k].src_b, depth + 1);
    }
    ccr_node *b = &a->nodes[blk];
    if (b->first_child != CCR_NONE) {
        uint32_t at = a->nodes[b->first_child].ext_b;
        if (alone_from_line_start(s, at))
            at = line_start(s, at);
        b = &a->nodes[blk];
        b->src_b = b->place_b = b->ext_b = at > empty_at ? at : empty_at;
        b->src_e = b->place_e = b->ext_e = a->nodes[b->last_child].ext_e;
    } else {
        b->src_b = b->place_b = b->ext_b = b->src_e = b->place_e = b->ext_e = empty_at;
    }
    b->indent = indent;
    b->close_indent = 0;
}

/* After the head statement of a scope: the end of its line. */
static uint32_t after_line(scan *S, const astmt *t)
{
    return trailing_comment(S, t->all.e, S->n);
}

/* Directives that only set things up (.globl, .type, section...): a hat
 * holding nothing else is not code yet. */
static bool setup_only(const char *w, int kind)
{
    static const char *const nasm_dir[] = { "global", "extern", "bits", "default", "align", "alignb", "cpu", "org",
                                            "section", "segment", NULL };
    if (kind == ST_PRE)
        return true;
    if (w[0] == '.')
        return !in_list(w, g_data);
    return w[0] == '[' || in_list(w, nasm_dir);
}

static bool build_tree(build *B, const char *name)
{
    ccr_ast *a = B->ast;
    scan *S = B->S;
    const ccr_node_id tu = node(B, CCR_NODE_TRANSLATION_UNIT, CCR_NONE, 0, S->n, 1);
    if (tu == CCR_NONE)
        return false;
    hat *hats = NULL;
    uint32_t nh = 0, hcap = 0;
    hat *h = NULL;
    frame st[64];
    int depth = 0;

    for (uint32_t i = 0; i < S->nst && !B->broken; ++i) {
        const astmt *t = &S->st[i];
        char w[32];
        name_of(B, t, w, sizeof w);
        const scope_role role = B->scopes && t->kind != ST_LABEL ? role_of(w) : SC_NONE;
        const bool global_label = depth == 0 && t->kind == ST_LABEL && is_global(B, t->name);
        const bool section = depth == 0 && t->kind == ST_INSN && is_section(w);

        if (section && h && h->kind != 2 && !h->code) {
            /* .text then .section .rodata, nothing between: one hat */
            char sec[64];
            const span sp = t->nargs ? S->args[t->arg0] : t->name;
            snprintf(sec, sizeof sec, "%.*s", (int)(sp.e - sp.b), S->s + sp.b);
            a->nodes[h->fn].name = ccr_ast_intern(a, sec);
            a->nodes[h->fn].type = ccr_ast_intern(a, "section");
            h->kind = 1;
        } else if (global_label || section || !h) {
            /* clang puts `.globl g`, `.type g`, `.p2align` at the end of the
             * function before g: they are g's, so they go with it */
            ccr_node_id run[16];
            int nrun = 0;
            if (global_label && h) {
                static const char *const named[] = { ".globl", ".global", ".type", ".weak", ".hidden", ".protected",
                                                     ".func", NULL };
                static const char *const align[] = { ".p2align", ".align", ".balign", NULL };
                int kids = 0;
                for (ccr_node_id c = a->nodes[h->block].first_child; c != CCR_NONE; c = a->nodes[c].next_sibling)
                    ++kids;
                for (ccr_node_id c = a->nodes[h->block].last_child; c != CCR_NONE && nrun < 16 && nrun + 1 < kids;) {
                    const ccr_node *k = &a->nodes[c];
                    char cw[32];
                    lower(ccr_ast_str(a, k->name), cw, sizeof cw);
                    const ccr_node_id arg = k->first_child;
                    const bool mine = k->kind == CCR_NODE_CALL && arg != CCR_NONE && in_list(cw, named) &&
                                      span_eq_str(S, t->name, ccr_ast_str(a, a->nodes[arg].name));
                    if (!mine && !(k->kind == CCR_NODE_CALL && in_list(cw, align)))
                        break;
                    run[nrun++] = c;
                    /* the one before it */
                    ccr_node_id before = CCR_NONE;
                    for (ccr_node_id q = a->nodes[h->block].first_child; q != c; q = a->nodes[q].next_sibling)
                        before = q;
                    c = before;
                }
                while (nrun && a->nodes[run[nrun - 1]].kind == CCR_NODE_CALL) {   /* alignment alone stays */
                    char cw[32];
                    lower(ccr_ast_str(a, a->nodes[run[nrun - 1]].name), cw, sizeof cw);
                    if (!in_list(cw, (const char *const[]){ ".p2align", ".align", ".balign", NULL }))
                        break;
                    --nrun;
                }
                bool any_named = false;
                for (int q = 0; q < nrun; ++q) {
                    char cw[32];
                    lower(ccr_ast_str(a, a->nodes[run[q]].name), cw, sizeof cw);
                    any_named |= !in_list(cw, (const char *const[]){ ".p2align", ".align", ".balign", NULL });
                }
                if (!any_named)
                    nrun = 0;
                /* a hat of `.text` and then main's: gcc's way, the hat says it already */
                if (nrun && nrun + 1 == kids && h->kind == 1)
                    nrun = 0;
            }
            if (!grow((void **)&hats, &hcap, nh + 1, sizeof *hats))
                goto oom;
            h = &hats[nh++];
            memset(h, 0, sizeof *h);
            h->kind = global_label ? 2 : section ? 1 : 0;
            const uint32_t at = global_label ? t->all.b : t->all.b;
            h->fn = node(B, CCR_NODE_FUNCTION, tu, at, global_label ? t->all.e : at, t->line);
            if (h->fn == CCR_NONE)
                goto oom;
            a->nodes[h->fn].flags |= CCR_FLAG_DEFINITION;
            if (global_label) {
                a->nodes[h->fn].name = bsub(B, t->name);
            } else if (section) {
                const span sp = t->nargs ? S->args[t->arg0] : t->name;
                a->nodes[h->fn].name = bsub(B, sp);
            } else {
                a->nodes[h->fn].name = ccr_ast_intern(a, name);
            }
            a->nodes[h->fn].type = ccr_ast_intern(a, h->kind == 2 ? "label" : h->kind == 1 ? "section" : "file");
            h->block = node(B, CCR_NODE_BLOCK, h->fn, t->all.e, t->all.e, t->line);
            if (h->block == CCR_NONE)
                goto oom;
            if (nrun) {
                /* its directives first, then the label itself, in the file's order */
                h->inside = true;
                a->nodes[h->fn].type = ccr_ast_intern(a, "function");
                for (int q = nrun - 1; q >= 0; --q) {
                    ccr_ast_unlink(a, run[q]);
                    ccr_ast_append(a, h->block, run[q]);
                }
                if (stmt_node(B, h->block, t) == CCR_NONE)
                    goto oom;
                continue;
            }
            if (global_label)
                continue;   /* the label is the hat's own text */
        }
        if (!(t->kind == ST_LABEL ? false : setup_only(w, t->kind)))
            h->code = true;

        const ccr_node_id parent = depth ? st[depth - 1].body : h->block;
        if (role == SC_OPEN || role == SC_OPEN_IF) {
            if (depth == 64) {
                B->broken = true;
                break;
            }
            const ccr_node_id s = node(B, role == SC_OPEN_IF ? CCR_NODE_IF : CCR_NODE_SCOPE, parent, t->all.b,
                                       t->all.e, t->line);
            if (s == CCR_NONE)
                goto oom;
            a->nodes[s].name = bsub(B, t->name);
            a->nodes[s].flags |= CCR_FLAG_ASM;
            if (one_text(B, s, t) == CCR_NONE)
                goto oom;
            const uint32_t at = after_line(S, t);
            frame *f = &st[depth++];
            f->node = s;
            f->is_if = role == SC_OPEN_IF;
            snprintf(f->head, sizeof f->head, "%s", w);
            f->body = node(B, CCR_NODE_BLOCK, s, at, at, t->line);
            if (f->body == CCR_NONE)
                goto oom;
            continue;
        }
        if (role == SC_ELSE) {
            frame *f = depth ? &st[depth - 1] : NULL;
            int kids = 0;
            for (ccr_node_id c = f ? a->nodes[f->node].first_child : CCR_NONE; c != CCR_NONE;
                 c = a->nodes[c].next_sibling)
                ++kids;
            if (!f || !f->is_if || kids != 2) {
                B->broken = true;
                break;
            }
            const uint32_t at = after_line(S, t);
            a->nodes[f->body].src_e = a->nodes[f->body].place_e = t->all.b;   /* until finish_block */
            f->body = node(B, CCR_NODE_BLOCK, f->node, at, at, t->line);
            if (f->body == CCR_NONE)
                goto oom;
            continue;
        }
        if (role == SC_END) {
            frame *f = depth ? &st[depth - 1] : NULL;
            if (!f || !closes(f->head, w)) {
                B->broken = true;
                break;
            }
            ccr_node *s = &a->nodes[f->node];
            s->src_e = s->place_e = s->ext_e = t->all.e;
            --depth;
            continue;
        }
        if (stmt_node(B, parent, t) == CCR_NONE)
            goto oom;
    }
    if (depth)
        B->broken = true;
    if (B->broken) {
        free(hats);
        return true;   /* read again, without scopes */
    }
    if (nh == 0) {   /* nothing but comments: a hat to start from */
        ccr_node_id fn = node(B, CCR_NODE_FUNCTION, tu, 0, S->n, 1);
        if (fn == CCR_NONE)
            goto oom;
        a->nodes[fn].flags |= CCR_FLAG_DEFINITION;
        a->nodes[fn].name = ccr_ast_intern(a, name);
        a->nodes[fn].type = ccr_ast_intern(a, "file");
        if (node(B, CCR_NODE_BLOCK, fn, S->n, S->n, 1) == CCR_NONE)
            goto oom;
        free(hats);
        return true;
    }

    /* each hat's range: where its text starts to its last statement */
    uint32_t prev = 0;
    for (uint32_t k = 0; k < nh; ++k) {
        ccr_node *fn = &a->nodes[hats[k].fn];
        if (hats[k].kind == 2 && !hats[k].inside) {
            const uint32_t label_e = fn->src_e;
            finish_block(B, hats[k].block, label_e, label_e, 0);
            fn = &a->nodes[hats[k].fn];
            const ccr_node *bl = &a->nodes[hats[k].block];
            fn->src_e = fn->place_e = fn->ext_e = bl->src_e > label_e ? bl->src_e : label_e;
        } else {
            finish_block(B, hats[k].block, prev, prev, 0);
            fn = &a->nodes[hats[k].fn];
            const ccr_node *bl = &a->nodes[hats[k].block];
            fn->src_b = fn->place_b = fn->ext_b = k == 0 ? 0 : bl->src_b;
            fn->src_e = fn->place_e = fn->ext_e = bl->src_e;
        }
        prev = fn->src_e;
    }
    free(hats);
    return true;
oom:
    free(hats);
    return false;
}

static bool fail_diag(ccr_ast *out, const char *msg, uint32_t line)
{
    ccr_diag d = { .severity = CCR_DIAG_ERROR, .file = out->source_path, .line = line, .column = 1,
                   .message = ccr_ast_intern(out, msg) };
    ccr_ast_add_diag(out, d);
    return false;
}

/* Back to an empty tree (strings and source kept). */
static void drop_nodes(ccr_ast *a)
{
    a->node_count = 0;
}

bool ccr_asm_parse_text(const char *src, size_t len, const char *name, ccr_asm_syntax syntax, bool cpp,
                        ccr_ast *out)
{
    out->lang = CCR_LANG_ASM;
    out->asm_syntax = syntax;
    if (len >= UINT32_MAX / 2)
        return fail_diag(out, "file too big", 0);
    out->source = malloc(len + 1);
    if (!out->source)
        return false;
    memcpy(out->source, src, len);
    out->source[len] = '\0';
    out->source_len = (uint32_t)len;

    scan S = { .s = out->source, .n = (uint32_t)len, .syn = syntax, .cpp = cpp };
    scan_all(&S);
    bool ok = !S.oom;
    build B = { .ast = out, .S = &S, .scopes = true };
    if (ok) {
        find_globals(&B);
        ok = build_tree(&B, name);
        if (ok && B.broken) {   /* an .endm without its .macro...: no mouths, then */
            drop_nodes(out);
            B.broken = false;
            B.scopes = false;
            ok = build_tree(&B, name);
        }
    }
    free(S.st);
    free(S.args);
    free(B.labels);
    free(B.globals);
    if (!ok)
        return fail_diag(out, "out of memory", 0);
    return true;
}

bool ccr_asm_parse_file(const char *path, ccr_ast *out)
{
    out->lang = CCR_LANG_ASM;
    out->source_path = ccr_ast_intern(out, path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail_diag(out, "cannot open the file", 0);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 16384;
            char *b = realloc(buf, cap);
            if (!b) {
                free(buf);
                fclose(f);
                return false;
            }
            buf = b;
        }
        const size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0)
            break;
    }
    fclose(f);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *e = ext_of(path);
    const bool ok = ccr_asm_parse_text(buf ? buf : "", len, base, ccr_asm_syntax_of(path, buf, len),
                                       strcmp(e, ".S") == 0, out);
    free(buf);
    return ok;
}

bool ccr_asm_parse_into(ccr_ast *ast, ccr_node_id block, uint32_t b, uint32_t e, ccr_asm_syntax syntax)
{
    scan S = { .s = ast->source, .i = b, .n = e, .syn = syntax };
    scan_all(&S);
    build B = { .ast = ast, .S = &S };
    bool ok = !S.oom;
    if (ok) {
        find_globals(&B);
        for (uint32_t i = 0; i < S.nst && ok; ++i)
            ok = stmt_node(&B, block, &S.st[i]) != CCR_NONE;
        if (ok)
            finish_block(&B, block, b, b, 0);
    }
    free(S.st);
    free(S.args);
    free(B.labels);
    free(B.globals);
    return ok;
}

/* ------------------------------------------------------ new blocks */

ccr_node_id ccr_asm_build(ccr_ast *ast, ccr_node_id parent, const char *line, ccr_asm_syntax syntax)
{
    scan S = { .s = line, .n = (uint32_t)strlen(line), .syn = syntax, .cpp = true };
    scan_all(&S);
    ccr_node_id id = CCR_NONE;
    if (!S.oom && S.nst >= 1) {
        const astmt *t = &S.st[0];
        if (t->kind == ST_LABEL) {
            id = ccr_asm_build_label(ast, parent);
            if (id != CCR_NONE) {
                const ccr_str nm = sub(ast, line, t->name.b, t->name.e);
                ast->nodes[id].name = nm;
                ast->nodes[ast->nodes[id].first_child].name = nm;
            }
        } else {
            id = ccr_template_node(ast, CCR_NODE_CALL, parent);
            if (id != CCR_NONE) {
                ast->nodes[id].flags |= CCR_FLAG_ASM;
                ast->nodes[id].name = sub(ast, line, t->name.b, t->name.e);
                if (t->label.e > t->label.b)
                    ast->nodes[id].type = sub(ast, line, t->label.b, t->label.e);
                for (uint32_t k = 0; k < t->nargs; ++k) {
                    char buf[512];
                    const span sp = S.args[t->arg0 + k];
                    snprintf(buf, sizeof buf, "%.*s", (int)(sp.e - sp.b), line + sp.b);
                    ccr_template_text(ast, id, buf);
                }
            }
        }
    }
    free(S.st);
    free(S.args);
    return id;
}

ccr_node_id ccr_asm_build_scope(ccr_ast *ast, ccr_node_id parent, const char *head, const char *args,
                                bool with_else)
{
    const bool is_if = role_of(head) == SC_OPEN_IF;
    ccr_node_id s = ccr_template_node(ast, is_if ? CCR_NODE_IF : CCR_NODE_SCOPE, parent);
    if (s == CCR_NONE)
        return CCR_NONE;
    ast->nodes[s].flags |= CCR_FLAG_ASM;
    ast->nodes[s].name = ccr_ast_intern(ast, head);
    ccr_template_text(ast, s, args);
    ccr_template_node(ast, CCR_NODE_BLOCK, s);
    if (is_if && with_else)
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
    return s;
}

static ccr_node_id named(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent, const char *word)
{
    ccr_node_id s = ccr_template_node(ast, kind, parent);
    if (s == CCR_NONE)
        return CCR_NONE;
    ast->nodes[s].flags |= CCR_FLAG_ASM;
    ccr_node_id w = ccr_template_text(ast, s, word);
    if (w != CCR_NONE) {
        ast->nodes[w].flags |= CCR_FLAG_WORD_NAME;
        ast->nodes[s].name = ast->nodes[w].name;
    }
    return s;
}

ccr_node_id ccr_asm_build_jump(ccr_ast *ast, ccr_node_id parent, const char *mnemonic)
{
    ccr_node_id s = named(ast, CCR_NODE_GOTO, parent, ".Lloop");
    if (s != CCR_NONE)
        ast->nodes[s].op = ccr_ast_intern(ast, mnemonic);
    return s;
}

ccr_node_id ccr_asm_build_label(ccr_ast *ast, ccr_node_id parent)
{
    return named(ast, CCR_NODE_LABEL, parent, ".Lloop");
}

ccr_node_id ccr_asm_build_inline(ccr_ast *ast, ccr_node_id parent)
{
    ccr_node_id s = ccr_template_node(ast, CCR_NODE_ASM, parent);
    if (s == CCR_NONE)
        return CCR_NONE;
    ast->nodes[s].name = ccr_ast_intern(ast, "__asm__");
    ast->nodes[s].op = ccr_ast_intern(ast, "volatile");
    ccr_template_node(ast, CCR_NODE_BLOCK, s);
    ccr_template_text(ast, s, "");   /* outputs: "=r"(x) */
    ccr_template_text(ast, s, "");   /* inputs */
    ccr_template_text(ast, s, "");   /* clobbers: "cc", "memory" */
    return s;
}

/* ------------------------------------------------------- asm() in C */

static uint32_t skip_c_space(const char *s, uint32_t i, uint32_t n)
{
    for (;;) {
        while (i < n && isspace((unsigned char)s[i]))
            ++i;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            i = i + 2 <= n ? i + 2 : n;
            continue;
        }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
            while (i < n && s[i] != '\n')
                ++i;
            continue;
        }
        return i;
    }
}

/* Past a C string literal at s[i] (with its L/u8 prefix done by the caller). */
static uint32_t skip_c_string(const char *s, uint32_t i, uint32_t n)
{
    ++i;
    while (i < n && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < n)
            ++i;
        ++i;
    }
    return i < n ? i + 1 : n;
}

static uint32_t c_word(const char *s, uint32_t i, uint32_t n)
{
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_'))
        ++i;
    return i;
}

bool ccr_asm_parts_of(const char *s, uint32_t n, ccr_asm_parts *p)
{
    memset(p, 0, sizeof *p);
    uint32_t i = skip_c_space(s, 0, n);
    uint32_t we = c_word(s, i, n);
    if (we == i)
        return false;
    p->keyword_e = we;
    i = skip_c_space(s, we, n);
    p->quals_b = i;
    p->quals_e = i;
    for (;;) {
        we = c_word(s, i, n);
        if (we == i)
            break;
        p->quals_e = we;
        i = skip_c_space(s, we, n);
    }
    if (i >= n || s[i] != '(')
        return false;
    i = skip_c_space(s, i + 1, n);
    if (i >= n || s[i] != '"')
        return false;
    p->tmpl_b = i;
    while (i < n && s[i] == '"') {
        i = skip_c_string(s, i, n);
        p->tmpl_e = i;
        i = skip_c_space(s, i, n);
    }
    /* : outputs : inputs : clobbers : labels */
    int depth = 0, section = -1;
    uint32_t op_b = 0;
    bool in_op = false;
    while (i < n) {
        const char c = s[i];
        if (c == '"') {
            const uint32_t e = skip_c_string(s, i, n);
            if (section >= 0 && section < 2 && depth == 0 && !in_op) {
                op_b = i;
                in_op = true;
            }
            i = e;
            continue;
        }
        if (c == '[' && depth == 0 && section >= 0 && section < 2 && !in_op) {   /* [name] "=r"(x) */
            op_b = i;
            in_op = true;
        }
        if (c == '(') {
            if (depth == 0 && in_op && p->op_count < CCR_ASM_MAX_OPERANDS) {
                ccr_asm_operand *o = &p->ops[p->op_count];
                o->cb = op_b;
                uint32_t ce = i;
                while (ce > op_b && isspace((unsigned char)s[ce - 1]))
                    --ce;
                o->ce = ce;
                o->eb = i + 1;
            }
            ++depth;
        } else if (c == ')') {
            if (depth == 0) {   /* the end of asm( ... ) */
                if (section >= 0)
                    p->sec_e[section] = i;
                p->sections = section + 1;
                return true;
            }
            if (--depth == 0 && in_op) {
                if (p->op_count < CCR_ASM_MAX_OPERANDS) {
                    p->ops[p->op_count].ee = i;
                    p->op_section[p->op_count] = section;
                    ++p->op_count;
                }
                in_op = false;
            }
        } else if (c == ':' && depth == 0) {
            if (section >= 0)
                p->sec_e[section] = i;
            if (++section >= 4)
                return false;
            p->sec_b[section] = i + 1;
        }
        ++i;
    }
    return false;
}

int ccr_asm_decode(const char *s, uint32_t b, uint32_t e, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t i = b;
    while (i < e) {
        i = skip_c_space(s, i, e);
        if (i >= e)
            break;
        if (s[i] != '"')
            return -1;
        ++i;
        while (i < e && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < e) {
                const char k = s[i++];
                switch (k) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '\'': c = '\''; break;
                case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                    int v = k - '0', d = 1;
                    while (d < 3 && i < e && s[i] >= '0' && s[i] <= '7') {
                        v = v * 8 + (s[i++] - '0');
                        ++d;
                    }
                    c = (char)v;
                    break;
                }
                default:
                    return -1;   /* \x.. and friends: left as they are */
                }
            }
            if (o + 1 >= cap)
                return -1;
            out[o++] = c;
        }
        if (i >= e)
            return -1;
        ++i;
    }
    out[o] = '\0';
    return (int)o;
}

void ccr_asm_escape(const char *line, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < cap; ++i) {
        const char c = line[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

void ccr_asm_split_inline(ccr_ast *ast)
{
    const uint32_t count = ast->node_count;
    for (uint32_t id = 0; id < count; ++id) {
        if (ast->nodes[id].kind != CCR_NODE_ASM)
            continue;
        const ccr_node n = ast->nodes[id];
        if (n.src_e <= n.src_b || n.src_e > ast->source_len)
            continue;
        ccr_asm_parts p;
        if (!ccr_asm_parts_of(ast->source + n.src_b, n.src_e - n.src_b, &p))
            continue;
        static char text[65536];
        const int len = ccr_asm_decode(ast->source, n.src_b + p.tmpl_b, n.src_b + p.tmpl_e, text, sizeof text);
        if (len < 0)
            continue;
        ast->nodes[id].name = sub(ast, ast->source, n.src_b, n.src_b + p.keyword_e);
        if (p.quals_e > p.quals_b)
            ast->nodes[id].op = sub(ast, ast->source, n.src_b + p.quals_b, n.src_b + p.quals_e);
        const uint32_t base = ccr_ast_append_source(ast, text, (uint32_t)len);
        if (base == UINT32_MAX)
            return;
        const ccr_node_id blk = ccr_ast_add(ast, CCR_NODE_BLOCK, CCR_NONE);
        if (blk == CCR_NONE)
            return;
        ccr_ast_insert_first(ast, id, blk);
        /* asm() for an ARM target: its syntax, from what it says */
        const ccr_asm_syntax own = guess_arch(text, (size_t)len);
        if (!ccr_asm_parse_into(ast, blk, base, base + (uint32_t)len,
                                CCR_ASM_IS_ARM(own) ? own : ast->asm_syntax))
            return;
        /* the Block stands for the string(s) in the C text */
        ccr_node *b = &ast->nodes[blk];
        b->src_b = b->place_b = b->ext_b = n.src_b + p.tmpl_b;
        b->src_e = b->place_e = b->ext_e = n.src_b + p.tmpl_e;
        b->line = n.line;
        /* which line each instruction is on, for messages: the line of the
         * "string" its text came from (comments between them and all) */
        uint32_t lit_b[256], lit_out[256], nlit = 0, out = 0;
        for (uint32_t i = n.src_b + p.tmpl_b; i < n.src_b + p.tmpl_e && nlit < 256;) {
            i = skip_c_space(ast->source, i, n.src_b + p.tmpl_e);
            if (i >= n.src_b + p.tmpl_e || ast->source[i] != '"')
                break;
            const uint32_t e = skip_c_string(ast->source, i, n.src_b + p.tmpl_e);
            static char piece[65536];
            const int pl = ccr_asm_decode(ast->source, i, e, piece, sizeof piece);
            lit_b[nlit] = i;
            lit_out[nlit++] = out;
            out += pl > 0 ? (uint32_t)pl : 0;
            i = e;
        }
        for (ccr_node_id c = b->first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling) {
            const uint32_t at = ast->nodes[c].src_b - base;   /* where in the joined text */
            uint32_t lit = 0;
            while (lit + 1 < nlit && lit_out[lit + 1] <= at)
                ++lit;
            uint32_t l = 0;
            const uint32_t upto = nlit ? lit_b[lit] : n.src_b;
            for (uint32_t k = n.src_b; k < upto; ++k)
                l += ast->source[k] == '\n';
            ast->nodes[c].line = n.line + l;
        }
    }
}
