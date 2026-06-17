// ScreenFlip indirect-display (IddCx) driver — header.
//
// This is a faithful, minimal IddCx user-mode (UMDF) driver adapted from the
// structure of Microsoft's public "IndirectDisplay / IddSampleDriver" sample
// (Windows-driver-samples, MIT-licensed). It exposes ONE headless virtual monitor
// (1920x1080@60, EDID name "SFlip Virtual") that the ScreenFlip app adopts as its
// workspace V. It is the Windows analogue of the macOS private CGVirtualDisplay.
//
// Build with the WDK (msbuild ScreenFlipIdd.vcxproj). It must be signed (test-sign
// for personal use). See README.md. UNTESTED on the author's hardware.
#pragma once

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>

namespace Microsoft { namespace WRL { namespace Wrappers {
    // (ComPtr is brought in via wrl.h)
}}}

// A Direct3D 11 device bound to a specific DXGI adapter (by LUID).
struct Direct3DDevice {
    Direct3DDevice(LUID adapterLuid);
    Direct3DDevice();
    HRESULT Init();

    LUID AdapterLuid{};
    Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device>  Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

// Consumes frames the compositor presents to the virtual monitor and discards
// them (we never display the virtual content — the app captures it instead).
struct SwapChainProcessor {
    SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
    ~SwapChainProcessor();

    IDDCX_SWAPCHAIN m_hSwapChain;
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE m_hAvailableBufferEvent;
    Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_hThread;
    Microsoft::WRL::Wrappers::Event m_hTerminateEvent;

    static DWORD CALLBACK RunThread(LPVOID Argument);
    void Run();
    void RunCore();
};

// Per-monitor context.
struct IndirectMonitorContext {
    IndirectMonitorContext(IDDCX_MONITOR hMonitor);
    ~IndirectMonitorContext();
    void AssignSwapChain(IDDCX_SWAPCHAIN hSwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
    void UnassignSwapChain();

    IDDCX_MONITOR m_hMonitor;
    std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
};

// Per-adapter (device) context — creates the virtual monitor.
struct IndirectDeviceContext {
    IndirectDeviceContext(WDFDEVICE WdfDevice);
    virtual ~IndirectDeviceContext() = default;

    void InitAdapter();
    void FinishInit();

    WDFDEVICE     m_WdfDevice;
    IDDCX_ADAPTER m_Adapter{};
    IDDCX_MONITOR m_Monitor{};
};

// WDF context-object plumbing.
struct IndirectDeviceContextWrapper {
    IndirectDeviceContext* pContext;
    void Cleanup() { delete pContext; pContext = nullptr; }
};
struct IndirectMonitorContextWrapper {
    IndirectMonitorContext* pContext;
    void Cleanup() { delete pContext; pContext = nullptr; }
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);
