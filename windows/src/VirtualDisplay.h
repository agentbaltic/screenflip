// Headless virtual workspace V (port of macOS VirtualDisplay.swift).
//
// THE big porting difference: Windows has no driver-free way to create a virtual
// monitor (macOS gets one from the private CGVirtualDisplay). So a virtual monitor
// must already exist — created by the bundled ScreenFlip IddCx driver (windows/
// driver) or an existing signed community virtual-display driver — and this class
// ADOPTS it as the workspace. If none is available Create() returns nullptr and the
// caller (FlipController) degrades to "mirror an existing display".
#pragma once

#include <windows.h>
#include <memory>
#include <set>
#include <string>

#include "Geometry.h"

namespace sf {

class VirtualDisplay {
public:
    // Adopt an unclaimed virtual monitor, trying to set its mode to (width,height)
    // (P's size) so the mirror is 1:1. `inUse` lists virtual monitor ids already
    // claimed by other workspaces so we never adopt the same one twice. Returns
    // nullptr if no virtual monitor is present.
    static std::unique_ptr<VirtualDisplay> Create(int width, int height,
                                                  const std::set<DisplayId>& inUse);
    ~VirtualDisplay();

    HMONITOR     Monitor() const { return mon_; }
    std::wstring GdiName() const { return gdiName_; }
    DisplayId    Id()      const { return id_; }
    int          Width()   const { return width_; }
    int          Height()  const { return height_; }

    // True while the virtual monitor still exists in the live display set.
    bool Alive() const;

    void Destroy();

private:
    VirtualDisplay() = default;

    HMONITOR     mon_ = nullptr;
    std::wstring gdiName_;
    DisplayId    id_;
    int          width_ = 0;
    int          height_ = 0;
};

} // namespace sf
