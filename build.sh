#!/usr/bin/env bash
# Builds Ccratch, for people who would rather not type CMake commands.
#
#   ./build.sh              the normal build (release)
#   ./build.sh debug        or: paranoid (sanitizers, -Werror)
#   ./build.sh clean        forget every build, start again next time
#   ./build.sh deps         install what building needs (asks first)
#
# Missing tools are found before anything starts, and it offers to install
# them (dnf, apt, pacman or zypper). Moved the folder? The old build is
# thrown away by itself. Double-clicked in a file manager? It opens a
# terminal to show what it does.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$here" || exit 1

preset=release
case "${1:-}" in
    ""|release) ;;
    debug|paranoid) preset=$1 ;;
    clean) rm -rf build && echo "Builds forgotten: the next ./build.sh starts from scratch."; exit 0 ;;
    deps) want_deps=1 ;;
    -h|--help|help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown: $1 (release, debug, paranoid, clean, deps)"; exit 2 ;;
esac

# Double-clicked, no terminal to tell anything in: open one, run there.
if [ ! -t 1 ] && [ -z "${CCR_IN_TERMINAL:-}" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    export CCR_IN_TERMINAL=1
    for t in x-terminal-emulator gnome-terminal konsole kgx ptyxis xfce4-terminal mate-terminal tilix alacritty kitty foot xterm; do
        command -v "$t" >/dev/null 2>&1 || continue
        case "$t" in
            gnome-terminal|kgx|ptyxis) exec "$t" -- bash "$0" "$@" ;;
            *) exec "$t" -e bash "$0" "$@" ;;
        esac
    done
fi

# In a terminal opened for us: wait before it closes, so it can be read
# (not after a good build run.sh asked for: the program starts instead).
pause() {
    if [ -n "${CCR_IN_TERMINAL:-}" ] && { [ -z "${CCR_FROM_RUN:-}" ] || [ "${1:-}" = always ]; }; then
        printf '\nPress Enter to close.'
        read -r _
    fi
}

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m!!!\033[0m %s\n' "$*"; pause always; exit 1; }

# ------------------------------------------------------- what it needs

pm=""
for p in dnf apt-get pacman zypper; do
    command -v "$p" >/dev/null 2>&1 && { pm=$p; break; }
done

packages() {
    case "$pm" in
        dnf)     echo "gcc make cmake git clang-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel gdb" ;;
        apt-get) echo "build-essential cmake git libclang-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev gdb" ;;
        pacman)  echo "base-devel cmake git clang libx11 libxrandr libxinerama libxcursor libxi mesa gdb" ;;
        zypper)  echo "gcc make cmake git clang-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel Mesa-libGL-devel gdb" ;;
    esac
}

install_deps() {
    if [ -z "$pm" ]; then
        echo "I don't know this system's package manager. Ccratch needs: a C compiler, make,"
        echo "CMake 3.21+, git, libclang (with headers) and the X11/OpenGL headers."
        return 1
    fi
    local list
    list=$(packages)
    echo "Building needs these packages:"
    echo "    $list"
    if [ ! -t 0 ]; then   # nobody to ask
        echo "Install them with $pm (or: ./build.sh deps in a terminal)."
        return 1
    fi
    printf 'Install them now with %s (asks for your password)? [Y/n] ' "$pm"
    read -r answer
    case "$answer" in [nN]*) return 1 ;; esac
    case "$pm" in
        dnf)     sudo dnf install -y $list ;;
        apt-get) sudo apt-get update && sudo apt-get install -y $list ;;
        pacman)  sudo pacman -S --needed --noconfirm $list ;;
        zypper)  sudo zypper install -y $list ;;
    esac
}

if [ -n "${want_deps:-}" ]; then
    install_deps && say "Done." || fail "Not installed."
    pause
    exit 0
fi

missing=""
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
command -v make  >/dev/null 2>&1 || missing="$missing make"
command -v git   >/dev/null 2>&1 || missing="$missing git"
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 \
    || missing="$missing C-compiler"
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    install_deps || fail "Can't build without them. Try again after installing: ./build.sh deps"
fi

# CMake 3.21 or newer (presets)
v=$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
if [ "$(printf '%s\n3.21\n' "$v" | sort -V | head -1)" != "3.21" ]; then
    fail "CMake $v is too old: 3.21 or newer is needed."
fi

# ------------------------------------------------------------ building

dir="build/$preset"
# moved or renamed the folder: the old build points at the old place
if [ -f "$dir/CMakeCache.txt" ] && ! grep -qF "CMAKE_HOME_DIRECTORY:INTERNAL=$here" "$dir/CMakeCache.txt"; then
    say "The project folder moved since the last build: starting that build again."
    rm -rf "$dir"
fi

jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

if [ ! -f "$dir/CMakeCache.txt" ]; then
    say "Setting up the $preset build (the first time takes a minute: raylib is downloaded)"
    if ! cmake --preset "$preset"; then
        rm -rf "$dir"
        echo
        echo "Setting up failed. Most often a library is missing (libclang, X11 headers)."
        install_deps || fail "Not built."
        cmake --preset "$preset" || fail "Setting up still fails: the messages above say why."
    fi
fi

say "Building ($jobs at once)"
cmake --build --preset "$preset" -j "$jobs" || fail "The build failed: the messages above say why."

say "Done: $here/$dir/bin/ccratch"
[ -z "${CCR_FROM_RUN:-}" ] && echo "    Start it with ./run.sh (or ./run.sh some_file.c)"
pause
