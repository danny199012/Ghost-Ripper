#include "capture_context.h"

#include "ghost/log.h"

#include <cstdio>

#include <windows.h>

#include <shlobj.h>  // SHGetFolderPathW, CSIDL_PERSONAL

namespace ghost {

namespace {

std::string narrow(const std::wstring& wide) {
    if (wide.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

bool dir_exists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void make_dirs(std::string path) {
    // Create each path component in turn (flat, no recursion needed beyond).
    for (size_t i = 3; i < path.size(); ++i) {  // skip "C:\"
        if (path[i] == '/' || path[i] == '\\') {
            path[i] = '\0';
            if (!dir_exists(path.c_str()))
                CreateDirectoryA(path.c_str(), nullptr);
            path[i] = '\\';
        }
    }
    if (!dir_exists(path))
        CreateDirectoryA(path.c_str(), nullptr);
}

} // namespace

CaptureContext& CaptureContext::instance() {
    static CaptureContext ctx;
    return ctx;
}

bool CaptureContext::initialize(void* module) {
    // --- config file -------------------------------------------------------
    wchar_t dll_path[MAX_PATH]{};
    GetModuleFileNameW(static_cast<HMODULE>(module), dll_path, MAX_PATH);
    std::wstring wdir(dll_path);
    const size_t slash = wdir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        wdir = wdir.substr(0, slash);
    ini_path_ = narrow(wdir + L"\\ghost_hook.ini");

    // --- process name / pid ------------------------------------------------
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring wexe(exe_path);
    const size_t exe_slash = wexe.find_last_of(L"\\/");
    config_.source_app = narrow(exe_slash != std::wstring::npos ? wexe.substr(exe_slash + 1)
                                                                : wexe);

    // --- output root --------------------------------------------------------
    wchar_t docs[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
        std::wcscpy(docs, L"C:");
    config_.output_root = narrow(std::wstring(docs) + L"\\GhostRipper");

    // INI overrides.
    char buffer[512];
    if (GetPrivateProfileStringA("capture", "output_dir", "", buffer, sizeof(buffer),
                                 ini_path_.c_str()) > 0)
        config_.output_root = buffer;
    config_.hotkey_vk = GetPrivateProfileIntA("capture", "hotkey", config_.hotkey_vk,
                                              ini_path_.c_str());
    config_.capture_textures =
        GetPrivateProfileIntA("capture", "textures", 1, ini_path_.c_str()) != 0;

    session_dir_ = config_.output_root + "\\" + config_.source_app + "_" +
                   std::to_string(GetCurrentProcessId());
    make_dirs(session_dir_);

    log::set_file(session_dir_ + "\\ghost_hook.log");
    log::info("Ghost-Ripper hook loaded: pid=%lu exe=%s", GetCurrentProcessId(),
              config_.source_app.c_str());
    log::info("captures go to: %s (hotkey VK=0x%02x, textures=%d)",
              session_dir_.c_str(), config_.hotkey_vk,
              static_cast<int>(config_.capture_textures));
    return true;
}

void CaptureContext::on_frame_boundary() {
    ++frame_counter_;

    const bool down = (GetAsyncKeyState(config_.hotkey_vk) & 0x8000) != 0;
    const bool pressed = down && !hotkey_down_;
    hotkey_down_ = down;

    if (capturing_.load()) {
        end_frame();
        capturing_.store(false);
    }
    if (pressed)
        begin_frame();
}

void CaptureContext::begin_frame() {
    char frame_name[64];
    std::snprintf(frame_name, sizeof(frame_name), "frame_%06llu",
                  static_cast<unsigned long long>(frame_counter_));
    frame_dir_ = session_dir_ + "\\" + frame_name;
    make_dirs(frame_dir_ + "\\meshes");
    make_dirs(frame_dir_ + "\\textures");
    mesh_index_ = 0;
    capturing_.store(true);
    log::info("capturing frame %llu -> %s",
              static_cast<unsigned long long>(frame_counter_), frame_dir_.c_str());
}

void CaptureContext::end_frame() {
    std::lock_guard<std::mutex> lock(write_mutex_);

    const std::string manifest = frame_dir_ + "\\manifest.json";
    if (FILE* f = std::fopen(manifest.c_str(), "w")) {
        std::fprintf(f,
                     "{\n"
                     "  \"tool\": \"Ghost-Ripper\",\n"
                     "  \"format\": \"GRMF\",\n"
                     "  \"format_version\": %u,\n"
                     "  \"api\": \"%s\",\n"
                     "  \"app\": \"%s\",\n"
                     "  \"pid\": %lu,\n"
                     "  \"frame\": %llu,\n"
                     "  \"mesh_count\": %u\n"
                     "}\n",
                     fmt::kVersion, config_.api_name.c_str(), config_.source_app.c_str(),
                     GetCurrentProcessId(),
                     static_cast<unsigned long long>(frame_counter_), mesh_index_);
        std::fclose(f);
    }
    log::info("frame capture complete: %u meshes", mesh_index_);
}

} // namespace ghost
