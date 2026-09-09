# Third-party dependencies fetched with FetchContent.
#
#   GLFW    - windowing for the viewer
#   ImGui   - viewer UI (fetched as sources; built in src/viewer)
#   MinHook - API hooking for the hook DLL (Windows only)
#
# Set FETCHCONTENT_FULLY_DISCONNECTED / pre-populated <name>_SOURCE_DIR vars
# to build offline.

if(GHOST_BUILD_VIEWER)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
        # Wayland tooling is often missing on minimal CI images; X11 is enough.
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(glfw)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
    # ImGui ships no build system; src/viewer/CMakeLists.txt compiles the
    # fetched sources (core + glfw/opengl3 backends) into the viewer.
endif()

if(WIN32 AND GHOST_BUILD_HOOK)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        v1.3.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(minhook)
endif()
