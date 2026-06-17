import AppKit
import QuartzCore
import IOSurface

enum FlipAxis: String {
    case horizontal   // left-right mirror (default)
    case vertical     // top-bottom
    case both         // 180°

    var transform: CGAffineTransform {
        switch self {
        case .horizontal: return CGAffineTransform(scaleX: -1, y: 1)
        case .vertical:   return CGAffineTransform(scaleX: 1, y: -1)
        case .both:       return CGAffineTransform(scaleX: -1, y: -1)
        }
    }
}

/// A borderless, full-screen window pinned to one physical display that shows the
/// captured frames mirrored. Rendering is done with CoreAnimation: the content layer's
/// `contents` is set to the capture IOSurface each frame, and a scale transform produces
/// the mirror (GPU-composited, no shader needed).
final class OverlayWindow: NSWindow {
    private let contentLayer = CALayer()
    private let axis: FlipAxis

    init(screen: NSScreen, axis: FlipAxis = .horizontal) {
        self.axis = axis
        super.init(contentRect: screen.frame,
                   styleMask: [.borderless],
                   backing: .buffered,
                   defer: false)

        isOpaque = true
        backgroundColor = .black
        hasShadow = false
        // Above normal windows but not stealing focus. Screen-saver level keeps it
        // on top of regular content on that display.
        level = .screenSaver
        collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
        ignoresMouseEvents = true          // clicks pass through to the real windows behind
        setFrame(screen.frame, display: true)

        let host = NSView(frame: screen.frame)
        host.wantsLayer = true
        host.layer = CALayer()
        host.layer?.backgroundColor = NSColor.black.cgColor

        contentLayer.frame = host.bounds
        contentLayer.contentsGravity = .resize
        contentLayer.backgroundColor = NSColor.black.cgColor
        contentLayer.magnificationFilter = .nearest
        // Mirror about the layer's center (default anchor 0.5,0.5).
        contentLayer.setAffineTransform(axis.transform)
        host.layer?.addSublayer(contentLayer)

        contentView = host
    }

    /// Update the displayed frame. Must be called on the main thread.
    func present(surface: IOSurfaceRef) {
        contentLayer.contents = surface
        // Keep the layer sized to the window in case of reconfiguration.
        if let b = contentView?.bounds, contentLayer.frame != b {
            contentLayer.frame = b
            contentLayer.setAffineTransform(axis.transform)
        }
    }

    /// CoreGraphics window id, used to exclude this overlay from screen capture.
    var cgWindowID: CGWindowID { CGWindowID(windowNumber) }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}
