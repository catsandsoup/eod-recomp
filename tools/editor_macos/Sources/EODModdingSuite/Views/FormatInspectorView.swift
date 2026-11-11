import AppKit
import SwiftUI

struct FormatInspectorView: View {
    @ObservedObject var state: StudioState
    let detail: AssetDetail
    @State private var loaded: FormatDetail?
    @State private var failure: String?
    @State private var edits: [String: String] = [:]
    @State private var showRaw = false
    @State private var saving = false

    private var identity: String { state.archive + "#" + state.asset + "#" + detail.sha256 }

    var body: some View {
        Group {
            if let format = loaded?.format {
                content(format)
            } else if let failure {
                Text(failure).foregroundStyle(.secondary).padding(20)
            } else {
                ProgressView().controlSize(.small).padding(20)
            }
        }
        .task(id: identity) { await load() }
    }

    private func load() async {
        loaded = nil; failure = nil; edits = [:]; showRaw = false
        do { loaded = try await Backend.call(state.request("inspect"), as: FormatDetail.self) }
        catch { failure = error.localizedDescription }
    }

    @ViewBuilder private func content(_ format: FormatView) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    header(format)
                    ForEach(format.warnings, id: \.self) { warning in
                        Label(warning, systemImage: "info.circle.fill")
                            .font(.callout).padding(10).frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.yellow.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
                    }
                    if let preview = format.preview { previewView(preview) }
                    let groups = grouped(format.fields)
                    ForEach(groups, id: \.name) { group in
                        VStack(alignment: .leading, spacing: 8) {
                            if !group.name.isEmpty { Text(group.name).font(.headline) }
                            ForEach(group.fields) { field in fieldRow(field, editable: format.editable) }
                        }
                    }
                    let unknown = format.blocks.filter { !$0.known }
                    let known = format.blocks.filter(\.known)
                    if !known.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Text in this file").font(.headline)
                            ForEach(known) { block in
                                Text(block.text ?? block.name).font(.callout).textSelection(.enabled)
                                    .help("Offset \(block.offset), \(block.size) bytes")
                            }
                        }
                    }
                    if !unknown.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Not yet decoded").font(.headline)
                            ForEach(unknown) { block in
                                Text("\(block.name) · \(block.size.formatted()) bytes")
                                    .font(.callout).foregroundStyle(.secondary).help("Starts at byte \(block.offset)")
                            }
                        }
                    }
                    if showRaw, let hex = loaded?.hex {
                        ScrollView(.horizontal) {
                            Text(hex).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        }
                    }
                }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
            }
            Divider()
            HStack {
                Toggle("Show raw bytes", isOn: $showRaw).toggleStyle(.checkbox).font(.caption)
                Spacer()
                if !edits.isEmpty {
                    Button("Revert") { edits = [:] }
                    Button("Save Changes") { Task { await save(format) } }
                        .keyboardShortcut(.defaultAction).disabled(saving || state.busy)
                }
            }.padding(10)
        }
    }

    private func header(_ format: FormatView) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                Text(format.title).font(.title3.weight(.semibold))
                ConfidenceBadge(level: format.confidence)
                if loaded?.edited == true { Text("Edited").font(.caption).foregroundStyle(.orange) }
            }
            Text(format.summary).font(.callout).foregroundStyle(.secondary)
        }
    }

    private struct Group2 { let name: String; let fields: [FormatField] }
    private func grouped(_ fields: [FormatField]) -> [Group2] {
        var order: [String] = []
        var map: [String: [FormatField]] = [:]
        for field in fields {
            let name = field.group ?? ""
            if map[name] == nil { order.append(name) }
            map[name, default: []].append(field)
        }
        return order.map { Group2(name: $0, fields: map[$0] ?? []) }
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
        }
        .help(helpText(field))
    }

    private func helpText(_ field: FormatField) -> String {
        var parts: [String] = []
        if let explanation = field.explanation { parts.append(explanation) }
        if let evidence = field.evidence { parts.append(evidence) }
        if let lower = field.min, let upper = field.max { parts.append("Allowed: \(lower.formatted()) to \(upper.formatted())") }
        return parts.joined(separator: "\n")
    }

    @ViewBuilder private func editor(_ field: FormatField) -> some View {
        let binding = Binding<String>(get: { edits[field.id] ?? field.value.display },
                                      set: { edits[field.id] = $0 == field.value.display ? nil : $0 })
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

    @ViewBuilder private func previewView(_ preview: FormatPreview) -> some View {
        switch preview.kind {
        case "image":
            if let path = preview.png, let image = NSImage(contentsOfFile: path) {
                Image(nsImage: image).resizable().interpolation(.none).scaledToFit().frame(maxHeight: 320)
            }
        case "path":
            if let points = preview.points, points.count > 1 {
                TopDownPath(points: points, closed: preview.closed ?? false)
                    .frame(height: 280).background(Color.black.opacity(0.06), in: RoundedRectangle(cornerRadius: 6))
            }
        case "table":
            if let columns = preview.columns, let rows = preview.rows {
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                    GridRow { ForEach(columns, id: \.self) { Text($0).font(.caption.weight(.semibold)).foregroundStyle(.secondary) } }
                    Divider()
                    ForEach(rows.indices, id: \.self) { row in
                        GridRow { ForEach(rows[row].indices, id: \.self) { Text(rows[row][$0].display).font(.callout) } }
                    }
                }
            }
        default: EmptyView()
        }
    }

    private func save(_ format: FormatView) async {
        var changes: [String: Any] = [:]
        for (id, text) in edits {
            guard let field = format.fields.first(where: { $0.id == id }) else { continue }
            guard let value = field.value.edited(from: text, type: field.type) else {
                state.error = "\(field.label) needs a number."; return
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
            edits = [:]
            await load()
        } catch { state.error = error.localizedDescription }
    }
}

struct ConfidenceBadge: View {
    let level: String
    private var text: String {
        switch level {
        case "verified": return "Verified"
        case "high": return "High confidence"
        case "medium": return "Medium confidence"
        default: return "Unconfirmed"
        }
    }
    private var color: Color {
        switch level { case "verified", "high": return .green; case "medium": return .orange; default: return .gray }
    }
    var body: some View {
        Text(text).font(.caption2.weight(.medium)).padding(.horizontal, 6).padding(.vertical, 2)
            .background(color.opacity(0.18), in: Capsule()).foregroundStyle(color)
            .accessibilityLabel(text)
    }
}

/// Top-down view of a 3D polyline (x across, z down).
struct TopDownPath: View {
    let points: [[Double]]
    let closed: Bool
    var body: some View {
        Canvas { context, size in
            let xs = points.map { $0[0] }, zs = points.map { $0.count > 2 ? $0[2] : $0[1] }
            guard let minX = xs.min(), let maxX = xs.max(), let minZ = zs.min(), let maxZ = zs.max() else { return }
            let span = Swift.max(maxX - minX, maxZ - minZ, 0.0001)
            let scale = (Swift.min(size.width, size.height) - 24) / span
            let offsetX = (size.width - (maxX - minX) * scale) / 2, offsetY = (size.height - (maxZ - minZ) * scale) / 2
            var path = Path()
            for index in points.indices {
                let point = CGPoint(x: offsetX + (xs[index] - minX) * scale, y: offsetY + (zs[index] - minZ) * scale)
                if index == 0 { path.move(to: point) } else { path.addLine(to: point) }
            }
            if closed { path.closeSubpath() }
            context.stroke(path, with: .color(.accentColor), style: StrokeStyle(lineWidth: 2, lineJoin: .round))
        }
    }
}
