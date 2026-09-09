cmake_minimum_required(VERSION 3.20)
project(GhostRipper VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Ghost-Ripper: game asset ripping tool (Ninja Ripper alternative)
#
#   ghost-inject   - CLI injector (Windows only)
#   ghost_hook     - in-process hook DLL: D3D9/11/12 + Vulkan capture (Windows)
#   ghost-viewer   - Dear ImGui + OpenGL model browser/viewer (cross-platform)
#   sample_gen     - writes sample .grm captures for testing
#   grinfo         - CLI inspector for .grm files
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GHOST_BUILD_VIEWER   "Build the ghost-viewer GUI"          ON)
option(GHOST_BUILD_TOOLS    "Build command-line tools"            ON)
option(GHOST_BUILD_HOOK     "Build the hook DLL (Windows only)"   ${WIN32})
option(GHOST_BUILD_INJECTOR "Build the injector (Windows only)"   ${WIN32})

if(MSVC)
    add_compile_options(/W3 /MP /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra)
endif()

# All executables/DLLs go to <build>/bin so the injector, hook and viewer sit
# next to each other like the release layout.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)
include(cmake/FetchDeps.cmake)

add_subdirectory(src/common)

if(GHOST_BUILD_TOOLS)
    add_subdirectory(tools/sample_gen)
    add_subdirectory(tools/grinfo)
endif()

if(GHOST_BUILD_VIEWER)
    add_subdirectory(src/viewer)
endif()

if(WIN32)
    if(GHOST_BUILD_HOOK)
        add_subdirectory(src/hook)
    endif()
    if(GHOST_BUILD_INJECTOR)
        add_subdirectory(src/injector)
    endif()
else()
    message(STATUS "Non-Windows host: hook DLL and injector are skipped (Windows-only).")
endif()
