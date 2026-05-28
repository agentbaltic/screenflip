import AppKit
import ScreenCaptureKit

/// Maps a physical output display to its hidden virtual workspace, for the cursor proxy.
struct WorkspaceMapping {
    let pRect: CGRect          // physical output (where the user looks), CG global
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
        vd.onTerminated = { [weak self] in self?.onNeedsReconcile?() }
        virtual = vd

        // Arrange: main | V (workspace) | P (physical output). The cursor crosses main->V
        // natively (V acts as a normal display to main's right); P sits just past V.
        if let main = Displays.main() {
            let vOrigin = CGPoint(x: main.bounds.maxX, y: main.bounds.minY)
            let pOrigin = CGPoint(x: main.bounds.maxX + CGFloat(w), y: main.bounds.minY)
            Displays.setOrigins([(vd.displayID, vOrigin), (displayID, pOrigin)])
            Log.line("arranged: main.maxX=\(main.bounds.maxX) -> V@\(vOrigin), P@\(pOrigin)")
        }

        let vBounds = CGDisplayBounds(vd.displayID)
        mapping = WorkspaceMapping(pRect: CGDisplayBounds(displayID), vOrigin: vBounds.origin,
                                   vSize: CGSize(width: vd.width, height: vd.height), vDisplayID: vd.displayID)

        repositionOverlay()
        overlay.orderFrontRegardless()
        capture.onSurface = { [weak self] s in self?.overlay.present(surface: s) }
        capture.onError = { [weak self] m in Log.line("output \(self?.displayID ?? 0) capture error: \(m)") }
        capture.start(displayID: vd.displayID)
        Log.line("FlipController started: P=\(displayID) shows flipped workspace vID=\(vd.displayID) vBounds=\(vBounds)")
    }

    /// Keep the overlay covering the physical output after arrangement changes.
    func repositionOverlay() {
        guard let screen = Displays.screen(for: displayID) else { return }
        overlay.setFrame(screen.frame, display: true)
        mapping = WorkspaceMapping(pRect: CGDisplayBounds(displayID),
                                   vOrigin: virtual.map { CGDisplayBounds($0.displayID).origin } ?? .zero,
                                   vSize: mapping?.vSize ?? .zero,
                                   vDisplayID: virtual?.displayID ?? 0)
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
