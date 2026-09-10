// Global capture state shared by every API hook: configuration, output paths,
// the capture hotkey and per-frame bookkeeping.
//
// Frame model: the swapchain Present (or EndScene for D3D9, vkQueuePresentKHR
// for Vulkan) detour calls on_frame_boundary() once per presented frame. When
// the hotkey is pressed, capturing begins on the *next* frame's draws and
// ends at the following boundary, capturing exactly one frame.
#pragma once

#include "ghost/capture_format.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace ghost {

struct CaptureConfig {
    int         hotkey_vk = 0x78;        // VK_F9
    bool        capture_textures = true;
    std::string output_root;             // <Documents>\GhostRipper
    std::string source_app;              // exe file name of the target process
    std::string api_name = "unknown";    // set by whichever hook installed
};

class CaptureContext {
public:
    static CaptureContext& instance();

    // Resolve config (ghost_hook.ini next to the DLL) and the session dir.
    // `module` is the hook DLL's HMODULE. Returns false on fatal errors.
    bool initialize(void* module);

    const CaptureConfig& config() const { return config_; }

    // Present/EndScene detour entry point. Polls the hotkey, opens/closes the
    // frame capture and bumps the frame counter.
    void on_frame_boundary();

    bool capturing() const { return capturing_.load(); }

    // Current frame directory, e.g. <root>\<exe>_<pid>\frame_000123.
    // Only valid while capturing().
    const std::string& frame_dir() const { return frame_dir_; }

    // Mesh index within the current frame (Mesh_0000, Mesh_0001, ...).
    uint32_t next_mesh_index() { return mesh_index_++; }

    void set_api_name(const char* api) { config_.api_name = api; }

private:
    void begin_frame();
    void end_frame();

    CaptureConfig config_;
    std::string   session_dir_;   // <root>\<exe>_<pid>
    std::string   frame_dir_;
    std::string   ini_path_;

    std::atomic<bool> capturing_{false};
    uint64_t frame_counter_ = 0;  // every presented frame
    uint32_t mesh_index_    = 0;  // meshes in the current captured frame
    bool     hotkey_down_   = false;

    std::mutex write_mutex_;      // serializes manifest writes
};

} // namespace ghost
