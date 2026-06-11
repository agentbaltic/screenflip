# ScreenFlip

Turn any Mac monitor into a **horizontally‑mirrored extended display you can actually work on** — with a cursor that still moves the right way and clicks that land where you expect.

## Why this exists

macOS can rotate a display (90°/180°), but it has **no built‑in way to mirror one left‑to‑right**. And on Apple Silicon the old low‑level framebuffer flip simply doesn't work anymore, so even most third‑party tools fall back to capturing the screen and re‑drawing it flipped — which leaves you with a backwards, hard‑to‑use cursor.

ScreenFlip solves the part everyone skips: the **cursor**. Move your mouse and it behaves naturally on the flipped display; click and it hits exactly what you see. That makes a mirrored screen genuinely usable for things like:

- **Teleprompters / beam‑splitter glass** — text reads correctly in the reflection.
- **Filming yourself** at a screen where the camera mirrors the image.
- **Mirror / practice setups** where you want a flipped view you can still operate.

## How it works (short version)

When you pick a display to flip, ScreenFlip:

1. Creates a **headless virtual display** — this becomes the real workspace your windows live on.
2. **Captures** that workspace and draws it **horizontally flipped, full‑screen** on your chosen physical monitor.
3. Lets the cursor live **natively** on the workspace (so movement, clicks, drags and typing all just work) and draws a matching cursor on the flipped monitor at the mirrored spot.

There is **no input interception** of any kind — nothing touches your mouse or keyboard on other screens. Your display arrangement is saved when ScreenFlip starts and restored when it quits.

## Requirements

- macOS 14 (Sonoma) or later, **Apple Silicon** (developed/tested on M3).
- Xcode **Command Line Tools** (`xcode-select --install`) — full Xcode is not required.
- One **Screen Recording** permission grant (prompted on first run). No Accessibility, no special entitlements.

> Note: ScreenFlip uses a private macOS virtual‑display API. It works well today but, like anything built on private APIs, could need updates after a major macOS release.

## Download

Download the latest bundled app here:

**[Download ScreenFlip.app](https://github.com/vbario/screenflip/raw/main/build/ScreenFlip.zip)**

Unzip it, open `ScreenFlip.app`, and grant Screen Recording permission when macOS asks.

## Build & run

```bash
git clone https://github.com/vbario/screenflip.git
cd screenflip

# One‑time: create a stable local signing identity so macOS remembers the
# Screen Recording permission across rebuilds (you'll be asked to authorize it).
./scripts/authorize-signing.sh        # optional but recommended

# Build the app bundle and launch it
./build.sh                            # stable signing is used automatically when available
open build/ScreenFlip.app
```

If the local **ScreenFlip Dev** signing identity is missing, `./build.sh` falls back to ad‑hoc signing — it still works, but macOS forgets the Screen Recording grant on each rebuild. Use `SF_USE_CERT=0 ./build.sh` only when you explicitly want an ad‑hoc build.

On first launch you'll be asked to enable **ScreenFlip** under **System Settings → Privacy & Security → Screen Recording**, then relaunch.

## Using it

ScreenFlip runs as a menu‑bar app (look for the **⇋** icon — no Dock icon, no window):

- **Pick which displays to flip** — toggle any connected display in the menu. Selected displays become flipped workspaces. Your choice is remembered.
- **Flip cursor to match mirror** — toggles whether the on‑screen cursor is also mirrored (↗) to match the flipped image, or kept normal‑facing (↖). Handy when you're viewing through a physical mirror.
- **Restart all** / **Quit ScreenFlip** — quitting restores your original display arrangement.

To use a flipped display, just move your mouse onto it and work normally — the windows you drag there appear mirrored, and the cursor stays correct.

## Help & feedback

Questions, bug reports, or ideas? Email **vladimir@vbar.io** — happy to help.

## License

Provided as‑is, for personal use. No warranty.
