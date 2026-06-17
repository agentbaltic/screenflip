// Persisted user settings (port of the macOS UserDefaults usage in AppDelegate).
// Backed by HKCU\Software\io.vbar.screenflip:
//   flippedDisplays : REG_MULTI_SZ  — stable DisplayIds the user wants flipped
//   flipCursor      : REG_DWORD     — mirror the proxy cursor image to match glass
#pragma once

#include <string>
#include <vector>

namespace sf {
namespace Settings {

std::vector<std::wstring> SelectedDisplays();
void SetSelectedDisplays(const std::vector<std::wstring>& ids);

bool FlipCursor();
void SetFlipCursor(bool on);

} // namespace Settings
} // namespace sf
