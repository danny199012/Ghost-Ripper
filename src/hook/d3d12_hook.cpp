#include "d3d12_hook.h"

#include "capture_context.h"
#include "hook_manager.h"

#include "ghost/log.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace ghost {
namespace d3d12 {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kIdxPresent = 8;  // IDXGISwapChain::Present

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_orig_present = nullptr;
bool      g_installed = false;

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swapchain, UINT sync, UINT flags) {
    CaptureContext::instance().on_frame_boundary();
    return g_orig_present(swapchain, sync, flags);
}

bool create_dummy_swapchain(ComPtr<IDXGISwapChain>& swapchain) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance   = GetModuleHandleA(nullptr);
    wc.lpszClassName = "GhostRipperDummy12";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "ghost", WS_OVERLAPPED, 0, 0, 2, 2,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
        return false;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        DestroyWindow(hwnd);
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    factory->EnumAdapters(0, &adapter);

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device)))) {
        DestroyWindow(hwnd);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                       = 1;
    sd.BufferDesc.Width                  = 2;
    sd.BufferDesc.Height                 = 2;
    sd.BufferDesc.Format                 = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage                       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                      = hwnd;
    sd.SampleDesc.Count                  = 1;
    sd.Windowed                          = TRUE;
    sd.SwapEffect                        = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    factory->CreateSwapChain(queue.Get(), &sd, &swapchain);
    DestroyWindow(hwnd);
    return swapchain != nullptr;
}

template <typename T>
void* vtable_entry(T* com_object, int index) {
    return (*reinterpret_cast<void***>(com_object))[index];
}

} // namespace

bool available() {
    return GetModuleHandleA("d3d12.dll") != nullptr;
}

bool install() {
    if (g_installed)
        return true;
    if (!available())
        return false;

    ComPtr<IDXGISwapChain> swapchain;
    if (!create_dummy_swapchain(swapchain)) {
        log::error("d3d12: failed to create dummy swapchain");
        return false;
    }

    bool ok = HookManager::instance().add(
        vtable_entry(swapchain.Get(), kIdxPresent),
        reinterpret_cast<void*>(&hk_present),
        reinterpret_cast<void**>(&g_orig_present),
        "IDXGISwapChain::Present (d3d12)");

    g_installed = ok;
    if (ok) {
        CaptureContext::instance().set_api_name("d3d12");
        log::info("d3d12 frame-boundary hook installed (capture TODO)");
    }
    return ok;
}

} // namespace d3d12
} // namespace ghost
