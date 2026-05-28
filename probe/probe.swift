// Toolchain probe: confirm we can compile + link the frameworks screenflip needs,
// create a Metal device, and enumerate displays — all WITHOUT triggering TCC prompts
// (no ScreenCaptureKit capture, no event taps) and WITHOUT touching the display config.
import Foundation
import AppKit
import Metal
import CoreGraphics
import ScreenCaptureKit   // link-only check; we do not call SCShareableContent here

func enumerateDisplays() {
    var count: UInt32 = 0
    CGGetActiveDisplayList(0, nil, &count)
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
    CGGetActiveDisplayList(count, &ids, &count)
    print("Active displays: \(count)")
    for id in ids {
        let b = CGDisplayBounds(id)
        let main = CGDisplayIsMain(id) != 0
        let builtin = CGDisplayIsBuiltin(id) != 0
        let vendor = CGDisplayVendorNumber(id)
        let model = CGDisplayModelNumber(id)
        let serial = CGDisplaySerialNumber(id)
        let unit = CGDisplayUnitNumber(id)
        print(String(format: "  id=%u  bounds=(%.0f,%.0f %.0fx%.0f)  main=%@ builtin=%@ vendor=%u model=%u serial=%u unit=%u",
                     id, b.origin.x, b.origin.y, b.size.width, b.size.height,
                     main ? "Y":"N", builtin ? "Y":"N", vendor, model, serial, unit))
    }
}

func metalCheck() {
    if let dev = MTLCreateSystemDefaultDevice() {
        print("Metal device: \(dev.name)  unifiedMemory=\(dev.hasUnifiedMemory)")
    } else {
        print("Metal device: NONE")
    }
}

func screenCheck() {
    print("NSScreen count: \(NSScreen.screens.count)")
    for s in NSScreen.screens {
        let n = (s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
        print("  NSScreen num=\(n) frame=\(s.frame) backingScale=\(s.backingScaleFactor)")
    }
}

print("== screenflip toolchain probe ==")
metalCheck()
enumerateDisplays()
screenCheck()
print("== ok ==")
