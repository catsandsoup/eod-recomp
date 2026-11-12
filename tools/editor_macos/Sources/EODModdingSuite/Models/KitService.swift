import Darwin
import Foundation

/// Client for the persistent Kit service (tools/kit_service, docs/KIT_SERVICE.md).
///
/// Requests are newline-delimited JSON-RPC 2.0 over the Unix socket named in
/// `<workspace>/service/service.json`. The service is started on first use
/// (`server.py --detach`, serialised by `service/start.lock`). Each call uses
/// its own connection, so cancelling a task just shuts that socket down.
final class KitServiceClient: @unchecked Sendable {
    static let shared = KitServiceClient()

    enum Failure: Error { case unavailable(String), closed }

    private let lock = NSLock()
    private var nextID = 0
    private var unavailableUntil = Date.distantPast

    static var workspace: URL {
        if let value = ProcessInfo.processInfo.environment["EOD_KIT_WORKSPACE"], !value.isEmpty {
            return URL(fileURLWithPath: (value as NSString).expandingTildeInPath)
        }
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/EODCreationKit")
    }
    static var disabled: Bool { ProcessInfo.processInfo.environment["EOD_KIT_SERVICE"] == "0" }
    static var serverScript: URL? {
        let helper = RuntimePaths.helper.deletingLastPathComponent()
        let script = helper.appendingPathComponent("kit_service/server.py")
        return FileManager.default.fileExists(atPath: script.path) ? script : nil
    }

    /// One JSON-RPC call. Returns the raw response line (decoded by the caller).
    /// `params` is the JSON object to send (serialised by the caller, so nothing non-Sendable crosses actors).
    func call(method: String, params: Data) async throws -> Data {
        let id: Int = lock.withLock { nextID += 1; return nextID }
        let line = Data("{\"jsonrpc\":\"2.0\",\"id\":\(id),\"method\":\"\(method)\",\"params\":".utf8) + params + Data("}\n".utf8)
        let workspace = Self.workspace
        let fd = try await Task.detached(priority: .userInitiated) { () throws -> Int32 in
            if let fd = try? Self.connect(workspace) { return fd }
            try Self.start(workspace)
            return try Self.connect(workspace)
        }.value
        return try await withTaskCancellationHandler {
            let data = try await Task.detached(priority: .userInitiated) { () throws -> Data in
                defer { close(fd) }
                try Self.send(fd, line)
                return try Self.receiveLine(fd)
            }.value
            try Task.checkCancellation()
            return data
        } onCancel: {
            shutdown(fd, SHUT_RDWR)
        }
    }

    /// Whether to try the service now; after a failed start, the helper is used for a minute.
    func shouldTry() -> Bool {
        if Self.disabled || Self.serverScript == nil { return false }
        return lock.withLock { Date() >= unavailableUntil }
    }

    func markUnavailable(_ reason: String) {
        lock.withLock { unavailableUntil = Date().addingTimeInterval(60) }
        FileHandle.standardError.write(Data("EOD Creation Kit: asset service unavailable (\(reason)); using one helper process per request\n".utf8))
    }

    // MARK: Socket plumbing (blocking; always called off the main actor)

    private static func socketPath(_ workspace: URL) -> String? {
        let info = workspace.appendingPathComponent("service/service.json")
        guard let data = try? Data(contentsOf: info),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return object["socket"] as? String
    }

    private static func connect(_ workspace: URL) throws -> Int32 {
        guard let path = socketPath(workspace) else { throw Failure.unavailable("not running") }
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw Failure.unavailable("socket() failed") }
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        let capacity = MemoryLayout.size(ofValue: address.sun_path)
        guard bytes.count < capacity else { close(fd); throw Failure.unavailable("socket path too long") }
        withUnsafeMutableBytes(of: &address.sun_path) { raw in
            raw.copyBytes(from: bytes)
            raw[bytes.count] = 0
        }
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard result == 0 else { close(fd); throw Failure.unavailable("not running") }
        return fd
    }

    private static func send(_ fd: Int32, _ data: Data) throws {
        try data.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let n = Darwin.write(fd, raw.baseAddress! + offset, raw.count - offset)
                if n < 0 && errno == EINTR { continue }
                guard n > 0 else { throw Failure.closed }
                offset += n
            }
        }
    }

    private static func receiveLine(_ fd: Int32) throws -> Data {
        var output = Data()
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        while true {
            let n = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if n < 0 && errno == EINTR { continue }
            guard n > 0 else { throw Failure.closed }
            if let end = buffer[0..<n].firstIndex(of: 0x0A) {
                output.append(contentsOf: buffer[0..<end])
                return output
            }
            output.append(contentsOf: buffer[0..<n])
        }
    }

    /// Start `server.py --detach` once, even when several calls race (flock on service/start.lock).
    private static func start(_ workspace: URL) throws {
        guard let script = serverScript else { throw Failure.unavailable("kit_service is not installed") }
        let folder = workspace.appendingPathComponent("service")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let lockFD = open(folder.appendingPathComponent("start.lock").path, O_CREAT | O_RDWR, 0o600)
        guard lockFD >= 0 else { throw Failure.unavailable("cannot open start.lock") }
        defer { close(lockFD) }
        let deadline = Date().addingTimeInterval(60)
        while flock(lockFD, LOCK_EX | LOCK_NB) != 0 {
            guard Date() < deadline else { throw Failure.unavailable("another client is starting the service") }
            usleep(50_000)
        }
        defer { flock(lockFD, LOCK_UN) }
        if let fd = try? connect(workspace) { close(fd); return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = ["-I", script.path, "--workspace", workspace.path, "--detach"]
        var environment = ProcessInfo.processInfo.environment
        environment["EOD_KIT_WORKSPACE"] = workspace.path
        process.environment = environment
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw Failure.unavailable("service did not start; see \(folder.appendingPathComponent("service.log").path)")
        }
    }
}

struct RPCFailure: Decodable { let code: Int; let message: String }
struct RPCResponse<T: Decodable>: Decodable { let result: T?; let error: RPCFailure? }
struct GameLock: Decodable { let resource: String }
