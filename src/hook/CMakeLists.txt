# ghost_hook.dll: injected into the game process to hook the graphics API and
# capture geometry + textures into .grm files. Windows-only.
add_library(ghost_hook SHARED
    capture_context.cpp
    dll_main.cpp
    d3d11_hook.cpp
    d3d12_hook.cpp
    d3d9_hook.cpp
    hook_manager.cpp
    texture_dump.cpp
    vulkan_hook.cpp
)

target_include_directories(ghost_hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ghost_hook PRIVATE
    ghost-common
    minhook
    d3d11
    d3d9
    dxgi
    dxguid
)

# Vulkan loader import library isn't always present; link against the module
# at runtime via GetProcAddress instead (see vulkan_hook.cpp).
target_link_libraries(ghost_hook PRIVATE d3d12)

set_target_properties(ghost_hook PROPERTIES
    OUTPUT_NAME "ghost_hook"
    PREFIX ""
)
