#!/usr/bin/env bash
# Starts Ccratch, building it first if it is not built yet (or the code
# changed since).
#
#   ./run.sh                    the start screen
#   ./run.sh game.c             with a file open
#   ./run.sh --debug game.c     the debug build (or --paranoid)
#
# Anything else goes to Ccratch as it is (./run.sh --help-ccratch lists
# what it understands). Double-clicked in a file manager: a terminal shows
# the build, if there is one to do, and the window opens after.
set -u

from=$PWD   # file names given are relative to here
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$here" || exit 1

preset=release
case "${1:-}" in
    --debug)    preset=debug;    shift ;;
    --paranoid) preset=paranoid; shift ;;
    --release)  shift ;;
    -h|--help)  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --help-ccratch) shift; set -- --help "$@" ;;
esac
bin="$here/build/$preset/bin/ccratch"
stamp="$here/build/$preset/.built-by-run.sh"   # when run.sh last saw a good build

# Paths from Windows (run.cmd under WSL): C:\Users\... -> /mnt/c/Users/...
args=()
for a in "$@"; do
    if [[ "$a" =~ ^[A-Za-z]:[\\/] ]] && command -v wslpath >/dev/null 2>&1; then
        a=$(wslpath -u "$a")
    fi
    args+=("$a")
done

# Built, and nothing changed since?
stale=1
if [ -x "$bin" ] && [ -f "$stamp" ]; then
    newer=$(find ccratch gui app include lang assets blocks themes cmake CMakeLists.txt CMakePresets.json \
                 -newer "$stamp" -type f ! -name '*~' -print -quit 2>/dev/null)
    [ -z "$newer" ] && stale=0
fi

start() {
    cd "$from" 2>/dev/null || true
    if [ -n "${CCR_IN_TERMINAL:-}" ]; then
        # in a terminal opened for the build: the window lives on without it
        setsid -f "$bin" "${args[@]}" >/dev/null 2>&1
        exit 0
    fi
    exec "$bin" "${args[@]}"
}

if [ "$stale" = 0 ]; then
    start
fi

# A build first. No terminal (double-clicked)? Open one for it.
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

if [ -x "$bin" ] && [ -f "$stamp" ]; then
    echo "The code changed since the last build: building again first."
elif [ -x "$bin" ]; then
    echo "Checking the build is up to date first."
else
    echo "Ccratch is not built yet: building it first."
fi
CCR_FROM_RUN=1 bash "$here/build.sh" "$preset" || exit 1
touch "$stamp"
start
