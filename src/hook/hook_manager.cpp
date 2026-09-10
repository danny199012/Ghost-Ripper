#include "hook_manager.h"

#include "ghost/log.h"

#include <MinHook.h>

namespace ghost {

HookManager& HookManager::instance() {
    static HookManager mgr;
    return mgr;
}

bool HookManager::initialize() {
    if (initialized_)
        return true;
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        log::error("MH_Initialize failed: %d", static_cast<int>(status));
        return false;
    }
    initialized_ = true;
    return true;
}

bool HookManager::add(void* target, void* detour, void** original, const char* name) {
    if (!initialize())
        return false;

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        log::error("MH_CreateHook(%s) failed: %s", name, MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        log::error("MH_EnableHook(%s) failed: %s", name, MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }
    targets_.push_back(target);
    log::info("hooked %s -> %p", name, target);
    return true;
}

void HookManager::shutdown() {
    if (!initialized_)
        return;
    for (void* target : targets_) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    targets_.clear();
    MH_Uninitialize();
    initialized_ = false;
}

} // namespace ghost
