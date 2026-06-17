// Link: d3d11.lib dxgi.lib
#include "DXShared.h"
#include "Log.h"

#include <d3d11_4.h>   // ID3D11Multithread

using Microsoft::WRL::ComPtr;

namespace sf {

DXShared& DXShared::Get() {
    static DXShared s;
    return s;
}

bool DXShared::Init() {
    if (initialized_) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // required for DirectComposition / WGC interop
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   device.ReleaseAndGetAddressOf(), &got,
                                   context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        Log::Linef(L"DXShared: hardware device failed hr=0x%08X, trying WARP", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               device.ReleaseAndGetAddressOf(), &got,
                               context.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        Log::Linef(L"DXShared: D3D11CreateDevice failed hr=0x%08X", hr);
        return false;
    }

    // The immediate context is shared by capture (WGC pool thread) and the overlay
    // render threads; make it internally synchronized so no external lock is needed.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(hr = device.As(&dxgiDevice))) {
        Log::Linef(L"DXShared: QI IDXGIDevice failed hr=0x%08X", hr);
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(hr = dxgiDevice->GetAdapter(&adapter))) {
        Log::Linef(L"DXShared: GetAdapter failed hr=0x%08X", hr);
        return false;
    }
    if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) {
        Log::Linef(L"DXShared: get IDXGIFactory2 failed hr=0x%08X", hr);
        return false;
    }

    initialized_ = true;
    Log::Linef(L"DXShared: device ready (feature level 0x%04X)", (unsigned)got);
    return true;
}

} // namespace sf
