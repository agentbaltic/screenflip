import Foundation
import ScreenCaptureKit
import CoreVideo
import IOSurface

/// Thin ScreenCaptureKit wrapper that captures one display and hands back the
/// IOSurface for each frame on the main queue.
@available(macOS 13.0, *)
final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private var startTask: Task<Void, Never>?
    private var selfRetain: Capture?
    private var shouldRun = false
    private var pendingStopCount = 0
    private let stateLock = NSLock()
    private let frameQueue = DispatchQueue(label: "io.vbar.screenflip.frames")
    var onSurface: ((IOSurfaceRef) -> Void)?
    var onError: ((String) -> Void)?
    private var frameCount = 0

    /// Start capturing the given display. `excludeBundleIDs` drops those apps (e.g.
    /// our own overlay) from the captured scene to avoid a feedback loop — required
    /// when the capture source and the output overlay are on the SAME display.
    /// `excludeWindowIDs` (our overlay) MUST be dropped to avoid a feedback loop; we
    /// retry fetching shareable content until those windows are registered.
    func start(displayID: CGDirectDisplayID, excludeWindowIDs: [CGWindowID] = []) {
        stop()
        stateLock.lock()
        shouldRun = true
        selfRetain = self
        stateLock.unlock()

        startTask = Task { [weak self] in
            guard let self else { return }
            do {
                Log.line("requesting shareable content (will trigger Screen Recording prompt if needed)…")
                var content = try await SCShareableContent.excludingDesktopWindows(false,
                                                                                   onScreenWindowsOnly: true)
                guard self.isRunning else {
                    self.releaseSelfRetainIfIdle()
                    return
                }
                // Wait until our overlay window(s) appear in the snapshot, so we can exclude them.
                var tries = 0
                while !excludeWindowIDs.isEmpty,
                      !excludeWindowIDs.allSatisfy({ wid in content.windows.contains { $0.windowID == wid } }),
                      tries < 15 {
                    try await Task.sleep(nanoseconds: 100_000_000)   // 100ms
                    guard self.isRunning else {
                        self.releaseSelfRetainIfIdle()
                        return
                    }
                    content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
                    tries += 1
                }
                Log.line("shareable content: \(content.displays.count) displays, \(content.windows.count) windows (after \(tries) retries)")
                guard let scDisplay = content.displays.first(where: { $0.displayID == displayID }) else {
                    self.reportError("Display \(displayID) not found among \(content.displays.count) SC displays")
                    self.finishStartFailure()
                    return
                }

                let excludedWindows = content.windows.filter { excludeWindowIDs.contains($0.windowID) }
                Log.line("excluding \(excludedWindows.count)/\(excludeWindowIDs.count) overlay window(s) from capture")
                let filter = SCContentFilter(display: scDisplay, excludingWindows: excludedWindows)

                let cfg = SCStreamConfiguration()
                cfg.width = Int(scDisplay.width) * self.scaleFactor(displayID)
                cfg.height = Int(scDisplay.height) * self.scaleFactor(displayID)
                cfg.pixelFormat = kCVPixelFormatType_32BGRA
                cfg.showsCursor = false                      // we draw our own mirrored cursor (M3)
                cfg.queueDepth = 5
                cfg.minimumFrameInterval = CMTime(value: 1, timescale: 60)
                cfg.colorSpaceName = CGColorSpace.sRGB

                let stream = SCStream(filter: filter, configuration: cfg, delegate: self)
                try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: self.frameQueue)
                try await stream.startCapture()
                guard self.installStartedStream(stream) else {
                    await self.stopStream(stream)
                    return
                }
                Log.line("capture started for display \(displayID) at \(cfg.width)x\(cfg.height)")
            } catch {
                if self.isRunning {
                    self.reportError("start failed: \(error.localizedDescription)")
                    self.finishStartFailure()
                } else {
                    self.releaseSelfRetainIfIdle()
                }
            }
        }
    }

    func stop() {
        stateLock.lock()
        shouldRun = false
        let streamToStop = stream
        stream = nil
        startTask?.cancel()
        startTask = nil
        if streamToStop != nil {
            pendingStopCount += 1
        }
        stateLock.unlock()

        guard let streamToStop else {
            releaseSelfRetainIfIdle()
            return
        }

        streamToStop.stopCapture { [weak self] error in
            if let error {
                Log.line("stream stop returned error: \(error.localizedDescription)")
            }
            self?.completePendingStop()
        }
    }

    private func scaleFactor(_ id: CGDirectDisplayID) -> Int {
        Int(Displays.screen(for: id)?.backingScaleFactor ?? 1)
    }

    // MARK: SCStreamOutput
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard isCurrentStream(stream) else { return }
        guard type == .screen, CMSampleBufferIsValid(sb) else { return }

        // Skip non-complete / idle frames.
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
           let raw = attachments.first?[.status] as? Int,
           let status = SCFrameStatus(rawValue: raw), status != .complete {
            return
        }
        guard let pb = CMSampleBufferGetImageBuffer(sb),
              let surface = CVPixelBufferGetIOSurface(pb)?.takeUnretainedValue() else { return }

        frameCount += 1
        if frameCount == 1 || frameCount % 120 == 0 {
            Log.line("frame \(frameCount) \(CVPixelBufferGetWidth(pb))x\(CVPixelBufferGetHeight(pb))")
        }
        let retainedSurface = Unmanaged.passRetained(surface)
        DispatchQueue.main.async { [weak self] in
            let surface = retainedSurface.takeRetainedValue()
            guard let self, self.isCurrentStream(stream) else { return }
            self.onSurface?(surface)
        }
    }

    // MARK: SCStreamDelegate
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        let message = "stream stopped: \(error.localizedDescription)"
        Log.line(message)
        guard clearIfCurrentStream(stream) else {
            releaseSelfRetainIfIdle()
            return
        }
        reportError(message)
        releaseSelfRetainIfIdle()
    }

    private var isRunning: Bool {
        stateLock.lock()
        let running = shouldRun
        stateLock.unlock()
        return running
    }

    private func installStartedStream(_ stream: SCStream) -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard shouldRun else { return false }
        self.stream = stream
        startTask = nil
        return true
    }

    private func isCurrentStream(_ stream: SCStream) -> Bool {
        stateLock.lock()
        let current = shouldRun && self.stream === stream
        stateLock.unlock()
        return current
    }

    private func clearIfCurrentStream(_ stream: SCStream) -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard self.stream === stream else { return false }
        self.stream = nil
        shouldRun = false
        startTask = nil
        return true
    }

    private func releaseSelfRetainIfIdle() {
        stateLock.lock()
        let idle = !shouldRun && stream == nil && pendingStopCount == 0 && startTask == nil
        if idle {
            selfRetain = nil
        }
        stateLock.unlock()
    }

    private func completePendingStop() {
        stateLock.lock()
        if pendingStopCount > 0 {
            pendingStopCount -= 1
        }
        let idle = !shouldRun && stream == nil && pendingStopCount == 0 && startTask == nil
        if idle {
            selfRetain = nil
        }
        stateLock.unlock()
    }

    private func finishStartFailure() {
        stateLock.lock()
        shouldRun = false
        startTask = nil
        let idle = stream == nil && pendingStopCount == 0
        if idle {
            selfRetain = nil
        }
        stateLock.unlock()
    }

    private func reportError(_ message: String) {
        DispatchQueue.main.async { [weak self] in
            self?.onError?(message)
        }
    }

    private func stopStream(_ stream: SCStream) async {
        await withCheckedContinuation { continuation in
            stream.stopCapture { _ in
                continuation.resume()
            }
        }
    }
}
