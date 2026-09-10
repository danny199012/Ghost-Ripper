// Thin RAII wrapper over MinHook. All API hook installers (D3D9/11/12,
// Vulkan) register their detours through here so shutdown is centralized.
#pragma once

#include <vector>

namespace ghost {

class HookManager {
public:
    static HookManager& instance();

    // Calls MH_Initialize (idempotent). Must succeed before add().
    bool initialize();

    // Creates and enables a hook on `target`. `original` receives the
    // trampoline. `name` is used for logging only.
    bool add(void* target, void* detour, void** original, const char* name);

    // Disables and removes all hooks, then uninitializes MinHook.
    void shutdown();

private:
    std::vector<void*> targets_;
    bool initialized_ = false;
};

} // namespace ghost
