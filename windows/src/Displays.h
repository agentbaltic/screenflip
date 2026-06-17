// Monitor enumeration, stable identity, and arrangement snapshot/set/restore
// (port of macOS Displays.swift).
//
// Identity: CCD `DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath` is the
// stable, reboot-surviving key — the Windows analogue of the macOS display UUID.
// Arrangement: repositioned with ChangeDisplaySettingsEx (per-device dmPosition,
// CDS_NORESET batched + a final apply) which is reliable and simple; CCD is used
// only to read the stable identity. The original layout is snapshotted at launch
// and restored on quit.
#pragma once

#include <windows.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Geometry.h"

namespace sf {

struct DisplayInfo {
    HMONITOR     mon = nullptr;     // runtime handle (NOT stable across changes)
    DisplayId    id;                // stable monitorDevicePath (UUID analogue)
    std::wstring gdiName;           // \\.\DISPLAYn — for ChangeDisplaySettingsEx
    std::wstring name;              // friendly label for the menu
    RECT         rect{};            // virtual-screen px
    bool         isPrimary = false;
    bool         isVirtual = false; // matches a known virtual-display EDID (vendor 0x5346 / product 0x5350)
};

namespace Displays {

// All active monitors, primary first is NOT guaranteed — use Primary().
std::vector<DisplayInfo> All();

// The primary monitor (origin 0,0). Returns false if none resolved.
bool Primary(DisplayInfo& out);

bool Find(const DisplayId& id, DisplayInfo& out);
bool FindByMonitor(HMONITOR mon, DisplayInfo& out);

// Live rect for a monitor handle (the edge guard's "fresh bounds by id" rule).
// Returns false (empty rect) if the handle is no longer valid.
bool LiveRect(HMONITOR mon, RECT& out);

// --- Arrangement ---
struct Arrangement;                                  // opaque; defined in .cpp
std::shared_ptr<Arrangement> Snapshot();
void Restore(const std::shared_ptr<Arrangement>& snap);

// Apply new top-left origins for the named displays in one batched transaction.
// `origins` pairs a GDI device name (\\.\DISPLAYn) with its desired top-left.
bool SetOrigins(const std::vector<std::pair<std::wstring, POINT>>& origins);

} // namespace Displays
} // namespace sf
