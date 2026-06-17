// Entry point (port of macOS main.swift). Windowless tray agent: single instance,
// C++/WinRT apartment init for WGC, then the App message pump.
// Link: windowsapp.lib (WinRT)  user32.lib
#include <windows.h>
#include <winrt/base.h>

#include "App.h"
#include "Log.h"

using namespace sf;

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Single instance — a second launch just exits.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\io.vbar.screenflip.instance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }

    // WGC requires an initialized apartment. The UI/tray thread is single-threaded;
    // the capture frame pool runs free-threaded on its own.
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    int rc = 1;
    {
        App app;
        if (app.Init(hInst))
            rc = app.Run();
    }

    winrt::uninit_apartment();
    if (instanceMutex) { ReleaseMutex(instanceMutex); CloseHandle(instanceMutex); }
    return rc;
}
