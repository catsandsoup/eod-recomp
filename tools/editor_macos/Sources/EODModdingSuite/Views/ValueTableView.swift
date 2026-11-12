import SwiftUI

/// Editable value tables (vehicle physics and similar). Sections are full-width headers; each value shows
/// its plain-language label, unit and explanation when the backend supplies them, and the raw key as
/// small secondary text.
struct ValueTableView: View {
    @ObservedObject var state: StudioState
    let detail: AssetDetail
    @State private var toggled: Set<Int> = []

    private var sections: [AssetSection] { detail.sections ?? [] }
    /// A short table opens fully; a long one starts closed so only the headers are built.
    private var openByDefault: Bool { sections.count <= 2 || sections.reduce(0) { $0 + $1.fields.count } <= 24 }
    private func isOpen(_ index: Int) -> Bool { openByDefault != toggled.contains(index) }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 12) {
                if let title = detail.title { Text(title).font(.title3.weight(.semibold)) }
                if let summary = detail.summary { Text(summary).font(.callout).foregroundStyle(.secondary) }
                if sections.count > 2 {
                    HStack {
                        Button("Open all") { toggled = openByDefault ? [] : Set(sections.indices) }
                        Button("Close all") { toggled = openByDefault ? Set(sections.indices) : [] }
                    }.font(.caption)
                }
                ForEach(sections.indices, id: \.self) { index in
                    let section = sections[index]
                    VStack(alignment: .leading, spacing: 8) {
                        SectionHeader(title: section.label ?? section.name,
                                      detail: "\(section.fields.count) value\(section.fields.count == 1 ? "" : "s")",
                                      open: isOpen(index)) {
                            if toggled.contains(index) { toggled.remove(index) } else { toggled.insert(index) }
                        }
                        if isOpen(index) {
                            if let explanation = section.explanation, !explanation.isEmpty {
                                Text(explanation).font(.caption).foregroundStyle(.secondary).padding(.horizontal, 4)
                            }
                            LazyVStack(spacing: 10) {
                                ForEach(section.fields.indices, id: \.self) { i in row(section.fields[i]) }
                            }.padding(.horizontal, 4)
                        }
                    }
                }
            }.padding(18)
        }.disabled(state.busy || state.gameRunning)
    }

    @ViewBuilder private func row(_ field: AssetField) -> some View {
        let unused = field.used == false
        VStack(alignment: .leading, spacing: 2) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 1) {
                    Text(field.label ?? field.key).font(field.label == nil ? .system(.callout, design: .monospaced) : .callout)
                    if field.label != nil, field.label != field.key {
                        Text(field.key).font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
                    }
                }
                if unused {
                    Text("Not used by the game").font(.caption2.weight(.medium)).padding(.horizontal, 6).padding(.vertical, 2)
                        .background(Color.gray.opacity(0.2), in: Capsule()).foregroundStyle(.secondary)
                        .help("The game reads this value but does nothing with it, so changing it has no effect.")
                }
                if let level = field.confidence, level != "verified", level != "high" { ConfidenceBadge(level: level) }
                Spacer()
                if field.editable {
                    let key = field.id
                    TextField(field.label ?? field.key, text: Binding(get: { state.values[key] ?? field.value }, set: { state.edit(key, $0) }))
                        .textFieldStyle(.roundedBorder).multilineTextAlignment(.trailing).frame(minWidth: 110, maxWidth: 200)
                } else {
                    Text(field.value).multilineTextAlignment(.trailing).textSelection(.enabled)
                }
                if !field.unit.isEmpty { Text(field.unit).foregroundStyle(.secondary).frame(minWidth: 28, alignment: .leading) }
            }
            if let explanation = field.explanation, !explanation.isEmpty {
                Text(explanation).font(.caption).foregroundStyle(.secondary)
            }
            if let lower = field.min, let upper = field.max {
                Text("Allowed range: \(lower.formatted()) to \(upper.formatted())").font(.caption2).foregroundStyle(.tertiary)
            }
        }
        .opacity(unused ? 0.6 : 1)
        .help(field.explanation ?? "")
    }
}
