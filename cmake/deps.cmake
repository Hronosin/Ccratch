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

# --- raygui: panels and buttons (single header) ---------------------------
FetchContent_Declare(raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG        4.0
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(raylib)

FetchContent_GetProperties(raygui)
if(NOT raygui_POPULATED)
    FetchContent_Populate(raygui)
endif()

add_library(raygui INTERFACE)
target_include_directories(raygui SYSTEM INTERFACE ${raygui_SOURCE_DIR}/src)
