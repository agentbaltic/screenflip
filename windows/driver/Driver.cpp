// ScreenFlip indirect-display (IddCx) driver — implementation.
//
// Adapted from the structure of Microsoft's public IndirectDisplay/IddSampleDriver
// sample (Windows-driver-samples, MIT). Exposes one headless virtual monitor that
// the ScreenFlip app adopts as its flipped workspace. Frames the compositor sends
// to the monitor are drained and discarded (the app captures the monitor with WGC
// instead). UNTESTED — build with the WDK and test-sign for personal use.
#include "Driver.h"

#include <list>

using namespace Microsoft::WRL;

// 1920x1080@60 virtual monitor EDID (see tools/make_edid.py).
static const unsigned char kScreenFlipEdid[128] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x4C, 0xD0, 0x50, 0x53,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x21, 0x01, 0x04, 0xA5, 0x33, 0x1D, 0x78,
    0x0A, 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38,
    0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0xFD, 0x1E, 0x11, 0x00, 0x00, 0x1E,
    0x00, 0x00, 0x00, 0xFC, 0x00, 0x53, 0x46, 0x6C, 0x69, 0x70, 0x20, 0x56,
    0x69, 0x72, 0x74, 0x75, 0x61, 0x6C, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x32,
    0x4B, 0x1E, 0x53, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC1,
};

struct ScreenFlipMode { DWORD Width; DWORD Height; DWORD VSync; };
static const ScreenFlipMode kModes[] = {
    { 1920, 1080, 60 },
    { 2560, 1440, 60 },
    { 3840, 2160, 60 },
    { 1600,  900, 60 },
    { 1280,  720, 60 },
};
static const DWORD kPreferredModeIndex = 0;

// ------------------------------------------------------------------ helpers ----

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD w, DWORD h, DWORD vsync,
                                                 IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo.totalSize  = { w, h };
    mode.MonitorVideoSignalInfo.activeSize = { w, h };
    mode.MonitorVideoSignalInfo.vSyncFreq  = { vsync, 1 };
    mode.MonitorVideoSignalInfo.hSyncFreq  = { vsync * (h + 45), 1 };
    mode.MonitorVideoSignalInfo.pixelRate  = (UINT64)(w + 280) * (h + 45) * vsync;
    mode.MonitorVideoSignalInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    mode.MonitorVideoSignalInfo.videoStandard    = D3DKMDT_VSS_OTHER;
    return mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD w, DWORD h, DWORD vsync) {
    IDDCX_TARGET_MODE mode{};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.totalSize  = { w + 280, h + 45 };
    mode.TargetVideoSignalInfo.activeSize = { w, h };
    mode.TargetVideoSignalInfo.vSyncFreq  = { vsync, 1 };
    mode.TargetVideoSignalInfo.hSyncFreq  = { vsync * (h + 45), 1 };
    mode.TargetVideoSignalInfo.pixelRate  = (UINT64)(w + 280) * (h + 45) * vsync;
    mode.TargetVideoSignalInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    mode.TargetVideoSignalInfo.videoStandard    = D3DKMDT_VSS_OTHER;
    return mode;
}

// ----------------------------------------------------------- Direct3DDevice ----

Direct3DDevice::Direct3DDevice(LUID adapterLuid) : AdapterLuid(adapterLuid) {}
Direct3DDevice::Direct3DDevice() : AdapterLuid{} {}

HRESULT Direct3DDevice::Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) return hr;
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) return hr;
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                           nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    return hr;
}

// -------------------------------------------------------- SwapChainProcessor ----

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain,
                                       std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent) {
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor() {
    SetEvent(m_hTerminateEvent.Get());
    if (m_hThread.Get()) WaitForSingleObject(m_hThread.Get(), INFINITE);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument) {
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run() {
    DWORD taskIndex = 0;
    HANDLE hAvTask = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);
    RunCore();
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;
    if (hAvTask) AvRevertMmThreadCharacteristics(hAvTask);
}

void SwapChainProcessor::RunCore() {
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr)) return;

    IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
    setDevice.pDevice = DxgiDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(m_hSwapChain, &setDevice))) return;

    for (;;) {
        ComPtr<IDXGIResource> AcquiredBuffer;
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);
        if (hr == E_PENDING) {
            HANDLE waits[] = { m_hAvailableBufferEvent, m_hTerminateEvent.Get() };
            DWORD r = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
            if (r == WAIT_OBJECT_0 + 1) break;          // terminate
            // else loop and try again (frame ready or timeout)
        } else if (SUCCEEDED(hr)) {
            // We don't display the frame — just acknowledge it (the app captures
            // the monitor via WGC). Releasing on the next iteration is enough.
            AcquiredBuffer.Attach(buffer.MetaData.pSurface);
            AcquiredBuffer.Reset();
        } else {
            break;                                       // device lost / fatal
        }
    }
}

// --------------------------------------------------- IndirectMonitorContext ----

IndirectMonitorContext::IndirectMonitorContext(IDDCX_MONITOR hMonitor) : m_hMonitor(hMonitor) {}
IndirectMonitorContext::~IndirectMonitorContext() { m_ProcessingThread.reset(); }

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN hSwapChain, LUID RenderAdapter, HANDLE NewFrameEvent) {
    m_ProcessingThread.reset();
    auto Device = std::make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init())) {
        WdfObjectDelete((WDFOBJECT)hSwapChain);
        return;
    }
    m_ProcessingThread = std::make_unique<SwapChainProcessor>(hSwapChain, Device, NewFrameEvent);
}

void IndirectMonitorContext::UnassignSwapChain() { m_ProcessingThread.reset(); }

// ---------------------------------------------------- IndirectDeviceContext ----

IndirectDeviceContext::IndirectDeviceContext(WDFDEVICE WdfDevice) : m_WdfDevice(WdfDevice) {}

void IndirectDeviceContext::InitAdapter() {
    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"ScreenFlip Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"ScreenFlip";
    caps.EndPointDiagnostics.pEndPointModelName = L"SFlip Virtual";

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = m_WdfDevice;
    init.pCaps = &caps;
    init.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT initOut{};
    if (SUCCEEDED(IddCxAdapterInitAsync(&init, &initOut)))
        m_Adapter = initOut.AdapterObject;
}

void IndirectDeviceContext::FinishInit() {
    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = sizeof(kScreenFlipEdid);
    info.MonitorDescription.pData = const_cast<unsigned char*>(kScreenFlipEdid);
    // A FIXED container id so the monitor identity is stable across boots (and so we
    // need no ole32/CoCreateGuid dependency). {5350464C-5346-5650-0000-53637246706C}
    info.MonitorContainerId = { 0x5350464C, 0x5346, 0x5650,
                                { 0x00, 0x00, 0x53, 0x63, 0x72, 0x46, 0x70, 0x6C } };

    IDARG_IN_MONITORCREATE monitorCreate{};
    monitorCreate.ObjectAttributes = nullptr;
    monitorCreate.pMonitorInfo = &info;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectMonitorContextWrapper);
    monitorCreate.ObjectAttributes = &attr;

    IDARG_OUT_MONITORCREATE monitorCreateOut{};
    if (FAILED(IddCxMonitorCreate(m_Adapter, &monitorCreate, &monitorCreateOut))) return;
    m_Monitor = monitorCreateOut.MonitorObject;

    auto* wrapper = WdfObjectGet_IndirectMonitorContextWrapper(m_Monitor);
    wrapper->pContext = new IndirectMonitorContext(m_Monitor);

    IDARG_OUT_MONITORARRIVAL arrivalOut{};
    IddCxMonitorArrival(m_Monitor, &arrivalOut);
}

// ------------------------------------------------------------ IddCx callbacks --

static NTSTATUS ScreenFlipDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE) {
    auto* wrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    wrapper->pContext->InitAdapter();
    return STATUS_SUCCESS;
}

static NTSTATUS ScreenFlipAdapterInitFinished(IDDCX_ADAPTER Adapter,
                                              const IDARG_IN_ADAPTER_INIT_FINISHED* pArgs) {
    auto* wrapper = WdfObjectGet_IndirectDeviceContextWrapper(Adapter);
    if (NT_SUCCESS(pArgs->AdapterInitStatus))
        wrapper->pContext->FinishInit();
    return STATUS_SUCCESS;
}

static NTSTATUS ScreenFlipAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;   // single fixed monitor; nothing to reconfigure
}

static NTSTATUS ScreenFlipParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pIn,
                                                  IDARG_OUT_PARSEMONITORDESCRIPTION* pOut) {
    pOut->MonitorModeBufferOutputCount = ARRAYSIZE(kModes);
    if (pIn->MonitorModeBufferInputCount < ARRAYSIZE(kModes)) {
        return (pIn->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    for (DWORD i = 0; i < ARRAYSIZE(kModes); ++i) {
        pIn->pMonitorModes[i] = CreateIddCxMonitorMode(
            kModes[i].Width, kModes[i].Height, kModes[i].VSync,
            (i == kPreferredModeIndex) ? IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
                                       : IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    pOut->PreferredMonitorModeIdx = kPreferredModeIndex;
    return STATUS_SUCCESS;
}

static NTSTATUS ScreenFlipMonitorGetDefaultModes(IDDCX_MONITOR,
                                                 const IDARG_IN_GETDEFAULTDESCRIPTIONMODES*,
                                                 IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOut) {
    pOut->DefaultMonitorModeBufferOutputCount = 0;   // we always have an EDID
    return STATUS_SUCCESS;
}

static NTSTATUS ScreenFlipMonitorQueryModes(IDDCX_MONITOR,
                                            const IDARG_IN_QUERYTARGETMODES* pIn,
                                            IDARG_OUT_QUERYTARGETMODES* pOut) {
    std::vector<IDDCX_TARGET_MODE> modes;
    for (auto& m : kModes) modes.push_back(CreateIddCxTargetMode(m.Width, m.Height, m.VSync));

    pOut->TargetModeBufferOutputCount = (UINT)modes.size();
    if (pIn->TargetModeBufferInputCount >= modes.size())
        for (size_t i = 0; i < modes.size(); ++i) pIn->pTargetModes[i] = modes[i];
    return STATUS_SUCCESS;
}

static NTSTATUS ScreenFlipMonitorAssignSwapChain(IDDCX_MONITOR Monitor,
                                                 const IDARG_IN_SETSWAPCHAIN* pIn) {
    auto* wrapper = WdfObjectGet_IndirectMonitorContextWrapper(Monitor);
    wrapper->pContext->AssignSwapChain(pIn->hSwapChain, pIn->RenderAdapterLuid, pIn->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

static NTSTATUS ScreenFlipMonitorUnassignSwapChain(IDDCX_MONITOR Monitor) {
    auto* wrapper = WdfObjectGet_IndirectMonitorContextWrapper(Monitor);
    wrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

// --------------------------------------------------------------- WDF plumbing --

static void ScreenFlipDeviceContextCleanup(WDFOBJECT Object) {
    auto* wrapper = WdfObjectGet_IndirectDeviceContextWrapper(Object);
    if (wrapper) wrapper->Cleanup();
}
static void ScreenFlipMonitorContextCleanup(WDFOBJECT Object) {
    auto* wrapper = WdfObjectGet_IndirectMonitorContextWrapper(Object);
    if (wrapper) wrapper->Cleanup();
}

static NTSTATUS ScreenFlipDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT pDeviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDeviceD0Entry = ScreenFlipDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpPower);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished            = ScreenFlipAdapterInitFinished;
    config.EvtIddCxAdapterCommitModes             = ScreenFlipAdapterCommitModes;
    config.EvtIddCxParseMonitorDescription        = ScreenFlipParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = ScreenFlipMonitorGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes        = ScreenFlipMonitorQueryModes;
    config.EvtIddCxMonitorAssignSwapChain         = ScreenFlipMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain       = ScreenFlipMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(pDeviceInit, &config);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);
    attr.EvtCleanupCallback = ScreenFlipDeviceContextCleanup;

    WDFDEVICE device{};
    status = WdfDeviceCreate(&pDeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) return status;

    auto* wrapper = WdfObjectGet_IndirectDeviceContextWrapper(device);
    wrapper->pContext = new IndirectDeviceContext(device);
    return status;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, ScreenFlipDeviceAdd);
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    NTSTATUS status = WdfDriverCreate(pDriverObject, pRegistryPath, &attr, &config, WDF_NO_HANDLE);
    return status;
}

// Provide the monitor-context cleanup symbol referenced via attributes if needed.
extern "C" void ScreenFlipMonitorCleanupShim(WDFOBJECT o) { ScreenFlipMonitorContextCleanup(o); }
