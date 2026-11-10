import Foundation

@MainActor
public final class EngineLauncherService {
    public static let shared = EngineLauncherService()

    private init() {}

    public func findGameBinary() -> String? {
        let candidates = [
            FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Documents/GitHub/eod-recomp/build/eod").path,
            "build/eod",
            "../build/eod"
        ]
        for c in candidates {
            if FileManager.default.isExecutableFile(atPath: c) {
                return c
            }
        }
        return nil
    }

    public func findDiscDataDir() -> String? {
        let candidates = [
            FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/TDEODRecomp/disc").path,
            "disc"
        ]
        for c in candidates {
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: c, isDirectory: &isDir), isDir.boolValue {
                return c
            }
        }
        return nil
    }

    public func launchGame(headless: Bool = false, frames: Int? = nil, completion: @escaping @MainActor @Sendable (Int32, String) -> Void) {
        guard let binaryPath = findGameBinary() else {
            completion(-1, "Game binary 'build/eod' not found or not executable.")
            return
        }

        guard let discDir = findDiscDataDir() else {
            completion(-2, "Disc assets directory not found.")
            return
        }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: binaryPath)
        
        var args = ["--data", discDir]
        if headless {
            args.append("--headless")
        }
        if let f = frames {
            args.append(contentsOf: ["--frames", "\(f)"])
        }
        task.arguments = args

        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe

        Task.detached(priority: .userInitiated) {
            do {
                try task.run()
                task.waitUntilExit()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(data: data, encoding: .utf8) ?? ""
                await completion(task.terminationStatus, output)
            } catch {
                await completion(-3, "Failed to launch process: \(error.localizedDescription)")
            }
        }
    }
}
