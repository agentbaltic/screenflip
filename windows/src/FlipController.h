// One "virtual flipped workspace" (port of macOS FlipController.swift).
//
// Owns: the headless virtual display V, the chosen physical output P, the capture
// of V, and the overlay that shows V flipped on P. Arranges the displays so the
// cursor flows main -> V natively but cannot roll onto P (P hangs off V's
// bottom-right CORNER, no shared edge). Builds the WorkspaceMapping consumed by
// MirrorInput. Treats capture loss as transient (restart after 2 s) and flips
// WorkspaceAlive() false if V dies, so App::reconcile() rebuilds it.
//
// Degraded mode: if no virtual display is available, V is null and the controller
// instead mirrors a *different* real display S onto P (no cursor correction). It
// publishes no WorkspaceMapping in that case, so MirrorInput leaves input alone.
#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

#include "Geometry.h"
#include "Displays.h"
#include "Capture.h"
#include "OverlayWindow.h"
#include "VirtualDisplay.h"

namespace sf {

class FlipController {
public:
    static std::unique_ptr<FlipController> Make(HINSTANCE hInst, const DisplayInfo& physical);
    ~FlipController();

    bool Start(const std::set<DisplayId>& inUseVirtual);
    void Stop();

    void RepositionOverlay();

    bool WorkspaceAlive() const;          // false => reconcile() rebuilds us
    bool Degraded() const { return degraded_; }
    bool HasMapping() const { return hasMapping_; }
    WorkspaceMapping Mapping() const { return mapping_; }
    OverlayWindow* Overlay() { return &overlay_; }
    DisplayId PhysicalId() const { return physical_.id; }
    DisplayId VirtualId() const;

    std::function<void()> onNeedsReconcile;

private:
    FlipController(HINSTANCE hInst, const DisplayInfo& physical);
    void ScheduleCaptureRestart();
    static void CALLBACK RestartTimerCb(PTP_CALLBACK_INSTANCE, void* ctx, PTP_TIMER);

    HINSTANCE   hInst_ = nullptr;
    DisplayInfo physical_;
    OverlayWindow overlay_;
    Capture       capture_;
    std::unique_ptr<VirtualDisplay> virtual_;
    HMONITOR      captureMon_ = nullptr;   // monitor capture re-targets on restart

    WorkspaceMapping mapping_{};
    bool hasMapping_ = false;
    bool degraded_   = false;

    std::mutex   timerMx_;                 // serializes restartTimer_ create/cancel/close
    PTP_TIMER restartTimer_ = nullptr;
    std::atomic<bool> restartPending_{false};
    std::atomic<bool> stopped_{true};
};

} // namespace sf
