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
                let localX = c.x - m.vOrigin.x
                let localY = c.y - m.vOrigin.y
                let sx = m.pRect.minX + (m.vSize.width - localX)   // horizontal mirror
                let sy = m.pRect.minY + localY
                cursorWindow.moveHotspot(toCG: CGPoint(x: sx, y: sy))
                lastOnWorkspace = true
                return
            }
        }
        if lastOnWorkspace { cursorWindow.hideCursor(); lastOnWorkspace = false }
    }
}
