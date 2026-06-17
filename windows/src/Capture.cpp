// Windows.Graphics.Capture (WGC) capture of one monitor → latest GPU texture.
// Link: windowsapp.lib  d3d11.lib  dxgi.lib   (C++/WinRT, /std:c++17)
//
// C++/WinRT is confined entirely to this translation unit (behind Capture::Impl)
// so no other file needs the WinRT headers.
//
// Threading & teardown: WGC delivers FrameArrived/Closed on a free-threaded pool.
// Revoking the event tokens does NOT block an in-flight callback, so Stop() uses an
// explicit DRAIN — it flips `running` false, closes the session/pool (no new
// callbacks), then waits until the in-flight count hits zero before nulling state.
// This mirrors the macOS Capture.swift shouldRun + pendingStop drain, and makes the
// FrameArrived/Closed handlers safe to touch `owner` and `impl` members.
#include "Capture.h"
#include "DXShared.h"
#include "Log.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <condition_variable>
#include <d3d11.h>

using Microsoft::WRL::ComPtr;
namespace wgc    = winrt::Windows::Graphics::Capture;
namespace wgdx   = winrt::Windows::Graphics::DirectX;
namespace wgdx3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace sf {

struct Capture::Impl {
    Capture* owner = nullptr;

    wgdx3d::IDirect3DDevice          device{ nullptr };
    wgc::GraphicsCaptureItem         item{ nullptr };
    wgc::Direct3D11CaptureFramePool  pool{ nullptr };
    wgc::GraphicsCaptureSession      session{ nullptr };
    winrt::event_token               frameTok{};
    winrt::event_token               closedTok{};
    winrt::Windows::Graphics::SizeInt32 size{ 0, 0 };

    std::mutex mx;                        // guards latest/latestSize + inFlight
    std::condition_variable idle;         // signalled when inFlight reaches 0
    std::atomic<bool> running{ false };   // read in callbacks without the lock
    int  inFlight = 0;
    ComPtr<ID3D11Texture2D> latest;
    SIZE latestSize{ 0, 0 };

    // RAII in-flight counter for a callback body.
    struct Guard {
        Impl* s;
        ~Guard() {
            std::lock_guard<std::mutex> lk(s->mx);
            if (--s->inFlight == 0) s->idle.notify_all();
        }
    };
    // Returns false (no Guard) if capture is no longer running.
    bool Enter() {
        std::lock_guard<std::mutex> lk(mx);
        if (!running.load()) return false;
        ++inFlight;
        return true;
    }

    void OnFrame(wgc::Direct3D11CaptureFramePool const& sender);
    void OnClosed();
};

void Capture::Impl::OnFrame(wgc::Direct3D11CaptureFramePool const& sender) {
    if (!Enter()) return;
    Guard g{ this };
    try {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        auto cs = frame.ContentSize();                 // read BEFORE Close()
        auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf())));
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);

        auto& dx = DXShared::Get();
        {
            // Acquire the shared render lock BEFORE the data lock (single global
            // ordering) so this CopyResource can't interleave an overlay's draw.
            std::lock_guard<std::recursive_mutex> rl(dx.renderLock);
            std::lock_guard<std::mutex> lk(mx);
            if (!latest || latestSize.cx != (LONG)desc.Width || latestSize.cy != (LONG)desc.Height) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = desc.Width; td.Height = desc.Height;
                td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> nt;
                if (FAILED(dx.device->CreateTexture2D(&td, nullptr, &nt))) { frame.Close(); return; }
                latest = nt;
                latestSize = SIZE{ (LONG)desc.Width, (LONG)desc.Height };
            }
            dx.context->CopyResource(latest.Get(), tex.Get());
        }
        frame.Close();

        owner->hasFrame_.store(true);
        if (running.load() && owner->onFrame) owner->onFrame();

        // Defensive: a virtual display is fixed-size, but handle a size change while
        // still running (and while the pool is still alive).
        if (running.load() && (cs.Width != size.Width || cs.Height != size.Height)) {
            size = cs;
            pool.Recreate(device, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
    } catch (winrt::hresult_error const& e) {
        if (running.load() && owner->onError) owner->onError(std::wstring(L"frame error: ") + e.message().c_str());
    } catch (...) {
        if (running.load() && owner->onError) owner->onError(L"frame error: unknown");
    }
}

void Capture::Impl::OnClosed() {
    if (!Enter()) return;
    Guard g{ this };
    owner->hasFrame_.store(false);
    if (running.load() && owner->onError) owner->onError(L"capture item closed");
}

Capture::Capture()  { impl_ = new Impl(); impl_->owner = this; }
Capture::~Capture() { Stop(); delete impl_; impl_ = nullptr; }

bool Capture::Start(HMONITOR monitor) {
    Stop();
    if (!DXShared::Get().Init()) return false;

    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            if (onError) onError(L"Windows.Graphics.Capture is not supported on this OS");
            return false;
        }

        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            DXShared::Get().dxgiDevice.Get(), insp.put()));
        impl_->device = insp.as<wgdx3d::IDirect3DDevice>();

        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
        auto interop = factory.as<IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(interop->CreateForMonitor(
            monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        impl_->item = item;
        impl_->size = item.Size();

        impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->device, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl_->size);
        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);

        // Don't capture the hardware cursor (we draw our own mirrored proxy). The
        // setter is absent before Windows 10 2004 — degrade gracefully if so.
        try { impl_->session.IsCursorCaptureEnabled(false); } catch (...) {}
        // Suppress the yellow capture border on Windows 11 (absent on older OS).
        try { impl_->session.IsBorderRequired(false); } catch (...) {}

        impl_->frameTok = impl_->pool.FrameArrived(
            [impl = impl_](wgc::Direct3D11CaptureFramePool const& sender,
                           winrt::Windows::Foundation::IInspectable const&) {
                impl->OnFrame(sender);
            });
        impl_->closedTok = impl_->item.Closed(
            [impl = impl_](wgc::GraphicsCaptureItem const&,
                           winrt::Windows::Foundation::IInspectable const&) {
                impl->OnClosed();
            });

        impl_->running.store(true);
        impl_->session.StartCapture();
        Log::Linef(L"Capture: started %dx%d", impl_->size.Width, impl_->size.Height);
        return true;
    } catch (winrt::hresult_error const& e) {
        if (onError) onError(std::wstring(L"start failed: ") + e.message().c_str());
        Stop();
        return false;
    } catch (...) {
        if (onError) onError(L"start failed: unknown");
        Stop();
        return false;
    }
}

void Capture::Stop() {
    if (!impl_) return;

    // 1) Stop accepting new callback work.
    impl_->running.store(false);

    // 2) Revoke handlers + close the session/pool so no NEW callbacks start.
    try {
        if (impl_->pool && impl_->frameTok)  impl_->pool.FrameArrived(impl_->frameTok);
        if (impl_->item && impl_->closedTok) impl_->item.Closed(impl_->closedTok);
        impl_->frameTok = {};
        impl_->closedTok = {};
        if (impl_->session) impl_->session.Close();
        if (impl_->pool)    impl_->pool.Close();
    } catch (...) {}

    // 3) DRAIN: wait for any in-flight callback to finish (it does not hold the
    //    render lock while waiting here, so no deadlock).
    { std::unique_lock<std::mutex> lk(impl_->mx); impl_->idle.wait(lk, [&] { return impl_->inFlight == 0; }); }

    // 4) Now nothing can touch these — tear down.
    impl_->session = nullptr;
    impl_->pool = nullptr;
    impl_->item = nullptr;
    impl_->device = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->mx);
        impl_->latest.Reset();
        impl_->latestSize = SIZE{ 0, 0 };
    }
    hasFrame_.store(false);
}

SIZE Capture::FrameSize() {
    std::lock_guard<std::mutex> lk(impl_->mx);
    return impl_->latestSize;
}

bool Capture::CopyLatestInto(ID3D11Texture2D* dst) {
    if (!dst) return false;
    auto& dx = DXShared::Get();
    std::lock_guard<std::recursive_mutex> rl(dx.renderLock);  // outer (matches OnFrame ordering)
    std::lock_guard<std::mutex> lk(impl_->mx);
    if (!impl_->latest) return false;
    D3D11_TEXTURE2D_DESC d{}; dst->GetDesc(&d);
    if ((LONG)d.Width != impl_->latestSize.cx || (LONG)d.Height != impl_->latestSize.cy)
        return false;  // overlay will resize its texture and retry next tick
    dx.context->CopyResource(dst, impl_->latest.Get());
    return true;
}

} // namespace sf
