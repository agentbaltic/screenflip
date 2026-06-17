// Borderless, topmost, click-through window covering one physical output P, that
// renders the captured virtual workspace horizontally FLIPPED, plus the proxy
// cursor (port of macOS OverlayWindow.swift, with the cursor proxy folded in).
//
// Rendering uses DirectComposition hosting a flip-model swap chain
// (CreateSwapChainForComposition) — a naive WS_EX_LAYERED window with a DXGI
// flip-model swap chain does NOT present correctly, so we compose instead. The
// window is WS_EX_TRANSPARENT|WS_EX_NOACTIVATE so clicks fall through, and is
// hidden from all capture via SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) so
// it can never feed back into a capture.
//
// A dedicated render thread ticks at ~90 Hz: it pulls the latest captured frame
// (Capture::CopyLatestInto), draws it flipped, overlays the proxy cursor quad at
// the position pushed by MirrorInput, and presents. Ticking on a steady timer
// (not only on frame arrival) keeps the proxy cursor moving even when the
// workspace content is static.
#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>

#include "Geometry.h"

namespace sf {

class Capture;

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    bool Create(HINSTANCE hInst, const RECT& monitorRect);
    void Destroy();
    void Reposition(const RECT& monitorRect);
    HWND Hwnd() const { return hwnd_; }

    // Source of flipped frames (owned by the FlipController).
    void SetCapture(Capture* cap) { capture_.store(cap); }

    // Proxy cursor state, set by MirrorInput (lock-free-ish via a small lock).
    void SetProxy(bool visible, POINT hotspotPos, bool flipped);
    void HideProxy();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void RenderThread();
    bool InitGraphics();
    void DiscardGraphics();
    bool EnsureFrameTexture(SIZE size);
    void DrawScene();   // records the draw on the shared context; caller holds renderLock and Presents

    HWND hwnd_ = nullptr;
    RECT rect_{};
    std::thread render_;
    std::atomic<bool> running_{false};
    std::atomic<Capture*> capture_{nullptr};

    // Geometry read by the render thread without locking (set on the UI thread by
    // Create/Reposition).
    std::atomic<long> originX_{0}, originY_{0};
    std::atomic<unsigned> targetW_{0}, targetH_{0};

    // Proxy state guarded by proxyMx_.
    std::mutex proxyMx_;
    bool  proxyVisible_ = false;
    POINT proxyPos_{};
    bool  proxyFlipped_ = false;

    struct Gfx;          // D3D/DComp state (defined in .cpp)
    Gfx* gfx_ = nullptr;
};

} // namespace sf
