import AppKit
import Combine
import Foundation

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
}
struct ExportedAsset: Decodable { let path: String; let bytes: Int; let sha256: String }
struct Catalog: Decodable { let archives: [ArchiveRecord]; let assetCount: Int; let errors: [String] }
struct EntryList: Decodable { let entries: [AssetRecord] }
struct LoadedProject: Decodable { let disc: String; let selectedArchive: String; let selectedAsset: String; let testCarArchive: String? }
struct SavedProject: Decodable { let project: String; let overlay: String; let archivePath: String; let sha256: String; let bytes: Int }
struct LaunchPlan: Decodable {
    let executable: String
    let arguments: [String]
    let environment: [String: String]
    let logPath: String
    let car: String
}
struct Envelope<T: Decodable>: Decodable { let ok: Bool; let result: T?; let error: String? }

enum StudioError: LocalizedError {
    case message(String)
    var errorDescription: String? { switch self { case .message(let text): return text } }
}

enum RuntimePaths {
    static var repository: URL {
        if let resource = Bundle.main.url(forResource: "studio-runtime", withExtension: "json"),
           let data = try? Data(contentsOf: resource),
           let config = try? JSONDecoder().decode([String: String].self, from: data),
           let root = config["repository"] { return URL(fileURLWithPath: root) }
        var source = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { source.deleteLastPathComponent() }
        return source
    }
    static var helper: URL {
        if let resources = Bundle.main.resourceURL {
            let bundled = resources.appendingPathComponent("Backend/modding_backend.py")
            if FileManager.default.fileExists(atPath: bundled.path) { return bundled }
        }
        return repository.appendingPathComponent("tools/modding_backend.py")
    }
}

@MainActor
enum Backend {
    static func call<T: Decodable>(_ request: [String: Any], as: T.Type) async throws -> T {
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
            return bytes
        }.value
        let envelope = try JSONDecoder().decode(Envelope<T>.self, from: data)
        guard envelope.ok, let result = envelope.result else {
            throw StudioError.message(envelope.error ?? "Asset backend returned no result")
        }
        return result
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
    @Published var physicsDetail: AssetDetail?
    @Published var editingAsset = ""
    private var editingDetail: AssetDetail?
    @Published var studio = "Assets"
    @Published var values: [String: String] = [:]
    @Published var search = ""
    @Published var assetCount = 0
    @Published var busy = false
    @Published var status = ""
    @Published var error: String?
    @Published var gameRunning = false
    @Published var gameLog = ""
    @Published var logPath: String?
    @Published var savedTestArchive: String?
    private var savedValues: [String: String] = [:]
    private var undoStack: [(String, String, String)] = []
    private var redoStack: [(String, String, String)] = []
    private var process: Process?
    private var sessionStopped = false
    private let preferences: UserDefaults

    init(preferences: UserDefaults = .standard) {
        self.preferences = preferences
        let prefs = preferences
        disc = prefs.string(forKey: "retailStudio.disc") ?? FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/TDEODRecomp/disc").path
        let root = RuntimePaths.repository
        executable = prefs.string(forKey: "retailStudio.executable") ?? root.appendingPathComponent("build/eod").path
        project = prefs.string(forKey: "retailStudio.project") ?? root.appendingPathComponent("mods/Studio/project.eodproject").path
        if let data = try? Data(contentsOf: URL(fileURLWithPath: project)),
           let saved = try? JSONDecoder().decode(LoadedProject.self, from: data) {
            savedTestArchive = saved.testCarArchive ?? (saved.selectedArchive.lowercased().hasSuffix(".car") ? saved.selectedArchive : "DATA/CAR/ALGAR.CAR")
        }
    }
    var dirty: Bool { values != savedValues }
    var canUndo: Bool { !undoStack.isEmpty && !busy && !gameRunning }
    var canRedo: Bool { !redoStack.isEmpty && !busy && !gameRunning }
    var canSave: Bool { !busy && !gameRunning && !editingAsset.isEmpty && !values.isEmpty }
    var canTest: Bool { !busy && !gameRunning && !dirty && savedTestArchive != nil && FileManager.default.fileExists(atPath: project) }
    var filteredAssets: [AssetRecord] { assets.filter { search.isEmpty || $0.name.localizedCaseInsensitiveContains(search) || $0.type.localizedCaseInsensitiveContains(search) } }

    func request(_ action: String) -> [String: Any] {
        ["action": action, "disc": disc, "project": project, "archive": archive, "asset": asset]
    }
    func remember() {
        preferences.set(disc, forKey: "retailStudio.disc")
        preferences.set(project, forKey: "retailStudio.project")
        preferences.set(executable, forKey: "retailStudio.executable")
    }
    func permitDiscard() -> Bool {
        guard dirty else { return true }
        let dialog = NSAlert()
        dialog.messageText = "Discard unsaved physics edits?"
        dialog.addButton(withTitle: "Cancel")
        dialog.addButton(withTitle: "Discard")
        return dialog.runModal() == .alertSecondButtonReturn
    }
    func refresh() async {
        guard !busy else { return }
        busy = true; error = nil
        defer { busy = false }
        do {
            let result = try await Backend.call(request("catalog"), as: Catalog.self)
            archives = result.archives; assetCount = result.assetCount
            status = "\(archives.count) archives · \(assetCount) assets"
            if !result.errors.isEmpty { error = result.errors.joined(separator: "\n") }
            remember()
        } catch { self.error = error.localizedDescription }
    }
    func selectArchive(_ value: String) {
        guard !busy, !gameRunning, value != archive, permitDiscard() else { return }
        archive = value; asset = ""; detail = nil; physicsDetail = nil; editingAsset = ""; values = [:]; savedValues = [:]
        undoStack = []; redoStack = []; assets = []
        busy = true; error = nil
        Task {
            defer { busy = false }
            do {
                assets = try await Backend.call(request("entries"), as: EntryList.self).entries
                try await loadPhysics()
                asset = assets.first(where: { $0.name.lowercased() == "low1.mod" })?.name ?? assets.first?.name ?? ""
                if !asset.isEmpty { await inspectSelection() }
            }
            catch { self.error = error.localizedDescription }
        }
    }
    func selectAsset(_ value: String) {
        guard !busy, !gameRunning, value != asset else { return }
        if assets.first(where: { $0.name == value })?.type == "VALT" && value != editingAsset && !permitDiscard() { return }
        asset = value; detail = nil; busy = true; status = "\(archive) / \(asset)"
        Task { await inspectSelection() }
    }
    func loadPhysics() async throws {
        physicsDetail = nil
        guard archive.lowercased().hasSuffix(".car"),
              let name = assets.first(where: { $0.name.lowercased() == "physics.val" })?.name else { return }
        var input = request("inspect"); input["asset"] = name
        let physics = try await Backend.call(input, as: AssetDetail.self)
        physicsDetail = physics
        editingDetail = physics
        editingAsset = name
        values = Dictionary(uniqueKeysWithValues: (physics.sections ?? []).flatMap { section in
            section.fields.filter(\.editable).map { ($0.id, $0.value) }
        })
        savedValues = values
    }
    func inspectSelection() async {
        busy = true; error = nil
        defer { busy = false }
        do {
            let result = try await Backend.call(request("inspect"), as: AssetDetail.self)
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
        } catch { self.error = error.localizedDescription }
    }
    func edit(_ key: String, _ new: String) {
        guard !busy, !gameRunning, let old = values[key], old != new else { return }
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
        input["asset"] = editingAsset
        input["edits"] = (editingDetail?.sections ?? []).flatMap { section -> [[String: Any]] in
            section.fields.filter(\.editable).compactMap { field in
                guard let value = values[field.id] else { return nil }
                return ["section": section.name, "key": field.key, "sectionIndex": field.sectionIndex,
                        "pairIndex": field.pairIndex, "value": value]
            }
        }
        busy = true; error = nil
        Task {
            defer { busy = false }
            do {
                let result = try await Backend.call(input, as: SavedProject.self)
                savedValues = values
                if archive.lowercased().hasSuffix(".car") { savedTestArchive = archive }
                if savedTestArchive == nil { savedTestArchive = "DATA/CAR/ALGAR.CAR" }
                remember()
                status = "Saved \(URL(fileURLWithPath: result.archivePath).lastPathComponent) · \(result.bytes.formatted()) bytes · SHA256 \(result.sha256.prefix(12))"
            } catch { self.error = error.localizedDescription }
        }
    }
    func exportNativeAsset() {
        guard !busy, !asset.isEmpty else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = asset
        panel.message = "Export the original native asset bytes"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var input = request("export_native"); input["destination"] = url.path
        busy = true; error = nil
        Task {
            defer { busy = false }
            do {
                let exported = try await Backend.call(input, as: ExportedAsset.self)
                status = "Exported \(exported.bytes.formatted()) bytes · \(exported.path)"
            } catch { self.error = error.localizedDescription }
        }
    }
    func chooseDisc() {
        guard !busy, !gameRunning, permitDiscard() else { return }
        let panel = NSOpenPanel(); panel.canChooseFiles = false; panel.canChooseDirectories = true
        panel.message = "Choose the extracted game folder containing DATA and SLUS_209.10"
        if panel.runModal() == .OK, let url = panel.url {
            disc = url.path; archive = ""; asset = ""; detail = nil; physicsDetail = nil; editingAsset = ""; assets = []; values = [:]; savedValues = [:]
            Task { await refresh() }
        }
    }
    func chooseExecutable() {
        guard !busy, !gameRunning else { return }
        let panel = NSOpenPanel(); panel.message = "Choose the native eod executable"
        if panel.runModal() == .OK, let url = panel.url { executable = url.path; remember() }
    }
    func newProject() {
        guard !busy, !gameRunning, permitDiscard() else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = "project.eodproject"
        panel.canCreateDirectories = true; panel.message = "Choose a separate folder for this mod project and its overlay"
        if panel.runModal() == .OK, let url = panel.url {
            project = url.path; values = [:]; savedValues = [:]; undoStack = []; redoStack = []
            archive = ""; asset = ""; detail = nil; physicsDetail = nil; editingAsset = ""; assets = []
            savedTestArchive = nil
            remember()
        }
    }
    func openProject() {
        guard !busy, !gameRunning, permitDiscard() else { return }
        let panel = NSOpenPanel(); panel.message = "Choose an EOD retail mod project"
        if panel.runModal() == .OK, let url = panel.url { Task { await loadProject(url.path) } }
    }
    func loadProject(_ path: String) async {
        guard !busy else { return }
        busy = true; error = nil
        do {
            let loaded = try await Backend.call(["action": "load", "project": path], as: LoadedProject.self)
            var input: [String: Any] = ["action": "catalog", "disc": loaded.disc, "project": path,
                "archive": loaded.selectedArchive, "asset": loaded.selectedAsset]
            let catalog = try await Backend.call(input, as: Catalog.self)
            input["action"] = "entries"
            let entries = try await Backend.call(input, as: EntryList.self)
            input["action"] = "inspect"
            let selected = try await Backend.call(input, as: AssetDetail.self)
            disc = loaded.disc; project = path; archive = loaded.selectedArchive; asset = loaded.selectedAsset
            archives = catalog.archives; assetCount = catalog.assetCount; assets = entries.entries
            detail = selected; physicsDetail = nil; editingAsset = ""
            editingDetail = nil
            values = [:]; savedValues = [:]; undoStack = []; redoStack = []
            if selected.kind == "table" {
                editingAsset = selected.name
                editingDetail = selected
                if selected.name.lowercased() == "physics.val" { physicsDetail = selected }
                values = Dictionary((selected.sections ?? []).flatMap { section in
                    section.fields.filter(\.editable).map { ($0.id, $0.value) }
                }, uniquingKeysWith: { first, _ in first })
                savedValues = values
            }
            savedTestArchive = loaded.testCarArchive ?? (loaded.selectedArchive.lowercased().hasSuffix(".car") ? loaded.selectedArchive : "DATA/CAR/ALGAR.CAR"); remember()
            status = "Opened \(URL(fileURLWithPath: path).lastPathComponent)"
        } catch { self.error = error.localizedDescription }
        busy = false
    }
    func testGame() {
        guard canTest else { return }
        var input = request("launch_plan"); input["executable"] = executable
        busy = true; error = nil
        Task {
            defer { busy = false }
            do {
                let plan = try await Backend.call(input, as: LaunchPlan.self)
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
                try task.run()
                process = task; gameRunning = true; logPath = plan.logPath
                status = "Game process started · requested \(plan.car) / Anoka"
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
        status = sessionStopped ? "Test session stopped · exit \(code)" : "Game exited with code \(code)"
        if code != 0 && !sessionStopped { error = "Game failed (\(code)). See the session log." }
    }
    func stopGame() { sessionStopped = true; process?.terminate() }

    func prepareToQuit() -> Bool {
        if busy {
            let dialog = NSAlert(); dialog.messageText = "Wait for the current asset operation to finish."
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
