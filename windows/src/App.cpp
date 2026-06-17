// Application lifecycle / tray / reconcile (port of macOS AppDelegate.swift).
// Link: user32.lib shell32.lib  (+ the libs pulled in by the modules it drives)
#include "App.h"
#include "Settings.h"
#include "MirrorInput.h"
#include "DXShared.h"
#include "Log.h"

#include <shellapi.h>
#include <vector>

namespace sf {

// Window messages / menu command ids.
static constexpr UINT WM_SF_TRAY      = WM_APP + 1;
static constexpr UINT WM_SF_RECONCILE = WM_APP + 2;   // posted from worker threads
static constexpr UINT CMD_DISPLAY_BASE = 1000;
static constexpr UINT CMD_FLIPCURSOR  = 2001;
static constexpr UINT CMD_RESTART     = 2002;
static constexpr UINT CMD_QUIT        = 2003;
static constexpr UINT TRAY_UID        = 1;
#define IDI_APPICON 101

static const wchar_t* kMsgClass = L"ScreenFlipMessageWindow";
static App* g_app = nullptr;

App* App::Get() { return g_app; }

LRESULT CALLBACK App::WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (App* a = App::Get()) return a->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

bool App::Init(HINSTANCE hInst) {
    g_app = this;
    hInst_ = hInst;

    Log::Reset();
    Log::Linef(L"=== ScreenFlip (Windows) launched, pid %lu ===", GetCurrentProcessId());

    if (!DXShared::Get().Init())
        Log::Line(L"DXShared init failed — capture/overlay will not work");

    // Hidden TOP-LEVEL window (not message-only) so it receives WM_DISPLAYCHANGE
    // and WM_POWERBROADCAST broadcasts, plus the tray callback.
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = &App::WndProcThunk;
    wc.hInstance = hInst;
    wc.lpszClassName = kMsgClass;
    RegisterClassExW(&wc);
    msgWnd_ = CreateWindowExW(0, kMsgClass, L"ScreenFlip", WS_OVERLAPPED,
                              0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!msgWnd_) { Log::Linef(L"App: message window failed (%lu)", GetLastError()); return false; }

    // Snapshot the arrangement so we can restore it on quit.
    savedArrangement_ = Displays::Snapshot();

    auto sel = Settings::SelectedDisplays();
    selected_ = std::set<DisplayId>(sel.begin(), sel.end());
    Log::Linef(L"App: restored %zu selected display(s)", selected_.size());
    for (auto& d : Displays::All())
        Log::Linef(L"  display id=%s \"%s\" %s", d.id.c_str(), d.name.c_str(), d.gdiName.c_str());

    MirrorInput::Shared().SetCursorFlipped(Settings::FlipCursor());

    SetupTray();
    Reconcile();
    return true;
}

void App::SetupTray() {
    nid_ = NOTIFYICONDATAW{};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = msgWnd_;
    nid_.uID = TRAY_UID;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_SF_TRAY;
    nid_.hIcon = (HICON)LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!nid_.hIcon) nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid_.szTip, L"ScreenFlip", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid_);
}

void App::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    menuDisplayCmd_.clear();

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Flip which displays:");
    UINT cmd = CMD_DISPLAY_BASE;
    bool anyDegraded = false;
    for (auto& d : Displays::All()) {
        UINT flags = MF_STRING | (selected_.count(d.id) ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(menu, flags, cmd, d.name.c_str());
        menuDisplayCmd_[cmd] = d.id;
        ++cmd;
    }
    for (auto& kv : controllers_) if (kv.second && kv.second->Degraded()) anyDegraded = true;

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Selected displays become flipped workspaces");
    AppendMenuW(menu, MF_STRING | (Settings::FlipCursor() ? MF_CHECKED : MF_UNCHECKED),
                CMD_FLIPCURSOR, L"Flip cursor to match mirror");
    if (anyDegraded)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    L"⚠ No virtual display — mirroring instead (cursor not corrected)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_RESTART, L"Restart all");
    AppendMenuW(menu, MF_STRING, CMD_QUIT,    L"Quit ScreenFlip");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(msgWnd_);                       // so the menu dismisses correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, msgWnd_, nullptr);
    PostMessageW(msgWnd_, WM_NULL, 0, 0);               // standard tray-menu workaround
    DestroyMenu(menu);
}

void App::ToggleDisplay(const DisplayId& id) {
    if (selected_.count(id)) selected_.erase(id); else selected_.insert(id);
    Settings::SetSelectedDisplays(std::vector<DisplayId>(selected_.begin(), selected_.end()));
    Reconcile();
}

void App::ToggleCursorFlip() {
    bool on = !Settings::FlipCursor();
    Settings::SetFlipCursor(on);
    MirrorInput::Shared().SetCursorFlipped(on);
}

void App::RestartAll() {
    for (auto& kv : controllers_) if (kv.second) kv.second->Stop();
    controllers_.clear();
    Reconcile();
}

void App::Reconcile() {
    if (reconciling_) return;                  // SetOrigins fires WM_DISPLAYCHANGE; avoid re-entry
    reconciling_ = true;

    auto active = Displays::All();
    std::map<DisplayId, DisplayInfo> activeById;
    for (auto& d : active) activeById[d.id] = d;

    // Stop controllers no longer wanted / whose display vanished / whose workspace died.
    std::vector<DisplayId> toErase;
    for (auto& kv : controllers_) {
        const DisplayId& id = kv.first;
        FlipController* fc = kv.second.get();
        if (!selected_.count(id) || activeById.find(id) == activeById.end() ||
            !fc || !fc->WorkspaceAlive()) {
            if (fc) fc->Stop();
            toErase.push_back(id);
        }
    }
    for (auto& id : toErase) controllers_.erase(id);

    // Virtual displays already claimed by survivors (so we never adopt one twice).
    std::set<DisplayId> inUseVirtual;
    for (auto& kv : controllers_)
        if (kv.second && !kv.second->Degraded()) {
            DisplayId v = kv.second->VirtualId();
            if (!v.empty()) inUseVirtual.insert(v);
        }

    // Start controllers for selected, present displays not yet running.
    for (auto& id : selected_) {
        if (controllers_.count(id)) continue;
        auto it = activeById.find(id);
        if (it == activeById.end()) continue;
        auto fc = FlipController::Make(hInst_, it->second);
        fc->onNeedsReconcile = [this] { PostMessageW(msgWnd_, WM_SF_RECONCILE, 0, 0); };
        if (fc->Start(inUseVirtual)) {
            DisplayId v = fc->VirtualId();
            if (!v.empty()) inUseVirtual.insert(v);
            controllers_[id] = std::move(fc);
        } else {
            fc->Stop();
        }
    }

    Log::Linef(L"App: reconcile -> %zu workspace(s) of %zu selected, %zu active",
               controllers_.size(), selected_.size(), active.size());

    PublishTargets();
    reconciling_ = false;
}

void App::PublishTargets() {
    std::vector<ProxyTarget> targets;
    for (auto& kv : controllers_) {
        FlipController* fc = kv.second.get();
        if (fc && fc->HasMapping())
            targets.push_back(ProxyTarget{ fc->Mapping(), fc->Overlay() });
    }
    MirrorInput::Shared().SetTargets(std::move(targets));
}

void App::OnDisplayChange() {
    if (reconciling_) return;
    Log::Line(L"App: display configuration changed");
    // Stop touching the cursor during the transition: a display-config change can
    // recycle HMONITORs, so the edge guard must not run against the about-to-be-
    // rebuilt mappings. Reconcile() republishes fresh targets at the end.
    MirrorInput::Shared().SetTargets({});
    for (auto& kv : controllers_) if (kv.second) kv.second->RepositionOverlay();
    Reconcile();
}

void App::Quit() {
    for (auto& kv : controllers_) if (kv.second) kv.second->Stop();
    controllers_.clear();
    MirrorInput::Shared().SetTargets({});
    Displays::Restore(savedArrangement_);
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    PostQuitMessage(0);
}

LRESULT App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SF_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_CONTEXTMENU)
            ShowMenu();
        return 0;
    case WM_SF_RECONCILE:
        Reconcile();
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(w);
        auto it = menuDisplayCmd_.find(id);
        if (it != menuDisplayCmd_.end()) { ToggleDisplay(it->second); return 0; }
        switch (id) {
        case CMD_FLIPCURSOR: ToggleCursorFlip(); return 0;
        case CMD_RESTART:    RestartAll();        return 0;
        case CMD_QUIT:       Quit();              return 0;
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        OnDisplayChange();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) OnDisplayChange();
        return TRUE;
    case WM_ENDSESSION:
        if (w) Displays::Restore(savedArrangement_);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int App::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Best-effort: ensure the arrangement is restored even on an unexpected exit.
    Displays::Restore(savedArrangement_);
    return (int)msg.wParam;
}

} // namespace sf
