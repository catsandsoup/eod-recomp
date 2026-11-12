import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

struct ArchiveRecord: Decodable, Identifiable {
    var id: String { path }
    let path: String
    let name: String
    let count: Int
    let size: Int
}
struct AssetRecord: Decodable, Identifiable {
    var id: String { name }
    let name: String
    let type: String
    let size: Int
}
struct AssetField: Decodable {
    let id: String
    let sectionIndex: Int
    let pairIndex: Int
    let key: String
    let value: String
    let original: String
    let editable: Bool
    let featured: Bool?
    let unit: String
}
struct AssetSection: Decodable { let name: String; let fields: [AssetField] }
struct MeshPart: Decodable {
    let name: String
    let positions: [[Double]]
    let normals: [[Double]]
    let uvs: [[Double]]
    let triangles: [[Int32]]
    let texture: String?
    let textureError: String?
}
struct AssetDetail: Decodable {
    let kind: String
    let name: String
    let type: String
    let size: Int
    let sha256: String
    let sections: [AssetSection]?
    let parts: [MeshPart]?
    let image: String?
    let metadata: [String: Int]?
    let message: String?
    let hex: String?
    let paintDescription: String?
    let format: FormatView?
    let warnings: [String]?
    let edited: Bool?
}
struct EnvironmentRecord: Decodable { let track: String; let textures: [String]; let skyTextures: [String]; let worldGeometryEditable: Bool }
struct TextureChange: Decodable {
    let kind: String
    let data: String
    let sha256: String
    var dictionary: [String: String] { ["kind": kind, "data": data, "sha256": sha256] }
}
struct TexturePreparation: Decodable { let edits: [String: TextureChange]; let image: String; let previewAsset: String; let count: Int }
struct ExportedAsset: Decodable { let path: String; let bytes: Int; let sha256: String }
struct Catalog: Decodable { let archives: [ArchiveRecord]; let assetCount: Int; let errors: [String] }
struct EntryList: Decodable { let entries: [AssetRecord] }
struct HostRecord: Decodable, Equatable { let image: String; let sha256: String; let k: Int; let width: Int; let height: Int }
struct LoadedProject: Decodable {
    let disc: String
    let selectedArchive: String?
    let selectedAsset: String?
    let testCarArchive: String?
    let testTrackArchive: String?
    let hostTextures: [String: HostRecord]?
}
struct ExportFailure: Decodable { let assetId: String; let error: String? }
struct ExportStatus: Decodable {
    let state: String
    let done: Int
    let total: Int
    let assetId: String?
    let resumable: Bool
    let exported: Int?
    let failed: Int?
    let unsupported: Int?
    let failures: [ExportFailure]?
    let error: String?
}
struct ReplacementSize: Decodable, Equatable { let width: Int; let height: Int }
struct ReplacementCheck: Decodable {
    let mode: String?
    let scale: Int?
    let png: ReplacementSize
    let original: ReplacementSize
    let reasons: [String: String]
}
struct HostImport: Decodable { let assetId: String; let image: String; let sha256: String; let k: Int; let width: Int; let height: Int }
struct PackageResult: Decodable { let path: String; let id: String; let version: String; let native: Int; let host: Int }
struct ProfilePackage: Decodable, Identifiable { let id: String; let version: String; let enabled: Bool; let native: Int?; let host: Int?; let error: String? }
struct ProfileConflict: Decodable { let key: String; let packages: [String]; let accepted: Bool }
struct ProfileActive: Decodable { let composed: Bool }
struct ProfileStatus: Decodable {
    let packages: [ProfilePackage]
    let conflicts: [ProfileConflict]
    let blocked: [String]
    let active: ProfileActive
}
struct RemovedHost: Decodable { let assetId: String }
struct Replacement { let png: String; let check: ReplacementCheck }
struct SavedProject: Decodable { let project: String; let overlay: String; let archivePath: String; let sha256: String; let bytes: Int }
struct LaunchPlan: Decodable {
    let executable: String
    let arguments: [String]
    let environment: [String: String]
    let logPath: String
    let car: String
    let track: String
    let carNote: String?
}
struct Envelope<T: Decodable>: Decodable { let ok: Bool; let result: T?; let error: String?; let detail: String? }

enum StudioError: LocalizedError {
    case message(String)
    var errorDescription: String? { switch self { case .message(let text): return text } }
}

enum RuntimePaths {
    static var repository: URL {
        if let resource = Bundle.main.url(forResource: "studio-runtime", withExtension: "json"),
           let data = try? Data(contentsOf: resource),
           let config = try? JSONDecoder().decode([String: String].self, from: data) {
            if config["portable"] == "1" {
                return FileManager.default.homeDirectoryForCurrentUser
                    .appendingPathComponent("Library/Application Support/EODCreationKit")
            }
            if let root = config["repository"] { return URL(fileURLWithPath: root) }
        }
        #if DEBUG
        var source = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { source.deleteLastPathComponent() }
        return source
        #else
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/EODCreationKit")
        #endif
    }
    static var gameExecutable: String {
        guard let resource = Bundle.main.url(forResource: "studio-runtime", withExtension: "json"),
              let data = try? Data(contentsOf: resource),
              let config = try? JSONDecoder().decode([String: String].self, from: data)
        else { return "" }
        if let relative = config["bundledGameExecutable"], let resources = Bundle.main.resourceURL {
            return resources.appendingPathComponent(relative).path
        }
        return config["gameExecutable"] ?? ""
    }
    static var initialProject: String? {
        guard let resource = Bundle.main.url(forResource: "studio-runtime", withExtension: "json"),
              let data = try? Data(contentsOf: resource),
              let config = try? JSONDecoder().decode([String: String].self, from: data)
        else { return nil }
        return config["initialProject"]
    }
    static var helper: URL {
        if let resources = Bundle.main.resourceURL {
            let bundled = resources.appendingPathComponent("Backend/modding_backend.py")
            if FileManager.default.fileExists(atPath: bundled.path) { return bundled }
        }
        return repository.appendingPathComponent("tools/modding_backend.py")
    }
}

/// Actions that only read; safe to repeat through the helper if the service drops mid-request.
private let readOnlyActions: Set<String> = ["catalog", "entries", "environment", "inspect", "load", "validate",
                                             "profile_status", "export_batch_status", "reimport_check", "launch_plan"]

@MainActor
enum Backend {
    /// Same request and envelope as tools/modding_backend.py, served by the persistent Kit service
    /// when it can be reached or started; otherwise one helper process per request (logged).
    static func call<T: Decodable & Sendable>(_ request: [String: Any], as: T.Type) async throws -> T {
        let client = KitServiceClient.shared
        if client.shouldTry() {
            do {
                let line = try await client.call(method: "backend", params: try JSONSerialization.data(withJSONObject: request))
                return try await decode(line, as: T.self)
            } catch let failure as KitServiceClient.Failure {
                switch failure {
                case .unavailable(let reason): client.markUnavailable(reason)
                case .closed:
                    if Task.isCancelled { throw CancellationError() }
                    guard readOnlyActions.contains(request["action"] as? String ?? "") else {
                        throw StudioError.message("The asset service stopped before confirming this change. Check the project, then try again.")
                    }
                    client.markUnavailable("connection closed")
                }
            }
        }
        return try await spawn(request, as: T.self)
    }

    /// Decodes a service response off the main actor (model and format views can be several MB).
    nonisolated static func decode<T: Decodable & Sendable>(_ line: Data, as: T.Type) async throws -> T {
        try await Task.detached(priority: .userInitiated) { () throws -> T in
            let response = try JSONDecoder().decode(RPCResponse<Envelope<T>>.self, from: line)
            if let envelope = response.result { return try unwrap(envelope) }
            throw StudioError.message(response.error?.message ?? "Asset service returned no result")
        }.value
    }

    nonisolated static func unwrap<T: Decodable>(_ envelope: Envelope<T>) throws -> T {
        guard envelope.ok, let result = envelope.result else {
            if let detail = envelope.detail, !detail.isEmpty {
                FileHandle.standardError.write(Data("EOD Creation Kit backend: \(detail)\n".utf8))
            }
            throw StudioError.message(envelope.error ?? "Asset backend returned no result")
        }
        return result
    }

    /// The original transport: a fresh helper process per request.
    static func spawn<T: Decodable & Sendable>(_ request: [String: Any], as: T.Type) async throws -> T {
        let input = try JSONSerialization.data(withJSONObject: request)
        let helper = RuntimePaths.helper.path
        let data = try await Task.detached(priority: .userInitiated) {
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
            task.arguments = ["-I", helper]
            let stdin = Pipe(), stdout = Pipe()
            task.standardInput = stdin
            task.standardOutput = stdout
            // Diagnostics go to the app's stderr, never into its JSON response.
            task.standardError = FileHandle.standardError
            try task.run()
            try stdin.fileHandleForWriting.write(contentsOf: input)
            try stdin.fileHandleForWriting.close()
            let bytes = stdout.fileHandleForReading.readDataToEndOfFile()
            task.waitUntilExit()
            return try unwrap(try JSONDecoder().decode(Envelope<T>.self, from: bytes))
        }.value
        return data
    }

    /// Claim the single game-test slot shared with MCP runs. Returns false only when another run holds it.
    static func acquireGame() async throws -> Bool {
        let client = KitServiceClient.shared
        guard client.shouldTry() else { return true }
        do {
            let params: [String: Any] = ["resource": "game", "pid": Int(ProcessInfo.processInfo.processIdentifier), "owner": "app"]
            let line = try await client.call(method: "lock.acquire", params: try JSONSerialization.data(withJSONObject: params))
            let response = try JSONDecoder().decode(RPCResponse<GameLock>.self, from: line)
            if let error = response.error { throw StudioError.message(error.message) }
            return true
        } catch is KitServiceClient.Failure { return true }
    }

    static func releaseGame() async {
        let client = KitServiceClient.shared
        guard client.shouldTry() else { return }
        let params: [String: Any] = ["resource": "game", "pid": Int(ProcessInfo.processInfo.processIdentifier), "owner": "app"]
        guard let body = try? JSONSerialization.data(withJSONObject: params) else { return }
        _ = try? await client.call(method: "lock.release", params: body)
    }
}

@MainActor
final class StudioState: ObservableObject {
    @Published var disc: String
    @Published var executable: String
    @Published var project: String
    @Published var archives: [ArchiveRecord] = []
    @Published var assets: [AssetRecord] = []
    @Published var archive: String = ""
    @Published var asset: String = ""
    @Published var detail: AssetDetail?
    @Published var environmentDetail: EnvironmentRecord?
    @Published var pendingTextures: [String: TextureChange] = [:]
    @Published var tintRed = "1.0"
    @Published var tintGreen = "1.0"
    @Published var tintBlue = "1.0"
    @Published var nativePreview: String?
    @Published var savedTestTrack: String?
    @Published var physicsDetail: AssetDetail?
    @Published var editingAsset = ""
    private var editingDetail: AssetDetail?
    @Published var studio = "Assets"
    @Published var values: [String: String] = [:]
    @Published var search = ""
    @Published var assetCount = 0
    /// Operations in flight, by lane. Reads (catalog, entries, inspect, prepare) never wait for
    /// exports, builds, saves or game runs; only conflicting project/profile writes exclude each other.
    @Published private(set) var running: [Operation: Int] = [:]
    @Published var status = ""
    @Published var error: String?
    @Published var gameRunning = false
    @Published var gameLog = ""
    @Published var logPath: String?
    @Published var savedTestArchive: String?
    @Published var exportStatus: ExportStatus?
    @Published var exportDestination: String?
    @Published var replacement: Replacement?
    @Published var hostTextures: [String: HostRecord] = [:]
    @Published var profile: ProfileStatus?
    @Published var builtPackage: PackageResult?
    var profileRoot: String?
    var jobsRoot: String?
    private var archiveTask: Task<Void, Never>?
    private var inspectTask: Task<Void, Never>?
    private var usingProfile = false
    private var savedValues: [String: String] = [:]
    private var undoStack: [(String, String, String)] = []
    private var redoStack: [(String, String, String)] = []
    private var process: Process?
    private var sessionStopped = false
    private let preferences: UserDefaults
    private var started = false

    init(preferences: UserDefaults = .standard) {
        self.preferences = preferences
        let prefs = preferences
        disc = prefs.string(forKey: "retailStudio.disc") ?? FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/TDEODRecomp/disc").path
        let root = RuntimePaths.repository
        executable = prefs.string(forKey: "retailStudio.executable") ?? RuntimePaths.gameExecutable
        project = prefs.string(forKey: "retailStudio.project") ?? RuntimePaths.initialProject ?? root.appendingPathComponent("mods/CreationKitDevelopment/project.eodproject").path
        if let data = try? Data(contentsOf: URL(fileURLWithPath: project)),
           let saved = try? JSONDecoder().decode(LoadedProject.self, from: data) {
            savedTestTrack = saved.testTrackArchive
            savedTestArchive = saved.testCarArchive ?? (saved.selectedArchive?.lowercased().hasSuffix(".car") == true ? saved.selectedArchive : (saved.selectedArchive == nil ? nil : "DATA/CAR/ALGAR.CAR"))
            hostTextures = saved.hostTextures ?? [:]
        }
    }
    var dirty: Bool { values != savedValues || !pendingTextures.isEmpty }
    var canUndo: Bool { !undoStack.isEmpty }
    var canRedo: Bool { !redoStack.isEmpty }
    var canSave: Bool { !isWriting && !gameRunning && ((!editingAsset.isEmpty && !values.isEmpty) || !pendingTextures.isEmpty) }
    var profileActive: Bool { profile?.packages.contains(where: \.enabled) == true && profile?.active.composed == true }
    var exportRunning: Bool { exportStatus?.state == "running" }
    var canTest: Bool { !isWriting && !gameRunning && !dirty && (savedTestArchive != nil || profileActive) && FileManager.default.fileExists(atPath: project) }

    enum Operation: Hashable, Sendable {
        case catalog, load, entries, inspect, prepare, file, export, game
        /// Writes to the project file or overlay (save, host import/remove).
        case write
        case build, profile
    }
    func begin(_ operation: Operation = .write) { running[operation, default: 0] += 1 }
    func end(_ operation: Operation = .write) {
        guard let count = running[operation] else { return }
        running[operation] = count > 1 ? count - 1 : nil
    }
    func isRunning(_ operation: Operation) -> Bool { running[operation] != nil }
    /// A project or profile write is in flight; another write would conflict with it.
    var isWriting: Bool { isRunning(.write) || isRunning(.build) || isRunning(.profile) }
    /// The selected archive or asset is still loading (for spinners).
    var loading: Bool { isRunning(.catalog) || isRunning(.load) || isRunning(.entries) || isRunning(.inspect) || isRunning(.prepare) }
    /// Compatibility for views not yet moved to `isWriting` / `loading`: true only while a conflicting
    /// write runs, so selecting and inspecting stay enabled during exports, builds and game runs.
    var busy: Bool { isWriting }
    private func report(_ failure: Error) {
        if failure is CancellationError { return }
        error = failure.localizedDescription
    }
    var currentAssetID: String { archive.isEmpty || asset.isEmpty ? "" : archive + "#" + asset }
    var currentHost: HostRecord? { hostTextures.first { $0.key.caseInsensitiveCompare(currentAssetID) == .orderedSame }?.value }
    var filteredAssets: [AssetRecord] { assets.filter { search.isEmpty || $0.name.localizedCaseInsensitiveContains(search) || $0.type.localizedCaseInsensitiveContains(search) } }

    func request(_ action: String) -> [String: Any] {
        var input: [String: Any] = ["action": action, "disc": disc, "project": project, "archive": archive, "asset": asset]
        if let texture = pendingTextures[asset] { input["textureEdit"] = texture.dictionary }
        input["textureEdits"] = pendingTextures.mapValues(\.dictionary)
        if let profileRoot { input["profileRoot"] = profileRoot }
        if let jobsRoot { input["jobsRoot"] = jobsRoot }
        return input
    }
    func remember() {
        preferences.set(disc, forKey: "retailStudio.disc")
        preferences.set(project, forKey: "retailStudio.project")
        preferences.set(executable, forKey: "retailStudio.executable")
    }
    func permitDiscard() -> Bool {
        guard dirty else { return true }
        let dialog = NSAlert()
        dialog.messageText = "Discard unsaved asset edits?"
        dialog.addButton(withTitle: "Cancel")
        dialog.addButton(withTitle: "Discard")
        return dialog.runModal() == .alertSecondButtonReturn
    }
    func startup() async {
        guard !started else { return }
        started = true
        if FileManager.default.fileExists(atPath: project) {
            await loadProject(project)
            if let openingError = error {
                await refresh()
                error = openingError
            }
        } else { await refresh(); await refreshProfile() }
    }
    func refresh() async {
        guard !isRunning(.catalog) else { return }
        begin(.catalog); error = nil
        defer { end(.catalog) }
        do {
            let result = try await Backend.call(request("catalog"), as: Catalog.self)
            archives = result.archives; assetCount = result.assetCount
            status = "\(archives.count) archives · \(assetCount) assets"
            if !result.errors.isEmpty { error = result.errors.joined(separator: "\n") }
            remember()
        } catch { self.error = error.localizedDescription }
    }
    func selectArchive(_ value: String) {
        guard value != archive, permitDiscard() else { return }
        archive = value; asset = ""; detail = nil; physicsDetail = nil; editingAsset = ""; values = [:]; savedValues = [:]
        undoStack = []; redoStack = []; assets = []; pendingTextures = [:]; environmentDetail = nil; nativePreview = nil
        error = nil
        archiveTask?.cancel(); inspectTask?.cancel()
        begin(.entries)
        archiveTask = Task {
            defer { end(.entries) }
            do {
                let entries = try await Backend.call(request("entries"), as: EntryList.self).entries
                guard archive == value, !Task.isCancelled else { return }
                assets = entries
                try await loadPhysics()
                guard archive == value, !Task.isCancelled else { return }
                if archive.lowercased().hasSuffix(".trk") {
                    let environment = try await Backend.call(request("environment"), as: EnvironmentRecord.self)
                    guard archive == value, !Task.isCancelled else { return }
                    environmentDetail = environment
                    asset = environmentDetail?.skyTextures.first ?? assets.first?.name ?? ""
                } else {
                asset = assets.first(where: { $0.name.lowercased() == "low1.mod" })?.name ?? assets.first?.name ?? ""
                }
                if !asset.isEmpty { await inspectSelection() }
            }
            catch { report(error) }
        }
    }
    func selectAsset(_ value: String) {
        guard value != asset else { return }
        if assets.first(where: { $0.name == value })?.type == "VALT" && value != editingAsset && !permitDiscard() { return }
        asset = value; detail = nil; status = "\(archive) / \(asset)"
        inspectTask?.cancel()
        begin(.inspect)  // counted before the task starts so `loading` is true from the click
        inspectTask = Task { defer { end(.inspect) }; await inspectSelection() }
    }
    func loadPhysics() async throws {
        physicsDetail = nil
        guard archive.lowercased().hasSuffix(".car"),
              let name = assets.first(where: { $0.name.lowercased() == "physics.val" })?.name else { return }
        var input = request("inspect"); input["asset"] = name
        let owner = archive
        let physics = try await Backend.call(input, as: AssetDetail.self)
        guard archive == owner else { return }
        physicsDetail = physics
        editingDetail = physics
        editingAsset = name
        values = Dictionary(uniqueKeysWithValues: (physics.sections ?? []).flatMap { section in
            section.fields.filter(\.editable).map { ($0.id, $0.value) }
        })
        savedValues = values
    }
    func inspectSelection() async {
        begin(.inspect); error = nil
        defer { end(.inspect) }
        let target = currentAssetID
        do {
            let result = try await Backend.call(request("inspect"), as: AssetDetail.self)
            guard currentAssetID == target else { return }  // the selection moved on while this loaded
            detail = result
            if result.kind == "table", editingAsset != asset {
                editingAsset = asset
                editingDetail = result
                values = Dictionary((result.sections ?? []).flatMap { section in
                    section.fields.filter(\.editable).map { ($0.id, $0.value) }
                }, uniquingKeysWith: { first, _ in first })
                savedValues = values; undoStack = []; redoStack = []
            }
            status = "\(archive) / \(asset)"
        } catch { if currentAssetID == target { report(error) } }
    }
    func edit(_ key: String, _ new: String) {
        guard let old = values[key], old != new else { return }
        undoStack.append((key, old, new)); redoStack = []; values[key] = new
    }
    func undo() {
        guard canUndo, let item = undoStack.popLast() else { return }
        values[item.0] = item.1; redoStack.append(item)
    }
    func redo() {
        guard canRedo, let item = redoStack.popLast() else { return }
        values[item.0] = item.2; undoStack.append(item)
    }
    func save() {
        guard canSave else { return }
        var input = request("save")
        let savingTable = !editingAsset.isEmpty && (values != savedValues || pendingTextures.isEmpty)
        input["asset"] = savingTable ? editingAsset : asset
        input["textureEdits"] = pendingTextures.mapValues(\.dictionary)
        if savingTable {
            input["edits"] = (editingDetail?.sections ?? []).flatMap { section -> [[String: Any]] in
                section.fields.filter(\.editable).compactMap { field in
                    guard let value = values[field.id] else { return nil }
                    return ["section": section.name, "key": field.key, "sectionIndex": field.sectionIndex,
                            "pairIndex": field.pairIndex, "value": value]
                }
            }
        }
        let sentValues = values, sentArchive = archive, sentTextures = Set(pendingTextures.keys)
        begin(.write); error = nil
        Task {
            defer { end(.write) }
            do {
                let result = try await Backend.call(input, as: SavedProject.self)
                // Edits made while the save was in flight stay pending (and keep the project dirty).
                if archive == sentArchive {
                    savedValues = sentValues
                    pendingTextures = pendingTextures.filter { !sentTextures.contains($0.key) }
                }
                tintRed = "1.0"; tintGreen = "1.0"; tintBlue = "1.0"
                if archive.lowercased().hasSuffix(".trk") { savedTestTrack = archive }
                if archive.lowercased().hasSuffix(".car") { savedTestArchive = archive }
                if savedTestArchive == nil { savedTestArchive = "DATA/CAR/ALGAR.CAR" }
                remember()
                status = "Saved \(URL(fileURLWithPath: result.archivePath).lastPathComponent) · \(result.bytes.formatted()) bytes · SHA256 \(result.sha256.prefix(12))"
            } catch { report(error) }
        }
    }
    func prepareTexture(allSky: Bool = false, png: URL? = nil) {
        guard !isRunning(.prepare), !isWriting else { return }
        var input = request("prepare_texture")
        input["textureEdits"] = pendingTextures.mapValues(\.dictionary)
        input["allSky"] = allSky
        if let png { input["png"] = png.path }
        else {
            guard let red = Double(tintRed), let green = Double(tintGreen), let blue = Double(tintBlue),
                  [red, green, blue].allSatisfy({ $0.isFinite && $0 >= 0 }) else {
                error = "RGB multipliers must be finite nonnegative numbers"; return
            }
            input["tint"] = [red, green, blue]
        }
        begin(.prepare); error = nil
        Task {
            defer { end(.prepare) }
            do {
                let result = try await Backend.call(input, as: TexturePreparation.self)
                pendingTextures.merge(result.edits) { _, new in new }
                asset = result.previewAsset
                await inspectSelection()
                status = "\(result.count) texture edits pending · Save & Build to test"
            } catch { report(error) }
        }
    }
    func replaceTexture() {
        guard !isWriting else { return }
        let panel = NSOpenPanel(); panel.allowedContentTypes = [.png]
        if panel.runModal() == .OK, let url = panel.url { Task { await checkReplacement(url) } }
    }
    func checkReplacement(_ url: URL) async {
        guard !isRunning(.prepare) else { return }
        var input = request("reimport_check"); input["png"] = url.path
        begin(.prepare); error = nil
        defer { end(.prepare) }
        do {
            let check = try await Backend.call(input, as: ReplacementCheck.self)
            if check.mode == nil {
                replacement = nil
                error = check.reasons.keys.sorted().map { "\($0): \(check.reasons[$0]!)" }.joined(separator: "\n")
            } else { replacement = Replacement(png: url.path, check: check) }
        } catch { replacement = nil; self.error = error.localizedDescription }
    }
    func cancelReplacement() { replacement = nil }
    func applyReplacement() async {
        guard !isWriting, !gameRunning, let plan = replacement else { return }
        if plan.check.mode == "native" {
            replacement = nil
            prepareTexture(png: URL(fileURLWithPath: plan.png))
            return
        }
        var input = request("import_host"); input["png"] = plan.png
        begin(); error = nil
        defer { end() }
        do {
            let result = try await Backend.call(input, as: HostImport.self)
            hostTextures[result.assetId] = HostRecord(image: result.image, sha256: result.sha256, k: result.k, width: result.width, height: result.height)
            replacement = nil
            status = "High-resolution replacement recorded · \(result.width) × \(result.height)"
            builtPackage = nil
        } catch { self.error = error.localizedDescription }
    }
    func removeHostReplacement() async {
        guard !isWriting, !gameRunning, currentHost != nil else { return }
        begin(); error = nil
        defer { end() }
        do {
            _ = try await Backend.call(request("remove_host"), as: RemovedHost.self)
            hostTextures = hostTextures.filter { $0.key.caseInsensitiveCompare(currentAssetID) != .orderedSame }
            builtPackage = nil
        } catch { self.error = error.localizedDescription }
    }

    // MARK: Export all textures
    func exportAllTextures() {
        guard !isRunning(.export), !exportRunning else { return }
        let panel = NSOpenPanel(); panel.canChooseFiles = false; panel.canChooseDirectories = true; panel.canCreateDirectories = true
        panel.prompt = "Export"
        if panel.runModal() == .OK, let url = panel.url { Task { await startExport(to: url.path) } }
    }
    /// Runs as a service job in the export lane; selecting, inspecting, saving and testing stay available.
    func startExport(to destination: String) async {
        guard !isRunning(.export) else { return }
        begin(.export); error = nil
        defer { end(.export) }
        do {
            var input = request("export_batch_status"); input["destination"] = destination
            let before = try await Backend.call(input, as: ExportStatus.self)
            input["action"] = "export_batch_start"; input["resume"] = before.resumable
            struct Started: Decodable { let jobId: String }
            _ = try await Backend.call(input, as: Started.self)
            exportDestination = destination
            exportStatus = ExportStatus(state: "running", done: before.resumable ? before.done : 0, total: before.total, assetId: nil,
                                        resumable: true, exported: nil, failed: nil, unsupported: nil, failures: nil, error: nil)
            input["action"] = "export_batch_status"; input.removeValue(forKey: "resume")
            while true {
                try await Task.sleep(for: .milliseconds(400))
                let current = try await Backend.call(input, as: ExportStatus.self)
                exportStatus = current
                status = "Exporting textures · \(current.done.formatted()) of \(current.total.formatted())"
                if current.state != "running" { break }
            }
            finishExport()
        } catch { report(error) }
    }
    func finishExport() {
        guard let result = exportStatus else { return }
        let count = result.exported ?? 0
        switch result.state {
        case "complete":
            status = "Exported \(count.formatted()) textures" + ((result.failed ?? 0) + (result.unsupported ?? 0) > 0
                ? " · \((result.failed ?? 0) + (result.unsupported ?? 0)) not exported" : "")
        case "cancelled": status = "Export cancelled · \(result.done.formatted()) of \(result.total.formatted()) finished"
        default: error = result.error ?? "Export failed"
        }
        let failures = result.failures ?? []
        if !failures.isEmpty {
            error = failures.prefix(5).map { "\($0.assetId): \($0.error ?? "failed")" }.joined(separator: "\n")
                + (failures.count > 5 ? "\n+ \(failures.count - 5) more in export-manifest.json" : "")
        }
    }
    func cancelExport() async {
        guard exportRunning, let destination = exportDestination else { return }
        var input = request("export_batch_cancel"); input["destination"] = destination
        do { _ = try await Backend.call(input, as: ExportStatus.self) } catch { self.error = error.localizedDescription }
    }

    // MARK: Mods
    func refreshProfile() async {
        do { profile = try await Backend.call(request("profile_status"), as: ProfileStatus.self) }
        catch { profile = nil }
    }
    var hasModContent: Bool { !hostTextures.isEmpty || savedTestArchive != nil && FileManager.default.fileExists(atPath: project) }
    func buildPackage() async {
        guard !isWriting, !gameRunning, !dirty else { return }
        begin(.build); error = nil
        defer { end(.build) }
        do {
            let result = try await Backend.call(request("package_project"), as: PackageResult.self)
            builtPackage = result
            status = "Built \(result.id) \(result.version) · \(result.native) native · \(result.host) high-resolution"
        } catch { self.error = error.localizedDescription }
    }
    func profileOperation(_ action: String, id: String? = nil, package: String? = nil) async {
        guard !isWriting, !gameRunning else { return }
        var input = request(action)
        if let id { input["id"] = id }
        if let package { input["package"] = package }
        begin(.profile); error = nil
        defer { end(.profile) }
        do {
            struct Done: Decodable { let profile: String }
            _ = try await Backend.call(input, as: Done.self)
            status = "Profile updated"
        } catch { self.error = error.localizedDescription }
        await refreshProfile()
    }
    func installPackage() async {
        guard let built = builtPackage else { return }
        await profileOperation("profile_install", package: built.path)
    }
    func exportTexture() {
        guard detail?.kind == "texture" else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = asset + ".png"; panel.allowedContentTypes = [.png]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var input = request("export_texture"); input["destination"] = url.path
        begin(.file); error = nil
        Task {
            defer { end(.file) }
            do {
                let result = try await Backend.call(input, as: ExportedAsset.self)
                status = "Exported PNG · \(result.path)"
            } catch { self.error = error.localizedDescription }
        }
    }
    func discardTextureEdits() {
        guard !isWriting, !isRunning(.prepare) else { return }
        pendingTextures = [:]
        Task { await inspectSelection() }
    }
    func exportNativeAsset() {
        guard !asset.isEmpty else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = asset
        panel.message = "Export the original native asset bytes"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var input = request("export_native"); input["destination"] = url.path
        begin(.file); error = nil
        Task {
            defer { end(.file) }
            do {
                let exported = try await Backend.call(input, as: ExportedAsset.self)
                status = "Exported \(exported.bytes.formatted()) bytes · \(exported.path)"
            } catch { self.error = error.localizedDescription }
        }
    }
    func chooseDisc() {
        guard !isWriting, !gameRunning, permitDiscard() else { return }
        let panel = NSOpenPanel(); panel.canChooseFiles = false; panel.canChooseDirectories = true
        panel.message = "Choose the extracted game folder containing DATA and SLUS_209.10"
        if panel.runModal() == .OK, let url = panel.url {
            pendingTextures = [:]; environmentDetail = nil; nativePreview = nil; savedTestTrack = nil; savedTestArchive = nil
            disc = url.path; archive = ""; asset = ""; detail = nil; physicsDetail = nil; editingAsset = ""; assets = []; values = [:]; savedValues = [:]
            Task { await refresh() }
        }
    }
    func chooseExecutable() {
        guard !gameRunning else { return }
        let panel = NSOpenPanel(); panel.message = "Choose the native eod executable"
        if panel.runModal() == .OK, let url = panel.url { executable = url.path; remember() }
    }
    func newProject() {
        guard !isWriting, !gameRunning, permitDiscard() else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = "project.eodproject"
        panel.canCreateDirectories = true; panel.message = "Choose a separate folder for this mod project and its overlay"
        if panel.runModal() == .OK, let url = panel.url {
            pendingTextures = [:]; environmentDetail = nil; nativePreview = nil; savedTestTrack = nil
            hostTextures = [:]; replacement = nil; builtPackage = nil
            project = url.path; values = [:]; savedValues = [:]; undoStack = []; redoStack = []
            archive = ""; asset = ""; detail = nil; physicsDetail = nil; editingAsset = ""; assets = []
            savedTestArchive = nil
            remember()
        }
    }
    func openProject() {
        guard !isWriting, !gameRunning, permitDiscard() else { return }
        let panel = NSOpenPanel(); panel.message = "Choose an EOD retail mod project"
        if panel.runModal() == .OK, let url = panel.url { Task { await loadProject(url.path) } }
    }
    func loadProject(_ path: String) async {
        guard !isWriting, !isRunning(.load) else { return }
        begin(.load); error = nil
        archiveTask?.cancel(); inspectTask?.cancel()
        do {
            let loaded = try await Backend.call(["action": "load", "project": path], as: LoadedProject.self)
            let selectedArchive = loaded.selectedArchive ?? "", selectedAsset = loaded.selectedAsset ?? ""
            var input: [String: Any] = ["action": "catalog", "disc": loaded.disc, "project": path,
                "archive": selectedArchive, "asset": selectedAsset]
            let catalog = try await Backend.call(input, as: Catalog.self)
            var entries = EntryList(entries: [])
            var selected: AssetDetail?
            if !selectedArchive.isEmpty {
                input["action"] = "entries"
                entries = try await Backend.call(input, as: EntryList.self)
                if !selectedAsset.isEmpty {
                    input["action"] = "inspect"
                    selected = try await Backend.call(input, as: AssetDetail.self)
                }
            }
            var loadedEnvironment: EnvironmentRecord?
            if selectedArchive.lowercased().hasSuffix(".trk") {
                input["action"] = "environment"
                loadedEnvironment = try await Backend.call(input, as: EnvironmentRecord.self)
            }
            disc = loaded.disc; project = path; archive = selectedArchive; asset = selectedAsset
            hostTextures = loaded.hostTextures ?? [:]; replacement = nil; builtPackage = nil
            archives = catalog.archives; assetCount = catalog.assetCount; assets = entries.entries
            detail = selected; physicsDetail = nil; editingAsset = ""; pendingTextures = [:]; environmentDetail = nil; nativePreview = nil
            environmentDetail = loadedEnvironment
            savedTestTrack = loaded.testTrackArchive
            editingDetail = nil
            values = [:]; savedValues = [:]; undoStack = []; redoStack = []
            if let selected, selected.kind == "table" {
                editingAsset = selected.name
                editingDetail = selected
                if selected.name.lowercased() == "physics.val" { physicsDetail = selected }
                values = Dictionary((selected.sections ?? []).flatMap { section in
                    section.fields.filter(\.editable).map { ($0.id, $0.value) }
                }, uniquingKeysWith: { first, _ in first })
                savedValues = values
            }
            savedTestArchive = loaded.testCarArchive ?? (selectedArchive.lowercased().hasSuffix(".car") ? selectedArchive : (selectedArchive.isEmpty ? nil : "DATA/CAR/ALGAR.CAR")); remember()
            status = "Opened \(URL(fileURLWithPath: path).lastPathComponent)"
            end(.load)
            await refreshProfile()
            return
        } catch { report(error) }
        end(.load)
    }
    func testGame() {
        guard canTest else { return }
        usingProfile = profileActive
        var input = request(usingProfile ? "launch_profile" : "launch_plan"); input["executable"] = executable
        begin(.game); error = nil
        Task {
            defer { end(.game) }
            do {
                let plan = try await Backend.call(input, as: LaunchPlan.self)
                _ = try await Backend.acquireGame()
                let task = Process(); task.executableURL = URL(fileURLWithPath: plan.executable); task.arguments = plan.arguments
                var environment = ProcessInfo.processInfo.environment
                // An editor launched by a headless test shell must still start the user's visible session.
                for key in environment.keys where key.hasPrefix("EOD_") { environment.removeValue(forKey: key) }
                environment.merge(plan.environment) { _, new in new }
                task.environment = environment
                let url = URL(fileURLWithPath: plan.logPath)
                FileManager.default.createFile(atPath: url.path, contents: nil)
                let log = try FileHandle(forWritingTo: url)
                task.standardOutput = log; task.standardError = log
                sessionStopped = false
                do { try task.run() } catch { await Backend.releaseGame(); throw error }
                process = task; gameRunning = true; logPath = plan.logPath
                status = "Game process started · requested \(plan.car) / \(plan.track)" + (usingProfile ? " · dev profile" : "")
                    + (plan.carNote.map { " · \($0)" } ?? "")
                nativePreview = nil
                let preview = url.deletingLastPathComponent().appendingPathComponent("first-race.png").path
                Task { [weak owner = self] in
                    while owner?.gameRunning == true {
                        if NSImage(contentsOfFile: preview) != nil { owner?.nativePreview = preview; break }
                        try? await Task.sleep(for: .seconds(1))
                    }
                }
                Task.detached { [weak owner = self] in
                    task.waitUntilExit(); try? log.close()
                    let text = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
                    await owner?.gameEnded(code: task.terminationStatus, log: text)
                }
            } catch { self.error = error.localizedDescription }
        }
    }
    func gameEnded(code: Int32, log: String) {
        gameLog = log; gameRunning = false; process = nil
        Task { await Backend.releaseGame() }
        status = sessionStopped ? "Test session stopped · exit \(code)" : "Game exited with code \(code)"
        if code != 0 && !sessionStopped { error = "Game failed (\(code)). See the session log." }
    }
    func stopGame() { sessionStopped = true; process?.terminate() }

    func prepareToQuit() -> Bool {
        if isWriting {
            let dialog = NSAlert(); dialog.messageText = "Wait for the current save, build or install to finish."
            dialog.runModal(); return false
        }
        guard permitDiscard() else { return false }
        if gameRunning {
            let dialog = NSAlert(); dialog.messageText = "Stop the test session and quit?"
            dialog.addButton(withTitle: "Cancel"); dialog.addButton(withTitle: "Stop & Quit")
            guard dialog.runModal() == .alertSecondButtonReturn else { return false }
            stopGame()
        }
        return true
    }
}
