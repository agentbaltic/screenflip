// Application lifecycle, tray UI, selection, and reconcile (port of macOS
// AppDelegate.swift). A windowless tray agent (the LSUIElement analogue): no
// taskbar button, a hidden message window receives tray callbacks, display-change
// and power events. reconcile() keeps exactly one FlipController running per
// selected+present display and pushes the resulting (workspace -> overlay)
// targets into MirrorInput. The original display arrangement is snapshotted at
// launch and restored on quit.
#pragma once

#include <windows.h>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "Displays.h"
#include "FlipController.h"

namespace sf {

class App {
public:
    static App* Get();

    bool Init(HINSTANCE hInst);
    int  Run();      // standard message pump

private:
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);

    void SetupTray();
    void ShowMenu();
    void Reconcile();
    void ToggleDisplay(const DisplayId& id);
    void ToggleCursorFlip();
    void RestartAll();
    void Quit();
    void OnDisplayChange();
    void PublishTargets();

    HINSTANCE hInst_ = nullptr;
    HWND      msgWnd_ = nullptr;
    NOTIFYICONDATAW nid_{};

    std::set<DisplayId> selected_;
    std::map<DisplayId, std::unique_ptr<FlipController>> controllers_;
    std::shared_ptr<Displays::Arrangement> savedArrangement_;
    bool reconciling_ = false;

    // Menu command ids -> the display they toggle (rebuilt each time the menu opens).
    std::map<UINT, DisplayId> menuDisplayCmd_;
};

} // namespace sf
