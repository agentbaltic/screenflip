import AppKit
import ScreenCaptureKit

/// Maps a physical output display to its hidden virtual workspace, for the cursor proxy.
struct WorkspaceMapping {
    let pRect: CGRect          // physical output (where the user looks), CG global
    let pDisplayID: CGDirectDisplayID
    let vOrigin: CGPoint       // virtual workspace origin, CG global
    let vSize: CGSize          // virtual workspace pixel size
    let vDisplayID: CGDirectDisplayID
}

/// Owns one "virtual flipped workspace":
///  • a headless virtual display V — placed in the arrangement as a NORMAL extended
///    display immediately to the RIGHT of the main screen, so the cursor flows onto it
///    natively (no input interception);
///  • the chosen PHYSICAL output P — moved just past V and covered with a full-screen
///    overlay that shows V captured + horizontally flipped.
/// The user looks at P and sees the workspace mirrored; the cursor lives natively on V.
@available(macOS 13.0, *)
final class FlipController {
    let displayID: CGDirectDisplayID      // physical output (P)
    let uuid: String
    private var overlay: OverlayWindow
    private let capture = Capture()
    private let axis: FlipAxis
    private var virtual: VirtualDisplay?
    var onNeedsReconcile: (() -> Void)?
    private(set) var mapping: WorkspaceMapping?

    init?(display: Displays.Info, axis: FlipAxis) {
        guard let screen = display.screen else {
            Log.line("FlipController: no NSScreen for display \(display.id)"); return nil
        }
        self.displayID = display.id
        self.uuid = display.uuid
        self.axis = axis
        self.overlay = OverlayWindow(screen: screen, axis: axis)
    }

    func start() {
        let w = Int(CGDisplayBounds(displayID).width)
        let h = Int(CGDisplayBounds(displayID).height)
        guard let vd = VirtualDisplay(width: w, height: h, name: "ScreenFlip Workspace", serial: displayID) else {
            Log.line("FlipController: failed to create virtual display for output \(displayID)"); return
        }
        vd.onTerminated = { [weak self] in
            guard let self else { return }
            // The workspace is gone: drop it so the capture-restart loop halts and
            // reconcile() tears this controller down and builds a fresh one.
            self.virtual = nil
            self.onNeedsReconcile?()
        }
        virtual = vd

        // Arrange: main | V (workspace), with P (physical output) hanging off V's
        // bottom-right CORNER. The cursor crosses main->V natively (V acts as a normal
        // display to main's right). Corner contact keeps the arrangement legal (displays
        // must touch) but leaves no shared edge, so the cursor cannot roll off the far
        // side of the workspace onto P — where the OS would draw a real, unmirrored
        // cursor on top of the flipped picture. MirrorInput's edge guard backstops this.
        if let main = Displays.main() {
            let vOrigin = CGPoint(x: main.bounds.maxX, y: main.bounds.minY)
            let pOrigin = CGPoint(x: main.bounds.maxX + CGFloat(w), y: main.bounds.minY + CGFloat(h))
            Displays.setOrigins([(vd.displayID, vOrigin), (displayID, pOrigin)])
            Log.line("arranged: main.maxX=\(main.bounds.maxX) -> V@\(vOrigin), P@\(pOrigin) (corner contact)")
        }

        let vBounds = CGDisplayBounds(vd.displayID)
        mapping = WorkspaceMapping(pRect: CGDisplayBounds(displayID), pDisplayID: displayID,
                                   vOrigin: vBounds.origin,
                                   vSize: CGSize(width: vd.width, height: vd.height), vDisplayID: vd.displayID)

        repositionOverlay()
        overlay.orderFrontRegardless()
        capture.onSurface = { [weak self] s in self?.overlay.present(surface: s) }
        capture.onError = { [weak self] m in
            guard let self else { return }
            Log.line("output \(self.displayID) capture error: \(m)")
            self.scheduleCaptureRestart()
        }
        capture.start(displayID: vd.displayID)
        Log.line("FlipController started: P=\(displayID) shows flipped workspace vID=\(vd.displayID) vBounds=\(vBounds)")
    }

    /// False once the virtual display failed to start or was terminated by the system —
    /// reconcile() tears such controllers down and recreates them with a fresh workspace.
    var workspaceAlive: Bool { virtual != nil }

    /// Keep the overlay covering the physical output after arrangement changes.
    func repositionOverlay() {
        guard let screen = Displays.screen(for: displayID) else { return }
        overlay.setFrame(screen.frame, display: true)
        // Never fabricate a degenerate mapping (zero-size workspace): MirrorInput's edge
        // guard would otherwise pin the cursor against an empty rect on a real display.
        guard let vd = virtual, let prior = mapping else { mapping = nil; return }
        mapping = WorkspaceMapping(pRect: CGDisplayBounds(displayID), pDisplayID: displayID,
                                   vOrigin: CGDisplayBounds(vd.displayID).origin,
                                   vSize: prior.vSize,
                                   vDisplayID: vd.displayID)
    }

    /// replayd occasionally tears the stream down mid-session (sleep/wake, daemon
    /// restart) — sometimes without even an error object. Treat stream death as
    /// transient: bring the capture back up instead of leaving a frozen overlay.
    /// Runs on the main queue (capture reports errors there). Keeps retrying every
    /// few seconds while the workspace exists, so it also recovers when a failed
    /// restart attempt itself reports an error.
    private var restartPending = false
    private func scheduleCaptureRestart() {
        guard virtual != nil, !restartPending else { return }
        restartPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self else { return }
            self.restartPending = false
            guard let vd = self.virtual else { return }   // stopped in the meantime
            Log.line("output \(self.displayID): restarting capture of workspace \(vd.displayID)")
            self.capture.start(displayID: vd.displayID)
        }
    }

    func stop() {
        capture.stop()
        overlay.orderOut(nil)
        virtual?.destroy()
        virtual = nil
        mapping = nil
        Log.line("FlipController stopped for output \(displayID)")
    }
}
