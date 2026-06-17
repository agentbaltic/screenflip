// One virtual flipped workspace (port of macOS FlipController.swift).
// Link: user32.lib (covered by the others)
#include "FlipController.h"
#include "Log.h"

#include <functional>

namespace sf {

FlipController::FlipController(HINSTANCE hInst, const DisplayInfo& physical)
    : hInst_(hInst), physical_(physical) {}

std::unique_ptr<FlipController> FlipController::Make(HINSTANCE hInst, const DisplayInfo& physical) {
    return std::unique_ptr<FlipController>(new FlipController(hInst, physical));
}

FlipController::~FlipController() { Stop(); }

DisplayId FlipController::VirtualId() const {
    return virtual_ ? virtual_->Id() : DisplayId();
}

bool FlipController::WorkspaceAlive() const {
    if (degraded_) return true;                       // degraded mode has no V to die
    return virtual_ && virtual_->Alive();
}

bool FlipController::Start(const std::set<DisplayId>& inUseVirtual) {
    stopped_.store(false);

    DisplayInfo pNow;
    if (!Displays::Find(physical_.id, pNow)) {
        Log::Linef(L"FlipController: physical display gone before start id=%s", physical_.id.c_str());
        return false;
    }
    physical_ = pNow;

    const int w = (int)RectWidth(pNow.rect);
    const int h = (int)RectHeight(pNow.rect);

    virtual_ = VirtualDisplay::Create(w, h, inUseVirtual);

    if (virtual_) {
        degraded_ = false;

        // Arrange: main | V (sharing main's right edge so the cursor flows onto V),
        // with P off V's bottom-right CORNER — corner contact is a legal arrangement
        // but leaves no shared edge, so the cursor can't roll from V onto P.
        DisplayInfo main;
        if (Displays::Primary(main)) {
            POINT vOrigin{ main.rect.right, main.rect.top };
            POINT pOrigin{ main.rect.right + w, main.rect.top + h };
            Displays::SetOrigins({ { virtual_->GdiName(), vOrigin },
                                   { physical_.gdiName,   pOrigin } });
            Log::Linef(L"FlipController: arranged V@(%ld,%ld) P@(%ld,%ld)",
                       vOrigin.x, vOrigin.y, pOrigin.x, pOrigin.y);
        }

        // Re-read live geometry after the rearrange — never trust requested origins.
        DisplayInfo pFresh, vFresh;
        Displays::Find(physical_.id, pFresh);
        bool haveV = Displays::Find(virtual_->Id(), vFresh);
        physical_ = pFresh.mon ? pFresh : physical_;

        mapping_ = WorkspaceMapping{};
        mapping_.pMon   = physical_.mon;
        mapping_.pRect  = physical_.rect;
        if (haveV) {
            mapping_.vMon    = vFresh.mon;
            mapping_.vOrigin = POINT{ vFresh.rect.left, vFresh.rect.top };
            mapping_.vSize   = SIZE{ RectWidth(vFresh.rect), RectHeight(vFresh.rect) };
            captureMon_ = vFresh.mon;
            hasMapping_ = true;
        }

        overlay_.Create(hInst_, physical_.rect);
        overlay_.SetCapture(&capture_);
        capture_.onError = [this](const std::wstring& m) {
            Log::Linef(L"FlipController(%s): capture error: %s", physical_.id.c_str(), m.c_str());
            ScheduleCaptureRestart();
        };
        capture_.Start(captureMon_);
        Log::Linef(L"FlipController: started P=%s shows flipped V=%s", physical_.id.c_str(), virtual_->Id().c_str());
        return true;
    }

    // ---- Degraded mode: mirror a different REAL display S onto P (no cursor fix). ----
    degraded_ = true;
    hasMapping_ = false;
    DisplayInfo source;
    bool haveSource = false;
    DisplayInfo prim;
    if (Displays::Primary(prim) && prim.id != physical_.id) { source = prim; haveSource = true; }
    if (!haveSource) {
        for (auto& d : Displays::All()) {
            if (d.id != physical_.id) { source = d; haveSource = true; break; }
        }
    }
    if (!haveSource) {
        Log::Line(L"FlipController: degraded mode needs a second display; only the target exists — not starting");
        return false;   // mirroring P onto itself would feed back
    }

    captureMon_ = source.mon;
    overlay_.Create(hInst_, physical_.rect);
    overlay_.SetCapture(&capture_);
    capture_.onError = [this](const std::wstring& m) {
        Log::Linef(L"FlipController(%s degraded): capture error: %s", physical_.id.c_str(), m.c_str());
        ScheduleCaptureRestart();
    };
    capture_.Start(captureMon_);
    Log::Linef(L"FlipController: DEGRADED — P=%s mirrors S=%s (cursor not corrected)",
               physical_.id.c_str(), source.id.c_str());
    return true;
}

void FlipController::RepositionOverlay() {
    DisplayInfo p;
    if (!Displays::Find(physical_.id, p)) return;
    physical_ = p;
    overlay_.Reposition(p.rect);
    if (!degraded_ && virtual_) {
        DisplayInfo v;
        if (Displays::Find(virtual_->Id(), v)) {
            mapping_.pMon = p.mon; mapping_.pRect = p.rect;
            mapping_.vMon = v.mon; mapping_.vOrigin = POINT{ v.rect.left, v.rect.top };
            mapping_.vSize = SIZE{ RectWidth(v.rect), RectHeight(v.rect) };
            captureMon_ = v.mon;
            hasMapping_ = true;
        } else {
            hasMapping_ = false;
        }
    }
}

void FlipController::ScheduleCaptureRestart() {
    bool expected = false;
    if (!restartPending_.compare_exchange_strong(expected, true)) return;
    // This runs on a WGC pool thread (via capture_.onError); Stop() runs on the UI
    // thread. Serialize restartTimer_ create/set against Stop()'s cancel/close, and
    // re-check stopped_ INSIDE the lock so we never arm a timer Stop() just freed.
    std::lock_guard<std::mutex> lk(timerMx_);
    if (stopped_.load()) { restartPending_.store(false); return; }
    if (!restartTimer_) {
        restartTimer_ = CreateThreadpoolTimer(&FlipController::RestartTimerCb, this, nullptr);
        if (!restartTimer_) { restartPending_.store(false); return; }
    }
    // Fire once, ~2 s out (relative time = negative 100ns units).
    ULARGE_INTEGER due; due.QuadPart = (ULONGLONG)(-(LONGLONG)(2 * 10'000'000));
    FILETIME ft; ft.dwLowDateTime = due.LowPart; ft.dwHighDateTime = due.HighPart;
    SetThreadpoolTimer(restartTimer_, &ft, 0, 0);
}

void CALLBACK FlipController::RestartTimerCb(PTP_CALLBACK_INSTANCE, void* ctx, PTP_TIMER) {
    auto* self = static_cast<FlipController*>(ctx);
    self->restartPending_.store(false);
    if (self->stopped_.load()) return;

    if (!self->degraded_) {
        if (!self->virtual_ || !self->virtual_->Alive()) {
            // Workspace is gone — let App::reconcile rebuild the whole controller.
            self->virtual_.reset();
            if (self->onNeedsReconcile) self->onNeedsReconcile();
            return;
        }
    }
    if (self->stopped_.load()) return;   // a Stop() raced in after the checks above
    Log::Linef(L"FlipController(%s): restarting capture", self->physical_.id.c_str());
    self->capture_.Start(self->captureMon_);
}

void FlipController::Stop() {
    if (stopped_.exchange(true)) return;   // already stopped (sets stopped_ before the lock)

    // Detach the timer handle UNDER the lock so a concurrent ScheduleCaptureRestart
    // (pool thread) sees stopped_==true + a null handle and bails. Then wait/close
    // OUTSIDE the lock: WaitForThreadpoolTimerCallbacks can run the callback, which
    // tears capture down and could make another thread call ScheduleCaptureRestart —
    // that thread must be able to take timerMx_ and bail, so we must not hold it here.
    PTP_TIMER timer = nullptr;
    {
        std::lock_guard<std::mutex> lk(timerMx_);
        timer = restartTimer_;
        restartTimer_ = nullptr;
    }
    if (timer) {
        SetThreadpoolTimerEx(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }
    // Stop capture FIRST — it drains any in-flight WGC callbacks — then it is safe
    // to clear onError (otherwise the pool thread could read a half-cleared handler).
    capture_.Stop();
    capture_.onError = nullptr;
    overlay_.Destroy();
    if (virtual_) { virtual_->Destroy(); virtual_.reset(); }
    hasMapping_ = false;
    mapping_ = WorkspaceMapping{};
    Log::Linef(L"FlipController: stopped P=%s", physical_.id.c_str());
}

} // namespace sf
