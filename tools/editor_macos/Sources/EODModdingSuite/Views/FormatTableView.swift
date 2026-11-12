import SwiftUI

/// A scrolling table for thousands of rows. Only the rows on screen are built, and the selected row
/// shows text fields for its editable cells, so large files cost the same as small ones.
struct FormatTableView: View {
    let table: FormatTable
    let editable: Bool
    @Binding var edits: [String: PendingEdit]
    /// Row id (the first cell) of the selected row.
    @Binding var selected: Int?
    @State private var search = ""
    @State private var matches: [Int] = []
    private let rowHeight: CGFloat = 26

    private var searchable: Bool { table.searchable ?? (table.rows.count > 20) }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if searchable {
                HStack {
                    TextField("Search", text: $search).textFieldStyle(.roundedBorder).frame(maxWidth: 260)
                    Text(search.isEmpty ? "\(table.rows.count.formatted()) rows"
                         : "\(matches.count.formatted()) of \(table.rows.count.formatted())")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            VStack(spacing: 0) {
                HStack(spacing: 8) {
                    ForEach(table.columns.indices, id: \.self) { index in
                        let column = table.columns[index]
                        Text(column.unit.map { "\(column.label) (\($0))" } ?? column.label)
                            .font(.caption.weight(.semibold)).foregroundStyle(.secondary).lineLimit(1)
                            .frame(width: widths[index], alignment: .leading)
                            .frame(maxWidth: widths[index] == nil ? .infinity : nil, alignment: .leading)
                    }
                }.padding(.horizontal, 8).frame(maxWidth: .infinity, alignment: .leading).frame(height: 24).background(Color.secondary.opacity(0.12))
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            ForEach(matches, id: \.self) { index in
                                row(index).id(index)
                            }
                        }
                    }
                    .frame(height: Swift.min(CGFloat(Swift.max(matches.count, 1)) * rowHeight + 60, 360))
                    .onChange(of: selected) { _, new in
                        guard let new, let position = matches.first(where: { rowID(table.rows[$0]) == new }) else { return }
                        withAnimation(.none) { proxy.scrollTo(position, anchor: .center) }
                    }
                }
            }
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.secondary.opacity(0.25)))
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .task(id: search + String(table.rows.count)) { await refilter() }
    }

    // MARK: Columns

    /// Fixed width per column; nil means the column takes the remaining space.
    private var widths: [CGFloat?] {
        let sample = table.rows.prefix(60)
        return table.columns.indices.map { index in
            let column = table.columns[index]
            if index == 0 { return 46 }
            let isLast = index == table.columns.count - 1
            let numeric = sample.allSatisfy { if case .number = $0[safe: index] ?? .empty { return true } else { return false } }
            if numeric { return 92 }
            if isLast || column.newline != nil { return nil }
            let longest = sample.map { ($0[safe: index]?.display.count ?? 0) }.max() ?? 8
            return CGFloat(Swift.min(Swift.max(longest, column.label.count, 6), 28)) * 7 + 12
        }
    }

    private func rowID(_ row: [FormatValue]) -> Int {
        if case .number(let value) = row.first ?? .empty { return Int(value) }
        return Int(row.first?.display ?? "") ?? -1
    }

    private func refilter() async {
        let needle = search.trimmingCharacters(in: .whitespaces).lowercased()
        let rows = table.rows
        if needle.isEmpty { matches = Array(rows.indices); return }
        try? await Task.sleep(nanoseconds: 120_000_000)  // wait for the typing to pause
        if Task.isCancelled { return }
        let found = await Task.detached(priority: .userInitiated) {
            rows.indices.filter { index in rows[index].contains { $0.display.lowercased().contains(needle) } }
        }.value
        if !Task.isCancelled { matches = found }
    }

    // MARK: Rows

    private func editID(_ column: FormatColumn, _ row: [FormatValue]) -> String? {
        guard editable, let template = column.edit else { return nil }
        return template.replacingOccurrences(of: "{id}", with: String(rowID(row)))
    }

    private func shown(_ column: FormatColumn, _ cell: FormatValue, id: String?) -> String {
        let text = id.flatMap { edits[$0]?.text } ?? cell.display
        if let marker = column.newline { return text.replacingOccurrences(of: "\n", with: marker) }
        return text
    }

    private func row(_ index: Int) -> some View {
        let row = table.rows[index]
        let id = rowID(row)
        let isSelected = selected == id
        return HStack(alignment: .firstTextBaseline, spacing: 8) {
            ForEach(table.columns.indices, id: \.self) { column in
                cell(table.columns[column], row, column: column, selected: isSelected)
                    .frame(width: widths[column], alignment: .leading)
                    .frame(maxWidth: widths[column] == nil ? .infinity : nil, alignment: .leading)
            }
        }
        .padding(.horizontal, 8).padding(.vertical, isSelected ? 4 : 0)
        .frame(maxWidth: .infinity, minHeight: rowHeight, alignment: .leading)
        .background(isSelected ? Color.accentColor.opacity(0.18) : (index % 2 == 0 ? Color.clear : Color.secondary.opacity(0.06)))
        .contentShape(Rectangle())
        .onTapGesture { selected = id }
    }

    @ViewBuilder private func cell(_ column: FormatColumn, _ row: [FormatValue], column position: Int, selected: Bool) -> some View {
        let value = row[safe: position] ?? .empty
        let id = editID(column, row)
        if selected, let id {
            TextField(column.label, text: Binding(
                get: { shown(column, value, id: id) },
                set: { new in
                    let real = column.newline.map { new.replacingOccurrences(of: $0, with: "\n") } ?? new
                    edits[id] = real == value.display ? nil
                        : PendingEdit(text: real, type: column.type ?? "str", label: column.label)
                }), axis: column.newline != nil ? .vertical : .horizontal)
                .textFieldStyle(.roundedBorder).lineLimit(1...6)
        } else {
            let edited = id.flatMap { edits[$0] } != nil
            Text(shown(column, value, id: id)).font(.callout).lineLimit(2)
                .foregroundStyle(edited ? Color.orange : Color.primary)
        }
    }
}

extension Array {
    subscript(safe index: Int) -> Element? { indices.contains(index) ? self[index] : nil }
}

/// Top-down racing line with start marker, direction arrows, hover/selection labels and draggable points.
struct PathPreview: View {
    /// Points as stored in the file; fixes the framing so dragging a point does not rescale the view.
    let original: [CGPoint]
    /// Points with unsaved edits applied.
    let positions: [CGPoint]
    let nodes: [Int]
    let closed: Bool
    let unit: String
    let startLabel: String?
    let arrows: Bool
    @Binding var selected: Int?
    let canDrag: Bool
    let onMove: (Int, Double, Double) -> Void
    @State private var hover: Int?
    @State private var dragging: Int?

    private struct Frame {
        var scale: CGFloat; var originX: CGFloat; var originY: CGFloat; var minX: CGFloat; var minZ: CGFloat
        func screen(_ p: CGPoint) -> CGPoint { CGPoint(x: originX + (p.x - minX) * scale, y: originY + (p.y - minZ) * scale) }
        func world(_ p: CGPoint) -> CGPoint { CGPoint(x: (p.x - originX) / scale + minX, y: (p.y - originY) / scale + minZ) }
    }

    private func frame(_ size: CGSize) -> Frame {
        let xs = original.map(\.x), zs = original.map(\.y)
        let minX = xs.min() ?? 0, maxX = xs.max() ?? 1, minZ = zs.min() ?? 0, maxZ = zs.max() ?? 1
        let span = Swift.max(maxX - minX, maxZ - minZ, 0.0001)
        let scale = (Swift.min(size.width, size.height) - 48) / span
        return Frame(scale: scale, originX: (size.width - (maxX - minX) * scale) / 2,
                     originY: (size.height - (maxZ - minZ) * scale) / 2, minX: minX, minZ: minZ)
    }

    private func nearest(_ location: CGPoint, in frame: Frame, within limit: CGFloat) -> Int? {
        var best: (Int, CGFloat)?
        for index in positions.indices {
            let p = frame.screen(positions[index])
            let distance = hypot(p.x - location.x, p.y - location.y)
            if distance <= limit, distance < (best?.1 ?? .infinity) { best = (index, distance) }
        }
        return best?.0
    }

    var body: some View {
        GeometryReader { geometry in
            let frame = frame(geometry.size)
            Canvas { context, size in
                draw(&context, frame: frame, size: size)
            }
            .contentShape(Rectangle())
            .onContinuousHover { phase in
                switch phase {
                case .active(let location): hover = nearest(location, in: frame, within: 14)
                case .ended: hover = nil
                }
            }
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { value in
                    if dragging == nil {
                        dragging = nearest(value.startLocation, in: frame, within: 14) ?? -1
                        if let index = dragging, index >= 0 { selected = nodes[index] }
                    }
                    guard canDrag, let index = dragging, index >= 0 else { return }
                    let world = frame.world(value.location)
                    onMove(nodes[index], Double(world.x), Double(world.y))
                }
                .onEnded { _ in dragging = nil })
        }
        .background(Color.black.opacity(0.06), in: RoundedRectangle(cornerRadius: 6))
        .overlay(alignment: .topLeading) {
            Text("Top-down map · X to the right, Z down").font(.caption2).foregroundStyle(.secondary).padding(8)
        }
    }

    private func draw(_ context: inout GraphicsContext, frame: Frame, size: CGSize) {
        guard positions.count > 1 else { return }
        let screen = positions.map(frame.screen)
        var path = Path()
        path.move(to: screen[0])
        for point in screen.dropFirst() { path.addLine(to: point) }
        if closed { path.closeSubpath() }
        context.stroke(path, with: .color(.accentColor), style: StrokeStyle(lineWidth: 2.5, lineJoin: .round))

        if arrows {
            let step = Swift.max(screen.count / 14, 1)
            for index in stride(from: step / 2, to: screen.count - 1, by: step) {
                let a = screen[index], b = screen[Swift.min(index + 3, screen.count - 1)]
                let angle = atan2(b.y - a.y, b.x - a.x)
                var head = Path()
                head.move(to: CGPoint(x: 7, y: 0)); head.addLine(to: CGPoint(x: -5, y: 5)); head.addLine(to: CGPoint(x: -5, y: -5))
                head.closeSubpath()
                let placed = head.applying(CGAffineTransform(rotationAngle: angle).concatenating(CGAffineTransform(translationX: a.x, y: a.y)))
                context.fill(placed, with: .color(.accentColor.opacity(0.9)))
                context.stroke(placed, with: .color(Color(nsColor: .windowBackgroundColor)), lineWidth: 1)
            }
        }
        if screen.count <= 300 {
            for point in screen {
                context.fill(Path(ellipseIn: CGRect(x: point.x - 2, y: point.y - 2, width: 4, height: 4)), with: .color(.accentColor.opacity(0.6)))
            }
        }

        // Start / finish marker on the first point.
        let start = screen[0]
        context.fill(Path(ellipseIn: CGRect(x: start.x - 7, y: start.y - 7, width: 14, height: 14)), with: .color(.green))
        context.stroke(Path(ellipseIn: CGRect(x: start.x - 7, y: start.y - 7, width: 14, height: 14)),
                       with: .color(Color(nsColor: .windowBackgroundColor)), lineWidth: 2)
        if let startLabel {
            context.draw(context.resolve(Text(startLabel).font(.caption.weight(.semibold))), at: CGPoint(x: start.x + 12, y: start.y - 12), anchor: .leading)
        }

        // Selected and hovered points.
        let selectedIndex = selected.flatMap { nodes.firstIndex(of: $0) }
        for (index, color) in [(hover, Color.secondary), (selectedIndex, Color.orange)] {
            guard let index, index < screen.count else { continue }
            let point = screen[index]
            context.stroke(Path(ellipseIn: CGRect(x: point.x - 8, y: point.y - 8, width: 16, height: 16)), with: .color(color), lineWidth: 2.5)
            let label = String(format: "Point %d · X %.1f, Z %.1f \(unit)", nodes[index], positions[index].x, positions[index].y)
            let text = context.resolve(Text(label).font(.caption.weight(.medium)))
            let anchorX = Swift.min(Swift.max(point.x, 110), size.width - 110)
            let above = point.y > 40
            let spot = CGPoint(x: anchorX, y: point.y + (above ? -18 : 18))
            let box = CGRect(x: spot.x - 100, y: spot.y - 9, width: 200, height: 18)
            context.fill(Path(roundedRect: box, cornerRadius: 4), with: .color(Color(nsColor: .windowBackgroundColor).opacity(0.92)))
            context.draw(text, at: spot, anchor: .center)
        }

        // Scale bar.
        let metres = niceLength(Double(60 / frame.scale))
        let width = CGFloat(metres) * frame.scale
        let y = size.height - 14
        var bar = Path()
        bar.move(to: CGPoint(x: 14, y: y)); bar.addLine(to: CGPoint(x: 14 + width, y: y))
        context.stroke(bar, with: .color(.secondary), lineWidth: 2)
        context.draw(context.resolve(Text("\(Int(metres)) \(unit)").font(.caption2)), at: CGPoint(x: 14 + width + 6, y: y), anchor: .leading)
    }

    private func niceLength(_ target: Double) -> Double {
        guard target > 0, target.isFinite else { return 10 }
        let power = pow(10, floor(log10(target)))
        for factor in [1.0, 2, 5, 10] where factor * power >= target * 0.7 { return factor * power }
        return power * 10
    }
}
