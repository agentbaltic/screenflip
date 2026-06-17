// Shared geometry / identity types for ScreenFlip (Windows).
//
// Windows uses a single, top-left-origin "virtual screen" pixel space for every
// monitor (GetCursorPos, monitor rects and display-config positions all agree),
// which is *simpler* than macOS — there is no CG/AppKit y-flip to undo. All
// coordinates below are virtual-screen pixels unless noted.
#pragma once

#include <windows.h>
#include <string>

namespace sf {

// A monitor's stable identity (CCD monitorDevicePath) — the analogue of the
// macOS display UUID. Survives reboot / re-enumeration far better than an
// HMONITOR (a runtime handle) or a \\.\DISPLAYn name (positional, not stable).
using DisplayId = std::wstring;

// Maps a physical output P to its hidden virtual workspace V, for the cursor
// proxy and edge guard (port of macOS `WorkspaceMapping`). The live-lookup
// handles (pMon / vMon) are re-queried every tick via GetMonitorInfo so the
// edge guard always uses LIVE bounds — never a stale cached rect that could
// trap the real cursor on a display the user actually uses.
struct WorkspaceMapping {
    HMONITOR pMon = nullptr;   // physical output P — live-lookup handle
    RECT     pRect{};          // cached P rect (virtual-screen px)
    HMONITOR vMon = nullptr;   // virtual workspace V — live-lookup handle
    POINT    vOrigin{};        // cached V origin (virtual-screen px)
    SIZE     vSize{};          // V pixel size
};

inline bool RectContains(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

inline bool RectIsEmpty(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

inline long RectWidth(const RECT& r)  { return r.right - r.left; }
inline long RectHeight(const RECT& r) { return r.bottom - r.top; }

} // namespace sf
