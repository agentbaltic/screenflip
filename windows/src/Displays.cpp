// Displays.cpp — monitor enumeration, stable identity, and arrangement
// snapshot/set/restore (port of macOS Displays.swift).
//
// Strategy:
//   * Enumeration: EnumDisplayMonitors + GetMonitorInfoW(MONITORINFOEXW) gives
//     the live rect, the GDI device name (\\.\DISPLAYn), and the PRIMARY flag.
//   * Stable identity: the CCD API (GetDisplayConfigBufferSizes +
//     QueryDisplayConfig over QDC_ONLY_ACTIVE_PATHS). For every active path we
//     read DISPLAYCONFIG_SOURCE_DEVICE_NAME (viewGdiDeviceName, i.e. the
//     \\.\DISPLAYn) to MATCH the GDI name from GetMonitorInfo, and
//     DISPLAYCONFIG_TARGET_DEVICE_NAME (monitorDevicePath + friendly name +
//     EDID vendor/product) to fill the stable id, the friendly name, and the
//     virtual-display flag. If the CCD lookup fails for a monitor we fall back
//     to id = gdiName.
//   * Arrangement: snapshot/restore uses the legacy, reliable per-device
//     ChangeDisplaySettingsExW(... DM_POSITION, CDS_UPDATEREGISTRY|CDS_NORESET)
//     batched, with a final ChangeDisplaySettingsExW(NULL,...) to apply
//     atomically — exactly the macOS snapshotOrigins/restoreOrigins shape.
//
// Link libraries required by this translation unit:
//   user32.lib   — EnumDisplayMonitors, GetMonitorInfoW, EnumDisplaySettingsW,
//                  ChangeDisplaySettingsExW, GetDisplayConfigBufferSizes,
//                  QueryDisplayConfig, DisplayConfigGetDeviceInfo
//   advapi32.lib — (none directly here, but Log.cpp uses it; harmless to list)
// Only user32.lib is strictly needed for this file.

#include "Displays.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

#include "Log.h"

namespace sf {

// The opaque type forward-declared in Displays.h. Keyed by GDI device name.
struct Displays::Arrangement {
    std::map<std::wstring, POINTL> origins;
};

namespace Displays {

namespace {

// ---- The virtual-display EDID magic (mirrors the macOS numbers) ------------
// EDID vendor id is a packed 3-letter code. 0x5346 corresponds to "SF…" in the
// classic 5-bit-per-letter encoding; we compare against both the friendly-name
// markers and the raw CCD ids, best-effort.
constexpr unsigned short kVirtualEdidVendor  = 0x5346;
constexpr unsigned short kVirtualEdidProduct = 0x5350;

// ---- A single resolved CCD record, matched by GDI device name --------------
struct CcdRecord {
    std::wstring gdiName;        // \\.\DISPLAYn  (from SOURCE_DEVICE_NAME)
    std::wstring monitorPath;    // stable monitorDevicePath (TARGET_DEVICE_NAME)
    std::wstring friendlyName;   // monitorFriendlyDeviceName
    unsigned short edidVendor  = 0;
    unsigned short edidProduct = 0;
    bool valid = false;
};

bool ContainsAnyMarker(const std::wstring& haystack) {
    // Case-insensitive search for known virtual-display markers.
    std::wstring lower = haystack;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    static const wchar_t* kMarkers[] = { L"virtual", L"iddcx", L"idd" };
    for (const wchar_t* m : kMarkers) {
        if (lower.find(m) != std::wstring::npos) return true;
    }
    return false;
}

// Query CCD for all active paths and build a map: GDI name -> CcdRecord.
// Returns an empty map (and logs) on failure; callers fall back to gdiName.
std::map<std::wstring, CcdRecord> QueryCcdByGdiName() {
    std::map<std::wstring, CcdRecord> result;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,
                                          &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        Log::Linef(L"Displays: GetDisplayConfigBufferSizes failed (%ld)", rc);
        return result;
    }
    if (pathCount == 0) {
        return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                            &pathCount, paths.data(),
                            &modeCount, modes.data(),
                            nullptr);
    if (rc != ERROR_SUCCESS) {
        Log::Linef(L"Displays: QueryDisplayConfig failed (%ld)", rc);
        return result;
    }
    // QueryDisplayConfig can shrink the counts; honor the returned sizes.
    paths.resize(pathCount);
    modes.resize(modeCount);

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        CcdRecord rec;

        // --- SOURCE: viewGdiDeviceName, the \\.\DISPLAYn we match against ----
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size      = sizeof(src);
        src.header.adapterId = path.sourceInfo.adapterId;
        src.header.id        = path.sourceInfo.id;
        LONG srcRc = DisplayConfigGetDeviceInfo(&src.header);
        if (srcRc != ERROR_SUCCESS) {
            Log::Linef(L"Displays: GET_SOURCE_NAME failed (%ld)", srcRc);
            continue;  // no GDI name -> cannot match this path
        }
        rec.gdiName = src.viewGdiDeviceName;
        if (rec.gdiName.empty()) {
            continue;
        }

        // --- TARGET: monitorDevicePath (stable id) + friendly name + EDID ----
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size      = sizeof(tgt);
        tgt.header.adapterId = path.targetInfo.adapterId;
        tgt.header.id        = path.targetInfo.id;
        LONG tgtRc = DisplayConfigGetDeviceInfo(&tgt.header);
        if (tgtRc == ERROR_SUCCESS) {
            rec.monitorPath  = tgt.monitorDevicePath;
            rec.friendlyName = tgt.monitorFriendlyDeviceName;
            rec.edidVendor   = tgt.edidManufactureId;
            rec.edidProduct  = tgt.edidProductCodeId;
            rec.valid        = true;
        } else {
            // We still keep the source mapping (so we at least know the GDI
            // name resolved a path) but without a stable id.
            Log::Linef(L"Displays: GET_TARGET_NAME failed for %s (%ld)",
                       rec.gdiName.c_str(), tgtRc);
            rec.valid = false;
        }

        // Keep the first valid record per GDI name; do not overwrite a valid
        // one with an invalid one.
        auto it = result.find(rec.gdiName);
        if (it == result.end() || (!it->second.valid && rec.valid)) {
            result[rec.gdiName] = std::move(rec);
        }
    }

    return result;
}

// Build a friendly menu label that mirrors the macOS format:
//   "<Friendly> (WxH)"  plus  " — main"  when primary.
std::wstring MakeLabel(const std::wstring& base, const RECT& rect, bool primary) {
    const long w = RectWidth(rect);
    const long h = RectHeight(rect);
    std::wstring label = base;
    label += L" (";
    label += std::to_wstring(w);
    label += L"x";
    label += std::to_wstring(h);
    label += L")";
    if (primary) {
        label += L" — main";  // em dash, matching macOS
    }
    return label;
}

// EnumDisplayMonitors callback: collect one DisplayInfo per HMONITOR with the
// fields obtainable from GDI (mon, gdiName, rect, isPrimary). CCD enrichment is
// applied afterward in All().
BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC /*hdc*/, LPRECT /*clip*/,
                              LPARAM lparam) {
    auto* list = reinterpret_cast<std::vector<DisplayInfo>*>(lparam);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        Log::Linef(L"Displays: GetMonitorInfoW failed (%lu)", GetLastError());
        return TRUE;  // skip this monitor, keep enumerating
    }

    DisplayInfo info;
    info.mon       = hMon;
    info.gdiName   = mi.szDevice;
    info.rect      = mi.rcMonitor;
    info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(std::move(info));
    return TRUE;
}

}  // namespace

std::vector<DisplayInfo> All() {
    std::vector<DisplayInfo> list;

    if (!EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                             reinterpret_cast<LPARAM>(&list))) {
        Log::Linef(L"Displays: EnumDisplayMonitors failed (%lu)", GetLastError());
    }

    // Enrich with stable identity / friendly name / virtual flag from CCD.
    std::map<std::wstring, CcdRecord> ccd = QueryCcdByGdiName();

    for (DisplayInfo& d : list) {
        const CcdRecord* rec = nullptr;
        auto it = ccd.find(d.gdiName);
        if (it != ccd.end() && it->second.valid) {
            rec = &it->second;
        }

        std::wstring base;
        if (rec) {
            d.id = rec->monitorPath.empty() ? d.gdiName : rec->monitorPath;
            base = rec->friendlyName.empty() ? d.gdiName : rec->friendlyName;

            // Virtual detection: friendly-name markers OR our EDID magic.
            const bool markerHit = ContainsAnyMarker(rec->friendlyName);
            const bool edidHit   = (rec->edidVendor  == kVirtualEdidVendor &&
                                    rec->edidProduct == kVirtualEdidProduct);
            d.isVirtual = markerHit || edidHit;
        } else {
            // CCD lookup failed for this monitor: fall back to the GDI name.
            d.id        = d.gdiName;
            base        = d.gdiName;
            d.isVirtual = false;
        }

        d.name = MakeLabel(base, d.rect, d.isPrimary);
    }

    return list;
}

bool Primary(DisplayInfo& out) {
    for (const DisplayInfo& d : All()) {
        if (d.isPrimary) {
            out = d;
            return true;
        }
    }
    // Defensive fallback: a monitor whose origin is (0,0).
    for (const DisplayInfo& d : All()) {
        if (d.rect.left == 0 && d.rect.top == 0) {
            out = d;
            return true;
        }
    }
    Log::Line(L"Displays: Primary() found no primary monitor");
    return false;
}

bool Find(const DisplayId& id, DisplayInfo& out) {
    for (const DisplayInfo& d : All()) {
        if (d.id == id) {
            out = d;
            return true;
        }
    }
    return false;
}

bool FindByMonitor(HMONITOR mon, DisplayInfo& out) {
    if (mon == nullptr) return false;
    for (const DisplayInfo& d : All()) {
        if (d.mon == mon) {
            out = d;
            return true;
        }
    }
    return false;
}

bool LiveRect(HMONITOR mon, RECT& out) {
    out = RECT{};
    if (mon == nullptr) {
        return false;
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        // Handle no longer valid (display removed / asleep) — empty rect, false.
        return false;
    }
    out = mi.rcMonitor;
    return true;
}

// --- Arrangement ------------------------------------------------------------

std::shared_ptr<Arrangement> Snapshot() {
    auto snap = std::make_shared<Arrangement>();

    for (const DisplayInfo& d : All()) {
        if (d.gdiName.empty()) continue;

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(d.gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
            Log::Linef(L"Displays: Snapshot EnumDisplaySettingsW failed for %s",
                       d.gdiName.c_str());
            continue;
        }
        // dmPosition is valid only when DM_POSITION is part of dmFields, but
        // ENUM_CURRENT_SETTINGS reliably populates it on the desktop driver.
        POINTL pos{};
        pos.x = dm.dmPosition.x;
        pos.y = dm.dmPosition.y;
        snap->origins[d.gdiName] = pos;
    }

    Log::Linef(L"Displays: Snapshot captured %zu display origins",
               snap->origins.size());
    return snap;
}

void Restore(const std::shared_ptr<Arrangement>& snap) {
    if (!snap) {
        Log::Line(L"Displays: Restore called with null snapshot — ignoring");
        return;
    }
    if (snap->origins.empty()) {
        Log::Line(L"Displays: Restore snapshot is empty — nothing to do");
        return;
    }

    std::vector<std::pair<std::wstring, POINT>> origins;
    origins.reserve(snap->origins.size());
    for (const auto& kv : snap->origins) {
        POINT p{ kv.second.x, kv.second.y };
        origins.emplace_back(kv.first, p);
    }
    SetOrigins(origins);
}

bool SetOrigins(const std::vector<std::pair<std::wstring, POINT>>& origins) {
    if (origins.empty()) {
        return true;
    }

    bool allOk = true;

    // Phase 1: queue each device's new position into the registry without
    // applying (CDS_NORESET) so the whole set lands atomically.
    for (const auto& kv : origins) {
        const std::wstring& gdiName = kv.first;
        const POINT& pos = kv.second;
        if (gdiName.empty()) {
            allOk = false;
            continue;
        }

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
            Log::Linef(L"Displays: SetOrigins EnumDisplaySettingsW failed for %s",
                       gdiName.c_str());
            allOk = false;
            continue;
        }

        dm.dmFields    |= DM_POSITION;
        dm.dmPosition.x = pos.x;
        dm.dmPosition.y = pos.y;

        LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr,
                                          CDS_UPDATEREGISTRY | CDS_NORESET,
                                          nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            Log::Linef(L"Displays: ChangeDisplaySettingsExW(%s -> %ld,%ld) "
                       L"returned %ld",
                       gdiName.c_str(), pos.x, pos.y, r);
            allOk = false;
        } else {
            Log::Linef(L"Displays: queued %s -> (%ld,%ld)",
                       gdiName.c_str(), pos.x, pos.y);
        }
    }

    // Phase 2: apply all queued changes atomically.
    LONG applyRc = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applyRc != DISP_CHANGE_SUCCESSFUL) {
        Log::Linef(L"Displays: SetOrigins apply (ChangeDisplaySettingsExW NULL) "
                   L"returned %ld", applyRc);
        allOk = false;
    } else {
        Log::Linef(L"Displays: SetOrigins applied %zu origin change(s)",
                   origins.size());
    }

    return allOk;
}

}  // namespace Displays
}  // namespace sf
