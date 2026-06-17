// Windows.Graphics.Capture (WGC) wrapper that captures ONE monitor and keeps the
// latest frame as a GPU texture (port of macOS Capture.swift / ScreenCaptureKit).
//
// Why WGC over Desktop Duplication: per-monitor capture is first-class
// (CreateForMonitor), the hardware cursor is excluded with a one-liner
// (IsCursorCaptureEnabled = false — the analogue of mac `showsCursor = false`),
// frames arrive as ID3D11Texture2D on the GPU, and our overlay can be hidden from
// capture with SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE).
//
// Threading: WGC delivers frames on a free-threaded pool. FrameArrived copies the
// frame into an internal `latest_` texture under a mutex and fires onFrame. The
// overlay's render thread pulls a private copy via CopyLatestInto() so it can draw
// without holding the lock during the draw (no tearing).
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace sf {

class Capture {
public:
    Capture();
    ~Capture();

    // Start capturing `monitor`. Auto-(re)creates the internal texture to match
    // the captured size. Returns false only on hard setup failure.
    bool Start(HMONITOR monitor);
    void Stop();

    // Fired (on a worker thread) when a fresh frame has been stored. Used to wake
    // the overlay render thread.
    std::function<void()> onFrame;
    // Fired (on a worker thread) on capture loss; FlipController restarts in ~2 s.
    std::function<void(const std::wstring&)> onError;

    bool HasFrame() const { return hasFrame_.load(); }
    SIZE FrameSize();    // last captured size (0,0 until first frame)

    // GPU-copy the latest frame into `dst` (must be the same size & BGRA format).
    // Returns false if no frame yet. Holds the internal lock only for the copy.
    bool CopyLatestInto(ID3D11Texture2D* dst);

private:
    struct Impl;
    Impl* impl_ = nullptr;             // hides the C++/WinRT types from this header
    std::atomic<bool> hasFrame_{false};
};

} // namespace sf
