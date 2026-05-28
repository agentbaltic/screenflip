import AppKit

/// A tiny borderless, click-through window that draws the synthetic cursor proxy at the
/// mirrored position on a flipped workspace's physical output. The proxy image can
/// optionally be mirrored too (to match the flipped content / read correctly through a
/// physical mirror), or kept normal-facing.
final class CursorWindow: NSWindow {
    private let imageView = NSImageView()
    private var baseImage: NSImage = NSCursor.arrow.image
    private var baseHotSpot: NSPoint = NSCursor.arrow.hotSpot
    private var effectiveHotSpot: NSPoint = NSCursor.arrow.hotSpot
    private var flipped = false

    init() {
        super.init(contentRect: NSRect(x: 0, y: 0, width: 32, height: 32),
                   styleMask: [.borderless], backing: .buffered, defer: false)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        ignoresMouseEvents = true
        level = NSWindow.Level(rawValue: Int(CGShieldingWindowLevel()) + 1)
        collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]

        let host = NSView(frame: NSRect(x: 0, y: 0, width: 32, height: 32))
        host.wantsLayer = true
        imageView.imageScaling = .scaleProportionallyDown
        host.addSubview(imageView)
        contentView = host

        setImage(NSCursor.arrow.image, hotSpot: NSCursor.arrow.hotSpot)
        orderOut(nil)
    }

    func setImage(_ image: NSImage, hotSpot: NSPoint) {
        baseImage = image
        baseHotSpot = hotSpot
        applyImage()
    }

    /// Whether to horizontally mirror the proxy cursor image.
    func setFlipped(_ on: Bool) {
        guard on != flipped else { return }
        flipped = on
        applyImage()
    }

    private func applyImage() {
        let size = baseImage.size == .zero ? NSSize(width: 24, height: 24) : baseImage.size
        if flipped {
            imageView.image = Self.mirroredHorizontally(baseImage, size: size)
            effectiveHotSpot = NSPoint(x: size.width - baseHotSpot.x, y: baseHotSpot.y)
        } else {
            imageView.image = baseImage
            effectiveHotSpot = baseHotSpot
        }
        setContentSize(size)
        imageView.frame = NSRect(origin: .zero, size: size)
    }

    private static func mirroredHorizontally(_ image: NSImage, size: NSSize) -> NSImage {
        let out = NSImage(size: size)
        out.lockFocus()
        let t = NSAffineTransform()
        t.translateX(by: size.width, yBy: 0)
        t.scaleX(by: -1, yBy: 1)
        t.concat()
        image.draw(at: .zero, from: NSRect(origin: .zero, size: size), operation: .sourceOver, fraction: 1)
        out.unlockFocus()
        return out
    }

    /// Place the cursor so its hotspot sits at `cgPoint` (CoreGraphics global, top-left origin).
    func moveHotspot(toCG cgPoint: CGPoint) {
        let mainH = CGDisplayBounds(CGMainDisplayID()).height
        let akX = cgPoint.x - effectiveHotSpot.x
        let akY = (mainH - cgPoint.y) - (frame.height - effectiveHotSpot.y)
        setFrameOrigin(NSPoint(x: akX, y: akY))
        if !isVisible { orderFrontRegardless() }
    }

    func showCursor() { if !isVisible { orderFrontRegardless() } }
    func hideCursor() { if isVisible { orderOut(nil) } }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}
