import AppKit
import SwiftUI

/// A change the modder has typed but not saved yet.
struct PendingEdit: Equatable {
    var text: String
    var type: String
    var label: String
}

struct FormatInspectorView: View {
    @ObservedObject var state: StudioState
    let detail: AssetDetail
    /// Full field list, blocks and bytes; loaded only when "Show file internals" is on.
    @State private var internals: AssetDetail?
    @State private var showInternals = false
    @State private var internalsLoading = false
    @State private var edits: [String: PendingEdit] = [:]
    @State private var saving = false
    @State private var toggledGroups: Set<String> = []
    @State private var showHow = false
    @State private var selectedRow: Int?

    private var assetKey: String { state.archive + "#" + state.asset }
    private var format: FormatView? { (showInternals ? internals?.format : nil) ?? detail.format }

    var body: some View {
        Group {
            if let format {
                content(format)
            } else {
                Text("This file can't be shown.").foregroundStyle(.secondary).padding(20)
            }
        }
        .task(id: assetKey) {
            edits = [:]; internals = nil; showInternals = false; selectedRow = nil; toggledGroups = []; showHow = false
            #if DEBUG
            // EOD_KIT_ROW=<n> selects row n, EOD_KIT_INTERNALS=1 turns on file internals (for screenshots).
            let environment = ProcessInfo.processInfo.environment
            if let row = environment["EOD_KIT_ROW"].flatMap({ Int($0) }) { selectedRow = row }
            if environment["EOD_KIT_INTERNALS"] != nil { setInternals(true) }
            if environment["EOD_KIT_HOW"] != nil { showHow = true }
            #endif
        }
        .onChange(of: detail.sha256) { _, _ in
            if showInternals { Task { await loadInternals() } }
        }
    }

    // MARK: Loading

    private func loadInternals() async {
        internalsLoading = true
        defer { internalsLoading = false }
        let target = assetKey
        var input = state.request("inspect")
        input["expert"] = true
        do {
            let result = try await Backend.call(input, as: AssetDetail.self)
            if assetKey == target { internals = result }
        } catch { state.error = error.localizedDescription }
    }

    // MARK: Layout

    @ViewBuilder private func content(_ format: FormatView) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 16) {
                    header(format)
                    ForEach(format.status == "empty" ? [] : format.warnings, id: \.self) { warning in
                        Label(warning, systemImage: "info.circle.fill")
                            .font(.callout).padding(10).frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.yellow.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
                    }
                    if let preview = format.preview { previewView(preview, format: format) }
                    if let table = format.table { tableView(table, format: format) }
                    ForEach(format.notes ?? [], id: \.self) { note in
                        Label(note, systemImage: "lightbulb").font(.callout).foregroundStyle(.secondary)
                    }
                    fieldGroups(format)
                    if showInternals { internalsSection(format) }
                    if let how = format.howWeKnow, !how.isEmpty { howWeKnow(how) }
                }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
            }
            Divider()
            HStack {
                Toggle("Show file internals", isOn: Binding(get: { showInternals }, set: { setInternals($0) }))
                    .toggleStyle(.checkbox).font(.caption)
                    .help("Shows every stored value, byte ranges and the raw bytes. Meant for people checking how the file is laid out.")
                if internalsLoading { ProgressView().controlSize(.mini) }
                Spacer()
                if !edits.isEmpty {
                    Text("\(edits.count) unsaved change\(edits.count == 1 ? "" : "s")").font(.caption).foregroundStyle(.orange)
                    Button("Revert") { edits = [:] }
                    Button("Save Changes") { Task { await save(format) } }
                        .keyboardShortcut(.defaultAction).disabled(saving || state.isWriting)
                }
            }.padding(10)
        }
    }

    private func setInternals(_ on: Bool) {
        showInternals = on
        if on, internals == nil { Task { await loadInternals() } }
    }

    private func header(_ format: FormatView) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(format.title).font(.title3.weight(.semibold))
                if format.status != "empty" { ConfidenceBadge(level: format.confidence) }
                if detail.edited == true { Text("Edited").font(.caption).foregroundStyle(.orange) }
            }
            if format.status != nil {
                VStack(alignment: .leading, spacing: 10) {
                    Label {
                        Text(format.summary).font(.callout)
                    } icon: {
                        Image(systemName: format.status == "empty" ? "doc" : "nosign").foregroundStyle(.secondary)
                    }
                    if let action = format.action, let target = actionTarget(action) {
                        Button(action.label) { state.selectAsset(target) }
                    }
                }
                .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.secondary.opacity(0.1), in: RoundedRectangle(cornerRadius: 8))
            } else {
                Text(format.summary).font(.callout).foregroundStyle(.secondary)
            }
        }
    }

    /// The asset the action points at, when it exists in the selected archive.
    private func actionTarget(_ action: FormatAction) -> String? {
        guard let wanted = action.asset?.lowercased(), action.archive == nil || action.archive == state.archive else { return nil }
        return state.assets.first(where: { $0.name.lowercased() == wanted })?.name
    }

    // MARK: Preview and table

    @ViewBuilder private func previewView(_ preview: FormatPreview, format: FormatView) -> some View {
        switch preview.kind {
        case "image":
            if let path = preview.png {
                FileImage(path: path, maxPixel: 1600, smooth: true).frame(maxHeight: 340)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        case "path":
            if let points = preview.points, points.count > 1 {
                pathPreview(preview, points: points, format: format)
            }
        case "table":
            if let columns = preview.columns, let rows = preview.rows {
                if rows.count <= 40 {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                        GridRow { ForEach(columns) { Text($0.label).font(.caption.weight(.semibold)).foregroundStyle(.secondary) } }
                        Divider()
                        ForEach(rows.indices, id: \.self) { row in
                            GridRow { ForEach(rows[row].indices, id: \.self) { Text(rows[row][$0].display).font(.callout) } }
                        }
                    }
                } else {
                    FormatTableView(table: FormatTable(name: nil, role: nil, searchable: true, columns: columns, rows: rows),
                                    editable: false, edits: $edits, selected: $selectedRow)
                }
            }
        default: EmptyView()
        }
    }

    private func tableView(_ table: FormatTable, format: FormatView) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            if let name = table.name { Text(name).font(.headline) }
            FormatTableView(table: table, editable: format.editable, edits: $edits, selected: $selectedRow)
        }
    }

    private func pathPreview(_ preview: FormatPreview, points: [[Double]], format: FormatView) -> some View {
        let nodes = preview.nodes ?? Array(points.indices)
        let xColumn = format.table?.columns.first(where: { $0.key == "x" })
        let zColumn = format.table?.columns.first(where: { $0.key == "z" })
        func editID(_ column: FormatColumn?, _ node: Int) -> String? {
            column?.edit?.replacingOccurrences(of: "{id}", with: String(node))
        }
        let positions = points.indices.map { index -> CGPoint in
            let node = nodes[index]
            let x = editID(xColumn, node).flatMap { edits[$0] }.flatMap { Double($0.text) } ?? points[index][0]
            let z = editID(zColumn, node).flatMap { edits[$0] }.flatMap { Double($0.text) } ?? points[index][2]
            return CGPoint(x: x, y: z)
        }
        let canDrag = format.editable && xColumn?.edit != nil && zColumn?.edit != nil
        return VStack(alignment: .leading, spacing: 4) {
            PathPreview(original: points.map { CGPoint(x: $0[0], y: $0[2]) }, positions: positions, nodes: nodes,
                        closed: preview.closed ?? false, unit: preview.unit ?? "m", startLabel: preview.start,
                        arrows: preview.direction ?? false, selected: $selectedRow, canDrag: canDrag) { node, x, z in
                move(node, x: x, z: z, xColumn: xColumn, zColumn: zColumn)
            }
            .frame(height: 320)
            if canDrag {
                Text("Drag a point to move it, or select a row below and type new X and Z values.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private func move(_ node: Int, x: Double, z: Double, xColumn: FormatColumn?, zColumn: FormatColumn?) {
        guard let xTemplate = xColumn?.edit, let zTemplate = zColumn?.edit else { return }
        func clamp(_ value: Double, _ column: FormatColumn?) -> Double {
            Swift.min(column?.max ?? .infinity, Swift.max(column?.min ?? -.infinity, value))
        }
        let id = String(node)
        edits[xTemplate.replacingOccurrences(of: "{id}", with: id)] =
            PendingEdit(text: String(format: "%.3f", clamp(x, xColumn)), type: "f32", label: "X")
        edits[zTemplate.replacingOccurrences(of: "{id}", with: id)] =
            PendingEdit(text: String(format: "%.3f", clamp(z, zColumn)), type: "f32", label: "Z")
    }

    // MARK: Fields

    private struct FieldGroup: Identifiable { let name: String; let fields: [FormatField]; var id: String { name } }

    private func groups(_ fields: [FormatField]) -> [FieldGroup] {
        var order: [String] = []
        var map: [String: [FormatField]] = [:]
        for field in fields {
            let name = field.group ?? ""
            if map[name] == nil { order.append(name) }
            map[name, default: []].append(field)
        }
        return order.map { FieldGroup(name: $0, fields: map[$0] ?? []) }
    }

    @ViewBuilder private func fieldGroups(_ format: FormatView) -> some View {
        let visible = format.fields.filter { showInternals || $0.expert != true }
        let all = groups(visible)
        let openByDefault = visible.count <= 30
        ForEach(all) { group in
            let open = group.name.isEmpty || (openByDefault != toggledGroups.contains(group.name))
            VStack(alignment: .leading, spacing: 8) {
                if !group.name.isEmpty {
                    SectionHeader(title: group.name, detail: "\(group.fields.count) value\(group.fields.count == 1 ? "" : "s")", open: open) {
                        if toggledGroups.contains(group.name) { toggledGroups.remove(group.name) } else { toggledGroups.insert(group.name) }
                    }
                }
                if open {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        ForEach(group.fields) { field in fieldRow(field, editable: format.editable) }
                    }.padding(.horizontal, 4)
                }
            }
        }
    }

    @ViewBuilder private func fieldRow(_ field: FormatField, editable: Bool) -> some View {
        let canEdit = editable && (field.editable ?? false)
        VStack(alignment: .leading, spacing: 2) {
            HStack(alignment: .firstTextBaseline) {
                Text(field.label)
                if let level = field.confidence, level != "verified", level != "high" { ConfidenceBadge(level: level) }
                Spacer()
                if canEdit { editor(field) }
                else { Text(field.value.display).textSelection(.enabled) }
                if let unit = field.unit, !unit.isEmpty { Text(unit).foregroundStyle(.secondary).frame(minWidth: 28, alignment: .leading) }
            }
            if let explanation = field.explanation, !explanation.isEmpty {
                Text(explanation).font(.caption).foregroundStyle(.secondary)
            }
            if showInternals, let evidence = field.evidence, !evidence.isEmpty {
                Text(evidence).font(.caption2).foregroundStyle(.tertiary)
            }
        }
        .help(helpText(field))
    }

    private func helpText(_ field: FormatField) -> String {
        var parts: [String] = []
        if let explanation = field.explanation { parts.append(explanation) }
        if let lower = field.min, let upper = field.max { parts.append("Allowed: \(lower.formatted()) to \(upper.formatted())") }
        return parts.joined(separator: "\n")
    }

    @ViewBuilder private func editor(_ field: FormatField) -> some View {
        let binding = Binding<String>(
            get: { edits[field.id]?.text ?? field.value.display },
            set: { edits[field.id] = $0 == field.value.display ? nil : PendingEdit(text: $0, type: field.type, label: field.label) })
        if let options = field.options, !options.isEmpty {
            Picker("", selection: binding) {
                ForEach(options.indices, id: \.self) { Text(options[$0].label).tag(options[$0].value.display) }
            }.labelsHidden().frame(width: 160)
        } else if field.type == "bool" {
            Toggle("", isOn: Binding(get: { binding.wrappedValue == "Yes" }, set: { binding.wrappedValue = $0 ? "Yes" : "No" })).labelsHidden()
        } else {
            HStack(spacing: 4) {
                TextField(field.label, text: binding).textFieldStyle(.roundedBorder).multilineTextAlignment(.trailing).frame(width: 130)
                if field.type != "str", field.type != "vec3", let lower = field.min, let upper = field.max {
                    Stepper("", onIncrement: { step(field, binding, 1, lower, upper) },
                            onDecrement: { step(field, binding, -1, lower, upper) }).labelsHidden()
                }
            }
        }
    }

    private func step(_ field: FormatField, _ binding: Binding<String>, _ direction: Double, _ lower: Double, _ upper: Double) {
        let current = Double(binding.wrappedValue) ?? lower
        let increment = field.type == "f32" ? Swift.max((upper - lower) / 100, 0.01) : 1
        binding.wrappedValue = FormatValue.number(Swift.min(upper, Swift.max(lower, current + direction * increment))).display
    }

    // MARK: Evidence and internals

    private func howWeKnow(_ items: [FormatHow]) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionHeader(title: "How we know this", detail: nil, open: showHow) { showHow.toggle() }
            if showHow {
                ForEach(items) { item in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        if let level = item.confidence { ConfidenceBadge(level: level) }
                        Text(item.text).font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                }
            }
        }
    }

    @ViewBuilder private func internalsSection(_ format: FormatView) -> some View {
        let known = format.blocks.filter(\.known)
        let unknown = format.blocks.filter { !$0.known }
        if !known.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                Text("Text found in this file").font(.headline)
                ForEach(known.prefix(200)) { block in
                    Text(block.text ?? block.name).font(.callout).textSelection(.enabled)
                        .help("Byte \(block.offset), \(block.size) bytes")
                }
                if known.count > 200 { Text("\(known.count - 200) more not shown").font(.caption).foregroundStyle(.secondary) }
            }
        }
        if !unknown.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                Text("Parts not understood yet").font(.headline)
                ForEach(unknown.prefix(200)) { block in
                    Text("\(block.name) · \(block.size.formatted()) bytes")
                        .font(.callout).foregroundStyle(.secondary).help("Starts at byte \(block.offset)")
                }
            }
        }
        if let hex = internals?.hex ?? detail.hex {
            VStack(alignment: .leading, spacing: 4) {
                Text("Raw bytes (first 8 KB)").font(.headline)
                ScrollView(.horizontal) {
                    Text(hex).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                }
            }
        }
    }

    // MARK: Saving

    private func save(_ format: FormatView) async {
        var changes: [String: Any] = [:]
        for (id, edit) in edits {
            guard let value = FormatValue.text(edit.text).edited(from: edit.text, type: edit.type) else {
                state.error = "\(edit.label) needs a number."; return
            }
            changes[id] = value
        }
        var input = state.request("save_format")
        input["formatEdits"] = changes
        saving = true; state.error = nil
        defer { saving = false }
        do {
            let saved = try await Backend.call(input, as: SavedProject.self)
            state.status = "Saved \(URL(fileURLWithPath: saved.archivePath).lastPathComponent)"
            await state.inspectSelection()
            edits = [:]
        } catch { state.error = error.localizedDescription }
    }
}

// MARK: - Shared pieces

/// A full-width, large-hit-area header that opens and closes the section under it.
struct SectionHeader: View {
    let title: String
    let detail: String?
    let open: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: "chevron.right").font(.system(size: 11, weight: .semibold))
                    .rotationEffect(.degrees(open ? 90 : 0)).frame(width: 14).foregroundStyle(.secondary)
                Text(title).font(.headline)
                Spacer()
                if let detail { Text(detail).font(.caption).foregroundStyle(.secondary) }
            }
            .padding(.horizontal, 10).frame(maxWidth: .infinity, minHeight: 38, alignment: .leading)
            .background(Color.secondary.opacity(0.1), in: RoundedRectangle(cornerRadius: 6))
            .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

struct ConfidenceBadge: View {
    let level: String
    private var text: String {
        switch level {
        case "verified": return "Confirmed in game"
        case "high": return "From the game's code"
        default: return "Best guess"
        }
    }
    private var tip: String {
        switch level {
        case "verified": return "Matches the game's own code and was seen working in a test run."
        case "high": return "Read from the game's code or clear in the data, but not yet seen in a test run."
        case "medium": return "The data points this way, but nothing in the game's code confirms it."
        default: return "A guess. Treat it with care and test in the game."
        }
    }
    private var color: Color {
        switch level { case "verified", "high": return .green; case "medium": return .orange; default: return .gray }
    }
    var body: some View {
        Text(text).font(.caption2.weight(.medium)).padding(.horizontal, 6).padding(.vertical, 2)
            .background(color.opacity(0.18), in: Capsule()).foregroundStyle(color)
            .help(tip)
            .accessibilityLabel(text)
    }
}

/// An image decoded off the main thread, optionally scaled down to `maxPixel`.
struct FileImage: View {
    let path: String
    var maxPixel: Int?
    var smooth = false
    @State private var loaded: LoadedImage?
    var body: some View {
        Group {
            if let loaded {
                Image(decorative: loaded.cg, scale: 1).resizable().interpolation(smooth ? .medium : .none).scaledToFit()
            } else {
                ProgressView().controlSize(.small).frame(maxWidth: .infinity, minHeight: 80)
            }
        }
        .task(id: path + String(maxPixel ?? 0)) {
            loaded = nil
            let file = path, limit = maxPixel
            loaded = await Task.detached(priority: .userInitiated) { ImageLoader.load(file, maxPixel: limit) }.value
        }
    }
}

struct LoadedImage: @unchecked Sendable { let cg: CGImage }

enum ImageLoader {
    /// Decodes fully on the calling thread, so the main thread only draws the finished bitmap.
    nonisolated static func load(_ path: String, maxPixel: Int? = nil) -> LoadedImage? {
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil) else { return nil }
        if let maxPixel, let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
           let width = props[kCGImagePropertyPixelWidth] as? Int, let height = props[kCGImagePropertyPixelHeight] as? Int,
           Swift.max(width, height) > maxPixel {
            let options: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                            kCGImageSourceCreateThumbnailWithTransform: true,
                                            kCGImageSourceShouldCacheImmediately: true,
                                            kCGImageSourceThumbnailMaxPixelSize: maxPixel]
            return CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary).map(LoadedImage.init)
        }
        return CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary)
            .map(LoadedImage.init)
    }
}
