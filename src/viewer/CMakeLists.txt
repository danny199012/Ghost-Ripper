# ghost-viewer: Dear ImGui + OpenGL3 + GLFW desktop app for browsing and
# inspecting .grm captures, with a 3D viewport and OBJ export. Cross-platform.
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

add_executable(ghost-viewer
    src/app.cpp
    src/gl_extra.cpp
    src/mesh_asset.cpp
    src/mesh_renderer.cpp
    src/obj_exporter.cpp
    src/texture_loader.cpp
)
target_include_directories(ghost-viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ghost-viewer PRIVATE
    ghost-common
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
