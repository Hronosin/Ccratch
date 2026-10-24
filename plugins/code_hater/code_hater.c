/*
 * Code Hater - Ccratch's first official plugin.
 *
 * A borrow checker, but for C, and in a much worse mood. It reads the tree
 * Ccratch draws blocks from and says, the way the old mailing lists did,
 * what is wrong: pointers to dead stack frames, use after free, double
 * free, leaks, realloc into the same pointer, gets(), strcpy(), void main,
 * VLAs, four levels of nesting, unchecked malloc... and anything that
 * smells of Rust; in assembly, registers trashed behind the compiler's
 * back, stacks that don't balance, callee-saved registers not saved.
 * Every rant ends with how to fix it: it hates the code, not you. Mostly.
 *
 * How brutal is a setting: grumpy, Linus on a bad day, or the mother cat,
 * who rubs your nose in it (ccr_plugin_file.poke) when you save.
 *
 * Plain C, no libraries, nothing kept between calls: a model plugin.
 */
#include "ccratch/ccr_plugin.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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
    R_RAW_NEW, R_USING_HEADER, R_SELF, R_RUST_MENTION,
    /* assembly: asm() in C, and .s/.asm files */
    R_ASM_CLOBBER, R_ASM_MEMORY, R_ASM_INTRINSIC, R_ASM_STACK, R_ASM_FALLTHROUGH, R_ASM_CALLEE, R_ASM_ZERO,
    R_ASM_NOSTACK, R_ASM_DEAD, R_ARM_LR, R_ARM_STACK, R_COUNT
};

#define R_FIRST_ASM R_ASM_CLOBBER

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
    [R_RUST_MENTION] = { CCR_FINDING_NIT,
        { "Rust, mentioned", "упомянут Rust" },
        { { "\"{x}\". Just a mention, nobody's asking to rewrite anything. Yet. I'm keeping an eye on this "
            "file.", "\"{x}\". A word about Rust, no sermon attached. Fine. But I saw it." },
          { "«{x}». Просто упоминание, никто ничего переписывать не просит. Пока. Я за этим файлом "
            "присматриваю.", "«{x}». Слово про Rust, без проповеди. Ладно. Но я видел." } } },
    [R_SELF] = { CCR_FINDING_PRAISE,
        { "this is me", "это же я" },
        { { "Oh. This file is me. Every \"Rust\" in here is a rant about Rust, not Rust: I don't NAK my own "
            "insults, they're perfect. Everything else in this file I review like anyone else's. "
            "Especially the mallocs.", NULL },
          { "О. Этот файл — это я. Каждый «Rust» здесь — ругань про Rust, а не Rust: свои оскорбления я не "
            "NAK-аю, они идеальны. Всё остальное в этом файле ревьюю как у всех. Особенно malloc-и.", NULL } } },
    [R_EMPTY_PARAMS] = { CCR_FINDING_NIT,
        { "() is not (void)", "() — это не (void)" },
        { { "{x}() is declared with (). In C that doesn't mean 'no arguments', it means 'any arguments, and "
            "the compiler won't check them'. Write (void). This isn't C++.", NULL },
          { "{x}() объявлена с (). В C это значит не «без аргументов», а «с любыми, и компилятор не "
            "проверит». Пиши (void). Это не C++.", NULL } } },

    [R_ASM_CLOBBER] = { CCR_FINDING_ERROR,
        { "NAK: {x} trashed behind the compiler's back", "NAK: {x} испорчен за спиной компилятора" },
        { { "This asm writes {x}, and nobody told the compiler. It thinks {x} still holds whatever it put there: "
            "a loop counter, a pointer, your hopes. And it will keep using it after this line. Add \"{x}\" to the "
            "clobbers (after the third colon), or better, let the compiler pick: an output like \"=r\"(var) and "
            "%0 instead of {x}.",
            "Writing {x} without a clobber. The compiler had plans for {x}. You just ate them. This is the kind "
            "of bug that only shows up at -O2, on a Tuesday, in somebody else's function. Clobber it, or use an "
            "operand." },
          { "Этот asm пишет в {x}, а компилятору никто не сказал. Он думает, что в {x} всё ещё лежит его счётчик "
            "цикла, указатель или твои надежды, и продолжит этим пользоваться после этой строки. Допиши \"{x}\" "
            "в clobbers (после третьего двоеточия), а лучше дай компилятору выбрать регистр самому: выход "
            "\"=r\"(переменная) и %0 вместо {x}.",
            "Пишешь в {x} без clobber. У компилятора были на {x} планы. Ты их только что съел. Такой баг "
            "вылезает только на -O2, во вторник и в чужой функции. Объяви clobber или возьми операнд." } } },
    [R_ASM_MEMORY] = { CCR_FINDING_WARNING,
        { "asm writes memory, says nothing", "asm пишет в память молча" },
        { { "This asm stores to memory ({x}), but the clobbers don't say \"memory\" and no output is \"=m\". "
            "The compiler may keep a copy of that memory in a register and never look at it again. Add "
            "\"memory\" to the clobbers, or make that place an \"=m\" output.", NULL },
          { "Этот asm пишет в память ({x}), но в clobbers нет \"memory\", и ни один выход не \"=m\". Компилятор "
            "вправе держать копию этой памяти в регистре и больше туда не смотреть. Допиши \"memory\" в clobbers "
            "или сделай это место выходом \"=m\".", NULL } } },
    [R_ASM_INTRINSIC] = { CCR_FINDING_NIT,
        { "{x}: there is a builtin", "{x}: есть встроенная функция" },
        { { "{x} in asm() works, but {y} does the same, and the compiler understands it: it can move code "
            "around it, and it reads as C. Inline asm is for what the compiler cannot say.", NULL },
          { "{x} в asm() работает, но {y} делает то же самое, и компилятор его понимает: может переставлять "
            "код вокруг, и читается это как C. Ассемблерные вставки — для того, чего компилятор сказать не "
            "может.", NULL } } },
    [R_ASM_STACK] = { CCR_FINDING_ERROR,
        { "NAK: the stack is off by {N} at ret", "NAK: стек съехал на {N} к ret" },
        { { "ret, with the stack pointer {N} away from where this function found it. ret takes the return "
            "address off the top of the stack, and the top is now something else: whatever you pushed last, or "
            "a local. The CPU will happily jump into your saved rbx. Pop what you push, in reverse order, give "
            "back what sub took, before every ret.",
            "Count the pushes and the subs. Now count the pops and the adds. They don't match: {N} apart. ret "
            "doesn't count anything: it takes whatever is on top and jumps there. Balance the stack." },
          { "ret, а указатель стека на {N} не там, где эта функция его нашла. ret снимает адрес возврата с "
            "вершины стека, а на вершине сейчас что-то другое: то, что ты положил последним, или локальная "
            "переменная. Процессор с радостью прыгнет в твой сохранённый rbx. Снимай всё, что положил, в обратном "
            "порядке, и отдавай всё, что взял sub, перед каждым ret.",
            "Посчитай push и sub. Теперь посчитай pop и add. Не сходится: разница {N}. ret ничего не считает: он "
            "берёт то, что сверху, и прыгает туда. Выровняй стек." } } },
    [R_ASM_FALLTHROUGH] = { CCR_FINDING_WARNING,
        { "{x} falls into whatever comes next", "{x} проваливается в следующее" },
        { { "{x} ends without ret or jmp. The CPU doesn't know where your function ends: it just keeps going, "
            "straight into whatever bytes come next. Another function, data, the abyss. End it with ret.", NULL },
          { "{x} кончается без ret и jmp. Процессор не знает, где кончается твоя функция: он просто едет "
            "дальше, прямо в следующие байты. В другую функцию, в данные, в бездну. Закончи её ret.", NULL } } },
    [R_ASM_CALLEE] = { CCR_FINDING_ERROR,
        { "NAK: {x} is not yours", "NAK: {x} — не твой" },
        { { "{x} is callee-saved: whoever called you expects it back exactly as it was. You write it without "
            "saving it first. Save it on the stack at the start, load it back before you return, or use {y}, "
            "which are yours to trash.", NULL },
          { "{x} сохраняется вызываемым: тот, кто тебя позвал, ждёт его обратно ровно таким, каким дал. А ты "
            "пишешь в него, ничего не сохранив. Положи его на стек в начале, достань перед возвратом, или бери "
            "{y}: их портить можно.", NULL } } },
    [R_ARM_LR] = { CCR_FINDING_ERROR,
        { "NAK: bl eats the way back", "NAK: bl съедает дорогу назад" },
        { { "This function calls {x} with bl, and bl writes its own return address into {y}. Yours was there: "
            "whoever called this function was waiting for it. Now the return goes back to the line after the "
            "bl, again and again, forever. Save {y} first (stp x29, x30, [sp, #-16]! on AArch64, push {lr} on "
            "32-bit ARM) and take it back at the end.",
            "bl {x} without saving {y} first. On ARM the way back is not on the stack, it is in {y}, and bl "
            "just overwrote it with its own. Your return is now a loop with extra steps. Save it, call, load it "
            "back." },
          { "Эта функция вызывает {x} через bl, а bl записывает свой адрес возврата в {y}. Там был твой: тот, "
            "кто позвал эту функцию, его ждал. Теперь возврат уходит на строку после bl — снова и снова, "
            "навсегда. Сохрани {y} заранее (stp x29, x30, [sp, #-16]! на AArch64, push {lr} на 32-битном ARM) и "
            "достань в конце.",
            "bl {x}, а {y} не сохранён. На ARM дорога назад лежит не на стеке, а в {y}, и bl только что "
            "затёр её своей. Твой возврат теперь — это цикл с дополнительными шагами. Сохрани, вызови, достань." } } },
    [R_ARM_STACK] = { CCR_FINDING_ERROR,
        { "NAK: sp is off by {N} at the return", "NAK: sp съехал на {N} к возврату" },
        { { "This returns with sp not where it found it: {N} more down than up. On ARM the return itself still "
            "finds its way (it jumps to the link register, not the stack), but the caller's locals now live at "
            "the wrong address, and the next one to touch the stack reads garbage. Every sub sp or stp ...]! has "
            "its add sp or ldp ..., #N; every push its pop.", NULL },
          { "Это возвращается со sp не там, где нашло: вниз на {N} больше, чем вверх. На ARM сам возврат дорогу "
            "найдёт (он прыгает по регистру связи, а не по стеку), но локальные переменные вызывающего теперь "
            "живут по чужому адресу, и следующий, кто тронет стек, прочитает мусор. У каждого sub sp и stp ...]! "
            "есть свой add sp или ldp ..., #N; у каждого push — свой pop.", NULL } } },
    [R_ASM_ZERO] = { CCR_FINDING_NIT,
        { "mov 0 into {x}", "mov нуля в {x}" },
        { { "Moving 0 into {x}. xor {x}, {x} is shorter (no four zero bytes in the instruction), and every CPU "
            "since the Pentium Pro knows it means zero. Unless the flags must stay as they are: then keep the "
            "mov and say so in a comment.", NULL },
          { "mov нуля в {x}. xor {x}, {x} короче (в инструкции нет четырёх нулевых байт), и любой процессор со "
            "времён Pentium Pro знает, что это ноль. Если флаги трогать нельзя — оставь mov и напиши об этом "
            "комментарий.", NULL } } },
    [R_ASM_NOSTACK] = { CCR_FINDING_NIT,
        { "no .note.GNU-stack", "нет .note.GNU-stack" },
        { { "This file never says .section .note.GNU-stack,\"\",@progbits. Without it the linker assumes your "
            "code needs an executable stack, and makes the whole program's stack executable. One line at the "
            "end of the file.", NULL },
          { "В файле нет .section .note.GNU-stack,\"\",@progbits. Без неё компоновщик решит, что твоему коду "
            "нужен исполняемый стек, и сделает стек исполняемым всей программе. Одна строка в конце файла.",
            NULL } } },
    [R_ASM_DEAD] = { CCR_FINDING_WARNING,
        { "nothing ever gets here", "сюда никто не попадёт" },
        { { "{x} comes right after a jmp or ret, with no label in between. Nothing can ever jump here, so it "
            "never runs. Either a label went missing, or this is dead code.", NULL },
          { "{x} стоит сразу после jmp или ret, и между ними нет метки. Сюда никто никогда не прыгнет, значит, "
            "это не выполнится никогда. Либо потерялась метка, либо это мёртвый код.", NULL } } },
};

/* More ways to say the same thing: picked by the block, so it never
 * flickers, but two files rarely get the same words. */
static const char *const g_extra[R_COUNT][2][3] = {
    [R_DANGLING] = {
        { "&{x} escapes the function that owns {x}. That's like handing someone the key to a hotel room after "
          "you've checked out. The next guest has moved in already." },
        { "&{x} убегает из функции, которой принадлежит {x}. Это как отдать кому-то ключ от номера в отеле, "
          "когда ты уже выехал. Там уже новый жилец." } },
    [R_UAF] = {
        { "{x} is dead. You freed it. And now you're poking the corpse to see if it still answers. Sometimes it "
          "does, which is exactly why this bug survives code review. Not this one. Stop using {x} after free().",
          "Here's {x} again, back from the dead. In a horror film this is where everyone screams. In C it's where "
          "the allocator hands that memory to someone else and your data quietly becomes theirs. Free it last, "
          "not first." },
        { "{x} мёртв. Ты его освободил. А теперь тычешь палкой в труп — вдруг отзовётся. Иногда отзывается, "
          "поэтому этот баг и проходит ревью. Только не это. Не трогай {x} после free().",
          "И снова {x}, восставший из мёртвых. В фильме ужасов тут все кричат. В C тут аллокатор отдаёт эту "
          "память кому-то другому, и твои данные тихо становятся чужими. Освобождай последним, а не первым." } },
    [R_DOUBLE_FREE] = {
        { "{x}, freed twice. The first free() was the funeral; the second digs it up to bury it again. The "
          "heap's bookkeeping does not survive that. Set it to NULL after the first one.",
          "Double free of {x}. A classic exploit primitive: attackers love it, allocators hate it. {x} = NULL "
          "right after free(), and the second call does nothing." },
        { "{x} освобождён дважды. Первый free() был похоронами, второй его выкапывает, чтобы похоронить снова. "
          "Учёт кучи такого не переживает. Обнули после первого.",
          "Двойной free {x}. Классический примитив для эксплойтов: атакующие его обожают, аллокаторы "
          "ненавидят. {x} = NULL сразу после free() — и второй вызов ничего не сделает." } },
    [R_LEAK] = {
        { "{x}: allocated, used, abandoned. Like a gym membership in February. The OS cleans up when the "
          "program exits, but a program that runs for weeks never exits, it just grows. free({x}).",
          "Every call leaks {x}. Small? Run it a million times. Valgrind would be screaming right now, if anyone "
          "ever ran valgrind. Every malloc gets its free, on every path out." },
        { "{x}: выделили, попользовались, бросили. Как абонемент в спортзал в феврале. ОС приберёт, когда "
          "программа выйдет, но программа, которая работает неделями, не выходит — она пухнет. free({x}).",
          "Каждый вызов теряет {x}. Мелочь? Запусти миллион раз. Valgrind бы сейчас орал, если бы его кто-нибудь "
          "когда-нибудь запускал. У каждого malloc — свой free, на каждом пути выхода." } },
    [R_REALLOC] = {
        { "{x} = realloc({x}, ...): the one-line memory leak. When it fails, the old block is still alive, and "
          "you just lost its only address. A temporary pointer, a check, then the assignment." },
        { "{x} = realloc({x}, ...): утечка памяти в одну строку. Если не вышло, старый блок ещё жив, а ты только "
          "что потерял его единственный адрес. Временный указатель, проверка, потом присваивание." } },
    [R_GETS] = {
        { "gets(). In this century. The manual page for gets() literally says \"Never use this function.\" It's "
          "the only advice on the page. Take it." },
        { "gets(). В этом веке. В man-странице gets() прямо написано: «Никогда не используйте эту функцию». Это "
          "единственный совет на странице. Послушай его." } },
    [R_STRCPY] = {
        { "{x}(). The function that put the overflow into buffer overflow. It doesn't know how big the "
          "destination is, and it doesn't want to know. You have to. Use one that takes a length.",
          "{x}() into a buffer of fixed size, from a source of whatever size the outside world felt like "
          "sending. What could possibly go wrong. Everything. Everything could." },
        { "{x}(). Функция, благодаря которой в «переполнении буфера» есть «переполнение». Размер приёмника она "
          "не знает и знать не хочет. Знать должен ты. Бери функцию с длиной.",
          "{x}() в буфер фиксированного размера, из источника того размера, какой внешнему миру захотелось "
          "прислать. Что может пойти не так. Всё. Всё может." } },
    [R_SPRINTF] = {
        { "{x}() with no size: a buffer overflow waiting for a long enough name. snprintf with sizeof, and look "
          "at what it returns." },
        { "{x}() без размера: переполнение буфера, которое ждёт достаточно длинного имени. snprintf с sizeof — "
          "и посмотри, что он вернул." } },
    [R_ATOI] = {
        { "{x}() returns 0 for garbage and has undefined behaviour on overflow. Two bugs for the price of one "
          "call. strtol, and check where it stopped." },
        { "{x}() возвращает 0 на мусор и даёт неопределённое поведение при переполнении. Два бага по цене "
          "одного вызова. strtol — и проверь, где он остановился." } },
    [R_ASSIGN_COND] = {
        { "= where == should be. The oldest typo in C, the one that made people write if (0 == x) for thirty "
          "years. {x} gets overwritten here, every single time. Fix the operator.",
          "Assignment inside a condition. The compiler would have warned you, if you had -Wall on. You don't, "
          "do you. Comparing {x} is ==." },
        { "= там, где должно быть ==. Самая старая опечатка в C, из-за неё тридцать лет писали if (0 == x). "
          "{x} здесь перезаписывается, каждый раз. Исправь оператор.",
          "Присваивание внутри условия. Компилятор бы предупредил, будь у тебя -Wall. А его нет, да? Для "
          "сравнения {x} — ==." } },
    [R_NESTING] = {
        { "Four levels deep. At this depth the code needs a submarine. Return early, continue early, and pull "
          "the inside out into a function with a name that says what it does.",
          "I had to scroll right to read this. Four nested blocks: the arrow anti-pattern, and the arrow points "
          "straight at the next bug. Flatten it." },
        { "Четыре этажа вложенности. На такой глубине коду нужна подводная лодка. Выходи раньше через return "
          "и continue, вынеси внутренности в функцию с именем, которое говорит, что она делает.",
          "Чтобы это прочитать, пришлось листать вправо. Четыре вложенных блока: антипаттерн «стрелка», и "
          "стрелка указывает прямо на следующий баг. Распрями." } },
    [R_LONG_FN] = {
        { "{x}(): {n} lines. That's not a function, that's a novella. Chapters were invented for a reason: give "
          "each part a function and a name." },
        { "{x}(): {n} строк. Это не функция, это повесть. Главы придумали не зря: дай каждой части свою функцию "
          "и имя." } },
    [R_LOCALS] = {
        { "{n} locals in {x}(). That's not a function, that's a crowd. Group what belongs together into a "
          "struct, or split it." },
        { "{n} локальных переменных в {x}(). Это не функция, это толпа. Сгруппируй связанное в struct или "
          "разбей её." } },
    [R_VOID_MAIN] = {
        { "void main(). The C standard says int main. The shell says $? is garbage now. Everyone says int main. "
          "Except you.",
          "main returns void here. Your program ends, the shell asks \"did it work?\", and gets random noise "
          "back. int main, and return 0." },
        { "void main(). Стандарт C говорит int main. Оболочка говорит, что в $? теперь мусор. Все говорят int "
          "main. Кроме тебя.",
          "main тут возвращает void. Программа завершается, оболочка спрашивает «получилось?» — а в ответ "
          "случайный шум. int main и return 0." } },
    [R_UNCHECKED] = {
        { "No NULL check on {x}. \"malloc never fails\", said everyone, right up until the container got a "
          "64 MB memory limit. if (!{x}), and deal with it.",
          "You trust malloc. That's sweet. malloc doesn't trust you back. Check {x} for NULL before the first "
          "use, every time." },
        { "{x} не проверен на NULL. «malloc никогда не падает», — говорили все, ровно до того дня, когда "
          "контейнеру дали лимит в 64 МБ. if (!{x}) — и разбирайся.",
          "Ты доверяешь malloc. Мило. malloc тебе — нет. Проверяй {x} на NULL перед первым использованием, "
          "каждый раз." } },
    [R_VLA] = {
        { "{x}: a VLA. A stack allocation of unknown size, with no way to fail gracefully. It's a stack "
          "overflow with a time fuse. malloc it, or cap the size." },
        { "{x}: VLA. Выделение на стеке неизвестного размера, без возможности красиво упасть. Это переполнение "
          "стека с часовым механизмом. malloc или ограничь размер." } },
    [R_GOTO_BACK] = {
        { "A goto that goes up is spaghetti with extra steps. The arrow on the canvas loops back on itself for "
          "a reason: it's a loop. Say so, with while." },
        { "goto вверх — это спагетти с дополнительными шагами. Стрелка на холсте не зря закручивается назад: "
          "это цикл. Так и напиши: while." } },
    [R_MALLOC_CAST] = {
        { "Casting what {x}() gives you. In C, void * converts by itself; the cast is noise, and noise hides "
          "real mistakes. Remove it." },
        { "Каст того, что вернул {x}(). В C void * преобразуется сам; каст — это шум, а за шумом прячутся "
          "настоящие ошибки. Убери." } },
    [R_SIZEOF_TYPE] = {
        { "sizeof(type) for {x}: two places to keep in step, and one of them will drift. sizeof *{x} keeps "
          "itself right." },
        { "sizeof(тип) для {x}: два места, которые надо держать в согласии, и одно из них уплывёт. sizeof *{x} "
          "сам за собой следит." } },
};

/* ----------------------------------------------------------- the tone
 *
 * How brutal (Settings > Plugins > Code Hater): grumpy says what is wrong;
 * Linus says it the way the old lists did, before and after; the mother
 * cat grabs you by the scruff, and when you save or build, rubs your nose
 * in it (the host flies there and makes a scene).
 */
static const char *const g_linus_open[2][17] = {
    { "No. Just no.", "What the hell is this?", "I'm not even angry. I'm disappointed. No, wait, I'm angry.",
      "This is the kind of code that makes me want to take up gardening.",
      "Who taught you this? Give me their name. I just want to talk.",
      "I've seen better code come out of a random number generator.",
      "Stop. Put the keyboard down. Step away from the keyboard.",
      "This isn't just wrong, it's wrong in an interesting way, which is worse.",
      "Did you read what you wrote, or did the cat walk across the keyboard?",
      "I would NAK this twice if the protocol allowed it.", "Christ, what a mess.",
      "Let me be very clear, since subtlety clearly doesn't work on you:",
      "Somewhere, a compiler engineer just started crying.", "This is why we can't have nice things.",
      "Oh good. Another one.", "Brace yourself.", NULL },
    { "Нет. Просто нет.", "Это что вообще такое?", "Я даже не злюсь. Я разочарован. А нет, всё-таки злюсь.",
      "От такого кода хочется бросить всё и уехать сажать картошку.",
      "Кто тебя этому научил? Дай мне его имя. Я просто поговорить.",
      "Генератор случайных чисел пишет код лучше.", "Стоп. Положи клавиатуру. Отойди от клавиатуры.",
      "Это не просто неправильно, это неправильно интересно. Что хуже.",
      "Ты вообще читал, что написал, или это кот прошёлся по клавиатуре?",
      "Я бы поставил NAK дважды, если бы протокол разрешал.", "Господи, ну и бардак.",
      "Скажу очень прямо, раз намёки на тебя не действуют:", "Где-то сейчас заплакал разработчик компилятора.",
      "Вот поэтому у нас и не может быть ничего хорошего.", "О, отлично. Ещё один.", "Держись.", NULL },
};

static const char *const g_linus_close[2][13] = {
    { "Fix it. Then fix it again, properly.",
      "And no, \"it works on my machine\" is not a defence. Your machine is not the world.",
      "Don't send me this again until it's fixed.", "I'll pretend I didn't see this. For about five minutes.",
      "Next time, think first. Then type.", "Consider this the polite version.",
      "This is not a review comment, this is a cry for help.", "Go read K&R. The whole thing. I'll wait.",
      "The code doesn't care about your feelings, and neither do I. Fix it.",
      "If this goes into a release, I'm changing my name.", "I hope you have backups. Of your dignity.",
      "Fix it before anyone sees it. Too late, I saw it.", NULL },
    { "Исправь. Потом исправь ещё раз, нормально.",
      "И нет, «у меня работает» — не оправдание. Твоя машина — не весь мир.",
      "Не присылай мне это, пока не исправишь.", "Сделаю вид, что не видел. Минут на пять.",
      "В следующий раз сначала думай. Потом печатай.", "Считай, что это я ещё вежливо.",
      "Это не комментарий к ревью, это крик о помощи.", "Иди перечитай K&R. Целиком. Я подожду.",
      "Коду плевать на твои чувства, и мне тоже. Исправляй.", "Если это попадёт в релиз, я сменю фамилию.",
      "Надеюсь, у тебя есть бэкапы. Твоего достоинства.", "Исправь, пока никто не увидел. Поздно, я увидел.",
      NULL },
};

static const char *const g_cat_open[2][16] = {
    { "*grabs you by the scruff of the neck*", "*hisses* Come here. Come HERE.",
      "Look. LOOK at it. No, don't look away.", "*picks you up by the scruff and carries you to the bug*",
      "Mrrrow. Who did this? Who did this on MY canvas?", "*ears flat, tail twitching*",
      "Sit. Sit. Now look down. That's yours.", "*drags you across the canvas by the scruff* Here. Right here.",
      "You did this. Don't give me those big eyes.", "*slow blink, and not the nice kind*",
      "Kitten. Kitten, what is THIS.", "*pushes your face toward the block* Smell it. That's what a bug smells like.",
      "I raised you better than this. I'm sure I did.",
      "*sits on your keyboard so you can't type any more of this*",
      "Hhhhhhh. *the hiss of a mother who has seen this before*", NULL },
    { "*хватает тебя за шкирку*", "*шипит* Иди сюда. Иди СЮДА.", "Смотри. СМОТРИ. Не отворачивайся.",
      "*берёт за шкирку и несёт прямо к багу*", "Мрррау. Кто это сделал? Кто это сделал на МОЁМ холсте?",
      "*уши прижаты, хвост дёргается*", "Сидеть. Сидеть. А теперь смотри вниз. Это твоё.",
      "*тащит тебя за шкирку через весь холст* Вот. Прямо вот тут.",
      "Это ты сделал. Не делай мне тут большие глаза.", "*медленно моргает, но не по-доброму*",
      "Котёнок. Котёнок, это ЧТО.", "*тычет мордой в блок* Нюхай. Вот так пахнет баг.",
      "Я тебя не так воспитывала. Я уверена, что не так.",
      "*садится на клавиатуру, чтобы ты больше такого не напечатал*",
      "Хххххх. *шипение матери, которая всё это уже видела*", NULL },
};

static const char *const g_cat_close[2][13] = {
    { "Now clean it up. Properly. With your tongue if you have to.", "We don't do that in this house.",
      "*licks the top of your head, aggressively* There. Now fix it.",
      "I'll be watching. I'm always watching. From the top of the fridge.",
      "Do it again and your monitor goes off the desk.", "*knocks your coffee off the desk, to make the point*",
      "You'll understand when you have kittens of your own.",
      "Next time I bring you a dead mouse and you bring me working code. Deal?",
      "*turns around and sits with her back to you*", "Again? AGAIN? *the nose goes back in*",
      "Fix it, and maybe you get a treat.", "Every time you save this, I'll be back. With the paw.", NULL },
    { "А теперь прибери за собой. Как следует. Языком, если придётся.", "В этом доме так не делают.",
      "*яростно вылизывает тебе макушку* Вот. Теперь исправляй.",
      "Я буду следить. Я всегда слежу. С холодильника.", "Ещё раз — и твой монитор полетит со стола.",
      "*сталкивает твою кружку со стола, для наглядности*", "Вот будут у тебя свои котята — поймёшь.",
      "В следующий раз я тебе мышку, а ты мне рабочий код. Договорились?",
      "*разворачивается и садится к тебе спиной*", "Опять? ОПЯТЬ? *мордой обратно в баг*",
      "Исправишь — может, дам вкусняшку.", "Каждый раз, как ты это сохранишь, я буду возвращаться. С лапой.",
      NULL },
};

/* What it shouts while your nose is in it. Short: it is a bubble. */
static const char *const g_pokes[2][13] = {
    { "LOOK. AT. IT.", "This. Is. Yours.", "Smell it. That's a bug.", "Here. HERE.", "No. Bad. BAD.",
      "What is this? WHAT IS THIS?", "Nose. In. It.", "Again with this?!", "Don't you look away!",
      "Mrrraow! Fix it!", "Who did this?!", "*poke* *poke* *POKE*", NULL },
    { "СМОТРИ. НА. ЭТО.", "Это. Твоё.", "Нюхай. Это баг.", "Вот. ВОТ ТУТ.", "Нельзя. Фу. ФУ.",
      "Это что? ЭТО ЧТО?", "Мордой. В него.", "Опять?!", "Не отворачивайся!", "Мрррау! Исправляй!",
      "Кто это сделал?!", "*тык* *тык* *ТЫК*", NULL },
};

/* Praise, from someone who does not do praise. */
static const char *const g_praise_open[3][2][6] = {
    { { "Hm.", "Well.", NULL }, { "Хм.", "Ну.", NULL } },
    { { "Hm. Fine.", "Well, look at that.", "Don't let it go to your head.", "I'll allow it.", NULL },
      { "Хм. Ладно.", "Ну надо же.", "Только не зазнавайся.", "Так и быть, разрешаю.", NULL } },
    { { "*purrs, reluctantly*", "*a slow blink. The nice kind. Once.*", "*brings you a dead mouse*",
        "Good kitten. GOOD kitten.", NULL },
      { "*мурлычет, нехотя*", "*медленно моргает. По-доброму. Один раз.*", "*приносит тебе дохлую мышь*",
        "Хороший котёнок. ХОРОШИЙ котёнок.", NULL } },
};

static const char *pick(const char *const *list, unsigned h)
{
    unsigned n = 0;
    while (list[n])
        ++n;
    return n ? list[h % n] : "";
}

/* ------------------------------------------------------------ the tree */

static const ccr_plugin_file *F;
static int g_lang;                 /* 0 en, 1 ru */
static bool g_cpp;                 /* the file is C++: f() means f(void), casts on malloc are needed */
static int g_said[R_COUNT];        /* per rant, this file */
static int g_total;
static bool g_self;                /* the file is Code Hater itself: its Rust is rants about Rust */
/* its settings, for this file */
static int  g_brutal;              /* 0 grumpy, 1 Linus, 2 the mother cat */
static bool g_opt_praise, g_opt_rust, g_opt_asm, g_opt_nits;
static const char *g_y = "";       /* {y}, for the rant being said */

/* What it will say, held until the file is read: the same thing about the
 * same block, said once ("and 11 more like it here"), not 12 times. */
typedef struct pending {
    uint32_t node;
    int      r, more;
    char     title[128], msg[1380];
    const char *poke;   /* the mother cat's shout, or NULL */
} pending;
static pending g_pend[96];
static int     g_npend;

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

/* ---------------------------------------------------- the tree vs the text
 *
 * The tree is what clang understood. When a header is missing, it drops what
 * it could not type: `t = gen(&a);` with `a` of an unknown struct is simply
 * not there, and `p->field` of an unknown struct comes out as a bare `p`. So
 * before saying something is used after free, or never given back, the text
 * gets a say too.
 */

static bool has_text(uint32_t i)
{
    return i != NONE && N(i)->src_end > N(i)->src_begin && N(i)->src_end <= F->source_len;
}

/* `name` as a whole word in the source from b to e, comments and literals
 * skipped: how many times. */
static int words_in(uint32_t b, uint32_t e, const char *name)
{
    const char *s = F->source;
    const size_t nl = strlen(name);
    if (e > F->source_len)
        e = F->source_len;
    int count = 0;
    for (uint32_t i = b; i < e;) {
        const char c = s[i];
        if (c == '/' && i + 1 < e && s[i + 1] == '/') {
            while (i < e && s[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < e && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < e && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            i += 2;
        } else if (c == '"' || c == '\'') {
            for (++i; i < e && s[i] != c && s[i] != '\n'; ++i)
                if (s[i] == '\\')
                    ++i;
            ++i;
        } else if (isalpha((unsigned char)c) || c == '_') {
            uint32_t j = i;
            while (j < e && (isalnum((unsigned char)s[j]) || s[j] == '_'))
                ++j;
            count += j - i == nl && memcmp(s + i, name, nl) == 0;
            i = j;
        } else {
            ++i;
        }
    }
    return count;
}

/* Where the subtree at `i` names `name`: its Refs (sizeof or not), and
 * its declaration. */
static int count_refs(uint32_t i, const char *name)
{
    if (i == NONE)
        return 0;
    int n = (is(i, "Ref") || is(i, "Var") || is(i, "Param")) && named(i, name);
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling)
        n += count_refs(c, name);
    return n;
}

/* Does the text of `i` name `name` more often than its tree does? Then
 * clang lost a piece of it, and nobody can say what that piece does. */
static bool tree_short(uint32_t i, const char *name)
{
    return has_text(i) && words_in(N(i)->src_begin, N(i)->src_end, name) > count_refs(i, name);
}

/* What f( ... ) has between its parentheses, in the text, is `name` (a
 * cast in front allowed). A macro, or p->x the tree turned into p: no. */
static bool call_arg_is(uint32_t call, const char *name)
{
    if (!has_text(call))
        return true;   /* made in the editor: the tree is all there is */
    const char *s = F->source + N(call)->src_begin;
    const size_t n = N(call)->src_end - N(call)->src_begin;
    const char *open = memchr(s, '(', n);
    if (!open)
        return false;
    const char *a = open + 1, *z = a;
    int depth = 0;
    for (; z < s + n; ++z) {
        if (*z == '(')
            ++depth;
        else if (*z == ')' && depth-- == 0)
            break;
    }
    for (;;) {
        while (a < z && isspace((unsigned char)*a))
            ++a;
        while (z > a && isspace((unsigned char)z[-1]))
            --z;
        if (a < z && *a == '(') {   /* (void *)p */
            const char *c = memchr(a, ')', (size_t)(z - a));
            if (!c)
                return false;
            a = c + 1;
            continue;
        }
        break;
    }
    return (size_t)(z - a) == strlen(name) && memcmp(a, name, (size_t)(z - a)) == 0;
}

/* The first Ref to `name` in the subtree at `i`, in the order it runs: in
 * p = ..., the right side first. *write: that touch is p = ..., or &p (the
 * pointer itself, not what it points to). */
static uint32_t first_touch(uint32_t i, const char *name, bool *write)
{
    if (i == NONE || is(i, "Sizeof"))
        return NONE;
    if (is(i, "Ref") && named(i, name)) {
        uint32_t at = i, p = N(i)->parent;
        while (is(p, "Cast")) {
            at = p;
            p = N(p)->parent;
        }
        *write = (is(p, "BinaryOp") && strcmp(N(p)->op, "=") == 0 && N(p)->first_child == at) ||
                 (is(p, "UnaryOp") && strcmp(N(p)->op, "&") == 0);
        return i;
    }
    if (is(i, "BinaryOp") && strcmp(N(i)->op, "=") == 0) {   /* p = p->next: the read first */
        const uint32_t r = first_touch(child(i, 1), name, write);
        return r != NONE ? r : first_touch(child(i, 0), name, write);
    }
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling) {
        const uint32_t r = first_touch(c, name, write);
        if (r != NONE)
            return r;
    }
    return NONE;
}

/* p && p->x, p ? p->x : 0, !p || ...: checked where it is used. */
static bool guarded(uint32_t i, const char *name)
{
    if (i == NONE)
        return false;
    if (((is(i, "BinaryOp") && (strcmp(N(i)->op, "&&") == 0 || strcmp(N(i)->op, "||") == 0)) || is(i, "Ternary")) &&
        mentions(child(i, 0), name))
        return true;
    for (uint32_t c = N(i)->first_child; c != NONE; c = N(c)->next_sibling)
        if (guarded(c, name))
            return true;
    return false;
}

/* After it, nothing below runs unless a label is jumped to. */
static bool leaves(uint32_t i)
{
    return is(i, "Return") || is(i, "Goto") || is(i, "Break") || is(i, "Continue");
}

/* static int x; (the text from the start of its declaration) */
static bool decl_is_static(uint32_t v)
{
    if (!has_text(v))
        return false;
    uint32_t b = N(v)->src_begin;
    while (b > 0 && F->source[b - 1] != '\n' && F->source[b - 1] != ';' && F->source[b - 1] != '{' &&
           F->source[b - 1] != '}')
        --b;
    const size_t len = N(v)->src_end - b;
    char head[160];
    snprintf(head, sizeof head, "%.*s", (int)(len < sizeof head - 1 ? len : sizeof head - 1), F->source + b);
    return strstr(head, "static") != NULL;
}

/* ------------------------------------------------------------- saying it */

static unsigned hash(const char *s, unsigned h)
{
    while (*s)
        h = h * 31u + (unsigned char)*s++;
    return h;
}

/* A string cut at `len` bytes: a letter cut in two is dropped whole. */
static void cut_utf8(char *s, size_t len)
{
    size_t k = len;
    while (k > 0 && ((unsigned char)s[k - 1] & 0xC0u) == 0x80u)
        --k;
    if (k > 0 && (unsigned char)s[k - 1] >= 0xC0u) {
        const unsigned char lead = (unsigned char)s[k - 1];
        const size_t need = lead >= 0xF0u ? 4 : lead >= 0xE0u ? 3 : 2;
        if (len - (k - 1) < need)
            len = k - 1;
    }
    s[len] = '\0';
}

/* snprintf, and if it did not fit, no half letter at the end. */
#define PRINT(out, cap, ...)                                     \
    do {                                                         \
        const int pn_ = snprintf((out), (cap), __VA_ARGS__);     \
        if (pn_ >= (int)(cap))                                   \
            cut_utf8((out), (cap) - 1);                          \
    } while (0)

/* "8 bytes", "8 байт", "24 байта" */
static void bytes(char *out, size_t cap, int n)
{
    if (g_lang == 1) {
        const int m10 = n % 10, m100 = n % 100;
        const char *w = m10 == 1 && m100 != 11 ? "байт" :
                        m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14) ? "байта" : "байт";
        snprintf(out, cap, "%d %s", n, w);
    } else {
        snprintf(out, cap, "%d byte%s", n, n == 1 ? "" : "s");
    }
}

static void fill(char *out, size_t cap, const char *tmpl, const char *x, int n)
{
    size_t o = 0;
    char num[16], nb[32];
    snprintf(num, sizeof num, "%d", n);
    bytes(nb, sizeof nb, n);
    bool cut = false;
    for (const char *p = tmpl; *p;) {
        const char *put = NULL;
        if (strncmp(p, "{x}", 3) == 0)
            put = x;
        else if (strncmp(p, "{y}", 3) == 0)
            put = g_y;
        else if (strncmp(p, "{n}", 3) == 0)
            put = num;
        else if (strncmp(p, "{N}", 3) == 0)
            put = nb;
        if (put) {
            for (; *put; ++put) {
                if (o + 1 >= cap) {
                    cut = true;
                    break;
                }
                out[o++] = *put;
            }
            p += 3;
        } else {
            if (o + 1 >= cap) {
                cut = true;
                break;
            }
            out[o++] = *p++;
        }
        if (cut)
            break;
    }
    if (cut)
        cut_utf8(out, o);
    else
        out[o] = '\0';
}

static bool is_rust_rant(int r) { return r == R_RUST_NAME || r == R_RUST_TEXT || r == R_RUST_MENTION; }

/* Its words in the tone the settings ask for. */
static void toned(char *out, size_t cap, const char *body, ccr_finding_level level, unsigned h)
{
    const char *open = NULL, *close = NULL;
    if (level == CCR_FINDING_PRAISE) {
        if (g_brutal > 0 || (h & 3u) == 0)
            open = pick(g_praise_open[g_brutal][g_lang], h >> 3);
    } else if (g_brutal == 1) {
        if (level >= CCR_FINDING_WARNING)
            open = pick(g_linus_open[g_lang], h >> 2);
        close = pick(g_linus_close[g_lang], h >> 7);
    } else if (g_brutal == 2) {
        if (level >= CCR_FINDING_WARNING)
            open = pick(g_cat_open[g_lang], h >> 2);
        close = pick(level >= CCR_FINDING_WARNING ? g_cat_close[g_lang] : g_linus_close[g_lang], h >> 7);
    }
    PRINT(out, cap, "%s%s%s%s%s", open ? open : "", open ? "\n\n" : "", body, close ? "\n\n" : "",
          close ? close : "");
}

static void say(uint32_t node, int r, const char *x, int n)
{
    if (node == NONE || outside(node))
        return;
    const rant *ra = &g_rants[r];
    if ((ra->level == CCR_FINDING_NIT && !g_opt_nits) || (ra->level == CCR_FINDING_PRAISE && !g_opt_praise) ||
        (is_rust_rant(r) && !g_opt_rust) || (r >= R_FIRST_ASM && !g_opt_asm))
        return;   /* switched off in the settings */
    for (int i = 0; i < g_npend; ++i)   /* this again, about the same block: counted, not repeated */
        if (g_pend[i].node == node && g_pend[i].r == r) {
            ++g_pend[i].more;
            return;
        }
    if (g_said[r] >= MAX_PER_RANT || g_total >= MAX_TOTAL || g_npend >= (int)(sizeof g_pend / sizeof g_pend[0]))
        return;
    ++g_said[r];
    ++g_total;
    /* the same block gets the same words every time: no flicker */
    const unsigned h = hash(x ? x : "", (unsigned)N(node)->line * 131u + (unsigned)r) * 2654435761u;
    const char *ways[6];
    int nw = 0;
    for (int k = 0; k < 2; ++k)
        if (ra->say[g_lang][k])
            ways[nw++] = ra->say[g_lang][k];
    for (int k = 0; k < 3; ++k)
        if (g_extra[r][g_lang][k])
            ways[nw++] = g_extra[r][g_lang][k];
    if (nw == 0)
        return;   /* nothing to say it with in this language */
    const char *tmpl = ways[(h >> 5) % (unsigned)nw];
    pending *p = &g_pend[g_npend++];
    p->node = node;
    p->r = r;
    p->more = 0;
    fill(p->title, sizeof p->title, ra->title[g_lang], x ? x : "", n);
    char body[1100];
    fill(body, sizeof body, tmpl, x ? x : "", n);
    toned(p->msg, sizeof p->msg, body, ra->level, h);
    /* the mother cat: what is really wrong gets your nose rubbed in it */
    p->poke = g_brutal == 2 && ra->level >= CCR_FINDING_WARNING ? pick(g_pokes[g_lang], h >> 11) : NULL;
}

/* Has the host this field of ccr_plugin_file? (An older one stops before it.) */
#define FILE_HAS(f, field) ((f)->struct_size >= offsetof(ccr_plugin_file, field) + sizeof((f)->field))

static void say_all(void)
{
    static const char *const more[3][2] = {
        { "\n\n(And %d more like it in this block. Once is enough to get the idea.)",
          "\n\n(И ещё %d таких же в этом блоке. Одного раза хватит, чтобы понять.)" },
        { "\n\n(And %d more like it in this block. Yes, I counted. Every single one.)",
          "\n\n(И ещё %d таких же в этом блоке. Да, я посчитал. Каждый.)" },
        { "\n\n(And %d more like it in this block. *counts them on her claws*)",
          "\n\n(И ещё %d таких же в этом блоке. *пересчитывает когтями*)" },
    };
    for (int i = 0; i < g_npend; ++i) {
        pending *p = &g_pend[i];
        if (p->more) {
            const size_t l = strlen(p->msg);
            PRINT(p->msg + l, sizeof p->msg - l, more[g_brutal][g_lang], p->more);
        }
        F->report(F, p->node, g_rants[p->r].level, p->title, p->msg);
        if (p->poke && FILE_HAS(F, poke) && F->poke)
            F->poke(F, p->node, p->poke);
    }
    g_npend = 0;
}

/* A setting's value, or `def` from a host that has none. */
static const char *opt(const char *key, const char *def)
{
    if (FILE_HAS(F, setting) && F->setting) {
        const char *v = F->setting(F, key);
        if (v && *v)
            return v;
    }
    return def;
}

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
        /* (char *)p, &p, p + 1, ok ? p : NULL: still that memory */
        while (is(p, "Cast") || (is(p, "UnaryOp") && strcmp(N(p)->op, "&") == 0) ||
               (is(p, "BinaryOp") && (strcmp(N(p)->op, "+") == 0 || strcmp(N(p)->op, "-") == 0)) ||
               (is(p, "Ternary") && N(p)->first_child != at)) {
            at = p;
            p = N(p)->parent;
        }
        if (is(p, "Call")) {
            if (named(p, "free") || !in_list(N(p)->name, g_borrowers))
                return true;
            /* return memcpy(d, s, n): what memcpy gives back is d */
            static const char *const gives_back[] = { "memcpy", "memmove", "memset", "strcpy", "strncpy", "strcat",
                                                      "strncat", NULL };
            if (in_list(N(p)->name, gives_back) && N(p)->first_child == at) {
                uint32_t q = N(p)->parent;
                while (is(q, "Cast"))
                    q = N(q)->parent;
                return is(q, "Return") || is(q, "Var") || (is(q, "BinaryOp") && strcmp(N(q)->op, "=") == 0);
            }
            return false;
        }
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
    if (br && br[1] != ']' && function_of(v) != NONE) {
        bool digits = true;
        for (const char *p = br + 1; *p && *p != ']'; ++p)
            digits &= isdigit((unsigned char)*p) != 0;
        /* in C++ a size that is not a number is a template's (buf[sizeof(T)]):
         * a constant, just not yet. char (*p)[n] points at one, on nobody's stack */
        const char *star = strstr(type, "(*");
        const bool constant = strstr(br, "sizeof") || strstr(br, "alignof");   /* buf[sizeof x.field] */
        if (!digits && !g_cpp && !constant && !(star && star < br) && !decl_is_static(v))
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
            /* a type-only sizeof has no child; malloc(sizeof(struct hdr) + len) is not one thing of
             * that type: only sizeof(T) or n * sizeof(T) */
            bool typed = is(a, "Sizeof") && N(a)->first_child == NONE;
            if (is(a, "BinaryOp") && strcmp(N(a)->op, "*") == 0)
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
    uint32_t prev_end = has_text(stmt) ? N(stmt)->src_end : 0;
    for (uint32_t t = stmt != NONE ? N(stmt)->next_sibling : NONE; t != NONE; t = N(t)->next_sibling) {
        if (has_text(t)) {
            if (prev_end && words_in(prev_end, N(t)->src_begin, name) > 0)
                break;   /* a statement clang could not read uses it first: no telling */
            prev_end = N(t)->src_end;
        }
        if (!mentions(t, name))
            continue;
        const bool checks = ((is(t, "If") || is(t, "While") || is(t, "Return")) && mentions(child(t, 0), name)) ||
                            (is(t, "For") && N(t)->first_child != NONE &&
                             (mentions(child(t, 0), name) || mentions(child(t, 1), name))) ||
                            (is(t, "Call") && named(t, "assert")) || guarded(t, name);
        if (!checks && derefs(t, name) && !tree_short(t, name))
            say(t, R_UNCHECKED, name, 0);
        break;
    }

    /* given back? freed, returned, stored, handed to someone; unless the
     * text names it somewhere the tree lost */
    if (!kept_in(fn, v, name) && !tree_short(fn, name))
        say(v, R_LEAK, name, 0);
}

/* After free(p) in a Block: what the statements after it do with p. */
static void check_free(uint32_t call)
{
    const uint32_t arg = uncast(child(call, 0));
    if (!is(arg, "Ref"))
        return;
    const char *name = N(arg)->name;
    if (!call_arg_is(call, name))
        return;   /* free(p->x) the tree read as free(p), or a macro: not about p */
    const uint32_t stmt = statement_of(call);
    if (stmt != call)
        return;   /* if (x) free(p);: freed or not, depends */
    uint32_t prev_end = has_text(stmt) ? N(stmt)->src_end : 0;
    for (uint32_t t = N(stmt)->next_sibling; t != NONE; t = N(t)->next_sibling) {
        if (has_text(t)) {
            if (prev_end && words_in(prev_end, N(t)->src_begin, name) > 0)
                return;   /* text the tree lost names p: maybe p = something new */
            prev_end = N(t)->src_end;
        }
        if (is(t, "Label"))
            return;       /* reached from somewhere else too */
        bool write = false;
        const uint32_t ref = first_touch(t, name, &write);
        if (ref != NONE) {
            if (write || tree_short(t, name))
                return;   /* p = something: alive again */
            uint32_t at = ref, p = N(ref)->parent;
            while (is(p, "Cast")) {
                at = p;
                p = N(p)->parent;
            }
            if (is(p, "Call") && named(p, "free") && N(p)->first_child == at && call_arg_is(p, name))
                say(statement_of(p), R_DOUBLE_FREE, name, 0);
            else
                say(statement_of(ref), R_UAF, name, 0);
            return;
        }
        if (leaves(t))
            return;
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
        if (decl_is_static(k))
            return;
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
    if (cond == NONE || is(cond, "Block"))
        return;
    /* FIX(x) that expands to an if: the condition is in the macro, and what
     * the tree says of it may be half of it */
    if (has_text(s)) {
        const char *kw = is(s, "If") ? "if" : is(s, "While") ? "while" : "do";
        const size_t kl = strlen(kw);
        const char *t = F->source + N(s)->src_begin;
        if (N(s)->src_end - N(s)->src_begin < kl || strncmp(t, kw, kl) != 0 ||
            isalnum((unsigned char)t[kl]) || t[kl] == '_')
            return;
    }
    /* the condition is what is between its parentheses; a statement the
     * compiler could not read (a type it did not know) has none, and its
     * body is no condition: no guessing */
    if (!is(s, "DoWhile") && N(s)->src_end > N(s)->src_begin && N(s)->src_end <= F->source_len) {
        uint32_t i = N(s)->src_begin;
        while (i < N(s)->src_end && F->source[i] != '(')
            ++i;
        int depth = 0;
        for (; i < N(s)->src_end; ++i) {
            depth += F->source[i] == '(';
            depth -= F->source[i] == ')';
            if (depth == 0)
                break;
        }
        if (N(cond)->src_begin > i)
            return;
    }
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

/* "rust" as a word of its own (not trust, crust, frustrated, and not "my C
 * is a bit rusty" or a rusty_sword), rustc, rustup, rustacean, or a phrase
 * only a Rustacean writes. Where, and how long, in *at / *len. */
static bool rusty(const char *s, size_t n, size_t *at, size_t *len)
{
    static const char *const phrases[] = { "rewrite it in rust", "riir", "blazingly fast", "borrow checker",
                                           "fearless concurrency", "rustacean", "cargo build", "unwrap()", NULL };
    static const char *const tails[] = { "", "acean", "aceans", "c", "up", "fmt", "doc", "lang", NULL };
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && letter(s[i - 1]))
            continue;
        for (const char *const *p = phrases; *p; ++p) {
            const size_t pl = strlen(*p);
            if (i + pl <= n && strncasecmp(s + i, *p, pl) == 0 && (i + pl == n || !letter(s[i + pl]))) {
                *at = i;
                *len = pl;
                return true;
            }
        }
        if (i + 4 <= n && strncasecmp(s + i, "rust", 4) == 0) {
            size_t e = i + 4;
            while (e < n && letter(s[e]))
                ++e;
            for (const char *const *t = tails; *t; ++t)
                if (e - (i + 4) == strlen(*t) && strncasecmp(s + i + 4, *t, e - (i + 4)) == 0) {
                    *at = i;
                    *len = e - i;
                    return true;
                }
        }
    }
    return false;
}

/* Does the text push Rust (rewrite, port, should, better, safer, TODO...),
 * or only mention it? A phrase only a Rustacean writes counts as pushing. */
static bool advocates(const char *s, size_t n)
{
    static const char *const pushy[] = { "rewrite", "riir", "port ", "porting", "migrat", "should", "better",
                                         "safer", "blazing", "fearless", "todo", "switch to", "move to",
                                         "memory safe", "memory-safe", "borrow checker", "rustacean", "cargo build", "love",
                                         "перепис", "переход", "лучше", "безопасн", "надо", "нужно", "стоит", NULL };
    char low[600];
    size_t m = n < sizeof low - 1 ? n : sizeof low - 1;
    for (size_t i = 0; i < m; ++i)
        low[i] = (char)tolower((unsigned char)s[i]);
    low[m] = '\0';
    for (const char *const *p = pushy; *p; ++p)
        if (strstr(low, *p))
            return true;
    return false;
}

static bool rusty_name(const char *name)
{
    /* Rust's own words, not C's: a bignum's borrow, Python's borrowed
     * reference, a cache entry's lifetime, a ship's cargo, AES key unwrap,
     * a game's crab enemy are all just C */
    static const char *const words[] = { "unwrap_or", "unwrap_unchecked", "unwrap_err", "borrowck", "borrow_checker",
                                         "borrowchecker", "borrow_mut", "rustc", "rustup", "rustacean", NULL };
    char low[128];
    size_t n = 0;
    for (; name[n] && n < sizeof low - 1; ++n)
        low[n] = (char)tolower((unsigned char)name[n]);
    low[n] = '\0';
    /* is_rust, detect_rust, mentions_rust, no_rust: it hunts Rust, it is on our side */
    static const char *const hunters[] = { "is_", "has_", "detect", "mention", "find", "check", "no_", "anti",
                                           "hate", "ban", "block", "reject", "nak", NULL };
    for (const char *const *h = hunters; *h; ++h)
        if (strstr(low, *h))
            return false;
    for (const char *const *w = words; *w; ++w)
        if (strstr(low, *w))
            return true;
    if (strcmp(low, "unwrap") == 0 || (n > 7 && strcmp(low + n - 7, "_unwrap") == 0))
        return true;   /* unwrap(), result_unwrap(): Rust in C clothes; aes_unwrap_key is crypto */
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

/* Text to quote, one line: escapes of a literal read back (\" is ", \320\242
 * is the letter it spells), and cut on a whole letter. */
static void quote(char *out, size_t cap, const char *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n) {
            const char e = s[++i];
            if (e >= '0' && e <= '7') {   /* \ooo */
                unsigned v = 0;
                int k = 0;
                for (; k < 3 && i < n && s[i] >= '0' && s[i] <= '7'; ++k, ++i)
                    v = v * 8u + (unsigned)(s[i] - '0');
                --i;
                c = (unsigned char)v;
            } else if (e == 'x') {        /* \xHH */
                unsigned v = 0;
                while (i + 1 < n && isxdigit((unsigned char)s[i + 1])) {
                    ++i;
                    v = v * 16u + (unsigned)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10));
                }
                c = (unsigned char)v;
            } else {
                c = (unsigned char)(e == 'n' || e == 't' || e == 'r' ? ' ' : e);
            }
        } else if (c == '\n' || c == '\t') {
            c = ' ';
        }
        out[o++] = (char)c;
    }
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0u) == 0x80u)   /* a letter cut in two: dropped */
        --o;
    if (o > 0 && ((unsigned char)out[o - 1] & 0xC0u) == 0xC0u)
        --o;
    out[o] = '\0';
}

/* Is the literal at `k` data, not something the code says: an entry of a
 * table ({...}), or what a search looks for (strstr(name, "rust"))? */
static bool literal_is_data(uint32_t k)
{
    static const char *const lookers[] = { "strcmp", "strncmp", "strcasecmp", "strncasecmp", "strstr",
                                           "strcasestr", "memcmp", "strchr", "strrchr", "strspn", "strcspn",
                                           "regcomp", "fnmatch", NULL };
    for (uint32_t p = N(k)->parent; p != NONE; p = N(p)->parent) {
        if (is(p, "InitList"))
            return true;
        if (is(p, "Call")) {
            for (const char *const *l = lookers; *l; ++l)
                if (named(p, *l))
                    return true;
            return false;
        }
        if (is(p, "Block") || is(p, "Function"))
            return false;
    }
    return false;
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
            while (qb > b && isalnum((unsigned char)s[qb - 1]))   /* not in the middle of a word */
                --qb;
            while (qb < b + at && (s[qb] == '/' || s[qb] == '*' || s[qb] == ' '))
                ++qb;
            char q[96];
            quote(q, sizeof q, s + qb, qe - qb);
            uint32_t at_line = line;
            for (size_t k = b; k < b + at; ++k)
                at_line += s[k] == '\n';
            say(node_at_line(at_line), advocates(s + b, e - b) ? R_RUST_TEXT : R_RUST_MENTION, q, 0);
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

/* Does the text (not NUL-terminated) say `what` somewhere? */
static bool says(const char *s, size_t n, const char *what)
{
    const size_t wl = strlen(what);
    for (size_t i = 0; i + wl <= n; ++i)
        if (s[i] == what[0] && memcmp(s + i, what, wl) == 0)
            return true;
    return false;
}

/* ------------------------------------------------------------ assembly */

static const char *const g_reg64[16] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                         "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15" };

/* Which register an operand is (%rax, %%eax, al, r12d...): 0..15, or -1. */
static int reg_family(const char *op)
{
    while (*op == '%' || *op == '*' || *op == ' ')
        ++op;
    char w[8];
    size_t n = 0;
    while (n < sizeof w - 1 && isalnum((unsigned char)op[n])) {
        w[n] = (char)tolower((unsigned char)op[n]);
        ++n;
    }
    w[n] = '\0';
    if (op[n] && op[n] != ' ')
        return -1;   /* (%rax), 8(%rbp): memory, not the register */
    static const char *const names[8][5] = {
        { "rax", "eax", "ax", "al", "ah" }, { "rbx", "ebx", "bx", "bl", "bh" }, { "rcx", "ecx", "cx", "cl", "ch" },
        { "rdx", "edx", "dx", "dl", "dh" }, { "rsi", "esi", "si", "sil", NULL }, { "rdi", "edi", "di", "dil", NULL },
        { "rbp", "ebp", "bp", "bpl", NULL }, { "rsp", "esp", "sp", "spl", NULL },
    };
    for (int f = 0; f < 8; ++f)
        for (int k = 0; k < 5 && names[f][k]; ++k)
            if (strcmp(w, names[f][k]) == 0)
                return f;
    if (w[0] == 'r' && isdigit((unsigned char)w[1])) {
        const int num = atoi(w + 1);
        size_t d = 1;
        while (isdigit((unsigned char)w[d]))
            ++d;
        if (num >= 8 && num <= 15 && (!w[d] || ((w[d] == 'd' || w[d] == 'w' || w[d] == 'b') && !w[d + 1])))
            return num;
    }
    return -1;
}

/* The mnemonic without AT&T's size letter, lower case: movq -> mov. */
static void base_op(const char *m, char *out, size_t cap)
{
    const char *sp = strrchr(m, ' ');   /* rep movsb: movsb */
    if (sp)
        m = sp + 1;
    size_t n = 0;
    for (; m[n] && n + 1 < cap; ++n)
        out[n] = (char)tolower((unsigned char)m[n]);
    out[n] = '\0';
    if (n > 2 && out[n - 2] == '.' && (out[n - 1] == 'w' || out[n - 1] == 'n'))
        out[n - 2] = '\0';   /* Thumb-2's push.w, ldr.n: the same instruction */
}

static bool op_is(const char *m, const char *const *bases)
{
    char b[24];
    base_op(m, b, sizeof b);
    if (in_list(b, bases))
        return true;
    const size_t n = strlen(b);
    if (n > 2 && strchr("bwlq", b[n - 1])) {
        b[n - 1] = '\0';
        return in_list(b, bases);
    }
    return false;
}

/* Does this instruction write its destination operand? */
static bool writes_dest(const char *m)
{
    static const char *const w[] = { "mov", "movabs", "movzx", "movsx", "movsxd", "lea", "add", "sub", "adc", "sbb",
                                     "and", "or", "xor", "not", "neg", "inc", "dec", "shl", "shr", "sal", "sar",
                                     "rol", "ror", "rcl", "rcr", "imul", "pop", "xchg", "bswap", "popcnt", "lzcnt",
                                     "tzcnt", "bsf", "bsr", "xadd", "cmpxchg", "movd", "movq", "andn", NULL };
    char b[24];
    base_op(m, b, sizeof b);
    return op_is(m, w) || strncmp(b, "cmov", 4) == 0 || (strncmp(b, "set", 3) == 0 && b[3]) ||
           strncmp(b, "movz", 4) == 0 || strncmp(b, "movs", 4) == 0;
}

/* Registers it writes without naming them: rdtsc, cpuid... (bits by family). */
static unsigned implicit_writes(const char *m, int nops)
{
    char b[24];
    base_op(m, b, sizeof b);
    const unsigned A = 1u << 0, B = 1u << 1, C = 1u << 2, D = 1u << 3;
    if (strcmp(b, "rdtsc") == 0) return A | D;
    if (strcmp(b, "rdtscp") == 0) return A | C | D;
    if (strcmp(b, "cpuid") == 0) return A | B | C | D;
    if (strcmp(b, "syscall") == 0) return A | C | (1u << 11);
    if (strcmp(b, "cqo") == 0 || strcmp(b, "cdq") == 0 || strcmp(b, "cltd") == 0 || strcmp(b, "cqto") == 0 ||
        strcmp(b, "cwd") == 0)
        return D;
    static const char *const muldiv[] = { "mul", "div", "idiv", NULL };
    if (nops == 1 && op_is(m, muldiv))
        return A | D;
    if (nops == 1 && op_is(m, (const char *const[]){ "imul", NULL }))
        return A | D;
    return 0;
}

static int operand_count(uint32_t insn)
{
    int n = 0;
    for (uint32_t c = N(insn)->first_child; c != NONE; c = N(c)->next_sibling)
        ++n;
    return n;
}

/* The destination: AT&T writes it last, Intel and NASM first. */
static uint32_t dest_of(uint32_t insn, bool att)
{
    const int n = operand_count(insn);
    return n == 0 ? NONE : child(insn, att ? n - 1 : 0);
}

static bool is_insn(uint32_t k)
{
    return (N(k)->flags & CCR_PNODE_ASM) && is(k, "Call") && N(k)->name[0] != '.' && N(k)->name[0] != '#' &&
           N(k)->name[0] != '%' && N(k)->name[0] != '[' && !*N(k)->type;
}

/* --- the pieces both machines share ------------------------------------ */

/* A number as an operand says it: $16, #-16, -48, 0x10. */
static bool imm_value(const char *t, long *v)
{
    while (*t == ' ' || *t == '$' || *t == '#')
        ++t;
    if (!*t)
        return false;
    char *end;
    *v = strtol(t, &end, 0);
    if (end == t)
        return false;
    while (*end == ' ')
        ++end;
    return *end == '\0';
}

/* The same, of t[0..n). */
static bool imm_value_n(const char *t, size_t n, long *v)
{
    char buf[40];
    if (n >= sizeof buf)
        return false;
    memcpy(buf, t, n);
    buf[n] = '\0';
    return imm_value(buf, v);
}

/* A call that never comes back: exit, abort, __stack_chk_fail, a throw. */
static bool noreturn_name(const char *t)
{
    char low[96];
    size_t n = 0;
    while (t[n] && t[n] != '@' && n < sizeof low - 1) {   /* exit@PLT */
        low[n] = (char)tolower((unsigned char)t[n]);
        ++n;
    }
    low[n] = '\0';
    static const char *const parts[] = { "exit", "abort", "panic", "fatal", "_fail", "throw", "terminate",
                                         "unreachable", "longjmp", "trap", "unwind_resume", NULL };
    for (const char *const *p = parts; *p; ++p)
        if (strstr(low, *p))
            return true;
    return strcmp(low, "die") == 0 || strncmp(low, "die_", 4) == 0 || (n > 4 && strcmp(low + n - 4, "_die") == 0) ||
           strcmp(low, "err") == 0 || strcmp(low, "errx") == 0;
}

/* The functions of this file that never return (no ret, no jump anywhere):
 * a call to one is the end of the road too. */
static const char *g_noreturn[64];
static int         g_nnoreturn;

static bool calls_noreturn(uint32_t k)
{
    if (!is(k, "Call") || N(k)->first_child == NONE)
        return false;
    char b[24];
    base_op(N(k)->name, b, sizeof b);
    if (strcmp(b, "call") != 0 && strcmp(b, "callq") != 0 && strcmp(b, "calll") != 0 && strcmp(b, "bl") != 0 &&
        strcmp(b, "blx") != 0)
        return false;
    const char *target = N(N(k)->first_child)->name;
    if (noreturn_name(target))
        return true;
    const size_t tl = strcspn(target, "@");   /* die@PLT */
    for (int i = 0; i < g_nnoreturn; ++i)
        if (strlen(g_noreturn[i]) == tl && strncmp(g_noreturn[i], target, tl) == 0)
            return true;
    return false;
}

/* The code of a hat: its Block. */
static uint32_t body_of(uint32_t fn)
{
    uint32_t blk = NONE;
    for (uint32_t c = N(fn)->first_child; c != NONE; c = N(c)->next_sibling)
        if (is(c, "Block"))
            blk = c;
    return blk;
}

/* --- ARM's registers ----------------------------------------------------- */

/* An ARM register's number (x5, w5, r5: 5; fp, lr, sp, pc by name), or -1.
 * AArch64: 31 the zero register, 32 sp. */
static int arm_reg(const char *op, bool a64)
{
    char w[8];
    size_t n = 0;
    while (*op == ' ' || *op == '{')
        ++op;
    while (n < sizeof w - 1 && isalnum((unsigned char)op[n])) {
        w[n] = (char)tolower((unsigned char)op[n]);
        ++n;
    }
    w[n] = '\0';
    if (op[n] && op[n] != ' ' && op[n] != '!' && op[n] != '}' && op[n] != ',' && op[n] != ']')
        return -1;
    if (a64) {
        if (strcmp(w, "xzr") == 0 || strcmp(w, "wzr") == 0) return 31;
        if (strcmp(w, "sp") == 0 || strcmp(w, "wsp") == 0) return 32;
        if (strcmp(w, "fp") == 0) return 29;
        if (strcmp(w, "lr") == 0) return 30;
        if ((w[0] == 'x' || w[0] == 'w') && isdigit((unsigned char)w[1]) && n <= 3 &&
            (n == 2 || isdigit((unsigned char)w[2])) && atoi(w + 1) <= 30)
            return atoi(w + 1);
        return -1;
    }
    static const struct { const char *name; int r; } named[] = {
        { "fp", 11 }, { "ip", 12 }, { "sp", 13 }, { "lr", 14 }, { "pc", 15 } };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; ++i)
        if (strcmp(w, named[i].name) == 0)
            return named[i].r;
    if (w[0] == 'r' && isdigit((unsigned char)w[1]) && n <= 3 && (n == 2 || isdigit((unsigned char)w[2])) &&
        atoi(w + 1) <= 15)
        return atoi(w + 1);
    return -1;
}

/* The registers of {r4-r7, lr}, as bits; *count: how many (d8-d15 too). */
static unsigned arm_list(const char *s, int *count)
{
    unsigned bits = 0;
    int prev = -1, n = 0, prev_d = -1;
    bool range = false;
    while (*s) {
        if (*s == '-') {
            range = true;
            ++s;
            continue;
        }
        if (!isalpha((unsigned char)*s)) {
            ++s;
            continue;
        }
        if ((*s == 'd' || *s == 's' || *s == 'q') && isdigit((unsigned char)s[1])) {   /* vpush {d8-d15} */
            const int r = atoi(s + 1);
            n += range && prev_d >= 0 && r > prev_d ? r - prev_d : 1;
            prev_d = r;
        } else {
            const int r = arm_reg(s, false);
            if (r >= 0) {
                if (range && prev >= 0)
                    for (int k = prev + 1; k <= r; ++k) {
                        bits |= 1u << k;
                        ++n;
                    }
                else
                    ++n;
                bits |= 1u << r;
                prev = r;
            }
        }
        range = false;
        while (isalnum((unsigned char)*s))
            ++s;
    }
    if (count)
        *count = n;
    return bits;
}

/* [sp, #-16]!, [x29, 16], [sp]: the base register, the offset, the ! */
typedef struct arm_mem {
    int  base;
    long off;
    bool off_known;   /* no offset, or a number (not a register) */
    bool pre;         /* written back: [sp, #-16]! */
} arm_mem;

static bool arm_mem_of(const char *t, bool a64, arm_mem *m)
{
    while (*t == ' ')
        ++t;
    if (*t != '[')
        return false;
    ++t;
    m->base = arm_reg(t, a64);
    const char *close = strchr(t, ']');
    const char *comma = strchr(t, ',');
    m->off = 0;
    m->off_known = true;
    if (comma && close && comma < close)
        m->off_known = imm_value_n(comma + 1, (size_t)(close - comma - 1), &m->off);
    m->pre = close && close[1] == '!';
    return true;
}

/* --- asm("...") in C --------------------------------------------------- */

/* Past a C comment at p (p itself if there is none). */
static const char *past_comment(const char *p, const char *e)
{
    if (p + 1 < e && p[0] == '/' && p[1] == '/') {
        while (p < e && *p != '\n')
            ++p;
    } else if (p + 1 < e && p[0] == '/' && p[1] == '*') {
        p += 2;
        while (p + 1 < e && !(p[0] == '*' && p[1] == '/'))
            ++p;
        p = p + 2 <= e ? p + 2 : e;
    }
    return p;
}

/* The constraints of the outputs and the clobbers of asm(...), from its
 * text (comments in it skipped: a "// XXX:" has a colon too). */
static void asm_operands(uint32_t a, unsigned *out_regs, bool *mem_out, unsigned *clobbered, bool *memory)
{
    *out_regs = *clobbered = 0;
    *mem_out = *memory = false;
    const ccr_plugin_node *n = N(a);
    if (n->src_end <= n->src_begin || n->src_end > F->source_len)
        return;
    const char *s = F->source + n->src_begin, *e = F->source + n->src_end;
    const char *p = memchr(s, '(', (size_t)(e - s));
    if (!p)
        return;
    int depth = 0, section = -1;
    for (++p; p < e; ++p) {
        const char *after = past_comment(p, e);
        if (after != p) {
            p = after - 1;
            continue;
        }
        if (*p == '"') {
            const char *q = p + 1;
            while (q < e && *q != '"') {
                if (*q == '\\' && q + 1 < e)
                    ++q;
                ++q;
            }
            if (depth == 0 && (section == 0 || section == 2)) {
                char w[32];
                const size_t len = (size_t)(q - p - 1) < sizeof w - 1 ? (size_t)(q - p - 1) : sizeof w - 1;
                memcpy(w, p + 1, len);
                w[len] = '\0';
                if (section == 0) {   /* "=a", "+r", "=m", "=&d" */
                    for (const char *c = w; *c; ++c) {
                        static const char letters[] = "abcdSD";
                        const char *at = strchr(letters, *c);
                        if (at && *c)
                            *out_regs |= 1u << (at - letters == 4 ? 4 : at - letters == 5 ? 5 : at - letters);
                        if (*c == 'm')
                            *mem_out = true;
                    }
                } else if (strcmp(w, "memory") == 0) {
                    *memory = true;
                } else {
                    const int f = reg_family(w);
                    if (f >= 0)
                        *clobbered |= 1u << f;
                }
            }
            p = q;
            continue;
        }
        if (*p == '(')
            ++depth;
        else if (*p == ')' && depth-- == 0)
            break;
        else if (*p == ':' && depth == 0)
            ++section;
    }
}

/* Is this asm() written for ARM? x86 names its registers %%eax (or eax in
 * Intel), ARM r0, x0, w0 (%w0 for an operand) and says #1 and {r4, lr}. */
static bool asm_block_is_arm(uint32_t blk)
{
    int x86 = 0, arm = 0;
    static const char *const arm_ops[] = { "ldr", "str", "ldp", "stp", "ldrb", "strb", "adr", "adrp", "bl", "blr",
                                           "bx", "blx", "cbz", "cbnz", "svc", "msr", "mrs", "dmb", "dsb", "isb",
                                           "wfi", "wfe", "sev", "ldxr", "stxr", "ldaxr", "stlxr", "ldrex", "strex",
                                           NULL };
    for (uint32_t k = N(blk)->first_child; k != NONE; k = N(k)->next_sibling) {
        if (!is_insn(k))
            continue;
        char b[24];
        base_op(N(k)->name, b, sizeof b);
        arm += in_list(b, arm_ops);
        for (uint32_t c = N(k)->first_child; c != NONE; c = N(c)->next_sibling) {
            const char *t = N(c)->name;
            if (t[0] == '%' && t[1] == '%')
                ++x86;
            else if (t[0] == '%' && (t[1] == 'w' || t[1] == 'x') && isdigit((unsigned char)t[2]))
                ++arm;   /* %w0: an AArch64 operand */
            else if (t[0] == '#' || t[0] == '{' || (t[0] != '%' && arm_reg(t, true) >= 0 && arm_reg(t, true) <= 30 &&
                                                    (t[0] == 'x' || t[0] == 'w')) ||
                     (t[0] == 'r' && arm_reg(t, false) >= 0 && arm_reg(t, false) <= 7) || strcmp(t, "lr") == 0)
                ++arm;
            else if (t[0] == '$' || (reg_family(t) >= 0 && reg_family(t) < 8 && strlen(t) >= 3))
                ++x86;
        }
    }
    return arm > x86;
}

static void check_inline_asm(uint32_t a)
{
    const uint32_t blk = N(a)->first_child;
    if (blk == NONE || !is(blk, "Block"))
        return;
    if (asm_block_is_arm(blk))
        return;   /* the x86 rules below are not ARM's */
    bool att = true;
    for (uint32_t k = N(blk)->first_child; k != NONE; k = N(k)->next_sibling)
        if (is(k, "Call") && strcmp(N(k)->name, ".intel_syntax") == 0)
            att = false;
    unsigned outs, clob;
    bool mem_out, memory;
    asm_operands(a, &outs, &mem_out, &clob, &memory);
    for (uint32_t k = N(blk)->first_child; k != NONE; k = N(k)->next_sibling) {
        if (!is_insn(k))
            continue;
        const char *m = N(k)->name;
        char b[24];
        base_op(m, b, sizeof b);
        /* there is a builtin for that */
        static const struct { const char *op, *use; } builtins[] = {
            { "rdtsc", "__rdtsc() (<x86intrin.h>)" }, { "pause", "__builtin_ia32_pause()" },
            { "cpuid", "__get_cpuid() (<cpuid.h>)" }, { "bswap", "__builtin_bswap32() / __builtin_bswap64()" },
            { "popcnt", "__builtin_popcount()" }, { "lzcnt", "__builtin_clz()" }, { "tzcnt", "__builtin_ctz()" },
            { "bsr", "__builtin_clz()" }, { "bsf", "__builtin_ctz()" }, { "mfence", "__atomic_thread_fence()" },
        };
        for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; ++i)
            if (strncmp(b, builtins[i].op, strlen(builtins[i].op)) == 0 &&
                (!b[strlen(builtins[i].op)] || strchr("lqw", b[strlen(builtins[i].op)]))) {
                g_y = builtins[i].use;
                say(k, R_ASM_INTRINSIC, builtins[i].op, 0);
                g_y = "";
            }
        /* a register written that the compiler was not told about */
        unsigned wrote = implicit_writes(m, operand_count(k));
        const uint32_t d = dest_of(k, att);
        if (d != NONE && writes_dest(m)) {
            const int f = reg_family(N(d)->name);
            if (f >= 0 && f != 7)
                wrote |= 1u << f;
            if ((strchr(N(d)->name, '(') || strchr(N(d)->name, '[')) && !memory && !mem_out)
                say(k, R_ASM_MEMORY, N(d)->name, 0);
        }
        if (op_is(m, (const char *const[]){ "xchg", "xadd", NULL }) && operand_count(k) == 2) {
            const int f = reg_family(N(child(k, att ? 0 : 1))->name);   /* both are written */
            if (f >= 0 && f != 7)
                wrote |= 1u << f;
        }
        wrote &= ~(outs | clob);
        for (int f = 0; f < 16 && wrote; ++f)
            if (wrote & (1u << f)) {
                say(k, R_ASM_CLOBBER, g_reg64[f], 0);
                break;   /* one is enough to make the point */
            }
    }
}

/* --- .s / .asm files ---------------------------------------------------- */

static bool ends_flow(uint32_t k)
{
    char b[24];
    if (is(k, "Goto"))
        base_op(N(k)->op, b, sizeof b);
    else if (is(k, "Call"))
        base_op(N(k)->name, b, sizeof b);
    else
        return false;
    const size_t n = strlen(b);
    if (n > 2 && b[n - 2] == '.' && (b[n - 1] == 'w' || b[n - 1] == 'n'))
        b[n - 2] = '\0';   /* Thumb's b.w */
    if (is(k, "Call") && (strcmp(b, "pop") == 0 || strncmp(b, "ldm", 3) == 0))
        for (uint32_t c = N(k)->first_child; c != NONE; c = N(c)->next_sibling)
            if (strchr(N(c)->name, '{') && (arm_list(N(c)->name, NULL) & (1u << 15)))
                return true;   /* pop {r4, pc}: ARM's way back */
    if (is(k, "Call") && N(k)->first_child != NONE && strcasecmp(N(N(k)->first_child)->name, "pc") == 0 &&
        (strcmp(b, "ldr") == 0 || strcmp(b, "mov") == 0))
        return true;   /* ldr pc, [sp], #4; mov pc, lr: the old ways back */
    return strcmp(b, "jmp") == 0 || strcmp(b, "jmpq") == 0 || strcmp(b, "jmpl") == 0 || strncmp(b, "ret", 3) == 0 ||
           strcmp(b, "ud2") == 0 || strcmp(b, "hlt") == 0 || strncmp(b, "iret", 4) == 0 || strcmp(b, "b") == 0 ||
           strcmp(b, "br") == 0 || strcmp(b, "bx") == 0 || strcmp(b, "eret") == 0 || strcmp(b, "udf") == 0 ||
           strcmp(b, "brk") == 0 || strcmp(b, "bkpt") == 0 || strcmp(b, "int3") == 0;
}

/* Code that never gets out: no ret, no jump (die: ... call exit). */
static bool never_leaves(uint32_t fn)
{
    const uint32_t blk = body_of(fn);
    bool code = false;
    for (uint32_t k = blk != NONE ? N(blk)->first_child : NONE; k != NONE; k = N(k)->next_sibling) {
        if (is(k, "Goto") || (is(k, "Call") && is_insn(k) && ends_flow(k)) || (!is(k, "Call") && !is(k, "Label")))
            return false;   /* (what an #ifdef holds: maybe) */
        code |= is(k, "Call") && is_insn(k);
    }
    return code;
}

/* The end of a function: ret, jmp, a call that never returns, exit's syscall. */
static bool ends_function(uint32_t last)
{
    if (ends_flow(last) || calls_noreturn(last))
        return true;
    if (!is(last, "Call"))
        return false;
    char b[24];
    long v = 0;
    base_op(N(last)->name, b, sizeof b);
    /* a syscall or svc last: exit, most likely; a program without the C library ends so */
    return strcmp(b, "syscall") == 0 || strcmp(b, "svc") == 0 || strcmp(b, "swi") == 0 ||
           (strcmp(b, "int") == 0 && N(last)->first_child != NONE &&
            imm_value(N(N(last)->first_child)->name, &v) && v == 0x80);   /* int $0x80, int $128 */
}

/* Never comes back to its caller: no ret, no jump, and it ends in exit
 * (sigreturn, a thread's own exit). A function that just stops, without
 * ret, is not that: it falls into the next one. */
static bool never_returns(uint32_t fn)
{
    if (!never_leaves(fn))
        return false;
    const uint32_t blk = body_of(fn);
    uint32_t last = NONE;
    for (uint32_t k = N(blk)->first_child; k != NONE; k = N(k)->next_sibling)
        if (is(k, "Call") && is_insn(k))
            last = k;
    return last != NONE && ends_function(last);
}

/* A label's depth, as the jumps to it left the stack. */
typedef struct label_depth {
    const char *name;
    int         depth;
} label_depth;

/* Where the stack is at each label: as the jumps to it left it, or as the
 * code fell into it. */
typedef struct stack_walk {
    label_depth jumps[64];   /* the first jump to each label, and its depth */
    int         nj;
    label_depth seen[64];    /* each label passed, at its depth */
    int         ns;
} stack_walk;

/* At label `name`; `fell`: the code before it goes on into it. */
static void walk_label(stack_walk *w, const char *name, bool fell, int *depth, bool *known)
{
    bool jumped = false;
    for (int i = 0; i < w->nj; ++i)
        if (strcmp(w->jumps[i].name, name) == 0) {
            *depth = w->jumps[i].depth;
            jumped = true;
        }
    if (jumped)
        *known = true;
    else if (!fell)
        *known = false;   /* nobody said how they get here */
    if (*known && w->ns < 64)
        w->seen[w->ns++] = (label_depth){ name, *depth };
}

/* A jump to `name` at `depth`. Back to a label passed at another depth: a
 * loop that moves the stack (stack probes, alloca): where it ends, no
 * telling. */
static void walk_jump(stack_walk *w, const char *name, int depth, bool *known)
{
    if (!*known)
        return;
    for (int i = 0; i < w->ns; ++i)
        if (strcmp(w->seen[i].name, name) == 0) {
            if (w->seen[i].depth != depth)
                *known = false;
            return;
        }
    for (int i = 0; i < w->nj; ++i)
        if (strcmp(w->jumps[i].name, name) == 0)
            return;
    if (w->nj < 64)
        w->jumps[w->nj++] = (label_depth){ name, depth };
}

static const char *const g_reg32[16] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
                                         "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };

/* How much a push or pop moves the stack: by its letter, else its register. */
static int push_size(const char *m, const char *operand, int slot)
{
    char b[24];
    base_op(m, b, sizeof b);
    const size_t n = strlen(b);
    if (n > 4 && (strncmp(b, "push", 4) == 0 || strncmp(b, "pop", 3) == 0)) {
        const char last = b[n - 1];
        if (last == 'q')
            return 8;
        if (last == 'l')
            return 4;
        if (last == 'w')
            return 2;
    }
    while (*operand == '%' || *operand == ' ')
        ++operand;
    if (reg_family(operand) >= 0)
        return tolower((unsigned char)operand[0]) == 'e' ? 4 : tolower((unsigned char)operand[0]) == 'r' ? 8 :
                                                                                                            2;
    return slot;
}

/* lea -8(%rsp), %rsp / lea rsp, [rsp+8]: how far it moves rsp. */
static bool lea_rsp(const char *src, long *off)
{
    const char *at = strchr(src, '(');
    if (at) {   /* AT&T */
        if (!strstr(at, "sp)") || strchr(at, ','))
            return false;
        if (at == src) {
            *off = 0;
            return true;
        }
        return imm_value_n(src, (size_t)(at - src), off);
    }
    const char *br = strchr(src, '[');
    if (!br || !strstr(br, "sp"))
        return false;
    const char *sign = strpbrk(br, "+-");
    if (!sign) {
        *off = 0;
        return true;
    }
    const char *close = strchr(sign, ']');
    if (!close || !imm_value_n(sign + 1, (size_t)(close - sign - 1), off))
        return false;
    if (*sign == '-')
        *off = -*off;
    return true;
}

/* ARM instructions whose first operand is not written. */
static const char *const g_arm_no_write[] = { "cmp", "cmn", "tst", "teq", "b", "bl", "blr", "blx", "bx", "br",
                                              "ret", "svc", "nop", "push", "pop", "prfm", "dmb", "dsb", "isb",
                                              "vpush", "vpop", NULL };

/* Does the function load the stack pointer from somewhere (longjmp,
 * swapcontext, a thread's start)? Then it switches stacks and puts back
 * the callee's registers on purpose: an ordinary function's rules are not
 * its rules. arch: 0 x86, 1 AArch64, 2 32-bit ARM. */
static bool switches_stack(uint32_t fn, int arch, bool att)
{
    const uint32_t blk = body_of(fn);
    for (uint32_t k = blk != NONE ? N(blk)->first_child : NONE; k != NONE; k = N(k)->next_sibling) {
        if (!is(k, "Call") || !is_insn(k))
            continue;
        const char *m = N(k)->name;
        char b[24];
        base_op(m, b, sizeof b);
        const int nops = operand_count(k);
        long imm;
        if (arch == 0) {
            const uint32_t d = dest_of(k, att);
            if (d == NONE || reg_family(N(d)->name) != 7)
                continue;
            const uint32_t src = nops == 2 ? child(k, att ? 0 : 1) : NONE;
            if (op_is(m, (const char *const[]){ "pop", NULL }))
                return true;
            if (!writes_dest(m) || op_is(m, (const char *const[]){ "and", NULL }))
                continue;
            if (op_is(m, (const char *const[]){ "add", "sub", NULL }) && src != NONE && imm_value(N(src)->name, &imm))
                continue;
            if (op_is(m, (const char *const[]){ "mov", NULL }) && src != NONE && reg_family(N(src)->name) == 6)
                continue;
            if (op_is(m, (const char *const[]){ "lea", NULL }) && src != NONE && lea_rsp(N(src)->name, &imm))
                continue;
            return true;
        }
        const bool a64 = arch == 1;
        const int spr = a64 ? 32 : 13;
        if (nops == 0 || arm_reg(N(child(k, 0))->name, a64) != spr)
            continue;
        const int r1 = nops > 1 ? arm_reg(N(child(k, 1))->name, a64) : -1;
        const bool frame = r1 == spr || r1 == (a64 ? 29 : 11) || (!a64 && r1 == 7);
        if ((strncmp(b, "add", 3) == 0 || strncmp(b, "sub", 3) == 0) && frame)
            continue;
        if (strcmp(b, "mov") == 0 && frame)
            continue;
        if (strncmp(b, "st", 2) == 0 || in_list(b, g_arm_no_write))
            continue;
        return true;
    }
    return false;
}

/* The function's code ends without getting out (no ret, no jump): it runs
 * on into whatever is next. Said by the file, which knows what is next. */
static bool check_asm_function(uint32_t fn, bool att, bool wide, bool continued, int *ninsn)
{
    const uint32_t blk = body_of(fn);
    *ninsn = 0;
    if (blk == NONE)
        return false;
    stack_walk w = { .nj = 0 };
    int depth = 0, frame = 0;   /* depth: bytes pushed since the function began */
    const bool context = switches_stack(fn, 0, att);
    const int slot = wide ? 8 : 4;
    bool known = true, after_end = false, dead_said = false, code = false, pp_tail = false, frame_set = false,
         frame_known = false;
    unsigned saved = 0, said_callee = 0;
    /* the entry point (nobody called it), the rest of a function cut in two
     * by a label of its own (what it pushed, who knows), or a longjmp: nothing
     * to give back the usual way */
    if (strcmp(N(fn)->name, "_start") == 0 || continued || context || never_returns(fn))
        said_callee = ~0u;
    if (continued || context)
        known = false;
    uint32_t last = NONE;
    /* the callee's: rbx, rbp, r12-r15 (32-bit: ebx, ebp; esi and edi are too
     * in i386's cdecl, but x32's arguments: not said) */
    const unsigned callee = wide ? (1u << 1) | (1u << 6) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15)
                                 : (1u << 1) | (1u << 6);
    for (uint32_t k = N(blk)->first_child; k != NONE; k = N(k)->next_sibling) {
        if (outside(k))
            continue;
        if (is(k, "Label")) {
            after_end = false;
            walk_label(&w, N(k)->name, last == NONE || !ends_flow(last), &depth, &known);
            continue;
        }
        if (!is(k, "Call") && !is(k, "Goto")) {   /* #ifdef ... #endif: what is in it, maybe */
            known = false;
            after_end = false;
            pp_tail = true;
            continue;
        }
        if (is(k, "Call") && !is_insn(k)) {   /* .size, .cfi_...: not code */
            if (strcmp(N(k)->name, ".intel_syntax") == 0)
                att = false;
            else if (strcmp(N(k)->name, ".att_syntax") == 0)
                att = true;
            else if (strncmp(N(k)->name, ".inst", 5) == 0 || strcmp(N(k)->name, ".byte") == 0) {
                pp_tail = true;   /* an instruction written as a number: what it does, who knows */
                after_end = false;
            }
            continue;
        }
        pp_tail = false;
        if (after_end && !dead_said) {
            say(k, R_ASM_DEAD, is(k, "Goto") ? N(k)->op : N(k)->name, 0);
            dead_said = true;
        }
        code = true;
        last = k;
        ++*ninsn;
        if (is(k, "Goto")) {
            walk_jump(&w, N(k)->name, depth, &known);
            after_end = ends_flow(k);
            continue;
        }
        const char *m = N(k)->name;
        char b[24];
        base_op(m, b, sizeof b);
        const int nops = operand_count(k);
        const uint32_t d = dest_of(k, att);
        const int f = d != NONE ? reg_family(N(d)->name) : -1;
        const uint32_t src = nops == 2 ? child(k, att ? 0 : 1) : NONE;
        const int sf = src != NONE ? reg_family(N(src)->name) : -1;
        long imm = 0;
        if (op_is(m, (const char *const[]){ "call", NULL }) && nops == 1) {
            /* call 1f / 1: pop %edx: a call to the very next line pushes its
             * address (i386's way to know where it is) */
            uint32_t nx = N(k)->next_sibling;
            while (nx != NONE && is(nx, "Call") && !is_insn(nx))
                nx = N(nx)->next_sibling;
            const char *t = N(child(k, 0))->name;
            const size_t tl = strlen(t);
            if (nx != NONE && is(nx, "Label") &&
                (strcmp(t, N(nx)->name) == 0 ||
                 (tl > 1 && t[tl - 1] == 'f' && strlen(N(nx)->name) == tl - 1 && strncmp(t, N(nx)->name, tl - 1) == 0)))
                depth += slot;
        }
        if (op_is(m, (const char *const[]){ "push", "pushf", NULL })) {
            depth += push_size(m, nops ? N(child(k, 0))->name : "", slot);
            if (f >= 0)
                saved |= 1u << f;
        } else if (op_is(m, (const char *const[]){ "pop", "popf", NULL })) {
            depth -= push_size(m, nops ? N(child(k, 0))->name : "", slot);
        } else if (strncmp(b, "ret", 3) == 0) {
            if (known && depth != 0 && !context)
                say(k, R_ASM_STACK, "", depth > 0 ? depth : -depth);
        } else if (strcmp(b, "leave") == 0 || strcmp(b, "leaveq") == 0 || strcmp(b, "leavel") == 0) {
            depth = frame - slot;   /* mov %rbp, %rsp; pop %rbp */
            known = frame_set && frame_known;
        } else if (f == 7 && writes_dest(m)) {   /* rsp itself */
            if (op_is(m, (const char *const[]){ "sub", NULL }) && src != NONE && imm_value(N(src)->name, &imm))
                depth += (int)imm;
            else if (op_is(m, (const char *const[]){ "add", NULL }) && src != NONE && imm_value(N(src)->name, &imm))
                depth -= (int)imm;
            else if (op_is(m, (const char *const[]){ "mov", NULL }) && sf == 6 && frame_set) {
                depth = frame;   /* mov %rbp, %rsp: back to the frame */
                known = frame_known;
            } else if (op_is(m, (const char *const[]){ "lea", NULL }) && src != NONE && lea_rsp(N(src)->name, &imm))
                depth -= (int)imm;
            else
                known = false;   /* and $-16, %rsp: where it is now, nobody can say */
        }
        if (sf >= 0 && d != NONE && f < 0 && op_is(m, (const char *const[]){ "mov", NULL }) &&
            (strchr(N(d)->name, '(') || strchr(N(d)->name, '[')))
            saved |= 1u << sf;   /* mov %rbx, 8(%rdi): kept somewhere, to be put back */
        if (f == 6 && sf == 7 && op_is(m, (const char *const[]){ "mov", NULL })) {
            frame = depth;   /* mov %rsp, %rbp */
            frame_set = true;
            frame_known = known;
        }
        if (f >= 0 && ((1u << f) & callee) && writes_dest(m) && !op_is(m, (const char *const[]){ "pop", NULL }) &&
            !((1u << f) & saved) && !((1u << f) & said_callee)) {
            said_callee |= 1u << f;
            g_y = wide ? "rax, rcx, rdx, rsi, rdi, r8-r11" : "eax, ecx, edx";
            say(k, R_ASM_CALLEE, wide ? g_reg64[f] : g_reg32[f], 0);
            g_y = "";
        }
        /* mov $0, %eax: xor is shorter */
        if (nops == 2 && op_is(m, (const char *const[]){ "mov", NULL }) && f >= 0) {
            const char *s = N(child(k, att ? 0 : 1))->name;
            if (strcmp(s, att ? "$0" : "0") == 0)
                say(k, R_ASM_ZERO, N(d)->name[0] == '%' ? N(d)->name + 1 : N(d)->name, 0);
        }
        after_end = ends_flow(k);
    }
    return code && last != NONE && !pp_tail && !ends_function(last);
}

/* --- ARM ---------------------------------------------------------------- */

static bool check_arm_function(uint32_t fn, bool a64, bool continued, int *ninsn)
{
    const uint32_t blk = body_of(fn);
    *ninsn = 0;
    if (blk == NONE)
        return false;
    const bool context = switches_stack(fn, a64 ? 1 : 2, true);
    const unsigned callee = a64 ? 0x3FF80000u /* x19-x29 */ : 0x0FF0u /* r4-r11 */;
    const int lr = a64 ? 30 : 14, spr = a64 ? 32 : 13, fpr = a64 ? 29 : 11;
    stack_walk w = { .nj = 0 };
    int depth = 0, frame = 0, frame_reg = -1;   /* depth in bytes; frame: the depth when fp took sp */
    bool known = true, after_end = false, dead_said = false, code = false, lr_saved = false, said_stack = false,
         table = false, pp_tail = false, frame_known = false;
    /* (nor a function that never comes back: sigreturn, exit) */
    unsigned saved = 0, said_callee = strcmp(N(fn)->name, "_start") == 0 || continued || context || never_returns(fn)
                                          ? ~0u : 0u;
    if (continued || context)
        known = false;
    uint32_t last = NONE, unsaved_bl = NONE;
    for (uint32_t k = N(blk)->first_child; k != NONE; k = N(k)->next_sibling) {
        if (outside(k))
            continue;
        if (is(k, "Label")) {
            after_end = false;
            walk_label(&w, N(k)->name, last == NONE || !ends_flow(last), &depth, &known);
            continue;
        }
        if (!is(k, "Call") && !is(k, "Goto")) {   /* #ifdef ... #endif */
            known = false;
            after_end = false;
            pp_tail = true;
            continue;
        }
        if (is(k, "Call") && !is_insn(k)) {
            if (strncmp(N(k)->name, ".inst", 5) == 0) {
                pp_tail = true;   /* .inst 0xe7f000f0: an instruction written as a number (a trap, here) */
                after_end = false;
            }
            continue;
        }
        pp_tail = false;
        if (after_end && !dead_said && !table) {
            say(k, R_ASM_DEAD, is(k, "Goto") ? N(k)->op : N(k)->name, 0);
            dead_said = true;
        }
        code = true;
        last = k;
        ++*ninsn;
        if (is(k, "Goto")) {
            walk_jump(&w, N(k)->name, depth, &known);
            after_end = ends_flow(k);
            continue;
        }
        char b[24];
        base_op(N(k)->name, b, sizeof b);
        const int nops = operand_count(k);
        const char *op0 = nops > 0 ? N(child(k, 0))->name : "";
        const char *op1 = nops > 1 ? N(child(k, 1))->name : "";
        const char *op2 = nops > 2 ? N(child(k, 2))->name : "";
        const char *opl = nops > 0 ? N(child(k, nops - 1))->name : "";
        const int r0 = arm_reg(op0, a64), r1 = arm_reg(op1, a64);
        bool returns = false;
        /* the memory operand of a load or store, and what comes after it */
        int mem_at = -1;
        arm_mem mem = { -1, 0, true, false };
        for (int q = 0; q < nops; ++q)
            if (arm_mem_of(N(child(k, q))->name, a64, &mem)) {
                mem_at = q;
                break;
            }
        long post = 0;
        const bool has_post = mem_at >= 0 && mem_at + 1 < nops && imm_value(N(child(k, mem_at + 1))->name, &post);
        const bool store = strncmp(b, "st", 2) == 0 && strncmp(b, "stm", 3) != 0;
        const bool load = strncmp(b, "ld", 2) == 0 && strncmp(b, "ldm", 3) != 0;
        if ((store || load) && mem_at >= 0 && mem.base == spr) {
            if (mem.pre && mem.off_known)
                depth -= (int)mem.off;    /* [sp, #-16]! */
            else if (mem.pre)
                known = false;
            if (has_post)
                depth -= (int)post;       /* [sp], #16 */
        }
        if (store && mem_at > 0)
            for (int q = 0; q < mem_at; ++q) {   /* stp x19, x20, [sp, #16], or into a jmp_buf: saved */
                const int r = arm_reg(N(child(k, q))->name, a64);
                if (r >= 0 && r <= 30) {
                    saved |= 1u << r;
                    lr_saved |= r == lr;
                }
            }
        if (load && mem_at >= 0 && r0 == 15 && !a64)
            returns = true;   /* ldr pc, [sp], #4 */
        if (!a64) {
            const bool sp_list = nops == 2 && arm_reg(op0, false) == 13 && strchr(op0, '!');
            const bool push = strcmp(b, "push") == 0 || strcmp(b, "vpush") == 0 ||
                              ((strcmp(b, "stmfd") == 0 || strcmp(b, "stmdb") == 0 || strcmp(b, "vstmdb") == 0) &&
                               sp_list);
            const bool pop = strcmp(b, "pop") == 0 || strcmp(b, "vpop") == 0 ||
                             ((strcmp(b, "ldmfd") == 0 || strcmp(b, "ldmia") == 0 || strcmp(b, "ldm") == 0 ||
                               strcmp(b, "vldmia") == 0) &&
                              sp_list);
            int count = 0;
            const unsigned bits = arm_list(opl, &count);
            const int unit = b[0] == 'v' && strchr(opl, 'd') ? 8 : 4;
            if (push) {
                saved |= bits;
                lr_saved |= (bits >> lr) & 1u;
                depth += count * unit;
            } else if (pop) {
                depth -= count * unit;
                returns |= (bits >> 15) & 1u;
            } else if (strncmp(b, "stm", 3) == 0 && nops == 2 && (arm_reg(op0, false) == 13 || arm_reg(op0, false) == 11)) {
                saved |= bits;   /* stm sp, {ip, lr} */
                lr_saved |= (bits >> lr) & 1u;
            }
            returns |= strcmp(b, "bx") == 0 && r0 == 14;
            returns |= strcmp(b, "mov") == 0 && r0 == 15 && r1 == 14;
            /* add pc, pc, r3, lsl #2: a jump table, what follows is reached */
            if (r0 == 15 && !returns && strcmp(b, "bx") != 0 && !(load && mem.base == spr))
                table = true;
        }
        /* sp moved by hand, and the frame register */
        const bool add = strcmp(b, "add") == 0 || strcmp(b, "adds") == 0 || strcmp(b, "addw") == 0;
        const bool sub = strcmp(b, "sub") == 0 || strcmp(b, "subs") == 0 || strcmp(b, "subw") == 0;
        const char *const *no_write = g_arm_no_write;
        long imm = 0;
        bool imm_ok = nops >= 3 && imm_value(op2, &imm);
        if (imm_ok && nops >= 4) {   /* #1, lsl #12 */
            const char *sh = N(child(k, 3))->name;
            long amount;
            if (strncasecmp(sh, "lsl", 3) == 0 && imm_value(sh + 3, &amount) && amount >= 0 && amount < 31)
                imm <<= amount;
            else
                imm_ok = false;
        }
        if (r0 == spr && (add || sub || strcmp(b, "mov") == 0)) {
            if ((add || sub) && r1 == spr && imm_ok)
                depth += sub ? (int)imm : -(int)imm;
            else if ((add || sub) && r1 >= 0 && r1 == frame_reg && imm_ok) {
                depth = sub ? frame + (int)imm : frame - (int)imm;   /* sub sp, fp, #4 */
                known = frame_known;
            } else if (strcmp(b, "mov") == 0 && r1 >= 0 && r1 == frame_reg) {
                depth = frame;   /* mov sp, x29 */
                known = frame_known;
            } else {
                known = false;
            }
        } else if (r0 == spr && !store && !in_list(b, no_write)) {
            known = false;   /* sp written some other way: where it is now, nobody can say */
        } else if (r0 >= 0 && r0 != spr && r1 == spr && (strcmp(b, "mov") == 0 || ((add || sub) && imm_ok)) &&
                   (r0 == fpr || (!a64 && r0 == 7))) {
            frame_reg = r0;   /* mov x29, sp; add fp, sp, #4; add r7, sp, #0 */
            frame = strcmp(b, "mov") == 0 ? depth : add ? depth - (int)imm : depth + (int)imm;
            frame_known = known;
        } else if (r0 >= 0 && r0 == frame_reg && r1 == frame_reg && (add || sub) && imm_ok) {
            frame += add ? -(int)imm : (int)imm;   /* adds r7, r7, #32 */
        }
        if (a64)
            returns |= strcmp(b, "ret") == 0;
        /* bl: the way back gets overwritten, unless it was saved */
        if ((strcmp(b, "bl") == 0 || strcmp(b, "blr") == 0 || strcmp(b, "blx") == 0) && !lr_saved &&
            unsaved_bl == NONE)
            unsaved_bl = k;
        if (returns) {
            if (unsaved_bl != NONE) {
                g_y = a64 ? "x30" : "lr";
                say(unsaved_bl, R_ARM_LR, operand_count(unsaved_bl) ? N(child(unsaved_bl, 0))->name : "", 0);
                g_y = "";
                unsaved_bl = NONE;
            }
            if (known && depth != 0 && !said_stack && !context) {
                say(k, R_ARM_STACK, "", depth > 0 ? depth : -depth);
                said_stack = true;
            }
        }
        /* writing a register that is the caller's */
        if (nops > 0 && !in_list(b, no_write) && !store && strncmp(b, "stm", 3) != 0 && strncmp(b, "ldm", 3) != 0 &&
            strncmp(b, "cb", 2) != 0 && strncmp(b, "tb", 2) != 0) {
            const int r = arm_reg(op0, a64);
            if (r >= 0 && r < 31 && ((1u << r) & callee) && !((1u << r) & saved) && !((1u << r) & said_callee)) {
                said_callee |= 1u << r;
                g_y = a64 ? "x0-x18" : "r0-r3, r12";
                say(k, R_ASM_CALLEE, op0, 0);
                g_y = "";
            }
        }
        after_end = ends_flow(k);
    }
    return code && last != NONE && !pp_tail && !ends_function(last);
}

/* Which machine the file is for, from what its operands say: 0 x86, 1
 * AArch64, 2 32-bit ARM; -1 none of those (MIPS, RISC-V, PowerPC, SuperH...):
 * their rules are not these, so nothing is said about their functions. */
static int asm_arch(void)
{
    int x86 = 0, a64 = 0, a32 = 0, armish = 0, other = 0, armops = 0;
    /* what only ARM says (SuperH, IA-64, SPARC have r1 and #1 too) */
    static const char *const arm_only[] = { "ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldp", "stp", "ldm",
                                            "ldmia", "ldmfd", "stm", "stmdb", "stmfd", "bl", "bx", "blx", "svc",
                                            "swi", "adr", "adrp", "cbz", "cbnz", "movw", "movt", "mrs", "msr",
                                            "push", "pop", NULL };
    for (uint32_t k = 0; k < F->node_count; ++k) {
        if ((is(k, "Call") || is(k, "Goto")) && (N(k)->flags & CCR_PNODE_ASM) && (is(k, "Goto") || is_insn(k))) {
            const char *m = is(k, "Goto") ? N(k)->op : N(k)->name;
            const size_t n = strlen(m);
            other += strncmp(m, "l.", 2) == 0 || (n > 2 && m[n - 2] == '.' && (m[n - 1] == 'l' || m[n - 1] == 'b'));
            char b[24];
            base_op(m, b, sizeof b);
            armops += in_list(b, arm_only) || (strncmp(b, "b.", 2) == 0 && is(k, "Goto"));
            continue;   /* or1k's l.addi, SuperH's mov.l */
        }
        if (!is(k, "Text") || N(k)->parent == NONE || !(N(N(k)->parent)->flags & CCR_PNODE_ASM))
            continue;
        const char *t = N(k)->name;
        const int f = reg_family(t);
        const char *w = t;
        while (*w == '%')
            ++w;
        const size_t wl = strlen(w);
        if (f >= 0 && (f < 8 ? wl >= 3 || (t[0] == '%' && f < 4) : strchr("dwb", w[wl - 1]) != NULL))
            ++x86;   /* rax, %eax, %al, %r8d; not a bare r8, s390's %r15, m68k's and SPARC's %sp */
        else if (t[0] == '@')
            ++other;   /* SuperH: @r15 */
        else if (t[0] == '#' || t[0] == '{' || t[0] == '[')
            ++armish;
        else if ((t[0] == 'x' || t[0] == 'w') && arm_reg(t, true) >= 0 && arm_reg(t, true) <= 30)
            ++a64;
        else if (t[0] == 'r' && isdigit((unsigned char)t[1]) && arm_reg(t, false) >= 0)
            ++a32;
    }
    if (x86 > 0 && x86 >= a64 + a32)
        return 0;
    if (other > 0 || armish == 0 || a64 + a32 == 0 || armops == 0)
        return -1;
    return a64 >= a32 ? 1 : 2;
}

/* .section .note.GNU-stack, or clang's ".note.GNU-stack" */
static bool is_note_stack(const char *s)
{
    if (*s == '"')
        ++s;
    return strncmp(s, ".note.GNU-stack", 15) == 0 && (s[15] == '\0' || s[15] == '"');
}

static void check_asm_file(void)
{
    const size_t pl = strlen(F->path);
    const bool nasm = (pl > 4 && strcasecmp(F->path + pl - 4, ".asm") == 0) ||
                      (pl > 5 && strcasecmp(F->path + pl - 5, ".nasm") == 0);
    /* AT&T unless it says otherwise (.intel_syntax, not a string that
     * mentions it): registers with %, the destination last */
    bool att = !nasm;
    const int arch = nasm ? 0 : asm_arch();
    /* 64-bit x86 unless only 32-bit registers are named */
    bool wide = nasm || says(F->source, F->source_len, "rsp") || says(F->source, F->source_len, "rax") ||
                says(F->source, F->source_len, "rbp") || says(F->source, F->source_len, "rdi") ||
                !(says(F->source, F->source_len, "esp") || says(F->source, F->source_len, "eax"));
    /* the functions of this file that never come back */
    g_nnoreturn = 0;
    for (uint32_t h = N(F->root)->first_child; h != NONE; h = N(h)->next_sibling)
        if (is(h, "Function") && body_of(h) != NONE && never_leaves(h) && g_nnoreturn < 64)
            g_noreturn[g_nnoreturn++] = N(h)->name;
    bool any_code = false, note = false;
    /* a function that runs off its end: said when what comes next is known */
    uint32_t falls = NONE;
    int falls_insns = 0;
    char section[64] = ".text";
    for (uint32_t h = N(F->root)->first_child; h != NONE; h = N(h)->next_sibling) {
        if (!is(h, "Function") || outside(h))
            continue;
        if (strcmp(N(h)->type, "section") == 0)
            snprintf(section, sizeof section, "%s", N(h)->name);
        const bool code = strcmp(N(h)->type, "label") == 0 || strcmp(N(h)->type, "function") == 0;
        /* the rest of the one before, cut off by a label of its own */
        const bool continued = falls != NONE && strcmp(N(h)->type, "label") == 0;
        if (falls != NONE) {
            /* into a label of its own (a function cut in two: musl's __cp_begin),
             * or a one-liner that is another name for the next one (ldexp: nop,
             * then scalbn): on purpose. Into anything else: say it */
            if (!(code && (strcmp(N(h)->type, "label") == 0 || falls_insns <= 1)))
                say(falls, R_ASM_FALLTHROUGH, N(falls)->name, 0);
            falls = NONE;
        }
        if (code) {
            /* .init and .fini are glued together from several files (crti.s,
             * crtn.s): running on is how they work */
            const bool glued = strncmp(section, ".init", 5) == 0 || strncmp(section, ".fini", 5) == 0;
            bool fell = false;
            int n = 0;
            if (arch == 1 || arch == 2)
                fell = check_arm_function(h, arch == 1, continued, &n);
            else if (arch == 0)
                fell = check_asm_function(h, att, wide, continued, &n);
            if (fell && !glued) {
                falls = h;
                falls_insns = n;
            }
            any_code = true;
        }
        const uint32_t blk = body_of(h);   /* the syntax and section it leaves for the next one */
        for (uint32_t k = blk != NONE ? N(blk)->first_child : NONE; k != NONE; k = N(k)->next_sibling) {
            if (!is(k, "Call"))
                continue;
            const char *d = N(k)->name;
            if (strcmp(d, ".intel_syntax") == 0 && !nasm)
                att = false;
            else if (strcmp(d, ".att_syntax") == 0 && !nasm)
                att = true;
            else if (strcmp(d, ".section") == 0 && N(k)->first_child != NONE)
                snprintf(section, sizeof section, "%s", N(N(k)->first_child)->name);
            else if (strcmp(d, ".text") == 0 || strcmp(d, ".data") == 0 || strcmp(d, ".bss") == 0)
                snprintf(section, sizeof section, "%s", d);
        }
    }
    if (falls != NONE)
        say(falls, R_ASM_FALLTHROUGH, N(falls)->name, 0);
    for (uint32_t k = 0; k < F->node_count; ++k)
        note |= is(k, "Call") && strcmp(N(k)->name, ".section") == 0 && N(k)->first_child != NONE &&
                is_note_stack(N(N(k)->first_child)->name);
    if (!nasm && any_code && !note) {
        uint32_t first = N(F->root)->first_child;
        say(first, R_ASM_NOSTACK, "", 0);
    }
}

static void check(const ccr_plugin_file *file)
{
    const bool cpp = strcmp(file->language, "c++") == 0, assembly = strcmp(file->language, "asm") == 0;
    g_cpp = cpp;
    if ((strcmp(file->language, "c") != 0 && !cpp && !assembly) || file->root == NONE)
        return;   /* CMake: somebody else's problem */
    F = file;
    g_lang = strncmp(file->ui_language, "ru", 2) == 0 ? 1 : 0;
    memset(g_said, 0, sizeof g_said);
    g_total = 0;
    g_npend = 0;
    const char *b = opt("brutality", "mother_cat");
    g_brutal = strcmp(b, "grumpy") == 0 ? 0 : strcmp(b, "linus") == 0 ? 1 : 2;
    g_opt_praise = strcmp(opt("praise", "on"), "off") != 0;
    g_opt_rust = strcmp(opt("rust", "on"), "off") != 0;
    g_opt_asm = strcmp(opt("asm", "on"), "off") != 0;
    g_opt_nits = strcmp(opt("nits", "on"), "off") != 0;
    if (assembly) {
        check_asm_file();
        say_all();
        F = NULL;
        return;
    }
    /* context: is this file Code Hater itself? (a plugin, its name, its rants) */
    g_self = says(F->source, F->source_len, "ccr_plugin.h") && says(F->source, F->source_len, "\"code_hater\"") &&
             says(F->source, F->source_len, "R_RUST_TEXT");
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
        else if (strcmp(kind, "Asm") == 0)
            check_inline_asm(k);
        else if (cpp && strcmp(kind, "New") == 0)
            say(statement_of(k), R_RAW_NEW, *N(k)->type ? N(k)->type : "T", 0);
        else if (cpp && strcmp(kind, "Using") == 0 && is_header(F->path) && N(k)->parent == F->root && N(k)->src_end > N(k)->src_begin &&
                 strncmp(F->source + N(k)->src_begin, "using namespace", 15) == 0)
            say(k, R_USING_HEADER, N(k)->name, 0);
        else if (strcmp(kind, "String") == 0 && !g_self && !literal_is_data(k)) {
            size_t at, len;
            /* as the file spells it (the tree's own copy has its letters escaped) */
            const char *s = N(k)->name;
            size_t sl = strlen(s);
            if (N(k)->src_end > N(k)->src_begin && N(k)->src_end <= F->source_len) {
                s = F->source + N(k)->src_begin;
                sl = N(k)->src_end - N(k)->src_begin;
            }
            if (rusty(s, sl, &at, &len)) {
                char q[96];
                if (sl >= 2 && s[0] == '"' && s[sl - 1] == '"') {   /* without its quotes */
                    ++s;
                    sl -= 2;
                }
                quote(q, sizeof q, s, sl);
                say(statement_of(k), advocates(s, sl) ? R_RUST_TEXT : R_RUST_MENTION, q, 0);
            }
        }
        if (!g_self &&
            (strcmp(kind, "Var") == 0 || strcmp(kind, "Function") == 0 || strcmp(kind, "Param") == 0 ||
             strcmp(kind, "Field") == 0 || strcmp(kind, "Struct") == 0 || strcmp(kind, "Typedef") == 0) &&
            rusty_name(N(k)->name))
            say(is(N(k)->parent, "Function") ? N(k)->parent : k, R_RUST_NAME, N(k)->name, 0);
    }
    if (g_self) {   /* says so once, on its own plugin descriptor (or the first thing in the file) */
        uint32_t at = NONE;
        for (uint32_t k = 0; k < F->node_count && at == NONE; ++k)
            if (is(k, "Var") && strstr(N(k)->type, "ccr_plugin") && N(k)->parent == F->root)
                at = k;
        say(at != NONE ? at : N(F->root)->first_child, R_SELF, "", 0);
    } else {
        check_rust_text();
    }
    say_all();
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
        return "Ревьюит твой C и ассемблер так, как это делали в старых рассылках: громко. Висячие указатели, "
               "обращение после free, утечки, gets(), регистры, испорченные за спиной компилятора, кривой стек и "
               "всё, что пахнет Rust. На уровне «мама-кошка» тычет мордой в баг. Злится на код, а не на тебя. "
               "В основном.";
    return "Reviews your C and assembly the way the old mailing lists did: loudly. Dangling pointers, use after "
           "free, leaks, gets(), registers trashed behind the compiler's back, unbalanced stacks, and anything "
           "that smells of Rust. At \"mother cat\" it rubs your nose in the bug. Hates the code, not you. Mostly.";
}

/* ------------------------------------------------------------- settings */

static const char *const g_brutal_values[] = { "grumpy", "linus", "mother_cat", NULL };
static const char *const g_brutal_titles[] = { "Grumpy", "Linus on a bad day", "Mother cat", NULL };

static const ccr_plugin_setting g_settings[] = {
    { "brutality", CCR_SETTING_CHOICE, "How brutal",
      "Grumpy says what is wrong. Linus says it the way the old mailing lists did. The mother cat grabs you by "
      "the scruff and, when you save or build, rubs your nose in every bug.",
      g_brutal_values, g_brutal_titles, "mother_cat" },
    { "praise", CCR_SETTING_TOGGLE, "Praise", "Says so when something is done right. It happens.", NULL, NULL,
      "on" },
    { "nits", CCR_SETTING_TOGGLE, "Nits", "The small stuff: style and taste, not bugs.", NULL, NULL, "on" },
    { "rust", CCR_SETTING_TOGGLE, "Rust patrol", "Anything that smells of Rust: names, strings, propaganda.", NULL,
      NULL, "on" },
    { "asm", CCR_SETTING_TOGGLE, "Assembly",
      "asm() in C and .s/.asm files: registers trashed behind the compiler's back, unbalanced stacks, "
      "callee-saved registers, functions that fall off their end.", NULL, NULL, "on" },
};

static const char *hater_setting_text(const char *lang, const char *what)
{
    if (strncmp(lang, "ru", 2) != 0)
        return NULL;
    static const struct { const char *what, *ru; } t[] = {
        { "brutality", "Насколько жёстко" },
        { "brutality.hint", "Ворчун говорит, что не так. Линус говорит это так, как в старых рассылках. Мама-кошка "
                            "хватает за шкирку и, когда сохраняешь или собираешь, тычет мордой в каждый баг." },
        { "brutality=grumpy", "Ворчун" },
        { "brutality=linus", "Линус в плохой день" },
        { "brutality=mother_cat", "Мама-кошка" },
        { "praise", "Похвала" },
        { "praise.hint", "Говорит, когда что-то сделано правильно. Бывает и такое." },
        { "nits", "Придирки" },
        { "nits.hint", "Мелочи: стиль и вкус, а не баги." },
        { "rust", "Rust-патруль" },
        { "rust.hint", "Всё, что пахнет Rust: имена, строки, пропаганда." },
        { "asm", "Ассемблер" },
        { "asm.hint", "asm() в C и файлы .s/.asm: регистры, испорченные за спиной компилятора, кривой стек, "
                      "чужие регистры, функции, которые проваливаются в следующую." },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i)
        if (strcmp(t[i].what, what) == 0)
            return t[i].ru;
    return NULL;
}

static const ccr_plugin g_plugin = {
    .abi_version = CCR_PLUGIN_ABI_VERSION,
    .struct_size = sizeof(ccr_plugin),
    .kinds       = CCR_PLUGIN_CHECKER | CCR_PLUGIN_PERSONALITY,
    .name        = "code_hater",
    .version     = "1.1.0",
    .description = "Reviews your C the way the old mailing lists did: loudly.",
    .init        = hater_init,
    .shutdown    = hater_shutdown,
    .title       = "Code Hater",
    .author      = "Ccratch",
    .describe    = hater_describe,
    .check       = check,
    .color       = 0xD7263DFFu,
    .settings    = g_settings,
    .setting_count = sizeof g_settings / sizeof g_settings[0],
    .setting_text  = hater_setting_text,
};

CCR_PLUGIN_EXPORT const ccr_plugin *ccr_plugin_entry(void);

CCR_PLUGIN_EXPORT const ccr_plugin *ccr_plugin_entry(void)
{
    return &g_plugin;
}
