import AppKit
import CoreGraphics

/// Helpers for enumerating displays and mapping between CoreGraphics display IDs
/// and AppKit NSScreens.
enum Displays {
    struct Info {
        let id: CGDirectDisplayID
        let uuid: String            // stable across reboots (numeric id is not)
        let bounds: CGRect          // global CG coords (top-left origin)
        let isMain: Bool
        let isBuiltin: Bool
        let screen: NSScreen?

        /// Human-friendly label for the menu, e.g. "LG FULL HD (1920×1080)".
        var label: String {
            let res = "\(Int(bounds.width))×\(Int(bounds.height))"
            let name = screen?.localizedName ?? (isMain ? "Main Display" : "Display \(id)")
            return "\(name) (\(res))" + (isMain ? " — main" : "")
        }
    }

    static func all() -> [Info] {
        var count: UInt32 = 0
        CGGetActiveDisplayList(0, nil, &count)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        CGGetActiveDisplayList(count, &ids, &count)
        return ids.map { id in
            Info(id: id,
                 uuid: uuidString(for: id),
                 bounds: CGDisplayBounds(id),
                 isMain: CGDisplayIsMain(id) != 0,
                 isBuiltin: CGDisplayIsBuiltin(id) != 0,
                 screen: screen(for: id))
        }
    }

    static func info(for id: CGDirectDisplayID) -> Info? { all().first { $0.id == id } }

    /// Stable per-display identifier (survives reboot / reconnection).
    static func uuidString(for id: CGDirectDisplayID) -> String {
        guard let cf = CGDisplayCreateUUIDFromDisplayID(id)?.takeRetainedValue() else {
            return "id-\(id)"
        }
        return CFUUIDCreateString(nil, cf) as String
    }

    static func main() -> Info? { all().first { $0.isMain } }

    /// The display we render the flipped image onto. For M1 this is the first
    /// non-main display (the user's 1366x768 monitor).
    static func firstSecondary() -> Info? { all().first { !$0.isMain } }

    static func screen(for id: CGDirectDisplayID) -> NSScreen? {
        NSScreen.screens.first {
            ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == id
        }
    }

    static func displayID(of screen: NSScreen) -> CGDirectDisplayID {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
    }

    // MARK: Arrangement save / set / restore

    /// Snapshot every active display's origin so we can restore the user's layout later.
    static func snapshotOrigins() -> [CGDirectDisplayID: CGPoint] {
        var map: [CGDirectDisplayID: CGPoint] = [:]
        for d in all() { map[d.id] = d.bounds.origin }
        return map
    }

    /// Apply a set of display origins in a single reconfiguration transaction.
    static func setOrigins(_ origins: [(CGDirectDisplayID, CGPoint)]) {
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return }
        for (id, o) in origins {
            CGConfigureDisplayOrigin(config, id, Int32(o.x.rounded()), Int32(o.y.rounded()))
        }
        CGCompleteDisplayConfiguration(config, .forSession)
    }

    static func restoreOrigins(_ map: [CGDirectDisplayID: CGPoint]) {
        let live = Set(all().map { $0.id })
        setOrigins(map.filter { live.contains($0.key) }.map { ($0.key, $0.value) })
        Log.line("restored display arrangement for \(map.count) display(s)")
    }
}
