/*
 * Code Hater - Ccratch's first official plugin.
 *
 * A borrow checker, but for C, and in a much worse mood. It reads the tree
 * Ccratch draws blocks from and says, the way the old mailing lists did,
 * what is wrong: pointers to dead stack frames, use after free, double
 * free, leaks, realloc into the same pointer, gets(), strcpy(), void main,
 * VLAs, four levels of nesting, unchecked malloc... and anything that
 * smells of Rust. Every rant ends with how to fix it: it hates the code,
 * not you. Mostly.
 *
 * Plain C, no libraries, nothing kept between calls: a model plugin.
 */
#include "ccratch/ccr_plugin.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NONE CCR_PLUGIN_NONE

/* ------------------------------------------------------------- the rants
 *
 * {x} is the name it is about, {n} a number. Two languages for now: what
 * the interface speaks if it is Russian, English for everyone else.
 */
enum {
    R_DANGLING, R_RETURN_ARRAY, R_UAF, R_DOUBLE_FREE, R_LEAK, R_REALLOC, R_GETS, R_STRCPY, R_SPRINTF,
    R_ATOI, R_ALLOCA, R_ASSIGN_COND, R_NESTING, R_LONG_FN, R_LOCALS, R_VOID_MAIN, R_UNCHECKED, R_VLA,
    R_RUST_NAME, R_RUST_TEXT, R_GOTO, R_GOTO_BACK, R_MALLOC_CAST, R_SIZEOF_TYPE, R_EMPTY_PARAMS,
    R_RAW_NEW, R_USING_HEADER, R_COUNT
};

typedef struct rant {
    ccr_finding_level level;
    const char       *title[2];   /* en, ru */
    const char       *say[2][2];  /* up to two ways to say it */
} rant;

static const rant g_rants[R_COUNT] = {
    [R_DANGLING] = { CCR_FINDING_ERROR,
        { "NAK: returns the address of a local", "NAK: адрес локальной переменной наружу" },
        { { "You return &{x}. {x} lives on the stack, and that stack frame dies the moment this function "
            "returns. The caller gets a pointer to a grave. This is not a subtle bug, this is a bug that takes "
            "its hat off and introduces itself. Make {x} static, malloc it, or have the caller pass the memory in.",
            "Returning a pointer to a local variable. In this day and age. The Rust crowd will now say their "
            "compiler catches this. Yes. It also takes four minutes to build hello world. Ours builds in a "
            "second and you still got it wrong. Return a value, or take the buffer as a parameter." },
          { "Ты возвращаешь &{x}. {x} живёт на стеке, а стек этой функции умирает в ту же секунду, как она "
            "вернулась. Вызывающий получает указатель на могилу. Это не тонкий баг, это баг, который снимает "
            "шляпу и представляется. Сделай {x} static, выдели через malloc или пусть вызывающий сам даст память.",
            "Указатель на локальную переменную наружу. В наше-то время. Сейчас придут растеры и скажут, что их "
            "компилятор такое ловит. Да. А ещё он собирает hello world четыре минуты. Наш собирает за секунду, "
            "а ты всё равно накосячил. Верни значение или принимай буфер параметром." } } },
    [R_RETURN_ARRAY] = { CCR_FINDING_ERROR,
        { "NAK: returns a local array", "NAK: локальный массив наружу" },
        { { "{x} is an array on this function's stack, and you hand it to the caller as if it would still be "
            "there. It won't. The very next call will scribble all over it. Take the buffer as a parameter, "
            "like every sane C API since 1978.", NULL },
          { "{x} — массив на стеке этой функции, а ты отдаёшь его наружу, будто он там останется. Не останется. "
            "Первый же следующий вызов его перезапишет. Принимай буфер параметром, как любой вменяемый C API "
            "с 1978 года.", NULL } } },
    [R_UAF] = { CCR_FINDING_ERROR,
        { "NAK: use after free", "NAK: обращение после free" },
        { { "{x} was freed a few lines up, and here you are, using it again. That memory belongs to the "
            "allocator now. Today it works, tomorrow it's someone else's data, next week it's a CVE with your "
            "name on it. Set {x} = NULL right after free() so this crashes loudly instead of lying quietly.",
            "Use after free. The single most boring way to corrupt memory, and you found it anyway. Every Rust "
            "evangelist on the planet just felt a disturbance and started writing a blog post. Don't give them "
            "the material: once {x} is freed, stop touching it." },
          { "{x} освободили парой строк выше, а ты опять им пользуешься. Эта память теперь принадлежит "
            "аллокатору. Сегодня работает, завтра там чужие данные, через неделю — CVE с твоим именем. Пиши "
            "{x} = NULL сразу после free(), чтобы падало громко, а не врало тихо.",
            "Обращение после free. Самый скучный способ испортить память, и ты его всё-таки нашёл. Где-то "
            "сейчас вздрогнул каждый растер и сел писать пост в блог. Не давай им материал: освободил {x} — "
            "не трогай." } } },
    [R_DOUBLE_FREE] = { CCR_FINDING_ERROR,
        { "NAK: double free", "NAK: двойной free" },
        { { "free({x}). And then free({x}) again. Did you think it didn't hear you the first time? A double "
            "free corrupts the heap, and a corrupted heap goes off three hours later in completely unrelated "
            "code. Free it once, then set {x} = NULL, so a second free() does nothing.", NULL },
          { "free({x}). А потом ещё раз free({x}). Думал, с первого раза не услышал? Двойное освобождение "
            "ломает кучу, а сломанная куча стреляет через три часа совсем в другом месте. Освобождай один раз "
            "и сразу пиши {x} = NULL — тогда второй free() ничего не сломает.", NULL } } },
    [R_LEAK] = { CCR_FINDING_WARNING,
        { "memory leak", "утечка памяти" },
        { { "{x} gets memory from the allocator and nobody ever gives it back. Not freed, not returned, not "
            "stored anywhere. It just leaks. Do this in a loop and watch the memory graph climb like a crypto "
            "chart in a bubble. free({x}) when you're done with it.",
            "You malloc {x} and then forget it exists. This is how programs end up eating 8 GB to show one "
            "text field. Ownership isn't a Rust invention, it's just something Rust made mandatory because "
            "people kept doing exactly this. Free it." },
          { "{x} берёт память у аллокатора, и никто её не возвращает. Ни free, ни return, никуда не "
            "сохранили. Просто утекло. Сделай так в цикле и любуйся, как график памяти растёт, как крипта в "
            "пузырь. Закончил с {x} — free({x}).",
            "Ты делаешь malloc для {x} и тут же забываешь, что он существует. Вот так программы и жрут 8 ГБ, "
            "чтобы показать одно поле ввода. Владение памятью — не изобретение Rust, это просто то, что в Rust "
            "сделали обязательным, потому что все делали ровно вот так. Освободи." } } },
    [R_REALLOC] = { CCR_FINDING_WARNING,
        { "realloc into the same pointer", "realloc в тот же указатель" },
        { { "{x} = realloc({x}, ...). When realloc fails it returns NULL and keeps the old block, which you "
            "just overwrote with that NULL. Congratulations: out of memory AND a leak, in one line. Use a "
            "temporary: void *tmp = realloc({x}, n); if (!tmp) handle it; {x} = tmp;", NULL },
          { "{x} = realloc({x}, ...). Когда realloc не может, он возвращает NULL и оставляет старый блок — "
            "который ты только что затёр этим NULL. Поздравляю: и памяти нет, и утечка, в одной строке. Через "
            "временную: void *tmp = realloc({x}, n); if (!tmp) разберись; {x} = tmp;", NULL } } },
    [R_GETS] = { CCR_FINDING_ERROR,
        { "NAK: gets()", "NAK: gets()" },
        { { "gets(). GETS. This function was so bad they removed it from the language in C11, which is "
            "something the C committee does about once per geological era. It has no idea how big your "
            "buffer is, so any long line walks straight over your stack. fgets({x}, sizeof {x}, stdin). Today.",
            NULL },
          { "gets(). GETS. Эта функция настолько плохая, что её выкинули из языка в C11, а комитет C делает "
            "такое примерно раз в геологическую эпоху. Она понятия не имеет, какой у тебя буфер, так что любая "
            "длинная строка топчется прямо по стеку. fgets({x}, sizeof {x}, stdin). Сегодня же.", NULL } } },
    [R_STRCPY] = { CCR_FINDING_WARNING,
        { "{x}(): no length", "{x}(): без длины" },
        { { "{x}() copies until it finds a zero byte and doesn't care where your buffer ends. That's the "
            "whole bug, in one function name. Use snprintf(dst, sizeof dst, \"%s\", src), or memcpy with a "
            "length you actually checked.", NULL },
          { "{x}() копирует, пока не встретит нулевой байт, и ему плевать, где кончается твой буфер. Вот и "
            "весь баг, в одном названии функции. snprintf(dst, sizeof dst, \"%s\", src) или memcpy с длиной, "
            "которую ты реально проверил.", NULL } } },
    [R_SPRINTF] = { CCR_FINDING_WARNING,
        { "{x}() without a size", "{x}() без размера" },
        { { "{x}() writes as much as the format produces, into a buffer whose size it doesn't know. snprintf "
            "exists. It has existed since 1999. It is older than some of the people writing Rust. Use it.",
            NULL },
          { "{x}() пишет столько, сколько выдал формат, в буфер, размера которого не знает. snprintf "
            "существует. С 1999 года. Он старше некоторых людей, пишущих на Rust. Пользуйся.", NULL } } },
    [R_ATOI] = { CCR_FINDING_NIT,
        { "{x}() can't fail", "{x}() не умеет ошибаться" },
        { { "{x}(\"banana\") is 0. {x}(\"0\") is also 0. Good luck telling them apart. strtol tells you "
            "where it stopped and whether it overflowed.", NULL },
          { "{x}(\"банан\") — это 0. {x}(\"0\") — тоже 0. Удачи их различить. strtol скажет, где "
            "остановился и не было ли переполнения.", NULL } } },
    [R_ALLOCA] = { CCR_FINDING_WARNING,
        { "alloca()", "alloca()" },
        { { "alloca() puts as much as you ask for on the stack, and it cannot say no. Ask for too much and "
            "you don't get NULL, you get a stack overflow somewhere else. Use malloc, or a fixed-size array "
            "you can reason about.", NULL },
          { "alloca() кладёт на стек сколько попросишь и не умеет говорить «нет». Попросишь много — получишь "
            "не NULL, а переполнение стека где-то ещё. malloc или массив фиксированного размера, про который "
            "можно подумать.", NULL } } },
    [R_ASSIGN_COND] = { CCR_FINDING_WARNING,
        { "= in a condition", "= в условии" },
        { { "That's =, not ==. You're not comparing {x}, you're overwriting it, and the condition is whatever "
            "you just stored. If you really meant to assign, wrap it in a second pair of parentheses so the "
            "next reader doesn't have to guess. If you didn't, you're welcome.", NULL },
          { "Это =, а не ==. Ты не сравниваешь {x}, ты его перезаписываешь, и условие — это то, что ты туда "
            "положил. Если правда хотел присвоить — оберни во вторые скобки, чтобы следующий читатель не "
            "гадал. Если нет — не благодари.", NULL } } },
    [R_NESTING] = { CCR_FINDING_NIT,
        { "four levels deep", "четыре этажа вложенности" },
        { { "Four levels of nesting. If you need more than three levels of indentation, the code is already "
            "in trouble, and you should fix the program, not your screen width. Pull the inside out into a "
            "function, or return early.", NULL },
          { "Четыре уровня вложенности. Если нужно больше трёх уровней отступа, код уже в беде, и чинить "
            "надо программу, а не ширину экрана. Вынеси внутренности в функцию или выходи пораньше через "
            "return.", NULL } } },
    [R_LONG_FN] = { CCR_FINDING_NIT,
        { "{n} lines in one function", "{n} строк в одной функции" },
        { { "{x}() is {n} lines long. A function should do one thing and fit on a screen or two. This one "
            "does several and needs a scroll wheel. Split it; the compiler will inline it back if it cares.",
            NULL },
          { "{x}() длиной {n} строк. Функция должна делать одно дело и влезать в экран-два. Эта делает "
            "несколько и требует колёсика мыши. Разбей; если компилятору надо, он сам заинлайнит обратно.",
            NULL } } },
    [R_LOCALS] = { CCR_FINDING_NIT,
        { "{n} local variables", "{n} локальных переменных" },
        { { "{x}() has {n} local variables. Nobody can keep that many in their head: not you, not me, and "
            "certainly not you in a month. It's a sign the function is doing too much.", NULL },
          { "У {x}() {n} локальных переменных. Столько в голове не удержит никто: ни ты, ни я, ни уж точно "
            "ты через месяц. Это знак, что функция делает слишком много.", NULL } } },
    [R_VOID_MAIN] = { CCR_FINDING_ERROR,
        { "NAK: void main", "NAK: void main" },
        { { "void main. main returns int. It has always returned int. That value goes to whoever started your "
            "program, and right now you're handing them garbage. Some textbook from 1995 lied to you; it's "
            "time to let it go.", NULL },
          { "void main. main возвращает int. Всегда возвращал int. Это значение получает тот, кто запустил "
            "программу, а ты сейчас отдаёшь ему мусор. Тебе соврал какой-то учебник 1995 года; пора его "
            "отпустить.", NULL } } },
    [R_UNCHECKED] = { CCR_FINDING_WARNING,
        { "malloc's result not checked", "результат malloc не проверен" },
        { { "{x} comes from the allocator and goes straight to work without a NULL check. malloc can fail. "
            "When it does, this dereferences NULL and the program dies of a segfault without a word. "
            "if (!{x}) handle it. It's one line.", NULL },
          { "{x} пришёл от аллокатора и сразу пошёл в дело, без проверки на NULL. malloc может не дать "
            "память. Тогда тут разыменуется NULL, и программа молча сдохнет от segfault. if (!{x}) — "
            "разберись. Одна строка.", NULL } } },
    [R_VLA] = { CCR_FINDING_WARNING,
        { "variable-length array", "массив переменной длины" },
        { { "{x} is a variable-length array: its size is decided at run time, on the stack, and it cannot "
            "fail. Pass in a big number and the stack is gone. The Linux kernel got rid of every single VLA "
            "for exactly this reason. Use malloc, or a fixed maximum.", NULL },
          { "{x} — массив переменной длины: размер решается во время работы, на стеке, и отказать он не "
            "умеет. Передай большое число — и стека нет. Из ядра Linux выпилили все VLA ровно поэтому. "
            "malloc или фиксированный максимум.", NULL } } },
    [R_RUST_NAME] = { CCR_FINDING_ERROR,
        { "NAK: Rust detected", "NAK: обнаружен Rust" },
        { { "{x}. I see what you're doing. This is C. We don't unwrap things here, we check return values "
            "like adults. Rename it before somebody suggests rewriting the whole project in Rust.",
            "An identifier called {x}. Is this a C program or a crab's diary? There is no borrow checker "
            "here. The only thing checking your borrows is me, and I'm in a bad mood." },
          { "{x}. Я вижу, что ты делаешь. Это C. Тут ничего не unwrap-ают, тут проверяют возвращаемые "
            "значения, как взрослые. Переименуй, пока кто-нибудь не предложил переписать весь проект на Rust.",
            "Идентификатор по имени {x}. Это программа на C или дневник краба? Борроу-чекера тут нет. "
            "Единственный, кто проверяет твои заимствования, — это я, и я сегодня не в духе." } } },
    [R_RUST_TEXT] = { CCR_FINDING_ERROR,
        { "NAK: Rust propaganda", "NAK: пропаганда Rust" },
        { { "\"{x}\". No. We are not rewriting it in Rust. The answer was no last time, it's no now, and it "
            "will still be no when the borrow checker finally finishes compiling your dependencies.",
            "Found \"{x}\" in this file. Let me be very clear: this is a C editor. If you want to fight a "
            "borrow checker for three days to write a linked list, there is a whole other ecosystem for that, "
            "and its mascot is a crab." },
          { "«{x}». Нет. Мы не переписываем это на Rust. В прошлый раз ответ был «нет», сейчас «нет», и будет "
            "«нет», когда борроу-чекер наконец докомпилирует твои зависимости.",
            "В этом файле нашлось «{x}». Скажу очень ясно: это редактор C. Если хочешь три дня воевать с "
            "борроу-чекером ради односвязного списка, для этого есть целая отдельная экосистема, и у неё "
            "маскот — краб." } } },
    [R_GOTO] = { CCR_FINDING_PRAISE,
        { "a goto, finally", "goto, наконец-то" },
        { { "A goto. Finally, someone who isn't scared of it. A jump to one cleanup label is how the Linux "
            "kernel unwinds errors, and it reads better than five nested ifs. Don't let anyone waving a 1968 "
            "essay tell you otherwise. Just jump forward, to cleanup, and nowhere else.", NULL },
          { "goto. Наконец-то кто-то его не боится. Прыжок на одну метку с очисткой — так ядро Linux "
            "разгребает ошибки, и читается это лучше пяти вложенных if. Не слушай никого, кто размахивает "
            "статьёй 1968 года. Только прыгай вперёд, к очистке, и больше никуда.", NULL } } },
    [R_GOTO_BACK] = { CCR_FINDING_WARNING,
        { "goto backwards", "goto назад" },
        { { "goto {x}, and {x} is above it. A goto that jumps back up is a loop in a cheap disguise. Forward, "
            "to one cleanup label at the end: fine, the kernel does it all day. Backward: write the while loop "
            "like a civilised person, so the next reader can see it's a loop.", NULL },
          { "goto {x}, а {x} выше. goto, который прыгает назад, — это цикл в дешёвом гриме. Вперёд, на одну "
            "метку с очисткой в конце, — пожалуйста, ядро так делает целыми днями. Назад — напиши while, как "
            "цивилизованный человек, чтобы следующий читатель видел, что это цикл.", NULL } } },
    [R_MALLOC_CAST] = { CCR_FINDING_NIT,
        { "cast of {x}()'s result", "каст результата {x}()" },
        { { "You cast what {x}() returns. This is C, not C++: void * turns into any object pointer by "
            "itself. The cast adds nothing, and on old compilers it even hid a missing #include <stdlib.h>. "
            "Drop it.", NULL },
          { "Ты приводишь результат {x}() к типу. Это C, а не C++: void * сам превращается в любой указатель. "
            "Каст ничего не даёт, а на старых компиляторах ещё и прятал забытый #include <stdlib.h>. Убери.",
            NULL } } },
    [R_SIZEOF_TYPE] = { CCR_FINDING_NIT,
        { "sizeof(type), not sizeof *{x}", "sizeof(тип), а не sizeof *{x}" },
        { { "The size for {x} is spelled as a type. Change {x}'s type one day and this silently allocates the "
            "wrong amount. sizeof *{x} always follows the pointer: free bug prevention, zero effort.", NULL },
          { "Размер для {x} записан через тип. Поменяешь когда-нибудь тип {x} — и тут молча выделится не "
            "столько. sizeof *{x} всегда следует за указателем: бесплатная защита от бага, ноль усилий.",
            NULL } } },
    [R_RAW_NEW] = { CCR_FINDING_NIT,
        { "raw new", "голый new" },
        { { "new {x}. And who deletes it? You, later, on every path out, including the ones an exception "
            "takes. You won't. Nobody does. Put it in a std::vector, or std::make_unique<{x}>(...), and the "
            "memory goes away by itself when nobody needs it.", NULL },
          { "new {x}. А удалит кто? Ты, потом, на каждом пути выхода, включая те, где летит исключение. Не "
            "удалишь. Никто не удаляет. Положи в std::vector или std::make_unique<{x}>(...) — и память "
            "уйдёт сама, когда станет никому не нужна.", NULL } } },
    [R_USING_HEADER] = { CCR_FINDING_WARNING,
        { "using namespace in a header", "using namespace в заголовке" },
        { { "using namespace {x}; in a header. Every file that includes this one now has all of {x} dumped "
            "into its global namespace, whether it asked or not. Names collide in files you've never seen. "
            "Write {x}:: in headers; save the using for your own .cpp.", NULL },
          { "using namespace {x}; в заголовке. Теперь каждый файл, который его подключит, получает весь {x} в "
            "глобальное пространство имён, хотел он того или нет. Имена конфликтуют в файлах, которых ты в "
            "глаза не видел. В заголовках пиши {x}::, а using оставь для своих .cpp.", NULL } } },
    [R_EMPTY_PARAMS] = { CCR_FINDING_NIT,
        { "() is not (void)", "() — это не (void)" },
        { { "{x}() is declared with (). In C that doesn't mean 'no arguments', it means 'any arguments, and "
            "the compiler won't check them'. Write (void). This isn't C++.", NULL },
          { "{x}() объявлена с (). В C это значит не «без аргументов», а «с любыми, и компилятор не "
            "проверит». Пиши (void). Это не C++.", NULL } } },
};

/* ------------------------------------------------------------ the tree */

static const ccr_plugin_file *F;
static int g_lang;                 /* 0 en, 1 ru */
static bool g_cpp;                 /* the file is C++: f() means f(void), casts on malloc are needed */
static int g_said[R_COUNT];        /* per rant, this file */
static int g_total;

#define MAX_PER_RANT 12
#define MAX_TOTAL    80

static const ccr_plugin_node *N(uint32_t i)
{
    return &F->nodes[i];
}

static bool is(uint32_t i, const char *kind)
{
    return i != NONE && strcmp(N(i)->kind, kind) == 0;
}

static bool named(uint32_t i, const char *name)
{
    return i != NONE && strcmp(N(i)->name, name) == 0;
}

static uint32_t child(uint32_t i, int k)
{
    if (i == NONE)
        return NONE;
    uint32_t c = N(i)->first_child;
    while (c != NONE && k-- > 0)
        c = N(c)->next_sibling;
    return c;
}

static int index_in_parent(uint32_t i)
{
    int k = 0;
    for (uint32_t c = N(N(i)->parent)->first_child; c != NONE && c != i; c = N(c)->next_sibling)
        ++k;
    return k;
}

static bool outside(uint32_t i)
{
    return (N(i)->flags & CCR_PNODE_OUTSIDE) != 0;
}

/* (int *)malloc(...) -> malloc(...) */
static uint32_t uncast(uint32_t i)
{
    while (is(i, "Cast"))
        i = child(i, 0);
    return i;
}

static uint32_t function_of(uint32_t i)
{
    while (i != NONE && !is(i, "Function"))
        i = N(i)->parent;
    return i;
}

/* The statement `i` is in: the node whose parent is a Block (or the file). */
static uint32_t statement_of(uint32_t i)
{
    while (i != NONE && N(i)->parent != NONE && !is(N(i)->parent, "Block") && !is(N(i)->parent, "TranslationUnit"))
        i = N(i)->parent;
    return i;
}

/* Does the subtree at `i` hold a Ref to `name` (not under sizeof)? */
static bool mentions(uint32_t i, const char *name)
{
    if (i == NONE || is(i, "Sizeof"))
        return false;
    if (is(i, "Ref") && named(i, name))
        return true;
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling)
        if (mentions(c, name))
            return true;
    return false;
}

static bool in_list(const char *s, const char *const *list)
{
    for (; *list; ++list)
        if (strcmp(s, *list) == 0)
            return true;
    return false;
}

static const char *const g_allocators[] = { "malloc", "calloc", "realloc", "strdup", "strndup", "aligned_alloc", NULL };

/* Functions that look at a pointer and give it back: passing it to them
 * does not hand the memory over. */
static const char *const g_borrowers[] = {
    "printf", "fprintf", "sprintf", "snprintf", "vsnprintf", "puts", "fputs", "fputc", "putc", "strcpy",
    "strncpy", "strcat", "strncat", "strlen", "strnlen", "strcmp", "strncmp", "strcasecmp", "strchr", "strrchr",
    "strstr", "strspn", "strcspn", "strtok", "memcpy", "memmove", "memset", "memcmp", "memchr", "fgets", "fread",
    "fwrite", "scanf", "sscanf", "fscanf", "atoi", "atol", "atof", "strtol", "strtoul", "strtod", "qsort",
    "bsearch", "toupper", "tolower", "assert", "perror", "fopen", "write", "read", NULL
};

/* Uses the memory behind `name`: *p, p->x, p[i], or a str/mem function. */
static bool derefs(uint32_t i, const char *name)
{
    if (i == NONE || is(i, "Sizeof"))
        return false;
    if ((is(i, "UnaryOp") && strcmp(N(i)->op, "*") == 0) || (is(i, "Member") && (N(i)->flags & CCR_PNODE_ARROW)) ||
        is(i, "Index")) {
        const uint32_t base = uncast(child(i, 0));
        if (is(base, "Ref") && named(base, name))
            return true;
    }
    if (is(i, "Call") && in_list(N(i)->name, g_borrowers) && !named(i, "printf") && !named(i, "assert"))
        for (uint32_t a = N(i)->first_child; a != NONE; a = N(a)->next_sibling)
            if (is(uncast(a), "Ref") && named(uncast(a), name))
                return true;
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling)
        if (derefs(c, name))
            return true;
    return false;
}

/* ------------------------------------------------------------- saying it */

static unsigned hash(const char *s, unsigned h)
{
    while (*s)
        h = h * 31u + (unsigned char)*s++;
    return h;
}

static void fill(char *out, size_t cap, const char *tmpl, const char *x, int n)
{
    size_t o = 0;
    char num[16];
    snprintf(num, sizeof num, "%d", n);
    for (const char *p = tmpl; *p && o + 1 < cap;) {
        const char *put = NULL;
        if (strncmp(p, "{x}", 3) == 0)
            put = x;
        else if (strncmp(p, "{n}", 3) == 0)
            put = num;
        if (put) {
            for (; *put && o + 1 < cap; ++put)
                out[o++] = *put;
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

static void say(uint32_t node, int r, const char *x, int n)
{
    if (node == NONE || outside(node) || g_said[r] >= MAX_PER_RANT || g_total >= MAX_TOTAL)
        return;
    ++g_said[r];
    ++g_total;
    const rant *ra = &g_rants[r];
    /* the same block gets the same words every time: no flicker */
    const unsigned h = hash(x ? x : "", (unsigned)N(node)->line * 131u + (unsigned)r);
    const char *tmpl = ra->say[g_lang][1] && (h & 1u) ? ra->say[g_lang][1] : ra->say[g_lang][0];
    char title[128], msg[1200];
    fill(title, sizeof title, ra->title[g_lang], x ? x : "", n);
    fill(msg, sizeof msg, tmpl, x ? x : "", n);
    F->report(F, node, ra->level, title, msg);
}

/* --------------------------------------------------------------- checks */

static bool is_control(uint32_t i)
{
    return is(i, "If") || is(i, "For") || is(i, "While") || is(i, "DoWhile") || is(i, "Switch");
}

/* `else if`: an If that is the else of the If around it (maybe in a Block
 * of its own). Same level as that one. */
static bool is_else_if(uint32_t i)
{
    if (!is(i, "If"))
        return false;
    uint32_t p = N(i)->parent;
    if (is(p, "Block") && N(p)->first_child == i && N(i)->next_sibling == NONE && is(N(p)->parent, "If") &&
        index_in_parent(p) == 2)
        return true;
    return is(p, "If") && index_in_parent(i) == 2;
}

/* Is the memory in `name` (allocated by Var `v`) given back somewhere in
 * the subtree at `i`: freed, returned, stored, handed to a function that
 * may keep it? */
static bool kept_in(uint32_t i, uint32_t v, const char *name)
{
    if (i == NONE || i == v || outside(i))
        return false;   /* not its own initialiser */
    if (is(i, "Ref") && named(i, name)) {
        uint32_t at = i, p = N(i)->parent;
        while (is(p, "Cast") || (is(p, "UnaryOp") && strcmp(N(p)->op, "&") == 0)) {
            at = p;
            p = N(p)->parent;
        }
        if (is(p, "Call"))
            return named(p, "free") || !in_list(N(p)->name, g_borrowers);
        if (is(p, "Return") || is(p, "Var") || is(p, "InitList") || is(p, "CompoundLiteral"))
            return true;
        if (is(p, "BinaryOp") && strcmp(N(p)->op, "=") == 0)
            return index_in_parent(at) == 1;   /* stored somewhere */
        return false;
    }
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling)
        if (kept_in(c, v, name))
            return true;
    return false;
}

/* The first Ref to `name` in the subtree at `i`, or NONE. */
static uint32_t first_ref(uint32_t i, const char *name)
{
    if (i == NONE || is(i, "Sizeof"))
        return NONE;
    if (is(i, "Ref") && named(i, name))
        return i;
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling) {
        const uint32_t r = first_ref(c, name);
        if (r != NONE)
            return r;
    }
    return NONE;
}

/* The Var `name` declared in the subtree at `i`, or NONE. */
static uint32_t find_var(uint32_t i, const char *name)
{
    if (i == NONE)
        return NONE;
    if (is(i, "Var") && named(i, name))
        return i;
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling) {
        const uint32_t r = find_var(c, name);
        if (r != NONE)
            return r;
    }
    return NONE;
}

/* Every Var in a function, and what it starts as. */
static void check_var(uint32_t v)
{
    const char *name = N(v)->name;
    const char *type = N(v)->type;
    /* a VLA: "char[n]" - a constant one is spelled with its number */
    const char *br = strchr(type, '[');
    if (br && br[1] != ']') {
        bool digits = true;
        for (const char *p = br + 1; *p && *p != ']'; ++p)
            digits &= isdigit((unsigned char)*p) != 0;
        if (!digits && function_of(v) != NONE)
            say(v, R_VLA, name, 0);
    }
    /* what it starts as */
    uint32_t init = NONE;
    for (uint32_t c = N(v)->first_child; c != NONE; c = N(c)->next_sibling)
        if (N(c)->flags & CCR_PNODE_INIT)
            init = c;
    const uint32_t call = uncast(init);
    if (!is(call, "Call") || !in_list(N(call)->name, g_allocators))
        return;
    if (is(init, "Cast") && !g_cpp)   /* C++ needs that cast */
        say(v, R_MALLOC_CAST, N(call)->name, 0);
    /* malloc(n * sizeof(int)) for an int *: sizeof *p would follow it */
    if (named(call, "malloc") || named(call, "calloc")) {
        for (uint32_t a = N(call)->first_child; a != NONE; a = N(a)->next_sibling) {
            /* a type-only sizeof has no child */
            bool typed = is(a, "Sizeof") && N(a)->first_child == NONE;
            for (uint32_t b = N(a)->first_child; b != NONE && !typed; b = N(b)->next_sibling)
                typed = is(b, "Sizeof") && N(b)->first_child == NONE;
            if (typed) {
                say(v, R_SIZEOF_TYPE, name, 0);
                break;
            }
        }
    }
    const uint32_t fn = function_of(v);
    if (fn == NONE)
        return;

    /* the first statement after it that touches it: is it a check? */
    uint32_t stmt = statement_of(v);
    for (uint32_t t = stmt != NONE ? N(stmt)->next_sibling : NONE; t != NONE; t = N(t)->next_sibling) {
        if (!mentions(t, name))
            continue;
        const bool checks = ((is(t, "If") || is(t, "While") || is(t, "Return")) && mentions(child(t, 0), name)) ||
                            (is(t, "Call") && named(t, "assert"));
        if (!checks && derefs(t, name))
            say(t, R_UNCHECKED, name, 0);
        break;
    }

    /* given back? freed, returned, stored, handed to someone */
    if (!kept_in(fn, v, name))
        say(v, R_LEAK, name, 0);
}

/* After free(p) in a Block: what the statements after it do with p. */
static void check_free(uint32_t call)
{
    const uint32_t arg = uncast(child(call, 0));
    if (!is(arg, "Ref"))
        return;
    const char *name = N(arg)->name;
    const uint32_t stmt = statement_of(call);
    if (stmt != call && !is(N(stmt)->parent, "Block"))
        return;
    for (uint32_t t = N(stmt)->next_sibling; t != NONE; t = N(t)->next_sibling) {
        /* p = something: alive again (unless something is p itself) */
        if (is(t, "BinaryOp") && strcmp(N(t)->op, "=") == 0 && is(uncast(child(t, 0)), "Ref") &&
            named(uncast(child(t, 0)), name)) {
            if (mentions(child(t, 1), name))
                say(t, R_UAF, name, 0);
            return;
        }
        if (is(t, "Call") && named(t, "free") && is(uncast(child(t, 0)), "Ref") && named(uncast(child(t, 0)), name)) {
            say(t, R_DOUBLE_FREE, name, 0);
            return;
        }
        const uint32_t ref = first_ref(t, name);
        if (ref != NONE) {   /* on the innermost statement that does it */
            say(statement_of(ref), R_UAF, name, 0);
            return;
        }
    }
}

static void check_call(uint32_t c)
{
    const char *f = N(c)->name;
    if (strcmp(f, "gets") == 0) {
        const uint32_t a = uncast(child(c, 0));
        say(statement_of(c), R_GETS, is(a, "Ref") ? N(a)->name : "buf", 0);
    } else if (strcmp(f, "strcpy") == 0 || strcmp(f, "strcat") == 0 || strcmp(f, "wcscpy") == 0) {
        say(statement_of(c), R_STRCPY, f, 0);
    } else if (strcmp(f, "sprintf") == 0 || strcmp(f, "vsprintf") == 0) {
        say(statement_of(c), R_SPRINTF, f, 0);
    } else if (strcmp(f, "atoi") == 0 || strcmp(f, "atol") == 0 || strcmp(f, "atof") == 0) {
        say(statement_of(c), R_ATOI, f, 0);
    } else if (strcmp(f, "alloca") == 0) {
        say(statement_of(c), R_ALLOCA, f, 0);
    } else if (strcmp(f, "free") == 0) {
        check_free(c);
    }
}

/* return &local; return local_array; */
static void check_return(uint32_t r)
{
    const uint32_t fn = function_of(r);
    uint32_t e = uncast(child(r, 0));
    const bool addr = is(e, "UnaryOp") && strcmp(N(e)->op, "&") == 0;
    if (addr)
        e = uncast(child(e, 0));
    if (fn == NONE || !is(e, "Ref"))
        return;
    const char *name = N(e)->name;
    const uint32_t k = find_var(fn, name);
    {
        if (k == NONE)
            return;   /* a parameter, a global: not this function's stack */
        /* static lives on: say nothing */
        const ccr_plugin_node *v = N(k);
        if (v->src_end > v->src_begin) {
            uint32_t b = v->src_begin;
            while (b > 0 && F->source[b - 1] != '\n' && F->source[b - 1] != ';' && F->source[b - 1] != '{')
                --b;
            const size_t len = v->src_end - b;
            char head[160];
            snprintf(head, sizeof head, "%.*s", (int)(len < sizeof head - 1 ? len : sizeof head - 1), F->source + b);
            if (strstr(head, "static"))
                return;
        }
        if (addr)
            say(r, R_DANGLING, name, 0);
        else if (strchr(v->type, '['))
            say(r, R_RETURN_ARRAY, name, 0);
    }
}

typedef struct fn_stats {
    uint32_t last;     /* its last line */
    int      locals;
    uint32_t deep;     /* the first statement four levels in */
    uint32_t jump;     /* the first goto forward */
    uint32_t back;     /* the first goto backward */
    uint32_t fn;
} fn_stats;

/* The Label `name` in the subtree at `i`, or NONE. */
static uint32_t find_label(uint32_t i, const char *name)
{
    if (i == NONE)
        return NONE;
    if (is(i, "Label") && named(i, name))
        return i;
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling) {
        const uint32_t r = find_label(c, name);
        if (r != NONE)
            return r;
    }
    return NONE;
}

/* Walks a function's body once; `level`: control statements around. */
static void stats(uint32_t i, int level, fn_stats *st)
{
    if (i == NONE || outside(i))
        return;
    if (N(i)->line > st->last)
        st->last = N(i)->line;
    st->locals += is(i, "Var");
    if (is_control(i) && !is_else_if(i)) {
        ++level;
        if (level >= 4 && st->deep == NONE)
            st->deep = i;
    }
    if (is(i, "Goto")) {
        const uint32_t label = find_label(st->fn, N(i)->name);
        const bool back = label != NONE && N(label)->line > 0 && N(label)->line < N(i)->line;
        if (back && st->back == NONE)
            st->back = i;
        else if (!back && st->jump == NONE)
            st->jump = i;
    }
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling)
        stats(c, level, st);
}

static void check_function(uint32_t fn)
{
    const char *name = N(fn)->name;
    const char *type = N(fn)->type;
    if (strcmp(name, "main") == 0 && strncmp(type, "void", 4) == 0)
        say(fn, R_VOID_MAIN, name, 0);
    const size_t tl = strlen(type);
    if (!g_cpp && tl >= 2 && strcmp(type + tl - 2, "()") == 0)
        say(fn, R_EMPTY_PARAMS, name, 0);
    if (!(N(fn)->flags & CCR_PNODE_DEFINITION))
        return;
    /* its size, its locals, its deepest nesting, a goto */
    fn_stats st = { .last = N(fn)->line, .deep = NONE, .jump = NONE, .back = NONE, .fn = fn };
    stats(fn, 0, &st);
    if (st.deep != NONE)
        say(st.deep, R_NESTING, name, 0);
    if (st.jump != NONE)
        say(st.jump, R_GOTO, N(st.jump)->name, 0);
    if (st.back != NONE)
        say(st.back, R_GOTO_BACK, N(st.back)->name, 0);
    const uint32_t last = st.last;
    const int locals = st.locals;
    const int lines = (int)(last - N(fn)->line) + 2;
    if (N(fn)->line > 0 && lines > 80)
        say(fn, R_LONG_FN, name, lines);
    if (locals > 12)
        say(fn, R_LOCALS, name, locals);
}

/* if (x = 3): unless written ((x = 3)), on purpose */
static void check_condition(uint32_t s)
{
    const uint32_t cond = is(s, "DoWhile") ? child(s, 1) : child(s, 0);
    if (!is(cond, "BinaryOp") || strcmp(N(cond)->op, "=") != 0)
        return;
    uint32_t b = N(cond)->src_begin;
    int parens = 0;
    while (b > 0 && parens < 2) {
        const char ch = F->source[b - 1];
        if (ch == '(')
            ++parens;
        else if (ch != ' ' && ch != '\t')
            break;
        --b;
    }
    if (parens >= 2 && N(cond)->src_end > N(cond)->src_begin)
        return;
    const uint32_t lhs = uncast(child(cond, 0));
    say(s, R_ASSIGN_COND, is(lhs, "Ref") ? N(lhs)->name : "it", 0);
}

/* x = realloc(x, n) */
static void check_assign(uint32_t a)
{
    if (strcmp(N(a)->op, "=") != 0)
        return;
    const uint32_t lhs = uncast(child(a, 0)), rhs = uncast(child(a, 1));
    if (is(lhs, "Ref") && is(rhs, "Call") && named(rhs, "realloc")) {
        const uint32_t first = uncast(child(rhs, 0));
        if (is(first, "Ref") && named(first, N(lhs)->name))
            say(a, R_REALLOC, N(lhs)->name, 0);
    }
}

/* ------------------------------------------------------------ the crab */

static bool letter(char c)
{
    return isalpha((unsigned char)c) != 0;
}

/* "rust" as a word of its own (not trust, crust, frustrated...), or a
 * phrase only a Rustacean writes. Where, and how long, in *at / *len. */
static bool rusty(const char *s, size_t n, size_t *at, size_t *len)
{
    static const char *const phrases[] = { "rewrite it in rust", "riir", "blazingly fast", "borrow checker",
                                           "fearless concurrency", "memory safe", "rustacean", "cargo build",
                                           "unwrap()", "ferris", NULL };
    for (size_t i = 0; i < n; ++i) {
        for (const char *const *p = phrases; *p; ++p) {
            const size_t pl = strlen(*p);
            if (i + pl <= n && strncasecmp(s + i, *p, pl) == 0 && (i == 0 || !letter(s[i - 1]))) {
                *at = i;
                *len = pl;
                return true;
            }
        }
        if (i + 4 <= n && strncasecmp(s + i, "rust", 4) == 0 && (i == 0 || !letter(s[i - 1])) &&
            (i + 4 == n || !letter(s[i + 4]) || tolower((unsigned char)s[i + 4]) == 'a' ||
             tolower((unsigned char)s[i + 4]) == 'y')) {   /* rust, Rust's, rustacean, rusty */
            *at = i;
            *len = 4;
            while (i + *len < n && letter(s[i + *len]))
                ++*len;
            return true;
        }
    }
    return false;
}

static bool rusty_name(const char *name)
{
    static const char *const words[] = { "unwrap", "borrow", "lifetime", "rustc", "cargo", "ferris", "crab", NULL };
    char low[128];
    size_t n = 0;
    for (; name[n] && n < sizeof low - 1; ++n)
        low[n] = (char)tolower((unsigned char)name[n]);
    low[n] = '\0';
    for (const char *const *w = words; *w; ++w)
        if (strstr(low, *w))
            return true;
    size_t at, len;
    return rusty(name, strlen(name), &at, &len);
}

/* The statement (or declaration) a comment on line `line` is about: the
 * first one at or after it. */
static uint32_t node_at_line(uint32_t line)
{
    uint32_t best = NONE, before = NONE;
    for (uint32_t k = 0; k < F->node_count; ++k) {
        if (outside(k) || N(k)->line == 0 || N(k)->parent == NONE)
            continue;
        const uint32_t p = N(k)->parent;
        if (!is(p, "Block") && !is(p, "TranslationUnit"))
            continue;
        if (N(k)->line >= line && (best == NONE || N(k)->line < N(best)->line))
            best = k;
        if (N(k)->line < line && (before == NONE || N(k)->line > N(before)->line))
            before = k;
    }
    return best != NONE ? best : before;
}

static void quote(char *out, size_t cap, const char *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ++i)
        out[o++] = (s[i] == '\n' || s[i] == '\t') ? ' ' : s[i];
    out[o] = '\0';
}

static void check_rust_text(void)
{
    const char *s = F->source;
    const size_t n = F->source_len;
    uint32_t line = 1;
    int found = 0;
    for (size_t i = 0; i < n && found < 3; ++i) {
        if (s[i] == '\n') {
            ++line;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {   /* a literal: its own check below */
            const char q = s[i];
            for (++i; i < n && s[i] != q && s[i] != '\n'; ++i)
                if (s[i] == '\\')
                    ++i;
            continue;
        }
        size_t b = i, e;
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            e = i;
            while (e < n && s[e] != '\n')
                ++e;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            e = i + 2;
            while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '/'))
                ++e;
            e = e + 2 <= n ? e + 2 : n;
        } else {
            continue;
        }
        size_t at, len;
        if (rusty(s + b, e - b, &at, &len)) {
            /* the phrase, with a few words around it */
            size_t qb = b + at, qe = b + at + len;
            while (qb > b + 2 && qb + 24 > b + at && s[qb - 1] != '\n')
                --qb;
            while (qe < e && qe < b + at + len + 60 && s[qe] != '\n' && !(s[qe] == '*' && qe + 1 < e && s[qe + 1] == '/'))
                ++qe;
            while (qe > b + at + len && (s[qe - 1] == ' ' || s[qe - 1] == '\t'))
                --qe;
            while (qb < b + at && (s[qb] == '/' || s[qb] == '*' || s[qb] == ' '))
                ++qb;
            char q[96];
            quote(q, sizeof q, s + qb, qe - qb);
            uint32_t at_line = line;
            for (size_t k = b; k < b + at; ++k)
                at_line += s[k] == '\n';
            say(node_at_line(at_line), R_RUST_TEXT, q, 0);
            ++found;
        }
        for (size_t k = i; k < e; ++k)
            line += s[k] == '\n';
        i = e > i ? e - 1 : i;
    }
}

/* ----------------------------------------------------------------- run */

static bool is_header(const char *path)
{
    const size_t n = strlen(path);
    return (n > 2 && strcmp(path + n - 2, ".h") == 0) || (n > 4 && strcmp(path + n - 4, ".hpp") == 0) ||
           (n > 3 && strcmp(path + n - 3, ".hh") == 0);
}

static void check(const ccr_plugin_file *file)
{
    const bool cpp = strcmp(file->language, "c++") == 0;
    g_cpp = cpp;
    if ((strcmp(file->language, "c") != 0 && !cpp) || file->root == NONE)
        return;   /* CMake: somebody else's problem */
    F = file;
    g_lang = strncmp(file->ui_language, "ru", 2) == 0 ? 1 : 0;
    memset(g_said, 0, sizeof g_said);
    g_total = 0;
    for (uint32_t k = 0; k < F->node_count; ++k) {
        if (outside(k))
            continue;
        const char *kind = N(k)->kind;
        if (strcmp(kind, "Function") == 0)
            check_function(k);
        else if (strcmp(kind, "Var") == 0)
            check_var(k);
        else if (strcmp(kind, "Call") == 0)
            check_call(k);
        else if (strcmp(kind, "Return") == 0)
            check_return(k);
        else if (strcmp(kind, "If") == 0 || strcmp(kind, "While") == 0 || strcmp(kind, "DoWhile") == 0)
            check_condition(k);
        else if (strcmp(kind, "BinaryOp") == 0)
            check_assign(k);
        else if (cpp && strcmp(kind, "New") == 0)
            say(statement_of(k), R_RAW_NEW, *N(k)->type ? N(k)->type : "T", 0);
        else if (cpp && strcmp(kind, "Using") == 0 && is_header(F->path) && N(k)->parent == F->root && N(k)->src_end > N(k)->src_begin &&
                 strncmp(F->source + N(k)->src_begin, "using namespace", 15) == 0)
            say(k, R_USING_HEADER, N(k)->name, 0);
        else if (strcmp(kind, "String") == 0) {
            size_t at, len;
            const char *s = N(k)->name;
            if (rusty(s, strlen(s), &at, &len)) {
                char q[96];
                size_t sl = strlen(s);
                if (sl >= 2 && s[0] == '"' && s[sl - 1] == '"') {   /* without its quotes */
                    ++s;
                    sl -= 2;
                }
                quote(q, sizeof q, s, sl);
                say(statement_of(k), R_RUST_TEXT, q, 0);
            }
        }
        if ((strcmp(kind, "Var") == 0 || strcmp(kind, "Function") == 0 || strcmp(kind, "Param") == 0 ||
             strcmp(kind, "Field") == 0 || strcmp(kind, "Struct") == 0 || strcmp(kind, "Typedef") == 0) &&
            rusty_name(N(k)->name))
            say(is(N(k)->parent, "Function") ? N(k)->parent : k, R_RUST_NAME, N(k)->name, 0);
    }
    check_rust_text();
    F = NULL;
}

/* --------------------------------------------------------------- plugin */

static const ccr_host_api *g_host;

static bool hater_init(const ccr_host_api *host)
{
    g_host = host;
    host->log(CCR_LOG_DEBUG, "code_hater", "watching. Write better code.");
    return true;
}

static void hater_shutdown(void)
{
    g_host = NULL;
}

static const char *hater_describe(const char *lang)
{
    if (strncmp(lang, "ru", 2) == 0)
        return "Ревьюит твой C так, как это делали в старых рассылках: громко. Висячие указатели, обращение "
               "после free, утечки, gets(), вложенность в четыре этажа и всё, что пахнет Rust. Злится на код, "
               "а не на тебя. В основном.";
    return "Reviews your C the way the old mailing lists did: loudly. Dangling pointers, use after free, leaks, "
           "gets(), four levels of nesting, and anything that smells of Rust. Hates the code, not you. Mostly.";
}

static const ccr_plugin g_plugin = {
    .abi_version = CCR_PLUGIN_ABI_VERSION,
    .struct_size = sizeof(ccr_plugin),
    .kinds       = CCR_PLUGIN_CHECKER | CCR_PLUGIN_PERSONALITY,
    .name        = "code_hater",
    .version     = "1.0.0",
    .description = "Reviews your C the way the old mailing lists did: loudly.",
    .init        = hater_init,
    .shutdown    = hater_shutdown,
    .title       = "Code Hater",
    .author      = "Ccratch",
    .describe    = hater_describe,
    .check       = check,
    .color       = 0xD7263DFFu,
};

CCR_PLUGIN_EXPORT const ccr_plugin *ccr_plugin_entry(void);

CCR_PLUGIN_EXPORT const ccr_plugin *ccr_plugin_entry(void)
{
    return &g_plugin;
}
