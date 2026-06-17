# ScreenFlip — Windows

Turn any Windows monitor into a **horizontally‑mirrored workspace you can actually
work on** — with a cursor that still moves the right way and clicks that land where
you expect. This is the Windows port of the macOS [ScreenFlip](../macos); it keeps
the same "Model B" design (a hidden virtual workspace shown flipped on your real
monitor, with a synthetic correct cursor).

> This is the **Windows** edition. For macOS see [`../macos`](../macos). Project
> overview: [`../README.md`](../README.md). The full design write‑up is
> [`SPEC.md`](SPEC.md).

## Why this exists

Windows can rotate a display (90/180/270°) but has **no built‑in left‑to‑right
mirror**, and a 180° rotation is not a horizontal flip. Tools that just capture and
redraw a screen flipped leave you with a **backwards cursor**. ScreenFlip solves the
cursor: it puts your real windows and the real pointer on a hidden workspace and
shows that workspace flipped on your chosen monitor, drawing a matching cursor at the
mirrored spot. Useful for:

- **Teleprompters / beam‑splitter glass** — text reads correctly in the reflection.
- **Filming yourself** at a screen the camera mirrors.
- **Mirror / practice setups** you still need to operate.

## How it works (short version)

1. Creates (or adopts) a **headless virtual display** — your windows live there.
2. **Captures** it with Windows.Graphics.Capture and draws it **horizontally
   flipped, full‑screen** on your chosen monitor via a Direct3D 11 / DirectComposition
   overlay (hidden from capture so it never feeds back).
3. Lets the cursor live **natively** on the workspace and draws a matching proxy
   cursor on the flipped monitor; a 90 Hz **edge guard** keeps the pointer on the
   workspace. **No input hooks** — clicks, drags and typing are all native.

Your display arrangement is saved on launch and restored on quit.

## ⚠️ Read this before expecting magic

1. **A real workspace needs a virtual‑display driver.** Unlike macOS (whose private
   `CGVirtualDisplay` needs no driver), Windows has **no driver‑free** way to create
   a virtual monitor. For the full experience you need an **IddCx** driver — either
   an **already‑signed community virtual‑display driver** (easiest; ScreenFlip adopts
   it automatically) or the bundled one in [`driver/`](driver) (must be **test‑signed**;
   `bcdedit /set testsigning on`).
2. **Degraded mode without a driver.** If no virtual display is present, ScreenFlip
   falls back to **mirroring another real display** flipped onto your target monitor.
   In this mode the cursor on the mirrored panel is the **real, backwards** pointer —
   it's a flipped *viewer*, not a flipped *workspace*. (Mirroring a display onto
   *itself* is disabled — it would feed back.)
3. **Untested on the author's hardware.** This port was **architected from the proven
   macOS app**, not yet run end‑to‑end on Windows. Treat the first run as bring‑up and
   watch the log: `%TEMP%\screenflip.log`.
4. **OS floor.** Needs **Windows 10 2004 (build 19041)+** for capture‑exclusion of the
   overlay; **Windows 11** recommended for borderless capture and the cleanest result.
5. **No special permission.** Windows screen capture needs no grant (there is no macOS
   "Screen Recording"/TCC equivalent). The app runs without admin; only a one‑time
   driver install needs elevation.
6. **Arrangement quirks.** Windows may normalize a corner‑only monitor arrangement;
   if so the cursor stays contained by the 90 Hz edge guard alone (a momentary blip at
   the boundary is possible).

## Build & run

Native C++ (Win32 + Direct3D 11 + Windows.Graphics.Capture), built from the command
line with the MSVC tools — no Visual Studio IDE required (the analogue of the macOS
`build.sh`/`swiftc` flow).

**Prerequisites**

- **Visual Studio Build Tools** with **"Desktop development with C++"** (gives
  `cl.exe`, `link.exe`, `rc.exe`, `mt.exe`).
- A **Windows 10/11 SDK** (gives `fxc.exe`, the WGC/DirectComposition headers, and the
  C++/WinRT projection headers).

**Build**

```bat
git clone https://github.com/vbario/screenflip.git
cd screenflip\windows

:: from an "x64 Native Tools Command Prompt for VS"
build.bat

:: run it (lives in the system tray — look for the flip icon)
build\ScreenFlip.exe
```

Optional virtual‑display driver (full fidelity): see [`driver/README.md`](driver/README.md)
or `build.bat driver`.

## Using it

ScreenFlip runs as a **system‑tray** app (no taskbar window). Right‑click the tray
icon:

- **Flip which displays** — check any monitor to turn it into a flipped workspace.
  Your choice is remembered (per display, by stable id).
- **Flip cursor to match mirror** — also mirror the proxy cursor image (handy when
  viewing through a physical mirror).
- **Restart all** / **Quit ScreenFlip** — quitting restores your original display
  arrangement.

## Help & feedback

Questions, bugs, ideas? Email **vladimir@vbar.io**.

## License

Provided as‑is, for personal use. No warranty. The bundled driver is adapted from
Microsoft's `Windows-driver-samples` (MIT).
