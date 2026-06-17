// One process-wide Direct3D 11 device shared by capture and every overlay.
//
// The immediate context is made thread-safe via ID3D11Multithread
// (SetMultithreadProtected), so WGC frame-arrival (a pool thread) and the
// overlay render threads can both issue D3D11 calls without an external lock.
// The DXGI factory hosts the per-overlay composition swap chains. The device is
// created BGRA-capable for DirectComposition / WGC interop.
#pragma once

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <mutex>

namespace sf {

class DXShared {
public:
    static DXShared& Get();

    // Idempotent; safe to call from any module's first use.
    bool Init();

    Microsoft::WRL::ComPtr<ID3D11Device>        device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;   // multithread-protected
    Microsoft::WRL::ComPtr<IDXGIFactory2>       factory;

    // The underlying DXGI device (used to build the WinRT IDirect3DDevice for WGC).
    Microsoft::WRL::ComPtr<IDXGIDevice>         dxgiDevice;

    // ID3D11Multithread protection makes each individual context call atomic, but a
    // logical render SEQUENCE (set RTV/state/draw) is NOT atomic, and several overlay
    // render threads + the capture thread all share this one immediate context. Hold
    // this lock around any multi-call GPU sequence (each overlay's draw, each
    // CopyResource). Recursive so a draw can call into CopyLatestInto. Acquire it
    // BEFORE any per-object lock to keep a single, deadlock-free ordering.
    std::recursive_mutex renderLock;

private:
    DXShared() = default;
    bool initialized_ = false;
};

} // namespace sf
