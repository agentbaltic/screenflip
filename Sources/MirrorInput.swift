import AppKit
import CoreGraphics

/// Draws the on-screen cursor proxy for virtual flipped workspaces.
///
/// The cursor lives NATIVELY on the headless workspace V (no input interception at all —
/// nothing touches the mouse on any display). Because V has no panel, its cursor is
/// invisible, so we poll the cursor position and, whenever it's on a workspace, draw a
/// synthetic cursor on the physical output P at the horizontally-mirrored position — i.e.
/// exactly where V's flipped capture shows the spot the cursor is really on.
///
/// Needs NO Accessibility and NO event tap.
final class MirrorInput {
    static let shared = MirrorInput()

    private var mappings: [WorkspaceMapping] = []
    private var timer: Timer?
    private let cursorWindow = CursorWindow()
    private var lastOnWorkspace = false

    /// Mirror the proxy cursor image to match the flipped content (off = normal-facing arrow).
    func setCursorFlipped(_ on: Bool) {
        cursorWindow.setFlipped(on)
        Log.line("MirrorInput(proxy): cursor flipped = \(on)")
    }

    func setMappings(_ maps: [WorkspaceMapping]) {
        mappings = maps
        Log.line("MirrorInput(proxy): \(maps.count) workspace(s)")
        if maps.isEmpty {
            stop()
        } else {
            start()
        }
    }

    private func start() {
        guard timer == nil else { return }
        let t = Timer(timeInterval: 1.0 / 90.0, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        Log.line("MirrorInput(proxy): cursor proxy started (no event tap)")
    }

    private func stop() {
        timer?.invalidate(); timer = nil
        cursorWindow.hideCursor()
    }

    private func tick() {
        guard let c = CGEvent(source: nil)?.location else { return }
        for m in mappings {
            let vRect = CGRect(origin: m.vOrigin, size: m.vSize)
            if vRect.contains(c) {
                drawProxy(at: c, mapping: m)
                return
            }
        }
        // Edge guard: the physical output P only ever shows V's mirrored picture, so the
        // cursor has no business there — were it allowed to stay, the OS would draw a
        // real, unmirrored cursor on top of the flipped content and its motion would
        // read backwards. If it slips onto P anyway (shared edge after the OS adjusts
        // the arrangement, an app-initiated warp, a fast flick), pin it back to the
        // nearest point of the workspace. This touches the cursor ONLY on P, a display
        // the user cannot meaningfully use; input everywhere else stays native.
        // Safety: containment uses LIVE bounds looked up by display ID, never the cached
        // rects — a stale mapping (mid-arrangement-change, dead workspace) must not let
        // the guard fight the cursor on a display the user actually uses.
        for m in mappings {
            guard CGDisplayBounds(m.pDisplayID).contains(c) else { continue }
            let vRect = CGDisplayBounds(m.vDisplayID)
            guard !vRect.isEmpty else { continue }      // workspace gone → leave the cursor alone
            let pinned = CGPoint(x: min(max(c.x, vRect.minX), vRect.maxX - 1),
                                 y: min(max(c.y, vRect.minY), vRect.maxY - 1))
            CGWarpMouseCursorPosition(pinned)
            CGAssociateMouseAndMouseCursorPosition(1)   // cancel post-warp move suppression
            drawProxy(at: pinned, mapping: m)
            return
        }
        if lastOnWorkspace { cursorWindow.hideCursor(); lastOnWorkspace = false }
    }

    /// Draw the synthetic cursor on P at the horizontal mirror of workspace point `c`.
    private func drawProxy(at c: CGPoint, mapping m: WorkspaceMapping) {
        let localX = c.x - m.vOrigin.x
        let localY = c.y - m.vOrigin.y
        let sx = m.pRect.minX + (m.vSize.width - localX)   // horizontal mirror
        let sy = m.pRect.minY + localY
        cursorWindow.moveHotspot(toCG: CGPoint(x: sx, y: sy))
        lastOnWorkspace = true
    }
}
