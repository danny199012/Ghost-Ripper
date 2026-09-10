// ghost_hook.dll entry point. On load we initialize capture config, install
// every available graphics-API hook, and register a cleanup on detach.
#include "capture_context.h"
#include "d3d11_hook.h"
#include "d3d12_hook.h"
#include "d3d9_hook.h"
#include "hook_manager.h"
#include "vulkan_hook.h"

#include "ghost/log.h"

#include <windows.h>

namespace {

void install_all() {
    auto& cap = ghost::CaptureContext::instance();
    // Install order matters for shutdown symmetry: install last first.
    if (ghost::vulkan::available()) ghost::vulkan::install();
    if (ghost::d3d12::available())  ghost::d3d12::install();
    if (ghost::d3d11::available())  ghost::d3d11::install();
    if (ghost::d3d9::available())   ghost::d3d9::install();

    if (cap.config().api_name == "unknown")
        ghost::log::warn("no known graphics API detected yet; "
                         "DLL loaded before renderer init?");
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            if (!ghost::CaptureContext::instance().initialize(module)) {
                ghost::log::error("CaptureContext::initialize failed");
                return FALSE;
            }
            install_all();
            break;
        case DLL_PROCESS_DETACH:
            ghost::HookManager::instance().shutdown();
            ghost::log::info("Ghost-Ripper hook unloaded");
            break;
        default:
            break;
    }
    return TRUE;
}
