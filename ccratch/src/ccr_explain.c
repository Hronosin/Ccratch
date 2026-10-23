#include "ccr_explain.h"
#include "ccr_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------ examples */

static const struct { ccr_template t; const char *code; } g_examples[] = {
    { CCR_TEMPLATE_CODE,        "int answer = 6 * 7;   /* any C at all */" },
    { CCR_TEMPLATE_IF,          "if (lives == 0) {\n    printf(\"Game over\\n\");\n}" },
    { CCR_TEMPLATE_IF_ELSE,     "if (age >= 18) {\n    printf(\"Welcome\\n\");\n} else {\n    printf(\"Too young\\n\");\n}" },
    { CCR_TEMPLATE_WHILE,       "while (fuel > 0) {\n    fuel = fuel - 1;\n}" },
    { CCR_TEMPLATE_FOR,         "for (int i = 0; i < 3; i++) {\n    printf(\"%d\\n\", i);   /* 0, 1, 2 */\n}" },
    { CCR_TEMPLATE_DO_WHILE,    "do {\n    answer = ask();\n} while (answer != 42);" },
    { CCR_TEMPLATE_RETURN,      "int twice(int n) {\n    return n * 2;\n}" },
    { CCR_TEMPLATE_BREAK,       "while (1) {\n    if (key == 'q')\n        break;   /* out of the loop */\n}" },
    { CCR_TEMPLATE_CONTINUE,    "for (int i = 0; i < 10; i++) {\n    if (i % 2 == 1)\n        continue;   /* skip odd ones */\n    printf(\"%d\\n\", i);\n}" },
    { CCR_TEMPLATE_SWITCH,      "switch (key) {\ncase 'w': y = y - 1; break;\ncase 's': y = y + 1; break;\n}" },
    { CCR_TEMPLATE_CASE,        "case 'w':\n    y = y - 1;\n    break;" },
    { CCR_TEMPLATE_DEFAULT,     "default:\n    printf(\"Unknown key\\n\");" },
    { CCR_TEMPLATE_GOTO,        "if (error)\n    goto cleanup;\n...\ncleanup:\n    free(buffer);" },
    { CCR_TEMPLATE_LABEL,       "cleanup:\n    free(buffer);" },
    { CCR_TEMPLATE_ADD,         "int total = price + tax;" },
    { CCR_TEMPLATE_SUB,         "int left = 10 - used;" },
    { CCR_TEMPLATE_MUL,         "int area = width * height;" },
    { CCR_TEMPLATE_DIV,         "int half = 7 / 2;      /* 3: whole numbers */\nfloat h = 7.0f / 2;   /* 3.5 */" },
    { CCR_TEMPLATE_MOD,         "int rest = 7 % 3;   /* 1 */\nif (n % 2 == 0)     /* n is even */" },
    { CCR_TEMPLATE_LT,          "if (hp < 20)\n    printf(\"Low health!\\n\");" },
    { CCR_TEMPLATE_GT,          "if (score > best)\n    best = score;" },
    { CCR_TEMPLATE_LE,          "while (i <= 10)\n    i++;" },
    { CCR_TEMPLATE_GE,          "if (age >= 18)\n    vote();" },
    { CCR_TEMPLATE_EQ,          "if (answer == 42)   /* two =, not one! */\n    win();" },
    { CCR_TEMPLATE_NE,          "while (key != 'q')\n    key = getchar();" },
    { CCR_TEMPLATE_AND,         "if (has_key && door_near)\n    open_door();" },
    { CCR_TEMPLATE_OR,          "if (hp <= 0 || time_left == 0)\n    game_over();" },
    { CCR_TEMPLATE_NOT,         "if (!found)\n    printf(\"Not found\\n\");" },
    { CCR_TEMPLATE_BIT_AND,     "if (flags & FLAG_BOLD)   /* is that bit set? */" },
    { CCR_TEMPLATE_BIT_OR,      "flags = flags | FLAG_BOLD;   /* set that bit */" },
    { CCR_TEMPLATE_BIT_XOR,     "light = light ^ 1;   /* 0 -> 1 -> 0... */" },
    { CCR_TEMPLATE_BIT_NOT,     "flags = flags & ~FLAG_BOLD;   /* clear that bit */" },
    { CCR_TEMPLATE_SHL,         "int eight = 1 << 3;   /* 1 * 2 * 2 * 2 */" },
    { CCR_TEMPLATE_SHR,         "int half = n >> 1;   /* n / 2 */" },
    { CCR_TEMPLATE_TERNARY,     "int biggest = a > b ? a : b;" },
    { CCR_TEMPLATE_DECL_INIT,   "int lives = 3;\nfloat speed = 1.5f;\nchar letter = 'A';" },
    { CCR_TEMPLATE_DECL,        "int score;   /* set it before reading it! */\nscore = 0;" },
    { CCR_TEMPLATE_ASSIGN,      "x = 10;\nx = x + 1;   /* now 11 */" },
    { CCR_TEMPLATE_ADD_ASSIGN,  "score += 100;   /* score = score + 100 */" },
    { CCR_TEMPLATE_SUB_ASSIGN,  "hp -= damage;   /* hp = hp - damage */" },
    { CCR_TEMPLATE_MUL_ASSIGN,  "speed *= 2;   /* twice as fast */" },
    { CCR_TEMPLATE_INC,         "count++;   /* count = count + 1 */" },
    { CCR_TEMPLATE_DEC,         "lives--;   /* lives = lives - 1 */" },
    { CCR_TEMPLATE_ADDR,        "int n = 5;\nint *p = &n;   /* p knows where n lives */" },
    { CCR_TEMPLATE_DEREF,       "int *p = &n;\n*p = 10;   /* now n is 10 */" },
    { CCR_TEMPLATE_INDEX,       "int days[7];\ndays[0] = 1;   /* the first one */\ndays[6] = 7;   /* the last one */" },
    { CCR_TEMPLATE_MEMBER,      "struct point p;\np.x = 3;\np.y = 4;" },
    { CCR_TEMPLATE_ARROW,       "struct point *p = &where;\np->x = 3;   /* (*p).x = 3 */" },
    { CCR_TEMPLATE_NULL_PTR,    "char *name = NULL;   /* nothing yet */\nif (name != NULL)\n    printf(\"%s\\n\", name);" },
    { CCR_TEMPLATE_CALL_STMT,   "draw_player(x, y);\nprintf(\"Hi\\n\");" },
    { CCR_TEMPLATE_CALL_EXPR,   "int n = rand() % 6 + 1;   /* a dice roll */" },
    { CCR_TEMPLATE_PRINTF,      "printf(\"Hello, %s!\\n\", name);\nprintf(\"Score: %d\\n\", score);" },
    { CCR_TEMPLATE_MALLOC,      "int *many = malloc(100 * sizeof(int));\n...\nfree(many);" },
    { CCR_TEMPLATE_FREE,        "char *text = malloc(64);\n...\nfree(text);   /* give it back, once */" },
    { CCR_TEMPLATE_CAST,        "float avg = (float)sum / count;" },
    { CCR_TEMPLATE_SIZEOF_TYPE, "int *p = malloc(10 * sizeof(int));" },
    { CCR_TEMPLATE_SIZEOF_EXPR, "int days[7];\nint n = sizeof days / sizeof days[0];   /* 7 */" },
    { CCR_TEMPLATE_FUNC_DEF,    "int square(int n)\n{\n    return n * n;\n}" },
    { CCR_TEMPLATE_FUNC_DECL,   "int square(int n);   /* made further down */" },
    { CCR_TEMPLATE_STRUCT_DEF,  "struct player {\n    char name[32];\n    int hp;\n};" },
    { CCR_TEMPLATE_ENUM_DEF,    "enum color { RED, GREEN, BLUE };\nenum color c = GREEN;" },
    { CCR_TEMPLATE_TYPEDEF,     "typedef struct player player;\nplayer hero;   /* no \"struct\" now */" },
    { CCR_TEMPLATE_GLOBAL_VAR,  "int high_score = 0;   /* every function sees it */" },
    { CCR_TEMPLATE_INCLUDE_SYS, "#include <stdio.h>    /* printf */\n#include <stdlib.h>   /* malloc, rand */" },
    { CCR_TEMPLATE_INCLUDE_LOC, "#include \"player.h\"   /* your own file */" },
    { CCR_TEMPLATE_DEFINE,      "#define MAX_ENEMIES 10\nint enemies[MAX_ENEMIES];" },
    { CCR_TEMPLATE_CM_MINIMUM,  "cmake_minimum_required(VERSION 3.16)" },
    { CCR_TEMPLATE_CM_PROJECT,  "project(game LANGUAGES C)" },
    { CCR_TEMPLATE_CM_EXE,      "add_executable(${PROJECT_NAME} main.c player.c)" },
    { CCR_TEMPLATE_CM_LIB,      "add_library(engine STATIC engine.c)\ntarget_link_libraries(game PRIVATE engine)" },
    { CCR_TEMPLATE_CM_SUBDIR,   "add_subdirectory(src)   # src/CMakeLists.txt" },
    { CCR_TEMPLATE_CM_INSTALL,  "install(TARGETS ${PROJECT_NAME})" },
    { CCR_TEMPLATE_CM_SOURCES,  "target_sources(${PROJECT_NAME} PRIVATE enemy.c)" },
    { CCR_TEMPLATE_CM_INCDIRS,  "target_include_directories(${PROJECT_NAME} PRIVATE include)" },
    { CCR_TEMPLATE_CM_OPTIONS,  "target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)" },
    { CCR_TEMPLATE_CM_DEFINES,  "target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG=1)" },
    { CCR_TEMPLATE_CM_LINK,     "target_link_libraries(${PROJECT_NAME} PRIVATE m)   # math" },
    { CCR_TEMPLATE_CM_PROPS,    "set_target_properties(${PROJECT_NAME} PROPERTIES C_STANDARD 11)" },
    { CCR_TEMPLATE_CM_FIND,     "find_package(SDL2 REQUIRED)\ntarget_link_libraries(game PRIVATE SDL2::SDL2)" },
    { CCR_TEMPLATE_CM_THREADS,  "find_package(Threads REQUIRED)\ntarget_link_libraries(game PRIVATE Threads::Threads)" },
    { CCR_TEMPLATE_CM_PKG,      "find_package(PkgConfig REQUIRED)\npkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4)\ntarget_link_libraries(app PRIVATE PkgConfig::GTK)" },
    { CCR_TEMPLATE_CM_STD,      "set(CMAKE_C_STANDARD 11)" },
    { CCR_TEMPLATE_CM_SET,      "set(SOURCES main.c player.c)\nadd_executable(game ${SOURCES})" },
    { CCR_TEMPLATE_CM_OPTION,   "option(USE_SOUND \"Play sounds\" ON)\nif(USE_SOUND)\n  target_sources(game PRIVATE sound.c)\nendif()" },
    { CCR_TEMPLATE_CM_APPEND,   "list(APPEND SOURCES extra.c)" },
    { CCR_TEMPLATE_CM_GLOB,     "file(GLOB SOURCES CONFIGURE_DEPENDS src/*.c)" },
    { CCR_TEMPLATE_CM_MESSAGE,  "message(STATUS \"Building with sound\")" },
    { CCR_TEMPLATE_CM_IF,       "if(WIN32)\n  target_sources(game PRIVATE windows.c)\nendif()" },
    { CCR_TEMPLATE_CM_IF_ELSE,  "if(MSVC)\n  target_compile_options(game PRIVATE /W4)\nelse()\n  target_compile_options(game PRIVATE -Wall)\nendif()" },
    { CCR_TEMPLATE_CM_FOREACH,  "foreach(file IN LISTS SOURCES)\n  message(STATUS \"${file}\")\nendforeach()" },
    { CCR_TEMPLATE_CM_WHILE,    "set(n 0)\nwhile(n LESS 3)\n  math(EXPR n \"${n} + 1\")\nendwhile()" },
    { CCR_TEMPLATE_CM_FUNCTION, "function(add_game name)\n  add_executable(${name} ${name}.c)\nendfunction()" },
    { CCR_TEMPLATE_CM_RETURN,   "if(NOT USE_SOUND)\n  return()\nendif()" },
    { CCR_TEMPLATE_CM_LINE,     "math(EXPR answer \"6 * 7\")   # any CMake at all" },
};

static const char *const g_ids[CCR_TEMPLATE_COUNT] = {
#define X(id, cat, label) #id,
    CCR_TEMPLATES(X)
#undef X
};

const char *ccr_explain_key(ccr_template t)
{
    static char keys[CCR_TEMPLATE_COUNT][24];
    if ((unsigned)t >= CCR_TEMPLATE_COUNT)
        return "";
    if (!keys[t][0]) {
        size_t i = 0;
        for (; g_ids[t][i] && i + 1 < sizeof keys[t]; ++i)
            keys[t][i] = (char)tolower((unsigned char)g_ids[t][i]);
        keys[t][i] = '\0';
    }
    return keys[t];
}

const char *ccr_explain_example(ccr_template t)
{
    for (size_t i = 0; i < sizeof g_examples / sizeof g_examples[0]; ++i)
        if (g_examples[i].t == t)
            return g_examples[i].code;
    return "";
}

/* ------------------------------------------------------------ the words */

static char *g_what[CCR_TEMPLATE_COUNT], *g_how[CCR_TEMPLATE_COUNT];

void ccr_explain_free(void)
{
    for (int i = 0; i < CCR_TEMPLATE_COUNT; ++i) {
        free(g_what[i]);
        free(g_how[i]);
        g_what[i] = g_how[i] = NULL;
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int find_template(const char *key)
{
    for (int i = 0; i < CCR_TEMPLATE_COUNT; ++i)
        if (strcmp(ccr_explain_key((ccr_template)i), key) == 0)
            return i;
    return -1;
}

/* Reads one .explain file over what is there (`apply`), or only checks it.
 * Problems are counted and told to `log`; `seen` (if any) marks the keys. */
static int read_file(const char *path, bool apply, FILE *log, bool (*seen)[2])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (log)
            fprintf(log, "%s: cannot open\n", path);
        return 1;
    }
    int problems = 0, line_no = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        ++line_no;
        char *l = trim(line);
        if (!*l || *l == '#' || *l == '@')
            continue;
        char *eq = strchr(l, '=');
        char *dot = eq ? memchr(l, '.', (size_t)(eq - l)) : NULL;
        if (!eq || !dot) {
            ++problems;
            if (log)
                fprintf(log, "%s:%d: expected 'block.what = text' or 'block.how = text'\n", path, line_no);
            continue;
        }
        *eq = '\0';
        *dot = '\0';
        char *key = trim(l), *part = trim(dot + 1), *text = trim(eq + 1);
        const int t = find_template(key);
        const int which = strcmp(part, "what") == 0 ? 0 : strcmp(part, "how") == 0 ? 1 : -1;
        if (t < 0 || which < 0) {
            ++problems;
            if (log)
                fprintf(log, "%s:%d: unknown '%s.%s'\n", path, line_no, key, part);
            continue;
        }
        if (seen)
            seen[t][which] = true;
        if (!apply)
            continue;
        char *copy = malloc(strlen(text) + 1);
        if (!copy)
            continue;
        /* \n is a new line */
        char *o = copy;
        for (const char *p = text; *p; ++p) {
            if (p[0] == '\\' && p[1] == 'n') {
                *o++ = '\n';
                ++p;
            } else {
                *o++ = *p;
            }
        }
        *o = '\0';
        char **slot = which == 0 ? &g_what[t] : &g_how[t];
        free(*slot);
        *slot = copy;
    }
    fclose(f);
    return problems;
}

/* Where NAME is, among the language packs' folders. */
static bool find_file(const char *name, char *path, size_t cap)
{
    char dirs[4][1024];
    const int nd = ccr_config_data_dirs("lang", "CCRATCH_LANG_DIR", dirs, 4);
    for (int d = 0; d < nd; ++d)
        if (snprintf(path, cap, "%s/%s", dirs[d], name) < (int)cap && access(path, R_OK) == 0)
            return true;
    return false;
}

void ccr_explain_load(const char *code, bool plain)
{
    ccr_explain_free();
    char path[1100], name[64];
    if (find_file("en.explain", path, sizeof path))
        read_file(path, true, NULL, NULL);
    if (code && *code && strcmp(code, "en") != 0) {
        snprintf(name, sizeof name, "%s.explain", code);
        if (find_file(name, path, sizeof path))
            read_file(path, true, NULL, NULL);
    }
    if (plain && code && *code) {
        snprintf(name, sizeof name, "%s-plain.explain", code);
        if (find_file(name, path, sizeof path))
            read_file(path, true, NULL, NULL);
    }
}

const char *ccr_explain_what(ccr_template t)
{
    return (unsigned)t < CCR_TEMPLATE_COUNT && g_what[t] ? g_what[t] : "";
}

const char *ccr_explain_how(ccr_template t)
{
    return (unsigned)t < CCR_TEMPLATE_COUNT && g_how[t] ? g_how[t] : "";
}

int ccr_explain_codepoints(int *out, int cap)
{
    int n = 0;
    for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t)
        for (int which = 0; which < 2; ++which) {
            const unsigned char *s = (const unsigned char *)(which ? g_how[t] : g_what[t]);
            while (s && *s) {
                unsigned c = *s;
                int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : 3;
                c &= extra == 0 ? 0x7Fu : extra == 1 ? 0x1Fu : extra == 2 ? 0x0Fu : 0x07u;
                ++s;
                for (int k = 0; k < extra && *s; ++k, ++s)
                    c = (c << 6) | (*s & 0x3Fu);
                if (c < 0x80)
                    continue;
                bool seen = false;
                for (int k = 0; k < n && !seen; ++k)
                    seen = out[k] == (int)c;
                if (!seen && n < cap)
                    out[n++] = (int)c;
            }
        }
    return n;
}

int ccr_explain_check(const char *path, bool complete, FILE *out)
{
    static bool seen[CCR_TEMPLATE_COUNT][2];
    memset(seen, 0, sizeof seen);
    int problems = read_file(path, false, out, seen);
    if (complete)
        for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t)
            for (int which = 0; which < 2; ++which)
                if (!seen[t][which]) {
                    ++problems;
                    if (out)
                        fprintf(out, "%s: missing '%s.%s'\n", path, ccr_explain_key((ccr_template)t),
                                which ? "how" : "what");
                }
    if (problems == 0 && out)
        fprintf(out, "%s: no problems\n", path);
    return problems;
}
