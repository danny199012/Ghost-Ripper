# ghost_viewer_core: viewer logic that needs no GL/ImGui (capture loading,
# texture decoding, exporters). Shared by the GUI app and the unit tests, so
# it is built even when the GUI is disabled.
add_library(ghost_viewer_core STATIC
    src/bc_decode.cpp
    src/gltf_exporter.cpp
    src/mesh_asset.cpp
    src/obj_exporter.cpp
)
target_include_directories(ghost_viewer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ghost_viewer_core PUBLIC ghost-common)

if(GHOST_BUILD_VIEWER)
    # Dear ImGui: fetched as sources (no build system of its own).
    add_library(ghost_imgui STATIC EXCLUDE_FROM_ALL)
    target_include_directories(ghost_imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_sources(ghost_imgui PRIVATE
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_compile_definitions(ghost_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    # imgui_impl_glfw.cpp includes <GLFW/glfw3.h>; those include dirs come
    # from the glfw target, so ghost_imgui has to link it as well.
    target_link_libraries(ghost_imgui PRIVATE glfw)

    # ghost-viewer: Dear ImGui + OpenGL3 + GLFW desktop app.
    add_executable(ghost-viewer
        src/app.cpp
        src/gl_extra.cpp
        src/mesh_renderer.cpp
        src/texture_loader.cpp
    )
    target_include_directories(ghost-viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(ghost-viewer PRIVATE
        ghost_viewer_core
        ghost_imgui
        glfw
    )

    # OpenGL: prefer the modern loader on Linux, link opengl32 on Windows.
    if(WIN32)
        target_link_libraries(ghost-viewer PRIVATE opengl32)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(ghost-viewer PRIVATE OpenGL::GL)
    endif()

    if(UNIX AND NOT APPLE)
        # GLFW needs these system libs on Linux.
        target_link_libraries(ghost-viewer PRIVATE X11 X11-xcb xcb dl pthread)
    endif()
endif()

