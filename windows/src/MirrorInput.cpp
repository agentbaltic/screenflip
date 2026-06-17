// Link: user32.lib  (GetCursorPos / SetCursorPos)  winmm.lib not required here.
#include "MirrorInput.h"
#include "OverlayWindow.h"
#include "Displays.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

namespace sf {

MirrorInput& MirrorInput::Shared() {
    static MirrorInput m;
    return m;
}

void MirrorInput::SetCursorFlipped(bool on) {
    flipped_.store(on);
    Log::Linef(L"MirrorInput: cursor flipped = %d", on ? 1 : 0);
}

void MirrorInput::SetTargets(std::vector<ProxyTarget> targets) {
    size_t count;
    {
        std::lock_guard<std::mutex> lk(mx_);
        targets_ = std::move(targets);
        count = targets_.size();
    }
    Log::Linef(L"MirrorInput: %zu workspace target(s)", count);
    if (count > 0) Start();
    else           Stop();
}

void MirrorInput::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return; // already running

    thread_ = std::thread([this] {
        // ~90 Hz high-resolution periodic timer (the macOS proxy runs at 90 Hz too).
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -1; // fire ~immediately
            SetWaitableTimer(timer, &due, 11 /*ms ≈ 90 Hz*/, nullptr, nullptr, FALSE);
        } else {
            Log::Line(L"MirrorInput: high-res timer unavailable, falling back to Sleep");
        }
        while (running_.load()) {
            if (timer) WaitForSingleObject(timer, 100);
            else       Sleep(11);
            if (!running_.load()) break;
            Tick();
        }
        if (timer) { CancelWaitableTimer(timer); CloseHandle(timer); }
    });
    Log::Line(L"MirrorInput: cursor proxy started (no input hook)");
}

void MirrorInput::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (thread_.joinable()) thread_.join();
    // Hide every proxy on the way out.
    std::lock_guard<std::mutex> lk(mx_);
    for (auto& t : targets_) if (t.overlay) t.overlay->HideProxy();
    Log::Line(L"MirrorInput: cursor proxy stopped");
}

// Draw the proxy on `t`'s overlay for a workspace point `cur` (inside V's live
// rect `vr`), at the horizontally-mirrored position on P. Scales V->P so a
// letterboxed / size-mismatched workspace still lands correctly.
static void DrawProxy(const ProxyTarget& t, POINT cur, const RECT& vr, bool flip) {
    if (!t.overlay) return;
    RECT pr;
    if (!Displays::LiveRect(t.mapping.pMon, pr) || RectIsEmpty(pr)) return;
    const double vw = (double)RectWidth(vr), vh = (double)RectHeight(vr);
    if (vw <= 0 || vh <= 0) return;
    double fx = (cur.x - vr.left) / vw;           // 0..1 across V
    double fy = (cur.y - vr.top)  / vh;
    double mx = 1.0 - fx;                          // horizontal mirror
    long sx = pr.left + (long)std::llround(mx * RectWidth(pr));
    long sy = pr.top  + (long)std::llround(fy * RectHeight(pr));
    t.overlay->SetProxy(true, POINT{sx, sy}, flip);
}

void MirrorInput::Tick() {
    POINT c;
    if (!GetCursorPos(&c)) return;
    const bool flip = flipped_.load();

    std::lock_guard<std::mutex> lk(mx_);

    // (1) Cursor on a workspace V → draw the proxy on its P, hide the others.
    for (size_t i = 0; i < targets_.size(); ++i) {
        RECT vr;
        if (!Displays::LiveRect(targets_[i].mapping.vMon, vr) || RectIsEmpty(vr)) continue;
        if (RectContains(vr, c)) {
            DrawProxy(targets_[i], c, vr, flip);
            for (size_t j = 0; j < targets_.size(); ++j)
                if (j != i && targets_[j].overlay) targets_[j].overlay->HideProxy();
            return;
        }
    }

    // (2) Edge guard. The physical output P only ever shows V's mirror, so the
    // cursor has no business there; if it slipped onto a P (shared edge after the
    // OS normalized the arrangement, an app warp, a fast flick), pin it back onto
    // that P's workspace. SAFETY: bounds are looked up LIVE by monitor handle every
    // tick — never the cached rect — and we bail if V's rect is empty (workspace
    // gone), so a stale mapping can never trap the real cursor on a usable display.
    for (size_t i = 0; i < targets_.size(); ++i) {
        RECT pr;
        if (!Displays::LiveRect(targets_[i].mapping.pMon, pr) || !RectContains(pr, c)) continue;
        RECT vr;
        if (!Displays::LiveRect(targets_[i].mapping.vMon, vr) || RectIsEmpty(vr)) continue; // leave cursor alone
        POINT pinned{
            std::min(std::max(c.x, vr.left), vr.right  - 1),
            std::min(std::max(c.y, vr.top),  vr.bottom - 1)
        };
        SetCursorPos(pinned.x, pinned.y);
        DrawProxy(targets_[i], pinned, vr, flip);
        for (size_t j = 0; j < targets_.size(); ++j)
            if (j != i && targets_[j].overlay) targets_[j].overlay->HideProxy();
        return;
    }

    // (3) Cursor is on a normal display → no proxy anywhere.
    for (auto& t : targets_) if (t.overlay) t.overlay->HideProxy();
}

} // namespace sf
