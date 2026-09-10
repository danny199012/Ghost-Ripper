// ghost-inject: injects ghost_hook.dll into a game process.
//
//   ghost-inject --pid <pid> [--dll path\to\ghost_hook.dll]
//   ghost-inject --exe <path\to\game.exe> [--args "..."] [--dll ...]
//                [--no-suspend]
//
// Launch mode creates the process suspended, injects, then resumes so the hook
// is in place before the renderer initializes. Attach mode (--pid) injects
// into an already-running process; the hook discovers the active API itself.
//
// Uses the classic LoadLibraryW-via-CreateRemoteThread technique. Same
// bitness (x64) and equal-or-higher integrity level are required.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

namespace {

struct Options {
    DWORD        pid = 0;
    std::wstring exe;
    std::wstring args;
    std::wstring dll;        // empty -> ghost_hook.dll next to this exe
    bool         no_suspend = false;
};

void print_usage() {
    std::printf(
        "ghost-inject - Ghost-Ripper DLL injector\n"
        "\n"
        "  ghost-inject --pid <pid> [options]\n"
        "  ghost-inject --exe <game.exe> [--args \"...\"] [options]\n"
        "\n"
        "options:\n"
        "  --dll <path>     path to ghost_hook.dll (default: alongside ghost-inject)\n"
        "  --no-suspend     launch the game normally, inject immediately after\n"
        "  --help\n");
}

std::wstring dll_path_next_to_exe() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"ghost_hook.dll";
}

bool file_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Best effort: attaching to elevated processes needs SeDebugPrivilege.
void enable_debug_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(token);
}
bool inject(DWORD pid, const std::wstring& dll_path) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "[ghost-inject] OpenProcess(%lu) failed: %lu\n", pid,
                     GetLastError());
        return false;
    }

    const size_t bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!remote) {
        std::fprintf(stderr, "[ghost-inject] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr)) {
        std::fprintf(stderr, "[ghost-inject] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const auto load_library =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
    if (!thread) {
        std::fprintf(stderr, "[ghost-inject] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 15000);

    DWORD module_handle = 0;
    GetExitCodeThread(thread, &module_handle);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (module_handle == 0) {
        std::fprintf(stderr, "[ghost-inject] LoadLibraryW failed inside the target process\n");
        return false;
    }
    std::printf("[ghost-inject] ghost_hook.dll loaded in pid %lu (module 0x%lx)\n", pid,
                module_handle);
    return true;
}

DWORD launch(const Options& opt, PROCESS_INFORMATION& pi) {
    std::wstring cmd = L"\"" + opt.exe + L"\"";
    if (!opt.args.empty())
        cmd += L" " + opt.args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    const DWORD flags = opt.no_suspend ? 0 : CREATE_SUSPENDED;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr,
                        &si, &pi)) {
        std::fprintf(stderr, "[ghost-inject] CreateProcess failed: %lu\n", GetLastError());
        return 0;
    }
    std::printf("[ghost-inject] launched pid %lu%s\n", pi.dwProcessId,
                opt.no_suspend ? "" : " (suspended)");
    return pi.dwProcessId;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        std::wstring value;
        const auto next = [&]() -> const wchar_t* {
            if (i + 1 >= argc) return nullptr;
            value = argv[++i];
            return value.c_str();
        };
        if (a == L"--pid") {
            const wchar_t* v = next();
            if (!v) { print_usage(); return 1; }
            opt.pid = static_cast<DWORD>(std::wcstoul(v, nullptr, 10));
        } else if (a == L"--exe") {
            const wchar_t* v = next();
            if (!v) { print_usage(); return 1; }
            opt.exe = v;
        } else if (a == L"--args") {
            const wchar_t* v = next();
            if (!v) { print_usage(); return 1; }
            opt.args = v;
        } else if (a == L"--dll") {
            const wchar_t* v = next();
            if (!v) { print_usage(); return 1; }
            opt.dll = v;
        } else if (a == L"--no-suspend") {
            opt.no_suspend = true;
        } else if (a == L"--help" || a == L"-h") {
            print_usage();
            return 0;
        } else {
            std::fwprintf(stderr, L"unknown argument: %s\n", a.c_str());
            print_usage();
            return 1;
        }
    }

    if (opt.dll.empty())
        opt.dll = dll_path_next_to_exe();
    if (!file_exists(opt.dll)) {
        std::fwprintf(stderr, L"[ghost-inject] hook DLL not found: %s\n", opt.dll.c_str());
        return 1;
    }
    if (opt.pid == 0 && opt.exe.empty()) {
        print_usage();
        return 1;
    }

    enable_debug_privilege();

    PROCESS_INFORMATION pi{};
    DWORD pid = opt.pid;
    if (pid == 0) {
        pid = launch(opt, pi);
        if (pid == 0)
            return 1;
    }

    const bool ok = inject(pid, opt.dll);

    if (pi.hThread) {
        if (!opt.no_suspend)
            ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok ? 0 : 1;
}

