<p align="center"><img src="assets/icon.png" width="160" alt="Ccratch icon"></p>

# Ccratch

Scratch, but for raw C. No ready-made loops, no hand-holding, no Cargo.
You drag `while`, `switch` and `break` around yourself, and the cat can segfault.

> Memory safety as a lifestyle choice.

## Status

- Step 1: a window, a pannable canvas and one lonely block.
- Step 2: libclang reads a `.c` file into Ccratch's own syntax tree.
- Step 3: that tree is drawn as Scratch-style blocks.
- Step 4: blocks can be dragged.
- Step 5: Ctrl+S writes the moves back to the `.c` file.
- Step 6: Ctrl+Z / Ctrl+Y undo and redo.
- Step 7: a palette of new blocks, so you can write code, not only move it.
- Step 8: a Blender-style pie menu, plus delete, duplicate and comment out.
- Step 9: the interface speaks other languages, from packs you drop in a folder.
- Step 10: expressions come out of their slots and go into other ones.
- Step 11: every `goto` has an arrow to its label.
- Step 12: themes, as files, and four of them to start with.

![examples/tour.c as blocks](docs/tour.png)

Real code works too. This is raylib's own `rshapes.c` (2400 lines):

![rshapes.c as blocks](docs/rshapes.png)

How the colors map to C:

| Color | What |
| --- | --- |
| Yellow hat | function definition |
| Gold | control flow: `if`, `for`, `while`, `switch`, `return`, `goto` |
| Orange | variables, assignment, `->`, `[]` |
| Green | operators; hexagons are conditions, like in Scratch |
| Red | pointer `*` and `&` (future wires start here) |
| Pink | function calls and declarations |
| Blue | types: `struct`, `enum`, `typedef` |
| Purple | preprocessor: `#include`, `#define`, macro values like `NULL` |
| White | literals |
| Grey | "here be dragons": no block for this yet, raw code shown |

Blocks fit together like a puzzle, as in Scratch: a notch on top of each
statement, a tab under it, so you can see where one ends and the next one
starts. `return`, `break`, `continue` and `goto` have flat bottoms:
nothing after them runs, and the shape says so.

Dragging works like Scratch:

- grab a statement and it comes with everything under it in its stack;
- grab a function hat, a struct or an `#include` and the whole thing moves;
- drop a stack near the white bar and it joins that stack, or into an
  empty `if`/`while`/`for` mouth;
- drop it anywhere else and it lies loose on the canvas, faded.
  **Loose blocks are not part of the program**: dragging a block out is
  how you remove it from the code.

Every move edits the syntax tree itself, and Ctrl+S writes it back.

## Writing new code

The palette on the left has `if`, `if / else`, `while`, `for`,
`do / while`, `return`, `break`, `continue` and a **code line** for
anything else (a call, an assignment, a declaration). Drag one out and it
behaves like any block: it snaps into stacks, takes other blocks inside it,
and old statements can be dropped into it.

The white fields are typed into: click one, type, Enter keeps it, Esc
throws it away, Ctrl+V pastes. Each edit is one undo step.

Drop a block back onto the palette to delete it (Ctrl+Z brings it back).
That works for statements from the file too.

![the palette: an if typed into sum(), a code line inside it](docs/palette.png)

New blocks have no text in the file, so saving prints them; everything
around them is still copied as written:

```c
    if (count > 0 && count < 1000) {
        // a comment that moved in with its statement
        printf("hello\n");
        n = 100;
    }
```

## Expressions

Like Scratch reporters: grab `count` out of `i < count` and it comes out
alone, leaving an empty white slot behind. Drop it into any empty slot
(it lights up yellow), or leave it lying on the canvas for later. Grab the
green `i < count` itself and the whole condition comes, into an `if` from
the palette, say.

![total on its way into the slot count left in i < _](docs/reporter.png)

- An empty slot can also be typed into: click it.
- A program with an empty slot is not saved: the status bar says which
  line still has a hole.
- **Delete** and **Duplicate** (pie menu, Del, Ctrl+D) on an expression
  act on that expression: delete leaves its slot empty, the copy follows
  the mouse until a click puts it in a slot.
- Saving rewrites only the slots that changed; the text around them, your
  spacing and comments included, is copied as it was.
- Code a macro made (the `n` inside `TWICE(n)`) does not come apart: the
  whole `TWICE(n)` comes out instead.

How sure is this? The test suite takes every expression of raylib's
sources that may move (about 64 000), and both moves each one into its
own slot and deletes and retypes it: the file has to come back byte for
byte.

## goto arrows

Every `goto` has an arrow to its label, out of its right side and along a
lane to the right of the function. Shorter jumps take the inner lanes, so
the arrows nest like brackets instead of crossing. Put the mouse on a
`goto` or a label and its arrows light up.

![left: goto close_file lit up; right: out: dragged away, goto out turns red](docs/goto.png)

Labels belong to their function. Drag a label out onto the canvas (or into
another function) and the arrow turns red and dashed, and the `goto` says
why; delete it and the `goto` gets a red mark. That is the "use of
undeclared label" clang would tell you about after saving, shown while
you are still holding the block. `examples/goto.c` has the classic kernel
style cleanup ladder to play with.

## Themes

The **Theme** button in the top bar goes round them, and remembers the one
you stop at:

![Shrine, Super Serious, Masochist and Scratch](docs/themes.png)

| Theme | What |
| --- | --- |
| **Shrine** | dark, shrine red; built in |
| **Super Serious** | soft pink pastels that do not burn your eyes, deeply serious about it |
| **Masochist** | white. All of it. You asked for this |
| **Scratch** | the editor all of this started from, blue bar and all; pointers are Pen green, dragons are Sound purple |

A theme is a text file like a language pack: `themes/NAME.theme`, found
in `$CCRATCH_THEME_DIR`, `~/.local/share/ccratch/themes/` and
`share/ccratch/themes/` next to the program. It sets only the colours it
changes, everything else stays Shrine:

```
@name    = Super Serious
@name.ru = Суровая
bg       = #F3E1E8
control  = #FFD9A8
```

Every key is in `ccratch/src/ccr_theme.h` (or copy one of `themes/`);
`ccratch --check-theme mine.theme` says what is wrong, `--theme NAME`
starts with one, `--list-themes` shows what was found.

## The pie menu

Everything the keyboard does, the mouse does too. Right click (without
dragging: a drag still pans) and a pie opens around the cursor. Each action
always sits in the same direction, so it turns into a flick and a click:

![the pie over a commented-out block](docs/pie.png)

| Direction | Action | Key |
| --- | --- | --- |
| up | Save | Ctrl+S |
| up-right | Comment out / Uncomment | Ctrl+/ |
| right | Redo | Ctrl+Y |
| down-right | Edit text | click a white field |
| down | Delete | Del |
| down-left | Fit view | Home |
| left | Undo | Ctrl+Z |
| up-left | Duplicate | Ctrl+D |

Only the direction counts, not hitting the button: anywhere past the small
ring in the middle picks the slice the mouse points into. Esc, right click
or a click in the ring closes it. Greyed out slices make no sense for what
is under the cursor (nothing to undo, no block there).

The block actions work on the block under the mouse, keys included:

- **Delete** takes out one statement (an `if` goes with everything inside
  it); the rest of the stack closes up. On a loose stack's top block, the
  whole stack goes.
- **Duplicate** makes a copy that follows the mouse, no button held, until
  you click: into a stack, anywhere on the canvas, or onto the palette to
  change your mind. One undo step, copy and drop together.
- **Comment out** turns a statement grey and keeps it in the file as `//`
  lines, its own comments and any blocks inside included. Uncomment
  brings it back exactly as it was:

```c
    // while (b < 10) {
    //     b++;
    //     // nothing else here
    // }
```

## Languages

The interface comes in English (built in) and Russian, and anyone can add
a language without touching the code: a pack is one text file.

```
@name   = Русский
@plural = slavic

pie.save = Сохранить
status.loaded_errors = %s: %u ошибка ...|%s: %u ошибки ...|%s: %u ошибок ...
```

- The **EN / RU** button in the top bar switches language and remembers it
  (in `~/.config/ccratch/config`). Otherwise Ccratch starts in the system
  language (`$LANG`), or the one given with `--lang ru` / `$CCRATCH_LANG`.
- Packs are looked for in `$CCRATCH_LANG_DIR`, `~/.local/share/ccratch/lang/`
  and `share/ccratch/lang/` next to the program. The ones in `lang/` here
  are copied there on every build.
- To translate: `ccratch --lang-template > uk.lang`, translate the right
  side, then `ccratch --check-lang uk.lang` lists what is missing or broken.
- Plurals have a form per case, split by `|`, as many as the language needs
  (`slavic`: 1 ошибка, 2 ошибки, 5 ошибок).
- Anything a pack leaves out or gets wrong falls back to English. A pack
  cannot crash Ccratch: a translation must use exactly the `%s`/`%u` of the
  English, or it is refused.

C on the blocks (`if`, `while`, `struct`) stays C, and the command line
stays English.

Text is drawn with Nunito (SIL Open Font License, `assets/fonts/`), built
into the program with Latin, Cyrillic and Latin Extended-A, so packs for
most European languages need no extra font.

## Saving

Saving splices instead of reprinting: everything you did not touch is
copied from the file byte for byte, so comments, `#ifdef`s, macros and your
formatting stay as they were. Only blocks whose statements moved are put
back together, out of each statement's original text:

- comments right above a statement, and at the end of its line, move with it;
- an empty line above a statement moves with it too;
- a statement keeps any extra indent it had (like inside `rlBegin`/`rlEnd`)
  and is re-indented to its new depth;
- dropping into a braceless `if (x) y;` turns it into `if (x) { ... }`;
- loose blocks on the canvas are left out of the file.

A block with something other than comments between its statements (an
`#if`, a `#define`, code made by a macro) is refused rather than guessed
at: the status bar says which line, and nothing is written.

The first save of a session copies the file as it was opened to `FILE~`,
and the file is written to a temporary name and renamed, so a crash never
leaves half a file. After saving, Ccratch reads the file back with clang and
tells you if your move made code clang rejects (say, a `return` moved out
of its function's type).

How sure is this? `ccratch --roundtrip FILE.c` checks an untouched file
comes back byte for byte, and the test suite forces every block of
raylib's sources (about 2000 of them) to be rebuilt and checks clang reads
the result back into the same tree.

![dragging: the for loop is about to snap in under the new if](docs/drag.png)

Object-like macros used as values (`NULL`, `true`, `DEG2RAD`) stay one
purple block. Function-like macros (`MAX(a, b)`) are shown expanded,
since their arguments are real code.

`--dump-ast` prints the tree the blocks are built from:

```
$ ccratch --dump-ast examples/tour.c
...
      ├─ Call free  [63:5]
      │  └─ Ref p  [63:10]
      ├─ Goto done  [64:5]
      └─ Label done  [65:1]
```

## Layout

```
app/        ccratch      - the launcher: main() calls ccr_run(), that's it
ccratch/    libccratch   - the whole IDE in one library (modular monolith)
  ccr_ast     own syntax tree: flat node array + string arena, no libclang
  ccr_parse   the only code that knows libclang exists
  ccr_blocks  one script -> blocks: flat display list + snap points
  ccr_editor  scripts on the canvas, drag and drop, edits the tree
  ccr_codegen tree -> C: copies untouched text, rebuilds changed blocks
  ccr_pie     the pie menu: picks an action by direction, knows no editor
  ccr_lang    every text the window shows, and the language packs
  ccr_theme   every colour, and the theme files
  ccr_config  ~/.config/ccratch/config, and where add-ons are found
gui/        libccr_gui   - the only code that knows raylib exists
include/    public headers: ccratch.h, ccr_plugin.h (plugin ABI)
plugins/    each plugin is its own .so; hello/ is the template
examples/   C files to feed the parser
lang/       language packs (ru.lang...); English is built in
themes/     themes (serious.theme...); Shrine is built in
assets/     icon and font, built into libccratch
tools/      make_font.py: how assets/fonts was made from upstream Nunito
docs/       screenshots
tests/      headless CTest suite
cmake/      dependencies, fetched from git by tag; FindLibClang
```

## Build

Needs GCC (or Clang), CMake 3.21+, libclang, and the X11/OpenGL dev headers
raylib uses. On Fedora:

```sh
sudo dnf install gcc cmake clang-devel libX11-devel libXrandr-devel \
                 libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel
```

If libclang lives somewhere unusual, pass `-DLIBCLANG_ROOT=/path/to/llvm`.

Then:

```sh
cmake --preset debug      # or: release, paranoid
cmake --build --preset debug -j
./build/debug/bin/ccratch
```

Presets use Unix Makefiles. Ninja is not invited.

`paranoid` turns on `-Werror`, AddressSanitizer and UBSan for our code.

Tests run without a window:

```sh
ctest --test-dir build/debug --output-on-failure
```

### If CMake complains about a different directory

`build/` remembers absolute paths. After moving or renaming the project
folder, delete it and configure again:

```sh
rm -rf build && cmake --preset debug
```

### If parsing complains about `stddef.h` or `size_t`

libclang could not find clang's own builtin headers. Ccratch looks for
them at run time next to the libclang it loaded (this covers Fedora, where
libclang is in `/usr/lib64` and the headers in `/usr/lib/clang/N`), then
falls back to the path found at build time. See what it picked:

```sh
ccratch --about
```

Override it with `CCRATCH_CLANG_RESOURCE_DIR=/usr/lib/clang/22`, or per
file with `ccratch file.c -- -resource-dir ...`.

## Command line

```
ccratch                                  open the editor
ccratch file.c                           open a file as blocks
ccratch file.c -- -Iinclude -DDEBUG      extra arguments go to clang
ccratch --dump-ast file.c                print the syntax tree, no window
ccratch --roundtrip file.c               check saving an untouched file is lossless
ccratch file.c --zoom 50                 start zoomed out
ccratch --lang ru                        interface in Russian
ccratch --list-langs                     language packs found, and where
ccratch --lang-template                  English text as a pack to translate
ccratch --check-lang uk.lang             check a pack before sharing it
```

`--dump-ast` exits with 0 if the file is clean, 1 if it has errors,
2 if clang could not read it at all.

## Controls

- Drop a `.c` file onto the window: open it
- Left mouse drag: move blocks
- Right click: the pie menu (above)
- Del, Ctrl+D, Ctrl+/: delete, duplicate, comment out the block under the mouse
- Ctrl+S: save (Esc no longer quits, so it cannot eat unsaved work)
- Ctrl+Z: undo one drag; Ctrl+Y or Ctrl+Shift+Z: redo. Undoing back to
  the saved state clears the `*`, and an undone change is gone from the
  file too: undo everything and the file saves byte for byte as it was
- Right or middle mouse drag: pan the canvas
- Mouse wheel: zoom around the cursor
- Home: back to the start of the file at 100%
- F5: reload the file from disk (throws away unsaved moves; the title shows
  `*` while there are any)

## License

Ccratch is licensed under the **IHATERUSTL** (Insane Harmonic Amazing
Trust Elementary Random United Standardized Temple License), which is
legally the [Mozilla Public License 2.0](LICENSE) wearing a shrine maiden
costume. Fork it and change it: keep the authors, keep your changes to
these files open. Plugins and programs of your own around it can be under
any license. The spirit of the thing, and the third-party licenses, are in
[IHATERUSTL.md](IHATERUSTL.md).

On first run Ccratch also asks you to accept an End User License
Agreement. It is a joke from start to finish and has no legal force;
`ccratch --eula` prints it.
