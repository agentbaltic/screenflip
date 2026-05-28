import AppKit

/// A tiny borderless, click-through window that draws our synthetic cursor at the
/// mirrored position on a flipped display (since the real HW cursor is hidden there).
final class CursorWindow: NSWindow {
    private let imageView = NSImageView()
    private var hotSpot = NSPoint.zero

    init() {
        super.init(contentRect: NSRect(x: 0, y: 0, width: 32, height: 32),
                   styleMask: [.borderless], backing: .buffered, defer: false)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        ignoresMouseEvents = true
        // Above our flip overlay (.screenSaver) and everything else short of the HW cursor.
        level = NSWindow.Level(rawValue: Int(CGShieldingWindowLevel()) + 1)
        collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]

        let host = NSView(frame: NSRect(x: 0, y: 0, width: 32, height: 32))
        host.wantsLayer = true
        imageView.frame = host.bounds
        imageView.imageScaling = .scaleProportionallyDown
        host.addSubview(imageView)
        contentView = host

        setImage(NSCursor.arrow.image, hotSpot: NSCursor.arrow.hotSpot)
        orderOut(nil)
    }

    func setImage(_ image: NSImage, hotSpot: NSPoint) {
        imageView.image = image
        self.hotSpot = hotSpot
        let size = image.size == .zero ? NSSize(width: 24, height: 24) : image.size
        setContentSize(size)
        imageView.frame = NSRect(origin: .zero, size: size)
    }

    /// Place the cursor so its hotspot sits at `cgPoint` (CoreGraphics global, top-left origin).
    func moveHotspot(toCG cgPoint: CGPoint) {
        let mainH = CGDisplayBounds(CGMainDisplayID()).height
        // CG (top-left) -> AppKit (bottom-left, primary-screen relative).
        let akX = cgPoint.x - hotSpot.x
        let akY = (mainH - cgPoint.y) - (frame.height - hotSpot.y)
        setFrameOrigin(NSPoint(x: akX, y: akY))
        if !isVisible { orderFrontRegardless() }
    }

    func showCursor() { if !isVisible { orderFrontRegardless() } }
    func hideCursor() { if isVisible { orderOut(nil) } }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}
