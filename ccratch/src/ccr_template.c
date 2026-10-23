#include "ccr_template.h"
#include "ccr_asm.h"
#include "ccr_cmake.h"
#include "ccr_lib.h"

#include <stdio.h>

#include <string.h>

static const char *const g_labels[CCR_TEMPLATE_COUNT] = {
#define X(id, cat, label) label,
    CCR_TEMPLATES(X)
#undef X
};

static const ccr_tcat g_cats[CCR_TEMPLATE_COUNT] = {
#define X(id, cat, label) CCR_TCAT_##cat,
    CCR_TEMPLATES(X)
#undef X
};

int ccr_template_total(void)
{
    return CCR_TEMPLATE_COUNT + ccr_lib_block_count() + 2 * ccr_lib_pack_count();
}

bool ccr_tcat_is_cmake(ccr_tcat c)
{
    return c >= CCR_TCAT_CM_PROJECT && c <= CCR_TCAT_CM_CONTROL;
}

ccr_lang_kind ccr_tcat_lang(ccr_tcat c)
{
    if (ccr_tcat_is_cmake(c))
        return CCR_LANG_CMAKE;
    if (c >= CCR_TCAT_ASM_MOVE && c < CCR_TCAT_COUNT)
        return CCR_LANG_ASM;
    return CCR_LANG_C;
}

/* Each assembly block, the three ways it is written. */
static const struct asm_line {
    ccr_template t;
    const char  *intel, *att, *nasm, *arm64, *arm32;
} g_asm_lines[] = {
    { CCR_TEMPLATE_INL_MOV,   "mov eax, ebx",        "movl %ebx, %eax",     "mov eax, ebx", "mov x0, x1", "mov r0, r1" },
    { CCR_TEMPLATE_INL_ADD,   "add eax, 1",          "addl $1, %eax",       "add eax, 1", "add x0, x0, #1", "add r0, r0, #1" },
    { CCR_TEMPLATE_INL_XOR,   "xor eax, eax",        "xorl %eax, %eax",     "xor eax, eax", "eor x0, x0, x0", "eor r0, r0, r0" },
    { CCR_TEMPLATE_INL_NOP,   "nop",                 "nop",                 "nop", "nop", "nop" },
    { CCR_TEMPLATE_INL_PAUSE, "pause",               "pause",               "pause", "yield", "yield" },
    { CCR_TEMPLATE_A_MOV,     "mov rax, rbx",        "movq %rbx, %rax",     "mov rax, rbx", "mov x0, x1", "mov r0, r1" },
    { CCR_TEMPLATE_A_MOV_IMM, "mov rax, 1",          "movq $1, %rax",       "mov rax, 1", "mov x0, #1", "mov r0, #1" },
    { CCR_TEMPLATE_A_LOAD,    "mov eax, DWORD PTR [rdi]", "movl (%rdi), %eax", "mov eax, [rdi]", "ldr w0, [x1]", "ldr r0, [r1]" },
    { CCR_TEMPLATE_A_STORE,   "mov DWORD PTR [rdi], eax", "movl %eax, (%rdi)", "mov [rdi], eax", "str w0, [x1]", "str r0, [r1]" },
    { CCR_TEMPLATE_A_LEA,     "lea rsi, [rip + msg]", "leaq msg(%rip), %rsi", "lea rsi, [rel msg]", "adr x0, msg", "adr r0, msg" },
    { CCR_TEMPLATE_A_PUSH,    "push rbp",            "pushq %rbp",          "push rbp", "stp x29, x30, [sp, #-16]!", "push {r4, lr}" },
    { CCR_TEMPLATE_A_POP,     "pop rbp",             "popq %rbp",           "pop rbp", "ldp x29, x30, [sp], #16", "pop {r4, pc}" },
    { CCR_TEMPLATE_A_ADD,     "add rax, rbx",        "addq %rbx, %rax",     "add rax, rbx", "add x0, x0, x1", "add r0, r0, r1" },
    { CCR_TEMPLATE_A_SUB,     "sub rsp, 16",         "subq $16, %rsp",      "sub rsp, 16", "sub sp, sp, #16", "sub sp, sp, #16" },
    { CCR_TEMPLATE_A_IMUL,    "imul rax, rbx",       "imulq %rbx, %rax",    "imul rax, rbx", "mul x0, x0, x1", "mul r0, r0, r1" },
    { CCR_TEMPLATE_A_INC,     "inc rcx",             "incq %rcx",           "inc rcx", "add x0, x0, #1", "add r0, r0, #1" },
    { CCR_TEMPLATE_A_DEC,     "dec rcx",             "decq %rcx",           "dec rcx", "sub x0, x0, #1", "sub r0, r0, #1" },
    { CCR_TEMPLATE_A_NEG,     "neg rax",             "negq %rax",           "neg rax", "neg x0, x0", "rsb r0, r0, #0" },
    { CCR_TEMPLATE_A_XOR,     "xor eax, eax",        "xorl %eax, %eax",     "xor eax, eax", "eor x0, x0, x1", "eor r0, r0, r1" },
    { CCR_TEMPLATE_A_AND,     "and rax, 15",         "andq $15, %rax",      "and rax, 15", "and x0, x0, #15", "and r0, r0, #15" },
    { CCR_TEMPLATE_A_OR,      "or rax, rbx",         "orq %rbx, %rax",      "or rax, rbx", "orr x0, x0, x1", "orr r0, r0, r1" },
    { CCR_TEMPLATE_A_SHL,     "shl rax, 1",          "shlq $1, %rax",       "shl rax, 1", "lsl x0, x0, #1", "lsl r0, r0, #1" },
    { CCR_TEMPLATE_A_SHR,     "shr rax, 1",          "shrq $1, %rax",       "shr rax, 1", "lsr x0, x0, #1", "lsr r0, r0, #1" },
    { CCR_TEMPLATE_A_CMP,     "cmp rax, rbx",        "cmpq %rbx, %rax",     "cmp rax, rbx", "cmp x0, x1", "cmp r0, r1" },
    { CCR_TEMPLATE_A_TEST,    "test rax, rax",       "testq %rax, %rax",    "test rax, rax", "tst x0, x0", "tst r0, r0" },
    { CCR_TEMPLATE_A_CALL,    "call puts",           "call puts@PLT",       "call puts wrt ..plt", "bl puts", "bl puts" },
    { CCR_TEMPLATE_A_RET,     "ret",                 "ret",                 "ret", "ret", "bx lr" },
    { CCR_TEMPLATE_A_SYSCALL, "syscall",             "syscall",             "syscall", "svc #0", "svc #0" },
    { CCR_TEMPLATE_A_NOP,     "nop",                 "nop",                 "nop", "nop", "nop" },
    { CCR_TEMPLATE_A_GLOBL,   ".globl main",         ".globl main",         "global main", ".globl main", ".globl main" },
    { CCR_TEMPLATE_A_TEXT,    ".text",               ".text",               "section .text", ".text", ".text" },
    { CCR_TEMPLATE_A_DATA,    ".data",               ".data",               "section .data", ".data", ".data" },
    { CCR_TEMPLATE_A_STRING,  ".string \"Hello\"",   ".string \"Hello\"",   "db \"Hello\", 0", ".string \"Hello\"", ".asciz \"Hello\"" },
    { CCR_TEMPLATE_A_QUAD,    ".quad 0",             ".quad 0",             "dq 0", ".quad 0", ".word 0" },
    { CCR_TEMPLATE_A_BYTE,    ".byte 1, 2, 3",       ".byte 1, 2, 3",       "db 1, 2, 3", ".byte 1, 2, 3", ".byte 1, 2, 3" },
};

const char *ccr_template_asm_line(ccr_template t, ccr_asm_syntax syntax)
{
    for (size_t i = 0; i < sizeof g_asm_lines / sizeof g_asm_lines[0]; ++i)
        if (g_asm_lines[i].t == t)
            return syntax == CCR_ASM_NASM ? g_asm_lines[i].nasm
                 : syntax == CCR_ASM_INTEL ? g_asm_lines[i].intel
                 : syntax == CCR_ASM_ARM64 ? g_asm_lines[i].arm64
                 : syntax == CCR_ASM_ARM32 ? g_asm_lines[i].arm32 : g_asm_lines[i].att;
    return NULL;
}

/* Assembly blocks: from their line, the file's way. */
static ccr_node_id build_asm(ccr_ast *ast, ccr_template t, ccr_node_id parent)
{
    const ccr_asm_syntax syn = ast->asm_syntax;
    const bool nasm = syn == CCR_ASM_NASM, a64 = syn == CCR_ASM_ARM64, arm = CCR_ASM_IS_ARM(syn);
    switch (t) {
    case CCR_TEMPLATE_INL_ASM:  return ccr_asm_build_inline(ast, parent);
    case CCR_TEMPLATE_A_LABEL:  return ccr_asm_build_label(ast, parent);
    case CCR_TEMPLATE_A_JMP:    return ccr_asm_build_jump(ast, parent, arm ? "b" : "jmp");
    case CCR_TEMPLATE_A_JE:     return ccr_asm_build_jump(ast, parent, a64 ? "b.eq" : arm ? "beq" : "je");
    case CCR_TEMPLATE_A_JNE:    return ccr_asm_build_jump(ast, parent, a64 ? "b.ne" : arm ? "bne" : "jne");
    case CCR_TEMPLATE_A_JL:     return ccr_asm_build_jump(ast, parent, a64 ? "b.lt" : arm ? "blt" : "jl");
    case CCR_TEMPLATE_A_JG:     return ccr_asm_build_jump(ast, parent, a64 ? "b.gt" : arm ? "bgt" : "jg");
    case CCR_TEMPLATE_A_MACRO:  return ccr_asm_build_scope(ast, parent, nasm ? "%macro" : ".macro", nasm ? "name 0" : "name", false);
    case CCR_TEMPLATE_A_REPT:   return ccr_asm_build_scope(ast, parent, nasm ? "%rep" : ".rept", "4", false);
    case CCR_TEMPLATE_A_LINE: {
        ccr_node_id x = ccr_template_text(ast, parent, "");
        if (x != CCR_NONE)
            ast->nodes[x].flags |= CCR_FLAG_ASM;
        return x;
    }
    default: {
        const char *line = ccr_template_asm_line(t, syn);
        return line ? ccr_asm_build(ast, parent, line, syn) : CCR_NONE;
    }
    }
}

bool ccr_tcat_is_std(ccr_tcat c)
{
    return c == CCR_TCAT_STD_C || c == CCR_TCAT_STD_CPP;
}

unsigned ccr_template_langs(ccr_template t)
{
    const unsigned both = CCR_TLANG_C | CCR_TLANG_CPP;
    if ((unsigned)t >= CCR_TEMPLATE_COUNT)
        return both;   /* a pack's: its header says */
    switch (ccr_template_category(t)) {
    case CCR_TCAT_CLASSES:
    case CCR_TCAT_STD_CPP: return CCR_TLANG_CPP;
    case CCR_TCAT_STD_C:   return CCR_TLANG_C;
    default:               break;
    }
    switch (t) {
    case CCR_TEMPLATE_NULL_PTR:    return CCR_TLANG_C;     /* C++ says nullptr */
    case CCR_TEMPLATE_FOR_RANGE:
    case CCR_TEMPLATE_NULLPTR:
    case CCR_TEMPLATE_NEW_OBJ:
    case CCR_TEMPLATE_DELETE_PTR:
    case CCR_TEMPLATE_STATIC_CAST:
    case CCR_TEMPLATE_NAMESPACE_DEF:
    case CCR_TEMPLATE_USING_NS:    return CCR_TLANG_CPP;
    default:                       return both;
    }
}

const char *ccr_std_header(const char *name)
{
    static const struct { const char *header; const char *names; } table[] = {
        { "<stdio.h>",  " printf puts scanf fgets fopen fclose fprintf fputs putchar getchar snprintf sscanf "
                        "fscanf perror fread fwrite fflush " },
        { "<stdlib.h>", " malloc calloc realloc free exit rand srand abs atoi atof strtol strtod qsort getenv "
                        "system " },
        { "<string.h>", " strlen strcmp strncmp strcpy strncpy strcat memcpy memmove memset memcmp strchr "
                        "strrchr strstr strdup " },
        { "<math.h>",   " sqrt pow sin cos tan atan2 fabs floor ceil round log exp fmin fmax " },
        { "<time.h>",   " time clock " },
        { "<ctype.h>",  " isalpha isdigit isspace isalnum isupper islower toupper tolower " },
        { "<iostream>", " std::cout std::cin std::cerr std::endl " },
        { "<string>",   " std::string std::getline std::to_string std::stoi std::stod " },
        { "<vector>",   " std::vector " },
        { "<algorithm>", " std::sort std::max std::min std::find std::reverse std::count std::fill " },
        { "<memory>",   " std::unique_ptr std::shared_ptr std::make_unique std::make_shared " },
        { "<map>",      " std::map " },
        { "<unordered_map>", " std::unordered_map " },
        { "<array>",    " std::array " },
        { "<utility>",  " std::move std::pair std::swap " },
    };
    if (!name || !*name)
        return NULL;
    /* the name itself, or the type it starts with: "std::vector<int>", "const std::string &" */
    char word[64];
    const char *p = name;
    if (strncmp(p, "const ", 6) == 0)
        p += 6;
    size_t n = 0;
    while (p[n] && (p[n] == ':' || p[n] == '_' || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') ||
                    (p[n] >= '0' && p[n] <= '9')) && n < sizeof word - 3)
        ++n;
    if (n == 0)
        return NULL;
    word[0] = ' ';
    memcpy(word + 1, p, n);
    word[n + 1] = ' ';
    word[n + 2] = '\0';
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (strstr(table[i].names, word))
            return table[i].header;
    return NULL;
}

ccr_template ccr_template_pack_link(int pack)
{
    return (ccr_template)(CCR_TEMPLATE_COUNT + ccr_lib_block_count() + 2 * pack);
}

ccr_template ccr_template_pack_find(int pack)
{
    return (ccr_template)(CCR_TEMPLATE_COUNT + ccr_lib_block_count() + 2 * pack + 1);
}

/* The pack block a template stands for, or NULL. */
static const ccr_lib_block *lib_block(ccr_template t)
{
    const int i = (int)t - CCR_TEMPLATE_COUNT;
    return i >= 0 && i < ccr_lib_block_count() ? ccr_lib_block_at(i) : NULL;
}

/* A pack's CMake template: its pack, and whether it finds (or links). */
static const ccr_lib_pack *pack_of(ccr_template t, bool *find)
{
    const int i = (int)t - CCR_TEMPLATE_COUNT - ccr_lib_block_count();
    *find = i >= 0 && (i & 1);
    return i >= 0 ? ccr_lib_pack_at(i / 2) : NULL;
}

const char *ccr_template_label(ccr_template t)
{
    bool find;
    if (lib_block(t))
        return lib_block(t)->call;
    if (pack_of(t, &find))
        return find ? "find_package" : "target_link_libraries";
    return (unsigned)t < CCR_TEMPLATE_COUNT ? g_labels[t] : "?";
}

ccr_tcat ccr_template_category(ccr_template t)
{
    bool find;
    if ((unsigned)t < CCR_TEMPLATE_COUNT)
        return g_cats[t];
    return pack_of(t, &find) ? CCR_TCAT_CM_LIBS : CCR_TCAT_COUNT;   /* a pack's */
}

ccr_node_id ccr_template_node(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent)
{
    ccr_node_id id = ccr_ast_add(ast, kind, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ast->nodes[id].flags |= CCR_FLAG_NEW;
    if (kind == CCR_NODE_BLOCK) {
        /* printed with relative indents; the block it lands in shifts it */
        ast->nodes[id].indent = ccr_ast_intern(ast, "    ");
        ast->nodes[id].flags |= CCR_FLAG_DIRTY;
    }
    if (parent != CCR_NONE)
        ccr_ast_append(ast, parent, id);
    return id;
}

ccr_node_id ccr_template_text(ccr_ast *ast, ccr_node_id parent, const char *text)
{
    ccr_node_id id = ccr_template_node(ast, CCR_NODE_TEXT, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ast->nodes[id].name = ccr_ast_intern(ast, text);
    if (parent != CCR_NONE)
        ccr_ast_append(ast, parent, id);
    return id;
}

/* A name or type field: the text lives in the parent too. */
static void word(ccr_ast *ast, ccr_node_id parent, uint32_t which, const char *text)
{
    ccr_node_id w = ccr_template_text(ast, parent, text);
    if (w == CCR_NONE || parent == CCR_NONE)
        return;
    ast->nodes[w].flags |= which;
    if (which == CCR_FLAG_WORD_NAME)
        ast->nodes[parent].name = ast->nodes[w].name;
    else
        ast->nodes[parent].type = ast->nodes[w].name;
}

static ccr_node_id op_node(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent, const char *op,
                           int slots)
{
    ccr_node_id n = ccr_template_node(ast, kind, parent);
    if (n == CCR_NONE)
        return CCR_NONE;
    ast->nodes[n].op = ccr_ast_intern(ast, op);
    for (int i = 0; i < slots; ++i)
        ccr_template_text(ast, n, "");
    return n;
}

/* A standard function: its name fixed, a slot per argument (with what
 * usually goes there). */
static ccr_node_id fcall(ccr_ast *ast, ccr_node_id parent, const char *name, int nargs, const char *const *args)
{
    ccr_node_id n = ccr_template_node(ast, CCR_NODE_CALL, parent);
    if (n == CCR_NONE)
        return CCR_NONE;
    ast->nodes[n].name = ccr_ast_intern(ast, name);
    for (int i = 0; i < nargs; ++i)
        ccr_template_text(ast, n, args[i]);
    return n;
}
#define FCALL(name, ...) fcall(ast, parent, name, (int)(sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(const char *)), \
                               (const char *[]){ __VA_ARGS__ })

/* v.method(args): a call whose callee is a member of the object slot. */
static ccr_node_id method_call(ccr_ast *ast, ccr_node_id parent, const char *object, const char *method,
                               const char *arg)
{
    ccr_node_id n = ccr_template_node(ast, CCR_NODE_CALL, parent);
    if (n == CCR_NONE)
        return CCR_NONE;
    ast->nodes[n].name = ccr_ast_intern(ast, method);
    const ccr_node_id m = ccr_template_node(ast, CCR_NODE_MEMBER, n);
    if (m != CCR_NONE) {
        ast->nodes[m].name = ccr_ast_intern(ast, method);
        ccr_template_text(ast, m, object);
    }
    if (arg)
        ccr_template_text(ast, n, arg);
    return n;
}

static ccr_node_id call(ccr_ast *ast, ccr_node_id parent, const char *name, const char *args)
{
    ccr_node_id n = ccr_template_node(ast, CCR_NODE_CALL, parent);
    if (n == CCR_NONE)
        return CCR_NONE;
    word(ast, n, CCR_FLAG_WORD_NAME, name);
    ccr_template_text(ast, n, args);
    return n;
}

const char *const *ccr_op_choices(const char *op, bool unary, int *count)
{
    static const char *const arith[]  = { "+", "-", "*", "/", "%" };
    static const char *const test[]   = { "==", "!=", "<", ">", "<=", ">=", "&&", "||" };
    static const char *const bits[]   = { "&", "|", "^", "<<", ">>" };
    static const char *const assign[] = { "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                                          "<<=", ">>=" };
    static const char *const prefix[] = { "!", "~", "-", "+" };
    static const char *const step[]   = { "++", "--" };
    static const struct { const char *const *v; int n; bool unary; } groups[] = {
        { arith, 5, false }, { test, 8, false }, { bits, 5, false }, { assign, 11, false },
        { prefix, 4, true }, { step, 2, true },
    };
    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; ++g) {
        if (groups[g].unary != unary)
            continue;
        for (int i = 0; i < groups[g].n; ++i)
            if (strcmp(groups[g].v[i], op) == 0) {
                *count = groups[g].n;
                return groups[g].v;
            }
    }
    *count = 0;
    return NULL;
}

bool ccr_template_is_toplevel(ccr_template t)
{
    return ccr_template_category(t) == CCR_TCAT_DECLS || t == CCR_TEMPLATE_CLASS_DEF;
}

bool ccr_template_is_reporter(ccr_template t)
{
    if (lib_block(t))
        return strcmp(lib_block(t)->returns, "void") != 0;
    switch (t) {
    case CCR_TEMPLATE_ADD: case CCR_TEMPLATE_SUB: case CCR_TEMPLATE_MUL:
    case CCR_TEMPLATE_DIV: case CCR_TEMPLATE_MOD:
    case CCR_TEMPLATE_LT: case CCR_TEMPLATE_GT: case CCR_TEMPLATE_LE:
    case CCR_TEMPLATE_GE: case CCR_TEMPLATE_EQ: case CCR_TEMPLATE_NE:
    case CCR_TEMPLATE_AND: case CCR_TEMPLATE_OR: case CCR_TEMPLATE_NOT:
    case CCR_TEMPLATE_BIT_AND: case CCR_TEMPLATE_BIT_OR: case CCR_TEMPLATE_BIT_XOR:
    case CCR_TEMPLATE_BIT_NOT: case CCR_TEMPLATE_SHL: case CCR_TEMPLATE_SHR:
    case CCR_TEMPLATE_TERNARY:
    case CCR_TEMPLATE_ADDR: case CCR_TEMPLATE_DEREF: case CCR_TEMPLATE_INDEX:
    case CCR_TEMPLATE_MEMBER: case CCR_TEMPLATE_ARROW: case CCR_TEMPLATE_NULL_PTR:
    case CCR_TEMPLATE_CALL_EXPR: case CCR_TEMPLATE_MALLOC:
    case CCR_TEMPLATE_CAST: case CCR_TEMPLATE_SIZEOF_TYPE: case CCR_TEMPLATE_SIZEOF_EXPR:
    case CCR_TEMPLATE_NULLPTR: case CCR_TEMPLATE_NEW_OBJ: case CCR_TEMPLATE_STATIC_CAST:
    case CCR_TEMPLATE_THIS:
    case CCR_TEMPLATE_CALLOC: case CCR_TEMPLATE_REALLOC: case CCR_TEMPLATE_STRLEN:
    case CCR_TEMPLATE_STRCMP: case CCR_TEMPLATE_RAND: case CCR_TEMPLATE_SQRT: case CCR_TEMPLATE_POW:
    case CCR_TEMPLATE_FOPEN:
    case CCR_TEMPLATE_SIZE: case CCR_TEMPLATE_TO_STRING:
        return true;
    default:
        return false;
    }
}

ccr_node_id ccr_template_build(ccr_ast *ast, ccr_template t, ccr_node_id parent)
{
    ccr_node_id s = CCR_NONE;
    bool find;
    const ccr_lib_pack *pk = pack_of(t, &find);
    if (pk) {
        char spec[400];
        if (find)
            snprintf(spec, sizeof spec, "find_package(%s)", pk->find);
        else
            snprintf(spec, sizeof spec, "target_link_libraries(${PROJECT_NAME} PRIVATE %s)", pk->link);
        return ccr_cmake_build_command(ast, parent, spec);
    }
    if ((unsigned)t < CCR_TEMPLATE_COUNT && (ccr_template_category(t) == CCR_TCAT_ASM_INLINE ||
                                             ccr_tcat_lang(ccr_template_category(t)) == CCR_LANG_ASM))
        return build_asm(ast, t, parent);
    if ((unsigned)t < CCR_TEMPLATE_COUNT && ccr_tcat_is_cmake(ccr_template_category(t))) {
        const char *spec = g_labels[t];
        if (t == CCR_TEMPLATE_CM_LINE)
            return ccr_template_text(ast, parent, "");
        static const char *const scopes[] = { "if", "foreach", "while", "function", "macro" };
        for (size_t i = 0; i < sizeof scopes / sizeof scopes[0]; ++i) {
            const size_t n = strlen(scopes[i]);
            if (strncmp(spec, scopes[i], n) == 0 && spec[n] == '(') {
                char args[200];
                snprintf(args, sizeof args, "%.*s", (int)(strlen(spec) - n - 2), spec + n + 1);
                return ccr_cmake_build_scope(ast, parent, scopes[i], args, t == CCR_TEMPLATE_CM_IF_ELSE);
            }
        }
        return ccr_cmake_build_command(ast, parent, spec);
    }
    const ccr_lib_block *lb = lib_block(t);
    if (lb) {
        /* a library call: its name fixed, its arguments slots with defaults */
        s = ccr_template_node(ast, CCR_NODE_CALL, parent);
        if (s == CCR_NONE)
            return CCR_NONE;
        ast->nodes[s].name = ccr_ast_intern(ast, lb->call);
        for (int i = 0; i < lb->nparams; ++i)
            ccr_template_text(ast, s, lb->defaults[i]);
        return s;
    }
    switch (t) {
    case CCR_TEMPLATE_CODE:
        s = ccr_template_text(ast, parent, "");
        break;
    case CCR_TEMPLATE_IF:
    case CCR_TEMPLATE_IF_ELSE:
        s = ccr_template_node(ast, CCR_NODE_IF, parent);
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        if (t == CCR_TEMPLATE_IF_ELSE)
            ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_WHILE:
    case CCR_TEMPLATE_SWITCH:
        s = ccr_template_node(ast, t == CCR_TEMPLATE_WHILE ? CCR_NODE_WHILE : CCR_NODE_SWITCH,
                              parent);
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_FOR:
        s = ccr_template_node(ast, CCR_NODE_FOR, parent);
        ccr_template_text(ast, s, "int i = 0");
        ccr_template_text(ast, s, "i < n");
        ccr_template_text(ast, s, "i++");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_DO_WHILE:
        s = ccr_template_node(ast, CCR_NODE_DO_WHILE, parent);
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_RETURN:
        s = ccr_template_node(ast, CCR_NODE_RETURN, parent);
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_BREAK:
        s = ccr_template_node(ast, CCR_NODE_BREAK, parent);
        break;
    case CCR_TEMPLATE_CONTINUE:
        s = ccr_template_node(ast, CCR_NODE_CONTINUE, parent);
        break;
    case CCR_TEMPLATE_CASE:
        s = ccr_template_node(ast, CCR_NODE_CASE, parent);
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_DEFAULT:
        s = ccr_template_node(ast, CCR_NODE_DEFAULT, parent);
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_GOTO:
    case CCR_TEMPLATE_LABEL:
        /* the same name in both, so a fresh pair is already an arrow */
        s = ccr_template_node(ast, t == CCR_TEMPLATE_GOTO ? CCR_NODE_GOTO : CCR_NODE_LABEL, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "done");
        break;

    case CCR_TEMPLATE_ADD:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "+", 2);  break;
    case CCR_TEMPLATE_SUB:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "-", 2);  break;
    case CCR_TEMPLATE_MUL:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "*", 2);  break;
    case CCR_TEMPLATE_DIV:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "/", 2);  break;
    case CCR_TEMPLATE_MOD:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "%", 2);  break;
    case CCR_TEMPLATE_LT:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "<", 2);  break;
    case CCR_TEMPLATE_GT:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, ">", 2);  break;
    case CCR_TEMPLATE_LE:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "<=", 2); break;
    case CCR_TEMPLATE_GE:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, ">=", 2); break;
    case CCR_TEMPLATE_EQ:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "==", 2); break;
    case CCR_TEMPLATE_NE:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "!=", 2); break;
    case CCR_TEMPLATE_AND:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "&&", 2); break;
    case CCR_TEMPLATE_OR:      s = op_node(ast, CCR_NODE_BINARY_OP, parent, "||", 2); break;
    case CCR_TEMPLATE_NOT:     s = op_node(ast, CCR_NODE_UNARY_OP, parent, "!", 1);   break;
    case CCR_TEMPLATE_BIT_AND: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "&", 2);  break;
    case CCR_TEMPLATE_BIT_OR:  s = op_node(ast, CCR_NODE_BINARY_OP, parent, "|", 2);  break;
    case CCR_TEMPLATE_BIT_XOR: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "^", 2);  break;
    case CCR_TEMPLATE_BIT_NOT: s = op_node(ast, CCR_NODE_UNARY_OP, parent, "~", 1);   break;
    case CCR_TEMPLATE_SHL:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "<<", 2); break;
    case CCR_TEMPLATE_SHR:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, ">>", 2); break;
    case CCR_TEMPLATE_TERNARY: s = op_node(ast, CCR_NODE_TERNARY, parent, "", 3);     break;

    case CCR_TEMPLATE_DECL_INIT:
    case CCR_TEMPLATE_DECL:
        s = ccr_template_node(ast, CCR_NODE_VAR, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        word(ast, s, CCR_FLAG_WORD_NAME, "x");
        if (t == CCR_TEMPLATE_DECL_INIT) {
            ccr_node_id v = ccr_template_text(ast, s, "0");
            if (v != CCR_NONE)
                ast->nodes[v].flags |= CCR_FLAG_INIT;
        }
        break;
    case CCR_TEMPLATE_ASSIGN:     s = op_node(ast, CCR_NODE_BINARY_OP, parent, "=", 2);  break;
    case CCR_TEMPLATE_ADD_ASSIGN: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "+=", 2); break;
    case CCR_TEMPLATE_SUB_ASSIGN: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "-=", 2); break;
    case CCR_TEMPLATE_MUL_ASSIGN: s = op_node(ast, CCR_NODE_BINARY_OP, parent, "*=", 2); break;
    case CCR_TEMPLATE_INC:
    case CCR_TEMPLATE_DEC:
        s = op_node(ast, CCR_NODE_UNARY_OP, parent, t == CCR_TEMPLATE_INC ? "++" : "--", 1);
        if (s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_POSTFIX;
        break;

    case CCR_TEMPLATE_ADDR:  s = op_node(ast, CCR_NODE_UNARY_OP, parent, "&", 1); break;
    case CCR_TEMPLATE_DEREF: s = op_node(ast, CCR_NODE_UNARY_OP, parent, "*", 1); break;
    case CCR_TEMPLATE_INDEX: s = op_node(ast, CCR_NODE_INDEX, parent, "", 2);     break;
    case CCR_TEMPLATE_MEMBER:
    case CCR_TEMPLATE_ARROW:
        s = ccr_template_node(ast, CCR_NODE_MEMBER, parent);
        ccr_template_text(ast, s, "");
        word(ast, s, CCR_FLAG_WORD_NAME, "field");
        if (t == CCR_TEMPLATE_ARROW && s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_ARROW;
        break;
    case CCR_TEMPLATE_NULL_PTR:
    case CCR_TEMPLATE_NULLPTR:
    case CCR_TEMPLATE_THIS:
        s = ccr_template_node(ast, CCR_NODE_REF, parent);
        if (s != CCR_NONE)
            ast->nodes[s].name = ccr_ast_intern(ast, t == CCR_TEMPLATE_NULL_PTR ? "NULL"
                                                     : t == CCR_TEMPLATE_NULLPTR ? "nullptr" : "this");
        break;
    case CCR_TEMPLATE_NEW_OBJ:
        s = ccr_template_node(ast, CCR_NODE_NEW, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_DELETE_PTR:
        s = ccr_template_node(ast, CCR_NODE_DELETE, parent);
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_FOR_RANGE:
        s = ccr_template_node(ast, CCR_NODE_FOR_RANGE, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "auto");
        word(ast, s, CCR_FLAG_WORD_NAME, "x");
        ccr_template_text(ast, s, "");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_STATIC_CAST:
        s = ccr_template_node(ast, CCR_NODE_CAST, parent);
        if (s != CCR_NONE)
            ast->nodes[s].raw = ccr_ast_intern(ast, "static_cast");
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_CLASS_DEF:
        s = ccr_template_node(ast, CCR_NODE_STRUCT, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "Player");
        if (s != CCR_NONE) {
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION | CCR_FLAG_CLASS;
            const ccr_node_id members = ccr_template_node(ast, CCR_NODE_BLOCK, s);
            const ccr_node_id pub = ccr_template_node(ast, CCR_NODE_ACCESS, members);
            if (pub != CCR_NONE)
                ast->nodes[pub].name = ccr_ast_intern(ast, "public");
        }
        break;
    case CCR_TEMPLATE_METHOD:
        s = ccr_template_node(ast, CCR_NODE_FUNCTION, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "void");
        word(ast, s, CCR_FLAG_WORD_NAME, "update");
        ccr_template_text(ast, s, "");
        if (s != CCR_NONE) {
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
            ccr_template_node(ast, CCR_NODE_BLOCK, s);
        }
        break;
    case CCR_TEMPLATE_PUBLIC:
    case CCR_TEMPLATE_PRIVATE:
        s = ccr_template_node(ast, CCR_NODE_ACCESS, parent);
        if (s != CCR_NONE)
            ast->nodes[s].name = ccr_ast_intern(ast, t == CCR_TEMPLATE_PUBLIC ? "public" : "private");
        break;
    case CCR_TEMPLATE_NAMESPACE_DEF:
        s = ccr_template_node(ast, CCR_NODE_NAMESPACE, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "game");
        ccr_template_node(ast, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_USING_NS:
        s = ccr_template_node(ast, CCR_NODE_USING, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "std");
        break;

    /* C's standard library: the usual arguments in the slots */
    case CCR_TEMPLATE_PUTS:    s = FCALL("puts", "\"Hello!\"");                      break;
    case CCR_TEMPLATE_SCANF:   s = FCALL("scanf", "\"%d\"", "&x");                    break;
    case CCR_TEMPLATE_FGETS:   s = FCALL("fgets", "line", "sizeof line", "stdin");     break;
    case CCR_TEMPLATE_CALLOC:  s = FCALL("calloc", "n", "sizeof *p");                  break;
    case CCR_TEMPLATE_REALLOC: s = FCALL("realloc", "p", "n * sizeof *p");             break;
    case CCR_TEMPLATE_STRLEN:  s = FCALL("strlen", "s");                               break;
    case CCR_TEMPLATE_STRCMP:  s = FCALL("strcmp", "a", "b");                          break;
    case CCR_TEMPLATE_MEMCPY:  s = FCALL("memcpy", "dst", "src", "n");                 break;
    case CCR_TEMPLATE_MEMSET:  s = FCALL("memset", "p", "0", "n");                     break;
    case CCR_TEMPLATE_RAND:    s = fcall(ast, parent, "rand", 0, NULL);                break;
    case CCR_TEMPLATE_SRAND:   s = FCALL("srand", "42");                               break;
    case CCR_TEMPLATE_SQRT:    s = FCALL("sqrt", "x");                                 break;
    case CCR_TEMPLATE_POW:     s = FCALL("pow", "x", "2");                             break;
    case CCR_TEMPLATE_FOPEN:   s = FCALL("fopen", "\"save.txt\"", "\"r\"");           break;
    case CCR_TEMPLATE_FPRINTF: s = FCALL("fprintf", "f", "\"%d\\n\"", "x");           break;
    case CCR_TEMPLATE_FCLOSE:  s = FCALL("fclose", "f");                               break;
    case CCR_TEMPLATE_EXIT:    s = FCALL("exit", "0");                                 break;

    /* C++'s */
    case CCR_TEMPLATE_COUT:
    case CCR_TEMPLATE_CIN: {
        const bool out = t == CCR_TEMPLATE_COUT;
        s = ccr_template_node(ast, CCR_NODE_BINARY_OP, parent);
        if (s == CCR_NONE)
            break;
        ast->nodes[s].op = ccr_ast_intern(ast, out ? "<<" : ">>");
        ccr_node_id left = s;
        if (out) {   /* std::cout << "Hello" << '\n' */
            left = ccr_template_node(ast, CCR_NODE_BINARY_OP, s);
            if (left == CCR_NONE)
                break;
            ast->nodes[left].op = ccr_ast_intern(ast, "<<");
        }
        const ccr_node_id stream = ccr_template_node(ast, CCR_NODE_REF, left);
        if (stream != CCR_NONE)
            ast->nodes[stream].name = ccr_ast_intern(ast, out ? "std::cout" : "std::cin");
        ccr_template_text(ast, left, out ? "\"Hello!\"" : "x");
        if (out)
            ccr_template_text(ast, s, "'\\n'");
        break;
    }
    case CCR_TEMPLATE_STRING_DECL:
    case CCR_TEMPLATE_VECTOR_DECL:
        s = ccr_template_node(ast, CCR_NODE_VAR, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, t == CCR_TEMPLATE_STRING_DECL ? "std::string" : "std::vector<int>");
        word(ast, s, CCR_FLAG_WORD_NAME, t == CCR_TEMPLATE_STRING_DECL ? "s" : "v");
        if (t == CCR_TEMPLATE_STRING_DECL && s != CCR_NONE) {
            const ccr_node_id v = ccr_template_text(ast, s, "\"\"");
            if (v != CCR_NONE)
                ast->nodes[v].flags |= CCR_FLAG_INIT;
        }
        break;
    case CCR_TEMPLATE_PUSH_BACK: s = method_call(ast, parent, "v", "push_back", "x");        break;
    case CCR_TEMPLATE_SIZE:      s = method_call(ast, parent, "v", "size", NULL);            break;
    case CCR_TEMPLATE_SORT:      s = FCALL("std::sort", "v.begin()", "v.end()");             break;
    case CCR_TEMPLATE_GETLINE:   s = FCALL("std::getline", "std::cin", "s");                 break;
    case CCR_TEMPLATE_TO_STRING: s = FCALL("std::to_string", "x");                           break;

    case CCR_TEMPLATE_CALL_STMT:
    case CCR_TEMPLATE_CALL_EXPR: s = call(ast, parent, "f", "");                       break;
    case CCR_TEMPLATE_PRINTF:    s = call(ast, parent, "printf", "\"Hello, world!\\n\""); break;
    case CCR_TEMPLATE_MALLOC:    s = call(ast, parent, "malloc", "");                  break;
    case CCR_TEMPLATE_FREE:      s = call(ast, parent, "free", "");                    break;

    case CCR_TEMPLATE_CAST:
        s = ccr_template_node(ast, CCR_NODE_CAST, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        ccr_template_text(ast, s, "");
        break;
    case CCR_TEMPLATE_SIZEOF_TYPE:
        s = ccr_template_node(ast, CCR_NODE_SIZEOF, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        break;
    case CCR_TEMPLATE_SIZEOF_EXPR:
        s = ccr_template_node(ast, CCR_NODE_SIZEOF, parent);
        ccr_template_text(ast, s, "");
        break;

    /* the file's own declarations; a function's return type is its `type`,
     * its parameters one text slot ("int a, char *b") */
    case CCR_TEMPLATE_FUNC_DEF:
    case CCR_TEMPLATE_FUNC_DECL:
        s = ccr_template_node(ast, CCR_NODE_FUNCTION, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "int");
        word(ast, s, CCR_FLAG_WORD_NAME, "name");
        ccr_template_text(ast, s, "void");
        if (t == CCR_TEMPLATE_FUNC_DEF && s != CCR_NONE) {
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
            ccr_node_id body = ccr_template_node(ast, CCR_NODE_BLOCK, s);
            if (body != CCR_NONE)
                ast->nodes[body].close_indent = ccr_ast_intern(ast, "");
        }
        break;
    case CCR_TEMPLATE_STRUCT_DEF:
        s = ccr_template_node(ast, CCR_NODE_STRUCT, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "point");
        if (s != CCR_NONE) {
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
            ccr_template_node(ast, CCR_NODE_BLOCK, s);   /* fields: declarations go in */
        }
        break;
    case CCR_TEMPLATE_ENUM_DEF:
        s = ccr_template_node(ast, CCR_NODE_ENUM, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "mood");
        ccr_template_text(ast, s, "CALM, ANGRY");
        if (s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_DEFINITION;
        break;
    case CCR_TEMPLATE_TYPEDEF:
        s = ccr_template_node(ast, CCR_NODE_TYPEDEF, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "struct point");
        word(ast, s, CCR_FLAG_WORD_NAME, "point");
        break;
    case CCR_TEMPLATE_GLOBAL_VAR:
        s = ccr_template_node(ast, CCR_NODE_VAR, parent);
        word(ast, s, CCR_FLAG_WORD_TYPE, "static int");
        word(ast, s, CCR_FLAG_WORD_NAME, "counter");
        if (s != CCR_NONE) {
            ccr_node_id v = ccr_template_text(ast, s, "0");
            if (v != CCR_NONE)
                ast->nodes[v].flags |= CCR_FLAG_INIT;
        }
        break;
    case CCR_TEMPLATE_INCLUDE_SYS:
    case CCR_TEMPLATE_INCLUDE_LOC:
        s = ccr_template_node(ast, CCR_NODE_INCLUDE, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, t == CCR_TEMPLATE_INCLUDE_SYS ? "stdio.h" : "util.h");
        if (t == CCR_TEMPLATE_INCLUDE_SYS && s != CCR_NONE)
            ast->nodes[s].flags |= CCR_FLAG_ANGLED;
        break;
    case CCR_TEMPLATE_DEFINE:
        s = ccr_template_node(ast, CCR_NODE_MACRO_DEFINE, parent);
        word(ast, s, CCR_FLAG_WORD_NAME, "SIZE");
        ccr_template_text(ast, s, "64");
        break;

    default:   /* CMake's and CCR_TEMPLATE_COUNT: above */
        break;
    }
    return s;
}
