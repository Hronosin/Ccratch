# Ccratch

Scratch, but for raw C. No ready-made loops, no hand-holding, no Cargo.
You drag `while`, `switch` and `break` around yourself, and the cat can segfault.

> Memory safety as a lifestyle choice.

## Status

Step 1 of "a little at a time": a window, a pannable canvas and one lonely block.

## Layout

```
app/        ccratch      - the launcher: main() calls ccr_run(), that's it
ccratch/    libccratch   - the whole IDE in one library (modular monolith)
gui/        libccr_gui   - the only code that knows raylib exists
include/    public headers: ccratch.h, ccr_plugin.h (plugin ABI)
plugins/    each plugin is its own .so; hello/ is the template
cmake/      dependencies, fetched from git by tag
```

## Build

Needs GCC (or Clang), CMake 3.21+, and the X11/OpenGL dev headers raylib uses.
On Fedora:

```sh
sudo dnf install gcc cmake libX11-devel libXrandr-devel libXinerama-devel \
                 libXcursor-devel libXi-devel mesa-libGL-devel
```

Then:

```sh
cmake --preset debug      # or: release, paranoid
cmake --build --preset debug -j
./build/debug/bin/ccratch
```

Presets use Unix Makefiles. Ninja is not invited.

`paranoid` turns on `-Werror`, AddressSanitizer and UBSan for our code.

## Controls

- Right or middle mouse drag: pan the canvas
- Mouse wheel: zoom around the cursor
