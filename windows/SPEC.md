# ScreenFlip for Windows — Implementation Spec (single source of truth)

> Native C++ (Win32 + Direct3D 11), buildable from the command line with MSVC `cl.exe`
> via `build.bat`. This is a faithful port of the macOS "Model B" virtual-workspace
> architecture. Read this top to bottom before writing code. Every decision below is
> deliberate; where Windows forces a different shape than macOS, the difference and the
> reason are called out explicitly.

---

## 0. The one-paragraph summary

A physical monitor **P** is turned into a horizontally-mirrored, *usable* workspace. We
create a **headless virtual display V** (via a bundled IddCx indirect-display driver),
arrange it so the cursor can flow onto it from the main desktop but **cannot** roll onto
P, **capture V** with Windows.Graphics.Capture (WGC), and **render it flipped** full-screen
on P through a borderless, click-through, layered/topmost D3D11 swap-chain window. The real
windows and the real OS cursor live on V; because V is headless its hardware cursor is never
shown on glass, so a **90 Hz proxy cursor** is drawn on P at the mirrored position, with an
**edge guard** that pins the cursor back onto V if it ever lands on P (`SetCursorPos`). No
input hook, no event interception — clicks/keys/drags are all native, exactly as on macOS.

If no virtual-display driver is installed, we fall back to a **degraded "mirror a real
display" mode** (capture P or another monitor, render flipped on P) — honest about the fact
that without V the cursor on the mirrored panel is the real OS cursor and reads backwards.

---

## 1. End-to-end architecture and the macOS → Windows file map

The macOS app is 13 Swift files + a bridging header + Info.plist + build.sh. Windows keeps
the **same responsibility boundaries** so the two ports stay mentally aligned. The table is
the contract: each Windows file replaces exactly one macOS file's job.

| macOS file | Windows file | Role | Key APIs |
|---|---|---|---|
| `main.swift` | `src/main.cpp` | `wWinMain` entry: message loop, single-instance mutex, COM/WinRT init, builds `App`. | `wWinMain`, `CreateMutexW`, `RoInitialize`/`winrt::init_apartment`, `GetMessage`/`DispatchMessage` |
| `AppDelegate.swift` | `src/App.cpp` / `App.h` | Lifecycle + tray + selection + `reconcile()` + display-change handling + quit/restore. | `Shell_NotifyIconW`, `WM_DISPLAYCHANGE`, `RegisterPowerSettingNotification`, settings I/O |
| `FlipController.swift` | `src/FlipController.cpp/.h` | One workspace: owns V, arranges displays, owns capture+overlay, restart loop, `WorkspaceMapping`. | `SetDisplayConfig`/CCD, ties Capture+Overlay+VirtualDisplay together |
| `VirtualDisplay.swift` | `src/VirtualDisplay.cpp/.h` | Create/destroy headless V; talk to the IddCx driver; resolve V's monitor identity. | Driver IOCTL via `CreateFileW`/`DeviceIoControl`, or SWDevice install; `EnumDisplayDevices` |
| `Capture.swift` | `src/Capture.cpp/.h` | Capture V → hand a D3D11 texture per frame; auto-restart on loss. | `Windows.Graphics.Capture` (WGC), `IDirect3D11CaptureFramePool`, `GraphicsCaptureItem` |
| `OverlayWindow.swift` | `src/OverlayWindow.cpp/.h` | Borderless topmost click-through window on P; renders the captured texture **flipped**. | `CreateWindowExW` (`WS_EX_LAYERED\|WS_EX_TRANSPARENT\|WS_EX_TOPMOST\|WS_EX_NOACTIVATE`), `IDXGISwapChain1`, D3D11, HLSL |
| `CursorWindow.swift` | `src/CursorWindow.cpp/.h` | Synthetic proxy cursor sprite on P (optionally mirrored image). | Layered window **or** a quad in the overlay swap-chain (we use the latter — see §5) |
| `MirrorInput.swift` | `src/MirrorInput.cpp/.h` | 90 Hz timer: read cursor, draw proxy at mirrored point, edge-guard pin-back. | `GetCursorPos`/`SetCursorPos`, `timeSetEvent`/high-res timer, `WorkspaceMapping` math |
| `Displays.swift` | `src/Displays.cpp/.h` | Enumerate monitors, stable identity (UUID-equivalent), snapshot/set/restore arrangement. | `EnumDisplayMonitors`, `QueryDisplayConfig`/`SetDisplayConfig` (CCD), `EnumDisplayDevices`, EDID |
| `Log.swift` | `src/Log.cpp/.h` | File logger for a windowless tray app. | `CreateFileW`/`WriteFile` to `%TEMP%\screenflip.log`, `OutputDebugStringW` |
| `Bridging.h` | `driver/` (IddCx) + `src/d3d/*.hlsl` | "Private API surface" analogue: the IddCx driver is our `CGVirtualDisplay` equivalent. | WDK / IddCx headers |
| `Info.plist` | `src/app.manifest` + `app.rc` | DPI-awareness, requestedExecutionLevel, version, tray icon resource. | `<dpiAwareness>PerMonitorV2`, `<requestedExecutionLevel level="asInvoker">` |
| `build.sh` | `build.bat` | `cl.exe`/`link.exe`, `fxc.exe` for HLSL, `rc.exe`/`mt.exe` for manifest, optional `msbuild` for the driver. | MSVC CLT |

Architectural mapping of the five "Model B" steps:

1. **Headless virtual display V** — macOS `CGVirtualDisplay` ⇒ Windows **IddCx indirect
   display driver** (`VirtualDisplay.cpp`). This is the single biggest porting difference;
   see §2 in full.
2. **Arrangement (main | V; P off V's corner)** — macOS `CGConfigureDisplayOrigin` ⇒
   Windows **CCD API** `SetDisplayConfig` with `SDC_TOPOLOGY_EXTEND` + per-source position
   via `DISPLAYCONFIG_SOURCE_MODE.position` (`Displays.cpp`, `FlipController.cpp`).
3. **Capture V (no cursor)** — macOS ScreenCaptureKit ⇒ Windows **WGC** with
   `IsCursorCaptureEnabled = false` (`Capture.cpp`).
4. **Render flipped on P** — macOS CALayer `scaleX:-1` ⇒ D3D11 full-screen triangle with a
   pixel/vertex shader (or flipped UVs) drawing the captured texture (`OverlayWindow.cpp`).
5. **Proxy cursor + edge guard** — macOS 90 Hz `CGEvent.location` + `CGWarpMouseCursorPosition`
   ⇒ Windows 90 Hz `GetCursorPos`/`SetCursorPos` (`MirrorInput.cpp` + `CursorWindow.cpp`).

Toggle "Flip cursor to match mirror", per-display persistent selection, arrangement
snapshot/restore on quit — all preserved (§6).

---

## 2. THE virtual-display decision (read this carefully — it is the crux)

> **Implementation note (what actually shipped):** of the options explored below, the code
> implements the **adopt-an-existing-virtual-monitor** path (Tier 1-alt) plus the **degraded
> "mirror a real display"** fallback (Tier 2). `VirtualDisplay::Create()` scans for an
> `isVirtual` monitor and adopts it; the bundled `driver/` simply *auto-creates* one such
> monitor (no app↔driver IOCTL control channel — the IOCTL design sketched here was dropped
> as unnecessary). So "install a virtual-display driver (ours or a community one) and the app
> uses it" is the real flow.

### 2.1 Honest landscape

Windows has **no public, driver-free** way to create a headless virtual display that the
desktop treats as a real extended monitor with a real cursor on it. The macOS trick
(`CGVirtualDisplay`, private but loadable under ad-hoc signing, no kext) has **no Windows
equivalent that ships in-box**. What exists:

- **IddCx (Indirect Display Driver Class eXtension)** — the *correct, Microsoft-blessed*
  way to add a virtual monitor. It is a **UMDF user-mode driver** (`.dll` + `.inf` + a
  catalog). This is what every virtual-monitor product uses (e.g. the well-known
  open-source `MttVirtualDisplayDriver` / "IddSampleDriver" lineage, parsec-vdd, etc.).
  A virtual monitor created this way **is a real display**: the cursor lives on it, windows
  maximize to it, WGC/DXGI enumerate it. This is the faithful analogue of `CGVirtualDisplay`.
- **DXGI/WDDM "fake" displays** — not a thing for user apps. There is no `CreateVirtualMonitor`
  Win32 call.
- **"Detach" / clone tricks** — cannot synthesize a new surface to host real windows.

So the honest statement for the README: **a true ScreenFlip workspace on Windows requires an
IddCx driver.** macOS gets away without an installed driver; Windows does not.

### 2.2 The cost of a driver, stated plainly

- A driver `.inf` package must be **installed** (Device Manager / `pnputil /add-driver
  /install`, or programmatically via `SwDeviceCreate`). Install needs **admin** once.
- Driver binaries that Windows will load without test-signing mode **must be signed** (ideally
  by an EV cert through the Windows Hardware Dev Center / attestation signing). For a
  **personal project** that is unrealistic, so the practical path is: build the driver, then
  either (a) enable **test signing** (`bcdedit /set testsigning on`, reboot, shows a desktop
  watermark) and self-sign the driver, or (b) install a **pre-built, already-signed**
  community IddCx driver and have our app *drive* it rather than ship our own. Both are
  legitimate; both are caveats the user must accept.

### 2.3 The recommended pragmatic path for THIS project

**Tier 1 (primary, full fidelity): bundle a minimal IddCx driver in `windows/driver/`** and
install it on first run.

- The driver exposes a single virtual monitor whose mode list we control. Our app sets the
  monitor's resolution to match P (e.g. 1920×1080@60) by selecting/forcing a mode the driver
  advertises. Communication app↔driver is via a tiny private IOCTL channel (the driver
  creates a control device; the app `CreateFileW`s it and `DeviceIoControl`s "add monitor at
  WxH" / "remove monitor"). This mirrors `VirtualDisplay.swift`'s create/apply/destroy.
- This requires the **WDK** to build the driver and one of the signing options in §2.2.
- This is the only mode where the macOS architecture is reproduced *exactly* (real cursor on
  a headless surface, perfect mirror, no backwards cursor on glass).

**Tier 1-alt (recommended default for a personal machine):** **don't author a driver at all —
depend on an already-installed, signed community IddCx driver** and detect its virtual
monitor at runtime. Author the IOCTL/registry contract against one specific driver
(document which), or, simplest, let the *user* add a virtual display via that driver's own
tool and have ScreenFlip just *pick it from the tray list like any other monitor*. This
removes all driver-signing pain from our shipping artifact. **Recommend this as the default
for the user's own use**, and keep the bundled driver (Tier 1) as the "real product" path.

**Tier 2 (graceful degraded mode, ALWAYS available, no driver): "Mirror an existing display."**

If no virtual display is present, ScreenFlip still does something useful: it captures a
*chosen real* source display S (could be the main desktop, or a second monitor) with WGC and
renders it **flipped** full-screen on P. This is exactly the macOS "self-capture a physical
display" approach the mac app *rejected*, and the same caveats apply:

- If S ≠ P, there is **no feedback loop** and it works cleanly; the user gets a flipped live
  view of S on the beam-splitter glass (great for teleprompter use where you just need to see
  flipped content). But the cursor on P is **not corrected** — moving the mouse over S moves
  the real cursor on S, and the proxy can still be drawn on P, but the user *works on S*, not
  on the glass. Clicks land on S correctly because S is a real display.
- If S = P (mirror P onto itself), you get a **feedback loop / occlusion freeze** unless the
  overlay is excluded from capture. WGC can exclude our overlay window
  (`CreateForWindow` of S won't capture an excluded HWND only if we capture the *monitor* and
  exclude via `IGraphicsCaptureItemInterop`+dirty-rect — in practice **avoid S = P**). So in
  degraded mode we **forbid S = P** and require the user to pick a different source.

Degraded mode is selected automatically when V can't be created, and the tray shows a clear
"⚠ No virtual display — mirroring <source> instead (cursor not corrected)" note.

**Decision:** ship the degraded mode unconditionally; ship the bundled IddCx driver as the
"full" path but make the *default runtime behavior* "use any virtual monitor that exists,
else degrade." Document loudly. This is the pragmatic, honest answer.

---

## 3. `windows/src/` file list (concrete) with responsibilities and APIs

```
windows/
  SPEC.md                      ← this document
  README.md                    ← user-facing, honesty caveats (§8)
  build.bat                    ← MSVC CLT build (§7)
  app.rc                       ← version + tray icon + manifest reference
  src/
    main.cpp                   ← wWinMain, single-instance, WinRT init, message pump
    App.h / App.cpp            ← tray icon, menu, selection set, reconcile(), display/power events, quit+restore
    FlipController.h / .cpp    ← one workspace; owns V+Capture+Overlay; arranges displays; restart loop; builds WorkspaceMapping
    VirtualDisplay.h / .cpp    ← create/destroy V via IddCx driver IOCTL (Tier 1) OR adopt an existing virtual monitor; resolve its monitor handle
    Capture.h / .cpp           ← WGC capture of V → ID3D11Texture2D per frame on a worker; auto-restart
    OverlayWindow.h / .cpp     ← borderless topmost click-through window on P; D3D11 swapchain; draws captured texture FLIPPED + proxy cursor quad
    CursorWindow.h / .cpp       ← proxy-cursor sprite source: loads current system cursor, builds (optionally mirrored) texture + hotspot
    MirrorInput.h / .cpp       ← 90 Hz loop: GetCursorPos → workspace? draw proxy : edge-guard SetCursorPos; flip toggle
    Displays.h / .cpp          ← monitor enumeration, stable identity, CCD snapshot/set/restore of positions
    Settings.h / .cpp          ← persisted selection + flipCursor toggle (HKCU registry or %APPDATA% JSON)  [absorbs UserDefaults]
    Log.h / .cpp               ← %TEMP%\screenflip.log + OutputDebugString
    DXShared.h / .cpp          ← shared ID3D11Device/context, DXGI factory, the WinRT IDirect3DDevice for WGC interop
    d3d/
      flip_vs.hlsl             ← full-screen triangle, emits UVs (flip handled here or in PS)
      flip_ps.hlsl             ← samples captured texture; horizontal mirror via uv.x = 1-uv.x
      cursor_vs.hlsl / cursor_ps.hlsl  ← textured quad for the proxy cursor (if drawn in-swapchain)
    app.manifest               ← PerMonitorV2 DPI awareness, asInvoker
  driver/                      ← IddCx UMDF driver (Tier 1 only)
    ScreenFlipIdd.inf
    Driver.cpp / Driver.h      ← IddCx callbacks: adapter init, monitor arrival, swapchain processing (we discard frames)
    Ioctl.cpp                  ← control device: "add/remove monitor at WxH@Hz"
    ScreenFlipIdd.vcxproj      ← built with msbuild + WDK (NOT cl.exe alone)
```

Per-file API detail (the load-bearing parts):

- **`main.cpp`**: `CreateMutexW(L"Local\\io.vbar.screenflip")` + `GetLastError()==ERROR_ALREADY_EXISTS` to enforce single instance; `winrt::init_apartment(winrt::apartment_type::single_threaded)`; standard `GetMessage`/`TranslateMessage`/`DispatchMessage` pump. The tray needs a real (hidden) message-only-ish HWND to receive `WM_APP` callbacks and `WM_DISPLAYCHANGE`.
- **`App.cpp`**: tray via `Shell_NotifyIconW(NIM_ADD/MODIFY/DELETE)` with `uCallbackMessage=WM_APP+1`; right-click builds a `CreatePopupMenu` with a checkable item per monitor (`MF_CHECKED`), a "Flip cursor to match mirror" check item, "Restart all", "Quit". Handles `WM_DISPLAYCHANGE` and a `RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE)`/`WM_POWERBROADCAST` to re-`reconcile()` after sleep/wake (the analogue of macOS `didChangeScreenParameters`). `reconcile()` is a direct port of `AppDelegate.reconcile()`: start a `FlipController` for each selected+present display, stop the rest, push the set of `WorkspaceMapping`s into `MirrorInput`. On quit: stop all controllers, `MirrorInput::SetMappings({})`, `Displays::RestoreArrangement(saved)`, remove tray icon.
- **`FlipController.cpp`**: on `Start()` — create V sized to P (`VirtualDisplay`), arrange `main | V` with **P off V's bottom-right corner** via `Displays::SetArrangement`, build `WorkspaceMapping{pRect,pMonitorId,vOrigin,vSize,vSourceId}`, create+show the `OverlayWindow` on P, start `Capture` of V, wire `Capture.onFrame → OverlayWindow.Present`. `onError → ScheduleCaptureRestart()` (2 s, identical policy to mac). `workspaceAlive` flips false if V dies → triggers `App::reconcile()`.
- **`VirtualDisplay.cpp`**: Tier 1 — `CreateFileW(L"\\\\.\\ScreenFlipIdd")`, `DeviceIoControl(ADD_MONITOR, {w,h,hz})`, then poll `EnumDisplayDevices`/`QueryDisplayConfig` until the new source appears and resolve its `DISPLAYCONFIG` source id + the `HMONITOR`/GDI device name. `Destroy()` → `DeviceIoControl(REMOVE_MONITOR)`. Tier 1-alt — scan for an existing monitor whose EDID vendor/product matches a known virtual driver and adopt it. Exposes `SourceId`, `MonitorRect`, `Alive`.
- **`Capture.cpp`**: build a `GraphicsCaptureItem` for V's `HMONITOR` via `IGraphicsCaptureItemInterop::CreateForMonitor`; `Direct3D11CaptureFramePool::CreateFreeThreaded` (worker-thread frames, like the mac frame queue); `session.IsCursorCaptureEnabled(false)`; (Win11) `session.IsBorderRequired(false)` to suppress the yellow capture border; on `FrameArrived`, copy/share the `ID3D11Texture2D` and invoke `onFrame`. Auto-restart on `Closed` or device-lost. Worker thread mirrors mac's `frameQueue`; final present must hop to the overlay's render thread.
- **`OverlayWindow.cpp`**: `CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW, WS_POPUP)` covering P's full pixel rect; `SetWindowPos(HWND_TOPMOST,...,SWP_NOACTIVATE)`. **Important:** a `WS_EX_LAYERED` window with a DXGI flip-model swapchain needs care — prefer a **`DCompositionDevice` + composition visual** hosting an `IDXGISwapChain1` created with `CreateSwapChainForComposition`, which composes correctly with a click-through layered window and remains topmost. Render: clear black, draw full-screen triangle sampling the captured texture with `uv.x = 1 - uv.x` (horizontal mirror), then draw the proxy-cursor quad on top. `WGCExcludeFromCapture`: call `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` so this overlay never appears in any capture (the analogue of mac's overlay exclusion).
- **`CursorWindow.cpp`**: not a window in the chosen design — it's the *proxy cursor sprite source*. `GetCursorInfo`/`CopyIcon` of the live `hCursor`, `GetIconInfo` for the hotspot and the 32×32 (or DPI-scaled) bitmap, upload to a `ID3D11Texture2D`; if "flip" is on, sample mirrored and adjust hotspot to `width - hotspot.x`. (Kept as a separate file to mirror `CursorWindow.swift`'s boundary; if a layered HWND proves simpler than an in-swapchain quad, this file can instead own a `WS_EX_LAYERED` `UpdateLayeredWindow` sprite window — see §5 risk note.)
- **`MirrorInput.cpp`**: a high-resolution periodic loop at ~90 Hz (`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on a dedicated thread, or `timeBeginPeriod(1)`+`timeSetEvent`). Each tick: `GetCursorPos(&pt)` (virtual-screen px). For each mapping, if `pt ∈ vRect` → compute mirrored point on P, tell the overlay to move/show the proxy, done. Else edge-guard: if `pt ∈ live pRect` (looked up fresh from `Displays`, never cached — same safety rule as mac) and vRect non-empty, `SetCursorPos(clampedToV)` and draw proxy at the pinned point. Else hide proxy. `SetCursorFlipped(bool)` forwards to the sprite source.
- **`Displays.cpp`**: `EnumDisplayMonitors`+`GetMonitorInfoW` for rects; **stable identity** from CCD `DISPLAYCONFIG_TARGET_DEVICE_NAME` (`monitorDevicePath`, the container-ID/EDID-based path) — that is the Windows analogue of macOS's display UUID (see §6.2). Arrangement snapshot/set/restore via `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` → edit `DISPLAYCONFIG_SOURCE_MODE.position` → `SetDisplayConfig(... SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_SAVE_TO_DATABASE?` — use **`SDC_NO_OPTIMIZATION`** off, and **do not** save to database so quit-time restore is clean). Restore replays the saved per-source positions.
- **`Settings.cpp`**: `HKCU\Software\io.vbar.screenflip` values `flippedDisplays` (REG_MULTI_SZ of device paths) and `flipCursor` (DWORD). Direct analogue of `UserDefaults`.
- **`Log.cpp`**: append to `%TEMP%\screenflip.log` + `OutputDebugStringW`. Same role as `Log.swift`.

---

## 4. Capture API: **Windows.Graphics.Capture (WGC)** — decided

Choice: **WGC**, not Desktop Duplication API (DDA). Reasons, decisive:

1. **Per-monitor capture of an arbitrary display** is first-class in WGC
   (`CreateForMonitor(HMONITOR)`). DDA also does per-output, but WGC is the modern path
   Microsoft actively develops.
2. **Cursor exclusion is a one-liner**: `GraphicsCaptureSession.IsCursorCaptureEnabled = false`.
   This is exactly mac's `cfg.showsCursor = false`. With DDA you get the cursor as a separate
   shape you must *not* composite — doable but more code. WGC wins.
3. **GPU textures, zero-copy-ish**: frames arrive as `Direct3D11CaptureFrame.Surface` →
   `ID3D11Texture2D`, stay on the GPU, and feed straight into the overlay's shader — the
   analogue of mac handing us an `IOSurface`/Metal texture. No CPU readback.
4. **Capture-exclusion of our own overlay**: `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
   keeps the overlay out of any capture, preventing feedback. (Win10 2004+.)
5. **Border suppression** on Win11 (`IsBorderRequired=false`, requires the right capability/OS
   build) removes the yellow recording outline; on Win10 the border may show on captured
   *windows* but **monitor capture of a headless virtual display the user never looks at**
   makes this irrelevant — we look at P, not V.
6. **Resilience**: WGC raises `FrameArrived`/`Closed`; device-lost is observable. We treat loss
   as transient and restart in 2 s, mirroring mac's replayd-death handling.

DDA's only edges (slightly lower latency, no "recording" semantics) don't outweigh WGC's
cursor-exclusion and modern surface model here. **Minimum OS: WGC requires Windows 10 1903+;
`WDA_EXCLUDEFROMCAPTURE` needs 2004+; borderless capture needs Win11.** Target **Windows 11**
for the clean experience, support Win10 2004+ with the (irrelevant-here) border caveat.

---

## 5. Cursor-proxy + edge-guard plan

Same principle as macOS: the real cursor lives on headless V (invisible there), and we paint
a synthetic one on P at the mirrored location, guarding P's territory.

**Coordinates.** Windows uses a single top-left-origin **virtual-screen** pixel space for all
monitors (`GetCursorPos`, monitor rects, CCD positions all agree) — *simpler than macOS*,
which mixes CG top-left and AppKit bottom-left. So the mac `CursorWindow.moveHotspot` y-flip
gymnastics **disappear**; we work entirely in virtual-screen pixels.

**Mirror math** (port of `MirrorInput.drawProxy`), all in px:
```
localX = cur.x - v.origin.x
localY = cur.y - v.origin.y
proxyX = p.rect.left + (v.size.w - localX)   // horizontal mirror across P
proxyY = p.rect.top  + localY
```
Draw the proxy sprite with its hotspot at (proxyX, proxyY). If "Flip cursor to match mirror"
is on, the sprite image is mirrored and its hotspot becomes `spriteW - hotspot.x`.

**Where the proxy is drawn — decision:** draw it as a **textured quad inside the overlay's own
D3D11 swapchain**, composited above the flipped video each frame. Rationale: it's always above
the flipped picture, always on P, never flickers against a separate layered window, and never
itself gets captured. (Alternative: a separate `WS_EX_LAYERED|WS_EX_TRANSPARENT` topmost
`UpdateLayeredWindow` sprite window — kept as the `CursorWindow.cpp` fallback if compositing
the quad with the right alpha proves fiddly. Either way `SetWindowDisplayAffinity` /
in-swapchain keeps it out of capture.)

**Proxy image refresh:** poll `GetCursorInfo` each tick (cheap); when `hCursor` changes
(I-beam, resize arrows, hand), rebuild the sprite texture from the new cursor via `GetIconInfo`
+ `DrawIconEx` into a DIB → upload. This makes the proxy track cursor *shape* like the mac app
does by reading `NSCursor`.

**Edge guard** (port of `MirrorInput.tick` else-branch): if the cursor is not on any V but is
inside a live `pRect` (looked up *fresh* every tick from `Displays`, never the cached rect —
the exact mac safety rule against stale mappings trapping the cursor on a real display), clamp
it to the nearest point inside V and `SetCursorPos(clamped)`. Windows has no post-warp
move-suppression, so there is **no `CGAssociateMouseAndMouseCursorPosition` analogue needed** —
one `SetCursorPos` is enough. Then draw the proxy at the pinned point. The corner-adjacent
arrangement (no shared edge between V and P) is the primary defense; the guard is the backstop,
identical to mac.

**No input hook anywhere.** Hard requirement preserved: we never install a `WH_MOUSE_LL` /
`SetWindowsHookEx` / raw-input sink that *alters* input. `SetCursorPos` only moves the pointer;
it does not intercept clicks. Clicks, keys, drags are 100% native on V.

---

## 6. Display arrangement, tray, persistence, stable identity

### 6.1 Arrangement (the cursor-containment geometry)

Use the **CCD API** (`QueryDisplayConfig`/`SetDisplayConfig`) — the modern, position-accurate
display-config API; the legacy `ChangeDisplaySettingsEx(... DM_POSITION)` per-device is the
fallback. Reproduce the mac layout exactly:

- `vOrigin = (main.right, main.top)` → V sits immediately right of main, **sharing main's
  right edge** so the cursor flows main→V natively.
- `pOrigin = (main.right + V.w, main.top + V.h)` → P hangs off V's **bottom-right corner**,
  corner-adjacent only. Windows requires monitors to form a connected arrangement with no
  gaps/overlaps along touching edges; **corner-only contact is accepted** (this is the same
  legality the mac app relies on) and crucially leaves **no shared edge** between V and P, so
  the cursor cannot roll from V onto P. Validate the applied config with `SetDisplayConfig(...
  SDC_VALIDATE ...)` first; if Windows rejects corner-only (it can normalize positions),
  fall back to placing P far below-right with the **edge guard doing the real work** — and log
  it. **This is a known risk; see §9.**

Snapshot all source positions at launch (`Displays::SnapshotArrangement`), restore on quit
(`Displays::RestoreArrangement`) — port of `snapshotOrigins`/`restoreOrigins`.

### 6.2 Stable display identity (the UUID analogue)

macOS uses `CGDisplayCreateUUIDFromDisplayID`. Windows has nothing that pretty, but the
**stable, reboot-survival identifier is the CCD `DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath`**
(a device-interface path containing the monitor's container/EDID identity, e.g.
`\\?\DISPLAY#XXX#...#{e6f07b5f-...}`). Use that string as the persistence key (REG_MULTI_SZ in
`Settings`). It is stable across reboot and re-enumeration far better than the `HMONITOR`
(a runtime handle) or the GDI `\\.\DISPLAY1` name (positional, not stable). For the virtual
display, identity comes from the driver's fixed EDID (vendor `0x5346`/product `0x5350` — reuse
the mac magic numbers for symmetry).

### 6.3 Tray + menu (port of `setupStatusItem`/`rebuildMenu`)

`Shell_NotifyIconW` tray icon (the "⇋" concept → a small mono icon resource in `app.rc`).
Right-click → `TrackPopupMenu` of a `CreatePopupMenu`:
- Header "Flip which displays:" (grayed).
- One checkable item per *real* monitor (checked if its device-path is in the selection set).
- Separator + note.
- Check item "Flip cursor to match mirror".
- If WGC/permission/driver missing: a warning item (e.g. "⚠ No virtual display — degraded
  mode" or "⚠ Install virtual display driver…").
- "Restart all", "Quit ScreenFlip".

Selection toggles persist immediately to `Settings` and call `reconcile()` — identical control
flow to `AppDelegate.toggleDisplay`.

### 6.4 Persistence

`Settings` ⇄ `HKCU\Software\io.vbar.screenflip`: `flippedDisplays` (REG_MULTI_SZ device paths),
`flipCursor` (REG_DWORD). Loaded at launch, written on every toggle. Exact `UserDefaults` role.

---

## 7. `build.bat` outline (MSVC command-line tools)

**Prerequisites (state these in README too):**
- **Visual Studio Build Tools** (or VS) with the **"Desktop development with C++"** workload →
  gives `cl.exe`, `link.exe`, `rc.exe`, `mt.exe`.
- **Windows 10/11 SDK** (gives `fxc.exe`, the headers `windows.graphics.capture.interop.h`,
  `dcomp.h`, `d3d11.h`, and C++/WinRT projection headers / `cppwinrt.exe`).
- For the **Tier 1 driver only**: the **WDK** matching the SDK, and **EWDK or `msbuild`** to
  build `driver/ScreenFlipIdd.vcxproj` (the driver cannot be built by `cl.exe` alone — it needs
  the WDK targets). Plus a signing story (test-signing self-cert, or Hardware Dev Center).
- Run from a **"x64 Native Tools Command Prompt for VS"** so `cl`/`link`/`rc`/`fxc` are on PATH
  and env vars (`INCLUDE`, `LIB`) are set. `build.bat` should `call vcvars64.bat` if not already.

**`build.bat` (app) — outline:**
```bat
@echo off
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="" call "%VCINSTALLDIR%Auxiliary\Build\vcvars64.bat"

set OUT=build
if not exist %OUT% mkdir %OUT%

rem ---- compile HLSL to headers (or .cso) ----
fxc /T vs_5_0 /E main /Fh %OUT%\flip_vs.h   /Vn g_flip_vs   src\d3d\flip_vs.hlsl
fxc /T ps_5_0 /E main /Fh %OUT%\flip_ps.h   /Vn g_flip_ps   src\d3d\flip_ps.hlsl
fxc /T vs_5_0 /E main /Fh %OUT%\cursor_vs.h /Vn g_cursor_vs src\d3d\cursor_vs.hlsl
fxc /T ps_5_0 /E main /Fh %OUT%\cursor_ps.h /Vn g_cursor_ps src\d3d\cursor_ps.hlsl

rem ---- C++/WinRT projection headers (if not vendored): cppwinrt.exe -in sdk -out build\winrt ----

rem ---- compile resources (icon + version + manifest reference) ----
rc /nologo /fo %OUT%\app.res app.rc

rem ---- compile + link the app ----
cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /MT ^
   /I %OUT% /I src /await ^
   src\*.cpp ^
   %OUT%\app.res ^
   /link /SUBSYSTEM:WINDOWS /OUT:%OUT%\ScreenFlip.exe ^
   user32.lib gdi32.lib shell32.lib d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib ^
   windowsapp.lib winmm.lib advapi32.lib ole32.lib runtimeobject.lib

rem ---- embed the manifest (DPI awareness etc.) ----
mt -nologo -manifest src\app.manifest -outputresource:%OUT%\ScreenFlip.exe;1

echo Built %OUT%\ScreenFlip.exe
endlocal
```

Key flag/lib notes:
- `/std:c++17 /await` (or `/std:c++20`) for C++/WinRT coroutines used by WGC.
- `/MT` static CRT so the exe is self-contained (analogue of the mac single-binary app).
- `windowsapp.lib` + `runtimeobject.lib` → WinRT (WGC). `d3d11.lib dxgi.lib dcomp.lib`
  `d3dcompiler.lib` → rendering. `winmm.lib` → `timeBeginPeriod`/`timeSetEvent`. `advapi32.lib`
  → registry. `shell32.lib` → tray. `/SUBSYSTEM:WINDOWS` → no console (windowless tray app,
  the `LSUIElement` analogue).
- HLSL compiled **ahead of time** with `fxc` to byte-header arrays (no runtime `D3DCompile`),
  the analogue of mac doing no shader at all but here we need the flip in a PS.
- **Manifest is required**, not optional: `PerMonitorV2` DPI awareness so capture/overlay px
  math is 1:1 on mixed-DPI setups; `asInvoker` so the *app* never needs admin (only the
  one-time *driver install* does, done by a separate elevated step).

**`driver/` build** (separate, optional): `msbuild driver\ScreenFlipIdd.vcxproj /p:Configuration=Release /p:Platform=x64`
under the EWDK/WDK; then `inf2cat` + `signtool` (test cert) + `pnputil /add-driver
ScreenFlipIdd.inf /install`. Keep this **out of the default `build.bat`** and behind a
`build.bat driver` argument, because most users will run Tier 1-alt / Tier 2.

---

## 8. HONESTY CAVEATS for `windows/README.md` (must appear verbatim-ish)

1. **A virtual display needs a driver.** Unlike the macOS build (which uses a private but
   driver-free `CGVirtualDisplay`), the full ScreenFlip experience on Windows requires an
   **IddCx indirect-display driver**. Installing a driver needs **admin once**, and Windows
   will only load it if it is **signed** — for a personal build that means either
   **test-signing mode** (`bcdedit /set testsigning on` + reboot, shows a desktop watermark)
   or relying on an **already-signed community virtual-display driver**.
2. **Bundled driver is unsigned / test-only.** If you build and install the driver in
   `windows/driver/`, it is **not WHQL/EV-signed**. Do not expect it to load on a Secure-Boot,
   non-test-signing machine. This is a hobby-grade artifact.
3. **Degraded mode without a driver.** If no virtual display is present, ScreenFlip falls back
   to **mirroring an existing display** flipped onto your target monitor. In this mode the
   **cursor on the mirrored panel is the real OS cursor and reads backwards** there; you work
   on the *source* display, not on the glass. It's useful as a flipped *viewer*, not a flipped
   *workspace*.
4. **Untested on this machine.** This Windows port was **architected, not yet run end-to-end on
   the author's hardware**. The macOS app is the proven one. Treat the first Windows run as
   bring-up: watch `%TEMP%\screenflip.log`.
5. **OS version floor.** Requires **Windows 10 version 2004 (build 19041)+** for
   `WDA_EXCLUDEFROMCAPTURE`; **Windows 11** recommended for borderless WGC capture
   (`IsBorderRequired=false`) and the cleanest result. WGC itself needs 1903+. Older Windows is
   unsupported.
6. **Corner-adjacency may be normalized by Windows.** Windows can snap monitor positions when
   it dislikes a corner-only arrangement; if so, the cursor-containment relies on the **90 Hz
   edge guard** alone, which is robust but can show a single-frame cursor blip at the boundary.
7. **App needs no admin; only driver install does.** The `ScreenFlip.exe` runs `asInvoker`.
   Screen capture (WGC) on Windows needs **no special permission grant** (unlike macOS Screen
   Recording) — there is no TCC analogue, so that whole permission dance is gone.
8. **GPU / device-lost.** On TDR/driver reset, capture and the swapchain are rebuilt; a brief
   black flash on the glass is expected.
9. **What may not work / not implemented:** multi-virtual-display at once (driver advertises one
   monitor in the minimal build); HDR/wide-gamut passthrough; per-monitor refresh > 60 Hz on V;
   exotic mixed-DPI scaling edge cases in the proxy hotspot.

---

## 9. Risks where the implementation could be subtly wrong (read before coding)

1. **Layered window + D3D swapchain compositing.** A naive `WS_EX_LAYERED` window with a
   flip-model `IDXGISwapChain` does **not** present correctly (layered windows expect GDI/
   `UpdateLayeredWindow` or DirectComposition). **Use DirectComposition**
   (`DCompositionCreateDevice` → visual → `CreateSwapChainForComposition`) for the overlay, or
   a regular topmost `WS_POPUP` (non-layered, fully opaque covering P) + `WS_EX_TRANSPARENT`
   for click-through. Do **not** assume `WS_EX_LAYERED` "just works" with DXGI. This is the
   single most likely thing to get wrong first.
2. **Click-through must be real.** `WS_EX_TRANSPARENT` makes hit-testing fall through, but only
   if the window is **also not capturing input via its WndProc** and `WS_EX_NOACTIVATE` so it
   never steals focus. Verify a click on the glass region lands on the window *behind* on V.
   The overlay is on P, not V, so in the correct corner arrangement there's nothing behind it
   on P anyway — but get the flags right so it never eats clicks during arrangement transitions.
3. **`WDA_EXCLUDEFROMCAPTURE` vs the proxy quad.** If the proxy cursor is a *separate* layered
   window, it too must be excluded from capture, or capturing V could see it (it won't, since
   it's on P, but be careful if degraded mode captures P). Drawing the proxy *inside* the
   overlay swapchain (the chosen design) sidesteps this entirely.
4. **CCD position units & validation.** `DISPLAYCONFIG_SOURCE_MODE.position` is in *pixels of
   that source's mode*, and `SetDisplayConfig` may **reject or normalize** the corner layout.
   Always `SDC_VALIDATE` first; never assume the position you asked for is the position you got
   — re-read with `QueryDisplayConfig` and rebuild `WorkspaceMapping` from the **actual** rects
   (the mac app does the same: it reads `CGDisplayBounds` back, never trusts the requested
   origin). Using a stale/requested rect is exactly what would let the edge guard fight the
   cursor on a real display.
5. **Live rects in the edge guard.** Port the mac safety rule literally: the guard must test
   `GetMonitorInfo`/CCD **live** bounds by stable id each tick, and **bail if V's rect is empty**
   (workspace gone), never pin against a cached/degenerate rect. Getting this wrong can *trap
   the user's cursor on a real monitor* — the worst possible bug. Treat this code path as
   safety-critical.
6. **DPI.** With PerMonitorV2, `GetCursorPos` and monitor rects are in **physical pixels** only
   if the process is fully DPI-aware; otherwise they're virtualized and the mirror math drifts
   on scaled monitors. The manifest MUST declare PerMonitorV2, and the overlay/capture sizes
   must use physical pixels (`GetDpiForMonitor` where needed). A mismatch shows as a cursor that
   doesn't line up with the content under it.
7. **WGC frame thread vs render thread.** WGC `FrameArrived` fires on a pool thread; the
   `ID3D11Texture2D` and the swapchain `Present` must be marshaled correctly (shared texture or
   a single device with a context lock). Don't `Present` from two threads. Mirror the mac model:
   capture delivers a surface, the UI/render thread presents it.
8. **Virtual display mode matching.** The IddCx driver must *advertise* the exact mode we want
   (P's resolution@60) or `SetDisplayConfig` can't select it; if the driver only offers a fixed
   list, pick the closest and letterbox in the shader. Don't assume arbitrary resolutions work.
9. **Cursor shape capture cost.** Rebuilding the proxy texture every tick is wasteful; only
   rebuild when `GetCursorInfo().hCursor` changes. Otherwise 90 Hz icon decoding burns CPU.
10. **Single-instance + tray cleanup.** If the process dies without `Shell_NotifyIcon(NIM_DELETE)`
    a ghost tray icon lingers; and if it dies without `RestoreArrangement`, the user is left with
    a weird monitor layout. Install a `SetConsoleCtrlHandler`/`WM_CLOSE`/`atexit` and best-effort
    restore on `WM_ENDSESSION` too. The mac app restores in both `quit()` and
    `applicationWillTerminate`; do the equivalent in both the menu-quit and the WM_DESTROY paths.

---

## 10. Build order for the implementer (suggested)

1. `Log`, `Settings`, `Displays` (enumerate + stable identity + arrange/restore) — get monitor
   data and persistence solid first; verify identity survives reboot.
2. `DXShared` + `OverlayWindow` rendering a **static test texture flipped**, full-screen,
   click-through, topmost, excluded-from-capture, via DirectComposition. Prove the hardest
   rendering risk (§9.1) before anything else.
3. `Capture` (WGC of an *existing* monitor) → feed `OverlayWindow`. Now you have flipped live
   mirror = **degraded mode** working with zero driver. Ship-able checkpoint.
4. `MirrorInput` + proxy cursor quad + edge guard, tested against the degraded source.
5. `VirtualDisplay` Tier 1-alt (adopt an existing community virtual monitor) → wire into
   `FlipController` → full Model-B behavior. Then optionally the bundled `driver/`.
6. `App` (tray, reconcile, power/display events, quit-restore) tying it together — the
   `AppDelegate` port — last.

This sequence front-loads the two genuine unknowns (overlay compositing, virtual display) and
gives a working, useful build (degraded mirror) early.
