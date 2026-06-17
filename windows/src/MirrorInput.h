// The cursor proxy + edge guard (port of macOS MirrorInput.swift).
//
// The real cursor lives NATIVELY on the headless workspace V (no input hook, no
// event interception anywhere — clicks/keys/drags are all native). Because V is
// headless its system cursor is invisible, so a ~90 Hz timer reads the cursor
// position and, when it is on a workspace, tells the matching overlay to draw a
// synthetic proxy on P at the horizontally-mirrored position. If the cursor ever
// lands on P (a display that only shows V's mirror), the edge guard pins it back
// onto V with SetCursorPos — using LIVE bounds looked up by monitor handle every
// tick, never a stale cached rect, so it can never trap the real cursor on a
// display the user actually uses.
#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "Geometry.h"

namespace sf {

class OverlayWindow;

struct ProxyTarget {
    WorkspaceMapping mapping;
    OverlayWindow*   overlay = nullptr;   // draws this mapping's proxy
};

class MirrorInput {
public:
    static MirrorInput& Shared();

    // Replace the active set of (workspace -> overlay) targets. Empty stops the
    // timer and hides all proxies (port of setMappings).
    void SetTargets(std::vector<ProxyTarget> targets);

    // Mirror the proxy cursor image to match the flipped content.
    void SetCursorFlipped(bool on);

private:
    MirrorInput() = default;
    void Start();
    void Stop();
    void Tick();

    std::mutex mx_;
    std::vector<ProxyTarget> targets_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> flipped_{false};
};

} // namespace sf
