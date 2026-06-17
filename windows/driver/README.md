# ScreenFlip virtual-display driver (IddCx)

This folder holds an **optional** IddCx user-mode (UMDF) driver that creates one
**headless virtual monitor** for ScreenFlip to use as its flipped workspace. It is
the Windows analogue of the private `CGVirtualDisplay` API the macOS build uses —
except Windows has **no driver-free way** to make a virtual monitor, so a real
workspace requires a driver.

> ⚠️ **You usually do not need to build this.** The simplest path on a personal
> machine is to install an already-signed **community virtual-display driver**
> (e.g. a maintained "Virtual Display Driver" / Parsec VDD) and let ScreenFlip
> **adopt** its monitor automatically (Tier 1-alt in `../SPEC.md`). Build this only
> if you want a self-contained ScreenFlip-branded virtual display.
>
> ⚠️ **Untested.** This driver is a faithful reference adapted from Microsoft's
> public `IndirectDisplay`/`IddSampleDriver` sample. It compiles against the WDK
> but has **not been run end-to-end** on the author's hardware. Treat first install
> as bring-up.

## What it does

- Advertises a single virtual monitor with a fixed EDID (name **"SFlip Virtual"**,
  preferred mode **1920×1080@60**, plus a few common modes). ScreenFlip detects it
  by that name (`Displays::All().isVirtual`) and adopts it.
- Frames the desktop compositor renders to the monitor are **drained and discarded**
  — ScreenFlip captures the monitor with Windows.Graphics.Capture instead, so the
  driver never needs to display anything.

## Prerequisites

- **Windows Driver Kit (WDK)** matching your installed Windows SDK, plus either the
  **EWDK** or Visual Studio with the **"WDF/UMDF"** components, so `msbuild` can use
  the `WindowsKernelModeDriver10.0` toolset.
- A way to load an unsigned driver on your test machine (one of):
  - **Test signing**: `bcdedit /set testsigning on`, reboot (a desktop watermark
    appears), then sign the driver with a self-made test certificate, **or**
  - Submit to the Microsoft Hardware Dev Center for attestation signing (overkill
    for personal use).

## Build

From the repo's `windows/` folder, in an EWDK / WDK-enabled prompt:

```bat
build.bat driver
```

or directly:

```bat
msbuild driver\ScreenFlipIdd.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output lands in `driver\x64\Release\` (`ScreenFlipIdd.dll`, `.inf`, `.cat`).

## Test-sign + install

```powershell
# 1) One-time: create + trust a test certificate
makecert -r -pe -ss PrivateCertStore -n "CN=ScreenFlip Test Cert" ScreenFlipTest.cer
certutil -addstore -f root  ScreenFlipTest.cer
certutil -addstore -f trustedpublisher ScreenFlipTest.cer

# 2) Sign the catalog (after building)
signtool sign /a /s PrivateCertStore /n "ScreenFlip Test Cert" ^
  /fd sha256 driver\x64\Release\ScreenFlipIdd.cat

# 3) Enable test signing (reboot afterwards)
bcdedit /set testsigning on

# 4) Create the root device + install the INF
devgen /add /instanceid ScreenFlipIdd /hardwareid "Root\ScreenFlipIdd"
pnputil /add-driver driver\x64\Release\ScreenFlipIdd.inf /install
```

(`devgen` ships with the WDK; alternatively use Device Manager → *Action* → *Add
legacy hardware* → *Have Disk*.) After install, a **"ScreenFlip Virtual Display"**
appears in Device Manager and as an extra monitor; launch `ScreenFlip.exe` and it
will adopt it.

## Uninstall

```powershell
pnputil /enum-devices /class Display          # find the device instance id
pnputil /remove-device "<instance-id>"
bcdedit /set testsigning off                  # optional, then reboot
```

## License / attribution

Adapted from Microsoft's `Windows-driver-samples` IndirectDisplay sample (MIT).
Provided as-is for personal use.
