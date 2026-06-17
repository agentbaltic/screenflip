// Headless virtual workspace V (port of macOS VirtualDisplay.swift).
// Link: user32.lib  (ChangeDisplaySettingsEx / EnumDisplaySettings)
//
// See SPEC.md §2 and windows/driver/README.md: Windows has no driver-free virtual
// display, so this adopts a virtual monitor that a driver has already created. The
// bundled ScreenFlip IddCx driver advertises one named "SFlip Virtual"; community
// drivers expose their own. Detection is by Displays::DisplayInfo::isVirtual.
#include "VirtualDisplay.h"
#include "Displays.h"
#include "Log.h"

namespace sf {

// Best-effort: set the adopted monitor's mode to match P, so the flipped image is
// pixel-for-pixel. Ignored if the (virtual) driver doesn't advertise that mode.
static void TrySetMode(const std::wstring& gdiName, int w, int h) {
    if (gdiName.empty()) return;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
    if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h) return;
    dm.dmPelsWidth = (DWORD)w; dm.dmPelsHeight = (DWORD)h;
    dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    Log::Linef(L"VirtualDisplay: set %s to %dx%d -> %ld", gdiName.c_str(), w, h, r);
}

std::unique_ptr<VirtualDisplay> VirtualDisplay::Create(int width, int height,
                                                       const std::set<DisplayId>& inUse) {
    for (auto& d : Displays::All()) {
        if (!d.isVirtual || inUse.count(d.id)) continue;

        auto vd = std::unique_ptr<VirtualDisplay>(new VirtualDisplay());
        vd->mon_ = d.mon; vd->gdiName_ = d.gdiName; vd->id_ = d.id;
        TrySetMode(d.gdiName, width, height);

        // Re-read live geometry after the mode change — never trust the request.
        DisplayInfo r;
        if (Displays::Find(d.id, r)) { vd->mon_ = r.mon; }
        const DisplayInfo& cur = (vd->mon_ == d.mon) ? d : r;
        vd->width_  = RectWidth(cur.rect);
        vd->height_ = RectHeight(cur.rect);
        Log::Linef(L"VirtualDisplay: adopted virtual monitor id=%s (%dx%d)",
                   vd->id_.c_str(), vd->width_, vd->height_);
        return vd;
    }
    Log::Line(L"VirtualDisplay: no virtual display available (will degrade)");
    return nullptr;
}

bool VirtualDisplay::Alive() const {
    DisplayInfo info;
    if (!Displays::Find(id_, info)) return false;
    return !RectIsEmpty(info.rect);
}

void VirtualDisplay::Destroy() { mon_ = nullptr; }

VirtualDisplay::~VirtualDisplay() { Destroy(); }

} // namespace sf
