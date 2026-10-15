# Dependencies are pulled straight from git by tag.
# No registry, no lockfile ceremony: this is the Ccratch way.

include(FetchContent)

# --- raylib: window, input, drawing ---------------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)

FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(raylib)

