import AppKit
import ScreenCaptureKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var controllers: [String: Any] = [:]      // uuid -> FlipController
    private var selected: Set<String> = []            // uuids the user wants flipped
    private let defaultsKey = "flippedDisplayUUIDs"
    private let cursorFlipKey = "flipCursor"
    private var savedArrangement: [CGDirectDisplayID: CGPoint] = [:]
    private var requestedScreenRecordingThisLaunch = false

    func applicationDidFinishLaunching(_ note: Notification) {
        Log.reset()
        Log.line("=== ScreenFlip launched (pid \(ProcessInfo.processInfo.processIdentifier)) ===")
        NSApp.setActivationPolicy(.accessory)

        // Remember the user's display layout so we can restore it on quit (we relocate
        // displays to host the virtual workspace).
        savedArrangement = Displays.snapshotOrigins()

        selected = Set(UserDefaults.standard.stringArray(forKey: defaultsKey) ?? [])
        Log.line("restored selection: \(selected)")
        for d in Displays.all() { Log.line("display \(d.id): \"\(d.label)\" uuid=\(d.uuid)") }

        setupStatusItem()

        // Warm up Screen Recording permission (the first SCShareableContent call triggers
        // the prompt). We do this even with nothing selected so the toggle is instant later.
        primeScreenRecordingPermission()

        // React to display hotplug / rearrangement.
        NotificationCenter.default.addObserver(self, selector: #selector(screensChanged),
                                               name: NSApplication.didChangeScreenParametersNotification, object: nil)

        reconcile()
        MirrorInput.shared.setCursorFlipped(UserDefaults.standard.bool(forKey: cursorFlipKey))
        rebuildMenu()
    }

    // MARK: Permission priming
    private func primeScreenRecordingPermission() {
        let granted = CGPreflightScreenCaptureAccess()
        Log.line("CGPreflightScreenCaptureAccess = \(granted)")
        guard !granted, !requestedScreenRecordingThisLaunch else { return }
        requestedScreenRecordingThisLaunch = true

        DispatchQueue.global(qos: .userInitiated).async {
            let allowed = CGRequestScreenCaptureAccess()
            Log.line("CGRequestScreenCaptureAccess = \(allowed)")
            DispatchQueue.main.async {
                self.reconcile()
                self.rebuildMenu()
            }
        }
    }

    // MARK: Selection / reconcile
    /// Ensure a running FlipController exists for every selected+active display, and
    /// none for the rest. Called on launch, on toggle, and on display changes.
    private var reconciling = false
    private func reconcile() {
        guard #available(macOS 13.0, *) else { return }
        guard !reconciling else { return }     // creating/repositioning the virtual display
        reconciling = true                      // fires didChangeScreenParameters; avoid re-entry
        defer { reconciling = false }

        guard CGPreflightScreenCaptureAccess() else {
            if !controllers.isEmpty {
                for (_, ctrl) in controllers { (ctrl as? FlipController)?.stop() }
                controllers.removeAll()
            }
            MirrorInput.shared.setMappings([])
            Log.line("reconcile deferred: Screen Recording permission is not available")
            return
        }

        let active = Displays.all()
        let activeByUUID = Dictionary(uniqueKeysWithValues: active.map { ($0.uuid, $0) })

        // Stop controllers that are no longer selected or whose physical display vanished.
        for (uuid, ctrl) in controllers where !selected.contains(uuid) || activeByUUID[uuid] == nil {
            (ctrl as? FlipController)?.stop()
            controllers[uuid] = nil
        }
        // Start controllers for selected, active displays not yet running.
        for uuid in selected {
            guard controllers[uuid] == nil, let info = activeByUUID[uuid] else { continue }
            if let ctrl = FlipController(display: info, axis: .horizontal) {
                ctrl.onNeedsReconcile = { [weak self] in self?.reconcile() }
                ctrl.start()
                controllers[uuid] = ctrl
            }
        }
        Log.line("reconcile: \(controllers.count) workspace(s) of \(selected.count) selected, \(active.count) active")

        // The portal input router is intrinsic to the virtual workspace: without it the
        // cursor would land on the headless display and be lost. It only redirects while the
        // perceived cursor is on a flipped output, and clamps to real displays — other
        // screens are untouched.
        let maps = controllers.values.compactMap { ($0 as? FlipController)?.mapping }
        MirrorInput.shared.setMappings(maps)
    }

    @objc private func toggleDisplay(_ sender: NSMenuItem) {
        guard let uuid = sender.representedObject as? String else { return }
        if selected.contains(uuid) { selected.remove(uuid) } else { selected.insert(uuid) }
        UserDefaults.standard.set(Array(selected), forKey: defaultsKey)
        reconcile()
        rebuildMenu()
    }

    @objc private func screensChanged() {
        guard !reconciling else { return }
        Log.line("display configuration changed")
        if #available(macOS 13.0, *) {
            for ctrl in controllers.values { (ctrl as? FlipController)?.repositionOverlay() }
        }
        reconcile()
        rebuildMenu()
    }

    // MARK: Menu
    private func setupStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.title = "⇋"
    }

    private func rebuildMenu() {
        let menu = NSMenu()
        let header = NSMenuItem(title: "Flip which displays:", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)

        for d in Displays.all() {
            let item = NSMenuItem(title: d.label, action: #selector(toggleDisplay(_:)), keyEquivalent: "")
            item.representedObject = d.uuid
            item.state = selected.contains(d.uuid) ? .on : .off
            item.target = self
            menu.addItem(item)
        }

        menu.addItem(.separator())
        let note = NSMenuItem(title: "Selected displays become flipped workspaces", action: nil, keyEquivalent: "")
        note.isEnabled = false
        menu.addItem(note)

        let flipCursorItem = NSMenuItem(title: "Flip cursor to match mirror",
                                        action: #selector(toggleCursorFlip), keyEquivalent: "")
        flipCursorItem.state = UserDefaults.standard.bool(forKey: cursorFlipKey) ? .on : .off
        flipCursorItem.target = self
        menu.addItem(flipCursorItem)

        menu.addItem(.separator())
        if !CGPreflightScreenCaptureAccess() {
            let warn = NSMenuItem(title: "⚠︎ Grant Screen Recording…", action: #selector(openScreenRecordingPrefs), keyEquivalent: "")
            warn.target = self
            menu.addItem(warn)
        }
        let restart = NSMenuItem(title: "Restart all", action: #selector(restartAll), keyEquivalent: "r")
        restart.target = self
        menu.addItem(restart)
        let quit = NSMenuItem(title: "Quit ScreenFlip", action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        statusItem.menu = menu
    }

    @objc private func toggleCursorFlip() {
        let on = !UserDefaults.standard.bool(forKey: cursorFlipKey)
        UserDefaults.standard.set(on, forKey: cursorFlipKey)
        MirrorInput.shared.setCursorFlipped(on)
        rebuildMenu()
    }

    @objc private func openScreenRecordingPrefs() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!)
    }

    @objc private func restartAll() {
        guard #available(macOS 13.0, *) else { return }
        for (_, ctrl) in controllers { (ctrl as? FlipController)?.stop() }
        controllers.removeAll()
        reconcile()
    }

    @objc private func quit() {
        if #available(macOS 13.0, *) {
            for (_, ctrl) in controllers { (ctrl as? FlipController)?.stop() }
        }
        MirrorInput.shared.setMappings([])
        Displays.restoreOrigins(savedArrangement)
        NSApp.terminate(nil)
    }

    func applicationWillTerminate(_ note: Notification) {
        Displays.restoreOrigins(savedArrangement)
    }
}
