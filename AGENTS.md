# ScreenFlip agent guidance

## Product

ScreenFlip is a macOS teleprompter utility. It horizontally flips one non-main
physical display while keeping pointer movement and click targets usable through
beam-splitter glass.

The macOS implementation creates a private `CGVirtualDisplay` workspace, captures it
with ScreenCaptureKit, renders it horizontally flipped on the selected physical
output, and draws a mirrored proxy cursor. The physical main display must remain
main and usable throughout activation and shutdown.

## Repository and scope

- Maintained fork: `https://github.com/agentbaltic/screenflip`
- Upstream: `https://github.com/vbario/screenflip`
- Primary development target: `macos/`
- Preserve upstream Git history and attribution. Upstream has no permissive
  open-source license, so do not republish this as an unrelated repository.
- Do not edit or replace the inherited signed `macos/build/ScreenFlip-0.3.dmg`.
  Source builds from this fork are newer than that inherited installer.

## Build and verification

Run from the repository root:

```bash
./macos/test.sh
./macos/probe-virtual-display.sh
SF_USE_CERT=0 ./macos/build.sh
open macos/build/ScreenFlip.app
```

The ad-hoc build may need Screen Recording permission again after recompilation.
Do not run `macos/scripts/authorize-signing.sh` unless a `ScreenFlip Dev` identity
already exists; that script authorizes an existing identity and does not create one.

For a read-only machine snapshot, run:

```bash
./macos/diagnose.sh
```

## Safety rules for live display tests

- Never select, move, or cover the current main display.
- Support exactly one non-main physical output.
- Filter ScreenFlip-created virtual displays by vendor `0x5346` and product `0x5350`.
- Snapshot physical display origins before activation and verify exact restoration
  after a normal quit.
- Prefer the app's Quit command or a graceful application termination. Test Force
  Quit only after recording the original display arrangement because crash recovery
  is not yet proven.
- Do not reboot or restart WindowServer until `./macos/diagnose.sh` and
  `/tmp/screenflip.log` have captured the failure state.
- During an active test, verify the main display ID with `CGDisplayIsMain`, confirm
  continuous captured frames in `/tmp/screenflip.log`, and test cursor mapping at
  both edges and the center.

## Known hardware result

The fork was technically validated on an M1 Max Mac Studio running macOS 26.4.1:

- Main: LG Ultrawide, 3840x1620 points at 50 Hz.
- Output: HG560T34, 1920x1200 points at 60 Hz.
- The main display remained main, more than 2,400 frames captured continuously,
  cursor proxy mapping passed at both edges and center, and graceful termination
  restored the original origins exactly.

See `PROJECT_PLAN.md` and GitHub issue #1 for the full result.

## Current bug to investigate

On an M1 laptop, the connected physical monitor can appear as `ScreenFlip Workspace`
after ScreenFlip is reportedly no longer running. The image is then not flipped, and
the menu-bar app does not list that workspace as a selectable output.

Before changing code, capture:

1. `./macos/diagnose.sh`
2. Whether any ScreenFlip process is alive.
3. Display IDs, UUIDs, vendor/product IDs, names, origins, and main status.
4. Whether the `ScreenFlip Workspace` entry has vendor `0x5346` and product `0x5350`
   or is actually the physical monitor with an unexpected label.
5. `/tmp/screenflip.log` from the affected launch.

Likely areas include lifecycle cleanup, a second/older ScreenFlip process or bundle,
virtual-display filtering, sleep/wake, hot-plug, and restoration after abnormal exit.
Do not assume the displayed name alone proves that a virtual display survived.

## Definition of done

- Unit tests and the virtual-display probe pass.
- The app builds with the current Apple command-line tools.
- The selected physical output flips and the reflected cursor behaves correctly.
- The main display never changes.
- Normal quit removes the virtual workspace and restores all original physical
  display origins.
- New lifecycle fixes include reproducible diagnostics and a live hardware test.
