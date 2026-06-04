import Foundation

/// Lightweight file logger so we can diagnose a windowless agent app.
/// Writes to /tmp/screenflip.log (and mirrors to NSLog).
enum Log {
    static let path = "/tmp/screenflip.log"
    private static let q = DispatchQueue(label: "io.vbar.screenflip.log")
    private static let fmt: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f
    }()

    static func line(_ msg: String) {
        NSLog("screenflip: %@", msg)
        q.async {
            let stamped = "[\(fmt.string(from: Date()))] \(msg)\n"
            if let data = stamped.data(using: .utf8) {
                if let fh = FileHandle(forWritingAtPath: path) {
                    fh.seekToEndOfFile(); fh.write(data); fh.closeFile()
                } else {
                    try? stamped.write(toFile: path, atomically: true, encoding: .utf8)
                }
            }
        }
    }

    static func reset() {
        q.sync {
            try? "".write(toFile: path, atomically: true, encoding: .utf8)
        }
    }
}
